cmake_minimum_required(VERSION 3.29)
project(tmloader VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "tmloader requires MSVC x86. Configure with -A Win32.")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# MSVC exposes C++23 through its preview mode. CMake still maps C++23 to
# c++latest, which also opts into later standards; request the bounded mode.
if(MSVC_VERSION LESS 1943)
    message(FATAL_ERROR "tmloader requires MSVC 19.43+ for /std:c++23preview.")
endif()
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "/std:c++23preview")
set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "/std:c++23preview")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN DIRECTINPUT_VERSION=0x0800
    _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
add_compile_options(/W4 /EHsc /utf-8)

find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(format
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_clang_format.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Format first-party C++ with clang-format 22")
    add_custom_target(format-check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_clang_format.py" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Check first-party C++ formatting without editing files")
endif()

# Dear ImGui 1.92.5, pinned to an immutable upstream commit and archive hash.
include(FetchContent)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/6d910d5487d11ca567b61c7824b0c78c569d62f0.tar.gz
    URL_HASH SHA256=b715a3226320aaebaea79dc6af6405ae37cdcb479817fddaef12a17d8bd15a3a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(loader_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp)
target_include_directories(loader_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(loader_imgui PUBLIC d3d9 dwmapi)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

set(WIL_BUILD_TESTS OFF CACHE BOOL "Build WIL's own test suite")
set(WIL_BUILD_PACKAGING OFF CACHE BOOL "Build WIL's distribution packages")
FetchContent_Declare(wil
    URL https://github.com/microsoft/wil/archive/cbf677fb0a942557d08fd129f4c106a76247b2ec.tar.gz
    URL_HASH SHA256=d7b3c903b501f6ea73a733ee4cd4215119dc10eee969bda2aa134732b8e8fe6f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(wil)

add_library(loader_config STATIC src/config.cpp src/endpoints.cpp src/validation.cpp)
target_include_directories(loader_config PUBLIC src)
target_link_libraries(loader_config PUBLIC bcrypt ws2_32 WIL::WIL)
add_library(loader_textures STATIC src/texture_pack.cpp src/texture_replacements.cpp src/texture_hooks.cpp)
target_include_directories(loader_textures PUBLIC src)
target_link_libraries(loader_textures PUBLIC loader_config PRIVATE nlohmann_json::nlohmann_json)
add_executable(tmloader WIN32 src/main.cpp src/loader.cpp src/runtime.cpp src/log.cpp
    src/patch.cpp src/files.cpp src/graphics.cpp src/input.cpp src/input_bridge.cpp src/network.cpp
    src/window.cpp src/settings_ui.cpp src/audio.cpp src/desktop.cpp src/render_scale.cpp src/screenshot.cpp)
target_link_libraries(tmloader PRIVATE loader_config loader_imgui loader_textures ole32 windowscodecs advapi32 user32 gdi32 ws2_32)
# The protected original DLL executes legacy generated code; preserve the
# confirmed process-local compatibility settings, not system DEP/UAC changes.
target_link_options(tmloader PRIVATE /ENTRY:mainCRTStartup /NXCOMPAT:NO /DYNAMICBASE:NO
    /MANIFEST:EMBED "/MANIFESTUAC:level='asInvoker' uiAccess='false'")

include(CTest)
if(BUILD_TESTING)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
        URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(Catch2)
    # The screenshot child receives a Unicode fixture path through wmain.
    target_compile_definitions(Catch2 PRIVATE UNICODE)
    target_compile_definitions(Catch2WithMain PRIVATE UNICODE)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    function(loader_test target)
        target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
        catch_discover_tests(${target} ADD_TAGS_AS_LABELS)
    endfunction()
    add_executable(input_bridge_tests tests/input_bridge_tests.cpp)
    target_include_directories(input_bridge_tests PRIVATE src)
    target_link_libraries(input_bridge_tests PRIVATE user32 dxguid)
    loader_test(input_bridge_tests)
    add_executable(config_tests tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE loader_config)
    loader_test(config_tests)
    add_executable(abi_tests tests/abi_tests.cpp)
    target_include_directories(abi_tests PRIVATE src)
    loader_test(abi_tests)
    add_test(NAME cli_help COMMAND tmloader --help)
    add_executable(window_settings_tests tests/window_settings_tests.cpp)
    target_include_directories(window_settings_tests PRIVATE src)
    target_link_libraries(window_settings_tests PRIVATE user32)
    loader_test(window_settings_tests)
    add_executable(window_messages_tests tests/window_messages_tests.cpp)
    target_link_libraries(window_messages_tests PRIVATE loader_config user32 gdi32)
    loader_test(window_messages_tests)
    add_executable(virtual_registry_tests tests/virtual_registry_tests.cpp)
    target_link_libraries(virtual_registry_tests PRIVATE loader_config advapi32)
    loader_test(virtual_registry_tests)
    add_executable(graphics_tests tests/graphics_tests.cpp src/render_scale.cpp)
    target_link_libraries(graphics_tests PRIVATE loader_config loader_textures user32 ole32)
    loader_test(graphics_tests)
    add_executable(screenshot_tests tests/screenshot_tests.cpp)
    target_link_libraries(screenshot_tests PRIVATE loader_config windowscodecs ole32 advapi32)
    target_link_libraries(screenshot_tests PRIVATE Catch2::Catch2)
    target_compile_definitions(screenshot_tests PRIVATE UNICODE)
    catch_discover_tests(screenshot_tests TEST_SPEC "~[.screenshot-child]" ADD_TAGS_AS_LABELS)
    add_executable(settings_ui_tests tests/settings_ui_tests.cpp)
    target_include_directories(settings_ui_tests PRIVATE src)
    target_link_libraries(settings_ui_tests PRIVATE loader_imgui user32 gdi32)
    loader_test(settings_ui_tests)
    add_executable(desktop_tests tests/desktop_tests.cpp)
    target_include_directories(desktop_tests PRIVATE src)
    target_link_libraries(desktop_tests PRIVATE user32)
    loader_test(desktop_tests)
    add_executable(audio_tests tests/audio_tests.cpp src/audio.cpp src/log.cpp src/runtime.cpp)
    target_link_libraries(audio_tests PRIVATE loader_config user32 dxguid advapi32)
    loader_test(audio_tests)
    add_executable(render_scale_tests tests/render_scale_tests.cpp src/render_scale.cpp src/log.cpp src/runtime.cpp)
    target_link_libraries(render_scale_tests PRIVATE loader_config user32 advapi32)
    loader_test(render_scale_tests)
    add_executable(texture_hooks_tests tests/texture_hooks_tests.cpp src/texture_hooks.cpp)
    target_include_directories(texture_hooks_tests PRIVATE src)
    loader_test(texture_hooks_tests)
    add_executable(texture_pack_tests tests/texture_pack_tests.cpp)
    target_link_libraries(texture_pack_tests PRIVATE loader_textures nlohmann_json::nlohmann_json)
    loader_test(texture_pack_tests)
    add_executable(texture_replacements_tests tests/texture_replacements_tests.cpp src/runtime.cpp src/log.cpp src/render_scale.cpp)
    target_link_libraries(texture_replacements_tests PRIVATE loader_textures user32 advapi32 ole32)
    loader_test(texture_replacements_tests)
endif()
