cmake_minimum_required(VERSION 3.24)
project(tmloader VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "tmloader requires MSVC x86. Configure with -A Win32.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN DIRECTINPUT_VERSION=0x0800
    _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
add_compile_options(/W4 /EHsc /utf-8)

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

add_library(loader_config STATIC src/config.cpp src/endpoints.cpp src/validation.cpp)
target_include_directories(loader_config PUBLIC src)
target_link_libraries(loader_config PUBLIC bcrypt ws2_32)
add_executable(tmloader WIN32 src/main.cpp src/loader.cpp src/runtime.cpp src/log.cpp
    src/patch.cpp src/files.cpp src/graphics.cpp src/input.cpp src/network.cpp
    src/window.cpp src/settings_ui.cpp src/audio.cpp src/desktop.cpp src/render_scale.cpp)
target_link_libraries(tmloader PRIVATE loader_config loader_imgui ole32 advapi32 user32 gdi32 ws2_32)
# The protected original DLL executes legacy generated code; preserve the
# confirmed process-local compatibility settings, not system DEP/UAC changes.
target_link_options(tmloader PRIVATE /ENTRY:mainCRTStartup /NXCOMPAT:NO /DYNAMICBASE:NO
    /MANIFEST:EMBED "/MANIFESTUAC:level='asInvoker' uiAccess='false'")

include(CTest)
if(BUILD_TESTING)
    add_executable(input_bridge_tests tests/input_bridge_tests.cpp)
    target_include_directories(input_bridge_tests PRIVATE src)
    target_link_libraries(input_bridge_tests PRIVATE user32 dxguid)
    add_test(NAME input_bridge COMMAND input_bridge_tests)
    add_executable(config_tests tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE loader_config)
    add_test(NAME config COMMAND config_tests)
    add_executable(abi_tests tests/abi_tests.cpp)
    target_include_directories(abi_tests PRIVATE src)
    add_test(NAME abi_dispatch COMMAND abi_tests)
    add_test(NAME cli_help COMMAND tmloader --help)
    add_executable(window_settings_tests tests/window_settings_tests.cpp)
    target_include_directories(window_settings_tests PRIVATE src)
    target_link_libraries(window_settings_tests PRIVATE user32)
    add_test(NAME window_settings COMMAND window_settings_tests)
    add_executable(window_messages_tests tests/window_messages_tests.cpp)
    target_link_libraries(window_messages_tests PRIVATE loader_config user32 gdi32)
    add_test(NAME window_messages COMMAND window_messages_tests)
    add_executable(virtual_registry_tests tests/virtual_registry_tests.cpp)
    target_link_libraries(virtual_registry_tests PRIVATE loader_config advapi32)
    add_test(NAME virtual_registry COMMAND virtual_registry_tests)
    add_executable(graphics_tests tests/graphics_tests.cpp src/render_scale.cpp)
    target_link_libraries(graphics_tests PRIVATE loader_config user32 ole32)
    add_test(NAME graphics_presentation COMMAND graphics_tests)
    add_executable(settings_ui_tests tests/settings_ui_tests.cpp)
    target_include_directories(settings_ui_tests PRIVATE src)
    target_link_libraries(settings_ui_tests PRIVATE loader_imgui user32 gdi32)
    add_test(NAME settings_ui_layout COMMAND settings_ui_tests)
    add_executable(desktop_tests tests/desktop_tests.cpp)
    target_include_directories(desktop_tests PRIVATE src)
    target_link_libraries(desktop_tests PRIVATE user32)
    add_test(NAME desktop_input_policy COMMAND desktop_tests)
    add_executable(audio_tests tests/audio_tests.cpp src/audio.cpp src/log.cpp src/runtime.cpp)
    target_link_libraries(audio_tests PRIVATE loader_config user32 dxguid advapi32)
    add_test(NAME audio_buffers COMMAND audio_tests)
    set_tests_properties(audio_buffers PROPERTIES SKIP_RETURN_CODE 77 LABELS "audio-device")
    add_executable(render_scale_tests tests/render_scale_tests.cpp src/render_scale.cpp src/log.cpp src/runtime.cpp)
    target_link_libraries(render_scale_tests PRIVATE loader_config user32 advapi32)
    add_test(NAME render_scale_pixels COMMAND render_scale_tests)
    set_tests_properties(render_scale_pixels PROPERTIES SKIP_RETURN_CODE 77 LABELS "graphics-device")
endif()
