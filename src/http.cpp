#include "common.h"

#include <winhttp.h>

#include <algorithm>
#include <cwctype>

namespace {

std::wstring Trim(const std::wstring& in) {
    size_t b = 0, e = in.size();
    while (b < e && std::iswspace(in[b])) ++b;
    while (e > b && std::iswspace(in[e - 1])) --e;
    return in.substr(b, e - b);
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

// WinHTTP exposes headers through a single query. Split them into a small
// associative view while preserving the original casing for lookups.
std::vector<std::pair<std::wstring, std::wstring>> SplitHeaders(
    const std::wstring& raw) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find(L"\r\n", pos);
        if (eol == std::wstring::npos) eol = raw.size();
        std::wstring line = raw.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.empty()) continue;
        size_t colon = line.find(L':');
        if (colon == std::wstring::npos) continue;
        out.emplace_back(Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
    }
    return out;
}

bool HeaderValue(const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                 const wchar_t* name, std::wstring& value) {
    const std::wstring want = Lower(name);
    for (const auto& kv : headers) {
        if (Lower(kv.first) == want) {
            value = kv.second;
            return true;
        }
    }
    return false;
}

// Returns true when the transfer ended because WinHTTP was cancelled from
// another thread rather than because of a real transport failure.
bool IsCancellation(DWORD error) {
    return error == ERROR_WINHTTP_OPERATION_CANCELLED;
}

}  // namespace

