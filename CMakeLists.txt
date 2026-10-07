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
    src/loader/game/datafiles.cpp
    src/loader/game/fixes.cpp
    src/loader/game/models.cpp
    src/loader/game/pausemenu.cpp
    src/loader/game/scripts.cpp
    src/loader/ui/dx12_hook.cpp
    src/loader/ui/overlay.cpp
    src/loader/ui/menu.cpp
    src/loader/web/browser.cpp
    src/loader/web/textures.cpp
    src/loader/debug/hangdump.cpp
    src/loader/debug/scripttrace.cpp
    src/loader/debug/watch.cpp
    src/loader/debug/dred.cpp
    src/loader/game/dlcpacks.cpp
    src/loader/game/pools.cpp
    src/loader/convert/resource.cpp
    src/loader/convert/archive.cpp
    src/loader/convert/effects.cpp
    src/loader/convert/ypt.cpp
    src/loader/convert/ytd.cpp
    src/loader/convert/yft.cpp
    src/loader/convert/peds.cpp
    src/loader/convert/packs.cpp
    src/loader/convert/oiv.cpp
    src/loader/convert/rel.cpp
    src/loader/convert/replace.cpp
    src/loader/convert/xmlmerge.cpp
    src/loader/convert/ysc.cpp)
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

set(EXAMPLE_MODS hello_mod settings_demo trainer property)
foreach(example IN LISTS EXAMPLE_MODS)
    file(GLOB example_sources CONFIGURE_DEPENDS examples/${example}/*.cpp)
    add_library(${example} SHARED ${example_sources})
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
        # Web pages of the mod (examples/<mod>/web -> ModLoader\mods\<mod>\web).
        if(EXISTS "${CMAKE_SOURCE_DIR}/examples/${example}/web")
            add_custom_command(TARGET ${example} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/examples/${example}/web" "${GTAV_DIR}/ModLoader/mods/${example}/web")
        endif()
    endforeach()
endif()

# Research tool: learns the legacy -> Enhanced script global table (tools/learn_ysc). Not part of the default build.
add_executable(learn_ysc EXCLUDE_FROM_ALL tools/learn_ysc/learn_ysc.cpp src/loader/convert/resource.cpp)
target_compile_features(learn_ysc PRIVATE cxx_std_20)
target_compile_options(learn_ysc PRIVATE /utf-8 /O2)

# Research tool: finds code in game builds and relocates addresses between builds (tools/relocate). Not built by default.
FetchContent_Declare(zycore
    URL https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.0.tar.gz)
FetchContent_Declare(zydis
    URL https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz)
FetchContent_GetProperties(zycore)
if(NOT zycore_POPULATED)
    FetchContent_Populate(zycore)
endif()
set(ZYAN_ZYCORE_PATH ${zycore_SOURCE_DIR} CACHE PATH "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zydis)
add_executable(relocate EXCLUDE_FROM_ALL
    tools/relocate/main.cpp
    tools/relocate/image.cpp
    tools/relocate/analysis.cpp
    tools/relocate/match.cpp
    tools/relocate/signature.cpp)
target_link_libraries(relocate PRIVATE Zydis)
target_compile_features(relocate PRIVATE cxx_std_20)
target_compile_options(relocate PRIVATE /utf-8 /O2)

# In-game web browser (src/browser): CEF (Chromium Embedded Framework, BSD) renders the game's own web pages off-screen;
# ModLoader.dll loads ModLoader\browser\mlbrowser.dll the first time the browser is opened.
option(ML_BROWSER "Build the in-game web browser (downloads CEF, about 170 MB)" ON)
if(ML_BROWSER)
    set(CEF_VERSION "154.0.34+g14c5a08+chromium-154.0.8037.98")
    string(REPLACE "+" "%2B" CEF_VERSION_URL "${CEF_VERSION}")
    FetchContent_Declare(cef
        URL https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION_URL}_windows64_minimal.tar.bz2)
    FetchContent_GetProperties(cef)
    if(NOT cef_POPULATED)
        FetchContent_Populate(cef)
    endif()
    set(CEF_ROOT ${cef_SOURCE_DIR})
    list(APPEND CMAKE_MODULE_PATH ${CEF_ROOT}/cmake)
    find_package(CEF REQUIRED)
    add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
    ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

    # mlbrowser.dll: the browser process side, driven by ModLoader.dll through src/browser/mlbrowser.h.
    add_library(mlbrowser SHARED src/browser/browser.cpp src/browser/app.cpp)
    SET_LIBRARY_TARGET_PROPERTIES(mlbrowser)
    add_dependencies(mlbrowser libcef_dll_wrapper)
    target_link_libraries(mlbrowser PRIVATE libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_compile_options(mlbrowser PRIVATE /utf-8)

    # ml_browser_helper.exe: Chromium's renderer / GPU / utility processes.
    add_executable(ml_browser_helper WIN32 src/browser/helper.cpp src/browser/app.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(ml_browser_helper)
    add_dependencies(ml_browser_helper libcef_dll_wrapper)
    target_link_libraries(ml_browser_helper PRIVATE libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_compile_options(ml_browser_helper PRIVATE /utf-8)

    if(GTAV_DIR)
        set(ML_BROWSER_DIR "${GTAV_DIR}/ModLoader/browser")
        add_custom_command(TARGET mlbrowser POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ML_BROWSER_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mlbrowser> "${ML_BROWSER_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_BINARY_DIR_RELEASE}" "${ML_BROWSER_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_RESOURCE_DIR}" "${ML_BROWSER_DIR}")
        add_custom_command(TARGET ml_browser_helper POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ML_BROWSER_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ml_browser_helper> "${ML_BROWSER_DIR}/")
        add_custom_command(TARGET loader POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/web" "${GTAV_DIR}/ModLoader/web")
    endif()
endif()

if(ML_BROWSER)
    # Test host for mlbrowser.dll outside the game (tools/browser_test). Not built by default.
    add_executable(browser_test EXCLUDE_FROM_ALL tools/browser_test/browser_test.cpp)
    target_compile_options(browser_test PRIVATE /utf-8)
endif()
