add_library(loader_common STATIC
    app/log.h
    app/log_format.cpp
    ui/window_settings.cpp
    ui/window_settings.h)
target_include_directories(loader_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(loader_common PRIVATE nlohmann_json::nlohmann_json)

add_library(loader_config STATIC
    config/config.cpp
    config/config.h
    config/policy.cpp
    config/validation.cpp)
target_link_libraries(loader_config PUBLIC loader_common bcrypt ws2_32 WIL::WIL PRIVATE CLI11::CLI11)

add_library(loader_login STATIC
    login/login.cpp
    login/login.h
    login/wire.cpp
    login/wire.h)
target_link_libraries(loader_login PUBLIC loader_common bcrypt ws2_32 WIL::WIL)

add_executable(tmloader WIN32
    app/abi.h
    app/com.h
    app/hooks.h
    app/loader.cpp
    app/log.cpp
    app/main.cpp
    app/runtime.cpp
    app/runtime.h
    graphics/d3d8_abi.h
    graphics/graphics.cpp
    graphics/render_scale.cpp
    graphics/render_scale.h
    graphics/screenshot.cpp
    graphics/screenshot.h
    input/input.cpp
    input/input_bridge.cpp
    input/input_bridge.h
    platform/audio.cpp
    platform/desktop.cpp
    platform/network.cpp
    platform/patch.cpp
    platform/patch.h
    platform/system.cpp
    ui/account.cpp
    ui/account.h
    ui/imgui_window.cpp
    ui/imgui_window.h
    ui/settings_ui.cpp
    ui/window.cpp
    ui/window.h)
target_link_libraries(tmloader PRIVATE
    loader_config loader_login loader_imgui nlohmann_json::nlohmann_json
    ole32 windowscodecs advapi32 user32 gdi32 shell32 ws2_32 dxguid dbghelp)
# Keep tmloader.exe at <build>/<config>/ rather than under src/.
set_target_properties(tmloader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
# The game DLL executes generated code and expects fixed addresses. CMake embeds
# the default asInvoker manifest for every generator.
target_link_options(tmloader PRIVATE /ENTRY:mainCRTStartup /NXCOMPAT:NO /DYNAMICBASE:NO)
# --check reads the retail file list from beside the executable, as in the release archive.
add_custom_command(TARGET tmloader POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/retail-files.txt" "$<TARGET_FILE_DIR:tmloader>")
set(loader_targets loader_common loader_config loader_login tmloader)

foreach(target IN LISTS loader_targets)
    get_target_property(sources ${target} SOURCES)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${sources})
endforeach()