std::wstring FileNameFromDisposition(const std::wstring& header) {
    // filename*=UTF-8''name takes precedence over filename="name".
    std::wstring lower = Lower(header);
    size_t star = lower.find(L"filename*=");
    if (star != std::wstring::npos) {
        size_t begin = star + 10;
        size_t end = header.find(L';', begin);
        std::wstring raw = header.substr(
            begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        size_t quote = raw.find(L"''");
        if (quote != std::wstring::npos) raw = raw.substr(quote + 2);
        std::string bytes;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == L'%' && i + 2 < raw.size()) {
                auto hex = [](wchar_t c) -> int {
                    if (c >= L'0' && c <= L'9') return c - L'0';
                    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                    return -1;
                };
                int hi = hex(raw[i + 1]), lo = hex(raw[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    bytes.push_back((char)((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            bytes.push_back((char)raw[i]);
        }
        std::wstring decoded = Utf8ToWide(bytes);
        if (!decoded.empty()) return SanitizeFileName(decoded);
    }

    size_t pos = lower.find(L"filename=");
    if (pos != std::wstring::npos) {
        size_t begin = pos + 9;
        size_t end = header.find(L';', begin);
        std::wstring raw = Trim(header.substr(
            begin, end == std::wstring::npos ? std::wstring::npos : end - begin));
        if (raw.size() >= 2 && raw.front() == L'"' && raw.back() == L'"') {
            raw = raw.substr(1, raw.size() - 2);
        }
        if (!raw.empty()) return SanitizeFileName(raw);
    }
    return L"";
}

HttpClient::HttpClient() = default;

HttpClient::~HttpClient() { Close(); }

bool HttpClient::Open(const Config& cfg) {
    Close();
    cfg_ = cfg;

    DWORD access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    LPCWSTR proxy = WINHTTP_NO_PROXY_NAME;
    if (cfg.proxyMode == 0) {
        access = WINHTTP_ACCESS_TYPE_NO_PROXY;
    } else if (cfg.proxyMode == 2 && !cfg.proxyAddress.empty()) {
        access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxy = cfg.proxyAddress.c_str();
    }

    HINTERNET session = WinHttpOpen(cfg.userAgent.c_str(), access, proxy,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    // Enable TLS 1.1 and TLS 1.2 for Windows 7 WinHTTP compatibility
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    WinHttpSetTimeouts(session, cfg.connectTimeoutMs, cfg.connectTimeoutMs,
                       cfg.readTimeoutMs, cfg.readTimeoutMs);

    session_ = (void*)session;
    return true;
}

void HttpClient::Close() {
    std::lock_guard<std::mutex> lock(handleMutex_);
    request_ = nullptr;
    if (connect_) {
        WinHttpCloseHandle((HINTERNET)connect_);
        connect_ = nullptr;
    }
    if (session_) {
        WinHttpCloseHandle((HINTERNET)session_);
        session_ = nullptr;
    }
}

void HttpClient::Abort() {
    abort_.store(true);
    std::lock_guard<std::mutex> lock(handleMutex_);
    if (request_) {
        // Closing the request from another thread makes the blocking WinHTTP
        // call return immediately with ERROR_WINHTTP_OPERATION_CANCELLED.
        WinHttpCloseHandle((HINTERNET)request_);
        request_ = nullptr;
    }
}

HttpResponse HttpClient::Request(const std::wstring& url,
                                 const RequestOptions& opts,
                                 const BodySink& sink) {
    HttpResponse response;
    response.contentLength = UINT64_MAX;
    response.finalUrl = url;

    UrlParts parts = ParseUrl(url);
    if (!parts.valid) {
        response.error = L"Invalid or unsupported URL";
        return response;
    }
    if (!session_) {
        response.error = L"HTTP session is not initialized";
        return response;
    }
    if (abort_.load()) {
        response.error = L"Cancelled";
        return response;
    }

    const bool secure = parts.scheme == L"https";
    const INTERNET_PORT port =
        parts.port.empty() ? (secure ? INTERNET_DEFAULT_HTTPS_PORT
                                     : INTERNET_DEFAULT_HTTP_PORT)
                           : (INTERNET_PORT)_wtoi(parts.port.c_str());

    HINTERNET connect = WinHttpConnect((HINTERNET)session_, parts.host.c_str(), port, 0);
    if (!connect) {
        response.error = L"Connection failed: " + GetErrorMessage(GetLastError());
        return response;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connect, opts.method.c_str(), parts.RequestTarget().c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        response.error = L"Request creation failed: " + GetErrorMessage(GetLastError());
        WinHttpCloseHandle(connect);
        return response;
    }

    // Follow redirects so that shortened or moved links resolve, and keep TLS
    // certificate validation at the WinHTTP default (strict).
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                     sizeof(redirectPolicy));

    std::wstring headers;
    if (!opts.extraHeaders.empty()) headers += opts.extraHeaders;
    if (opts.useRange) {
        wchar_t range[128];
        if (opts.hasRangeEnd && opts.rangeEnd >= opts.rangeStart) {
            swprintf_s(range, L"Range: bytes=%llu-%llu\r\n",
                       (unsigned long long)opts.rangeStart,
                       (unsigned long long)opts.rangeEnd);
        } else {
            swprintf_s(range, L"Range: bytes=%llu-\r\n",
                       (unsigned long long)opts.rangeStart);
        }
        headers += range;
    }
    headers += L"Accept-Encoding: identity\r\n";

    {
        std::lock_guard<std::mutex> lock(handleMutex_);
        request_ = request;
    }

    auto releaseHandles = [&]() {
        {
            std::lock_guard<std::mutex> lock(handleMutex_);
            if (request_) {
                WinHttpCloseHandle(request_);
                request_ = nullptr;
            }
        }
        if (connect) WinHttpCloseHandle(connect);
        connect = nullptr;
    };

    BOOL sent = WinHttpSendRequest(
        request,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent) {
        const DWORD error = GetLastError();
        response.error = abort_.load() ? L"Cancelled"
                                       : L"Send failed: " + GetErrorMessage(error);
        releaseHandles();
        return response;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        const DWORD error = GetLastError();
        response.error = (abort_.load() || IsCancellation(error))
                             ? L"Cancelled"
                             : L"Response failed: " + GetErrorMessage(error);
        releaseHandles();
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    response.statusCode = (int)status;

    // The final URL after redirects is what the user should see in the list.
    DWORD urlSize = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &urlSize);
    if (urlSize > sizeof(wchar_t)) {
        std::wstring finalUrl(urlSize / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, finalUrl.data(),
                               &urlSize)) {
            finalUrl.resize(wcslen(finalUrl.c_str()));
            if (!finalUrl.empty()) response.finalUrl = finalUrl;
        }
    }

    DWORD rawSize = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &rawSize,
                        WINHTTP_NO_HEADER_INDEX);
    std::wstring raw;
    if (rawSize > 0) {
        raw.resize(rawSize / sizeof(wchar_t));
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &rawSize, WINHTTP_NO_HEADER_INDEX)) {
            raw.resize(wcslen(raw.c_str()));
        } else {
            raw.clear();
        }
    }

    auto headersList = SplitHeaders(raw);

    std::wstring value;
    if (HeaderValue(headersList, L"Content-Length", value)) {
        response.contentLength = _wcstoui64(value.c_str(), nullptr, 10);
    }
    if (HeaderValue(headersList, L"Accept-Ranges", value)) {
        response.acceptRanges = Lower(value) == L"bytes";
    }
    if (HeaderValue(headersList, L"Content-Range", value)) {
        // Content-Range: bytes start-end/total
        size_t slash = value.find(L'/');
        if (slash != std::wstring::npos) {
            std::wstring total = Trim(value.substr(slash + 1));
            if (total != L"*") {
                response.contentLength = _wcstoui64(total.c_str(), nullptr, 10);
            }
        }
        response.acceptRanges = true;
    }
    if (HeaderValue(headersList, L"Transfer-Encoding", value)) {
        response.chunked = Lower(value).find(L"chunked") != std::wstring::npos;
    }
    if (HeaderValue(headersList, L"Content-Type", value)) {
        response.contentType = value;
    }
    if (HeaderValue(headersList, L"Content-Disposition", value)) {
        response.contentDisposition = value;
    }
    if (HeaderValue(headersList, L"ETag", value)) {
        response.etag = value;
    }
    if (HeaderValue(headersList, L"Last-Modified", value)) {
        response.lastModified = value;
    }

    response.ok = status >= 200 && status < 300;

    // Drain the body, honouring the abort flag and the optional sink.
    const DWORD kChunk = 64 * 1024;
    std::vector<uint8_t> buffer(kChunk);
    for (;;) {
        if (abort_.load()) {
            response.ok = false;
            response.error = L"Cancelled";
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            const DWORD error = GetLastError();
            response.ok = false;
            response.error = (abort_.load() || IsCancellation(error))
                                 ? L"Cancelled"
                                 : L"Read failed: " + GetErrorMessage(error);
            break;
        }
        if (available == 0) break;

        DWORD toRead = std::min<DWORD>(available, kChunk);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), toRead, &read)) {
            const DWORD error = GetLastError();
            response.ok = false;
            response.error = (abort_.load() || IsCancellation(error))
                                 ? L"Cancelled"
                                 : L"Read failed: " + GetErrorMessage(error);
            break;
        }
        if (read == 0) break;
        if (sink && !sink((int)status, buffer.data(), read)) {
            response.ok = false;
            response.error = L"Cancelled";
            break;
        }
    }

    releaseHandles();
    return response;
}
