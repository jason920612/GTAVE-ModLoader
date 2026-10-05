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
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz)
FetchContent_MakeAvailable(minhook json imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

# version.dll: tiny proxy that loads ModLoader\ModLoader.dll (see src/stub/main.cpp).
add_library(stub SHARED
    src/stub/main.cpp
    src/stub/proxy.cpp
    src/stub/proxy_version.asm
    src/stub/version.def)
set_target_properties(stub PROPERTIES OUTPUT_NAME version)
target_compile_definitions(stub PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stub PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)

# ModLoader.dll: the loader itself.
add_library(loader SHARED
    src/loader/dllmain.cpp
    src/loader/log.cpp
    src/loader/paths.cpp
    src/loader/config.cpp
    src/loader/pattern.cpp
    src/loader/bootstrap.cpp
    src/loader/core.cpp
    src/loader/crossmap_update.cpp
    src/loader/mods.cpp
    src/loader/game/pointers.cpp
    src/loader/game/natives.cpp
    src/loader/game/script.cpp
    src/loader/game/text.cpp
    src/loader/game/text_override.cpp
    src/loader/game/pausemenu.cpp
    src/loader/ui/dx12_hook.cpp
    src/loader/ui/overlay.cpp
    src/loader/ui/menu.cpp
    src/loader/debug/watch.cpp
    src/loader/game/dlcpacks.cpp)
set_target_properties(loader PROPERTIES OUTPUT_NAME ModLoader)
target_compile_definitions(loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive- /Zi>)
target_include_directories(loader PRIVATE sdk/include)
target_link_libraries(loader PRIVATE minhook nlohmann_json::nlohmann_json imgui winhttp d3d12 dxgi d3dcompiler delayimp)
# Only needed after startup (crossmap download, overlay); keep them out of DllMain.
target_link_options(loader PRIVATE /DELAYLOAD:winhttp.dll /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:d3dcompiler_47.dll /DEBUG /OPT:REF /OPT:ICF)

# SDK: header-only, used by mods.
add_library(modloader_sdk INTERFACE)
target_include_directories(modloader_sdk INTERFACE sdk/include)
target_compile_features(modloader_sdk INTERFACE cxx_std_20)

set(EXAMPLE_MODS hello_mod settings_demo)
foreach(example IN LISTS EXAMPLE_MODS)
    add_library(${example} SHARED examples/${example}/main.cpp)
    target_link_libraries(${example} PRIVATE modloader_sdk)
    target_compile_options(${example} PRIVATE /W4 /utf-8 /bigobj)
endforeach()

# Optional: GTAV_DIR=<game dir> installs the loader there after each build.
if(GTAV_DIR)
    add_custom_command(TARGET stub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:stub> "${GTAV_DIR}/version.dll")
    add_custom_command(TARGET loader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GTAV_DIR}/ModLoader"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:loader> "${GTAV_DIR}/ModLoader/ModLoader.dll")
    foreach(example IN LISTS EXAMPLE_MODS)
        add_custom_command(TARGET ${example} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${GTAV_DIR}/ModLoader/mods"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${example}> "${GTAV_DIR}/ModLoader/mods/")
    endforeach()
endif()
