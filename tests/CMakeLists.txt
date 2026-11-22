function(loader_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE loader_common Catch2::Catch2WithMain)
    catch_discover_tests(${target} ADD_TAGS_AS_LABELS)
endfunction()

set(loader_source_dir "${PROJECT_SOURCE_DIR}/src")

loader_test(abi_tests app/abi_tests.cpp)

loader_test(runtime_tests
    app/runtime_tests.cpp
    ${loader_source_dir}/app/log.cpp
    ${loader_source_dir}/app/runtime.cpp)
target_link_libraries(runtime_tests PRIVATE loader_config advapi32 dbghelp)
add_test(NAME cli_help COMMAND tmloader --help)

loader_test(config_tests config/config_tests.cpp)
target_link_libraries(config_tests PRIVATE loader_config)

loader_test(render_scale_tests
    graphics/render_scale_tests.cpp
    ${loader_source_dir}/graphics/render_scale.cpp
    ${loader_source_dir}/app/log.cpp
    ${loader_source_dir}/app/runtime.cpp)
target_link_libraries(render_scale_tests PRIVATE nlohmann_json::nlohmann_json loader_config user32 advapi32 dbghelp)

# This test supplies wmain for its separate low-integrity capture process.
add_executable(screenshot_tests graphics/screenshot_tests.cpp)
target_link_libraries(screenshot_tests PRIVATE
    loader_config windowscodecs ole32 advapi32 Catch2::Catch2)
target_compile_definitions(screenshot_tests PRIVATE UNICODE)
catch_discover_tests(screenshot_tests TEST_SPEC "~[.screenshot-child]" ADD_TAGS_AS_LABELS)

loader_test(input_bridge_tests input/input_bridge_tests.cpp)
target_link_libraries(input_bridge_tests PRIVATE nlohmann_json::nlohmann_json user32 dxguid)

loader_test(audio_tests
    platform/audio_tests.cpp
    ${loader_source_dir}/app/log.cpp
    ${loader_source_dir}/app/runtime.cpp)
target_link_libraries(audio_tests PRIVATE loader_config user32 dxguid advapi32 dbghelp)

loader_test(desktop_tests platform/desktop_tests.cpp)
target_link_libraries(desktop_tests PRIVATE user32)

loader_test(virtual_registry_tests platform/virtual_registry_tests.cpp)
target_link_libraries(virtual_registry_tests PRIVATE loader_config advapi32)

loader_test(settings_ui_tests ui/settings_ui_tests.cpp ${loader_source_dir}/ui/imgui_window.cpp)
target_link_libraries(settings_ui_tests PRIVATE nlohmann_json::nlohmann_json WIL::WIL loader_imgui user32 gdi32 shell32 ole32)

loader_test(window_messages_tests ui/window_messages_tests.cpp)
target_link_libraries(window_messages_tests PRIVATE nlohmann_json::nlohmann_json loader_config user32 gdi32)

loader_test(window_settings_tests ui/window_settings_tests.cpp)
target_link_libraries(window_settings_tests PRIVATE nlohmann_json::nlohmann_json user32)

loader_test(patch_tests
    platform/patch_tests.cpp
    ${loader_source_dir}/platform/patch.cpp)

loader_test(graphics_tests
    graphics/graphics_tests.cpp
    ${loader_source_dir}/graphics/render_scale.cpp)
target_link_libraries(graphics_tests PRIVATE nlohmann_json::nlohmann_json loader_config user32 ole32)

if(TMLOADER_HOSTED)
    loader_test(hosted_tests hosted/auth_tests.cpp hosted/gateway_tests.cpp)
    target_link_libraries(hosted_tests PRIVATE loader_hosted nlohmann_json::nlohmann_json httplib::httplib)
endif()
