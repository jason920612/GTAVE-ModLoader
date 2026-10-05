cmake_minimum_required(VERSION 3.24)
project(GTAVEModLoader LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so players never need a VC++ redistributable for the loader.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(minhook json)

add_library(loader SHARED
    src/loader/dllmain.cpp
    src/loader/proxy.cpp
    src/loader/proxy_version.asm
    src/loader/version.def
    src/loader/log.cpp
    src/loader/paths.cpp
    src/loader/config.cpp
    src/loader/pattern.cpp
    src/loader/bootstrap.cpp)
set_target_properties(loader PROPERTIES OUTPUT_NAME version)
target_compile_definitions(loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
target_link_libraries(loader PRIVATE minhook nlohmann_json::nlohmann_json)

# Optional: GTAV_DIR=<game dir> copies version.dll there after each build.
if(GTAV_DIR)
    add_custom_command(TARGET loader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:loader> "${GTAV_DIR}/version.dll")
endif()
