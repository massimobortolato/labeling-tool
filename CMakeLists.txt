cmake_minimum_required(VERSION 3.10.0)
project(labeling-tool VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    easy-webview
    GIT_REPOSITORY https://github.com/massimobortolato/easy-webview.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(easy-webview)

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(labeling-tool WIN32 main.cpp)
target_compile_definitions(labeling-tool PRIVATE UNICODE _UNICODE)
target_link_libraries(labeling-tool PRIVATE easy_webview::easy_webview nlohmann_json::nlohmann_json ole32 shell32 uuid)

# Embed the static web assets into the executable (see embedded/example_static.h).
include(cmake/embed_dir.cmake)
easy_webview_embed_dir(labeling-tool example_static "${CMAKE_CURRENT_SOURCE_DIR}/static")

