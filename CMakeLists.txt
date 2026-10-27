cmake_minimum_required(VERSION 3.20)
project(FLEXDownloadManager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Core Gui Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Core Gui Network)

add_executable(FLEXDownloadManager WIN32
    src/app.rc
    src/resources.qrc
    src/main.cpp
    src/common.cpp
    src/config.cpp
    src/download.cpp
    src/http.cpp
    src/bridge.cpp
    src/localization.cpp
    src/ui_main.cpp
    src/ui_dialogs.cpp
    src/url.cpp
    src/ytdlp.cpp
)

target_include_directories(FLEXDownloadManager PRIVATE src)
target_link_libraries(FLEXDownloadManager PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Network
    winhttp
    shlwapi
    shell32
    ole32
    user32
    gdi32
    advapi32
    dwmapi
)

if(MSVC)
    # Enforce Windows 7 compatibility (Subsystem 6.01)
    target_link_options(FLEXDownloadManager PRIVATE "/SUBSYSTEM:WINDOWS,6.01")
    target_compile_options(FLEXDownloadManager PRIVATE /utf-8 /EHsc /W4)
endif()
