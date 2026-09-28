add_library(loader_config STATIC
    config/config.cpp
    config/config.h
    config/endpoints.cpp
    config/validation.cpp)
target_include_directories(loader_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(loader_config PUBLIC bcrypt ws2_32 WIL::WIL)

add_library(loader_textures STATIC
    textures/texture_hooks.cpp
    textures/texture_hooks.h
    textures/texture_pack.cpp
    textures/texture_pack.h
    textures/texture_replacement.h
    textures/texture_replacements.cpp
    textures/texture_replacements.h)
target_include_directories(loader_textures PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(loader_textures PUBLIC loader_config PRIVATE nlohmann_json::nlohmann_json)

add_executable(tmloader WIN32
    app/abi.h
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
    graphics/screenshot_capture.h
    input/input.cpp
    input/input_bridge.cpp
    input/input_bridge.h
    platform/audio.cpp
    platform/desktop.cpp
    platform/files.cpp
    platform/network.cpp
    platform/patch.cpp
    ui/settings_ui.cpp
    ui/window.cpp
    ui/window.h
    ui/window_settings.h)
target_link_libraries(tmloader PRIVATE
    loader_config loader_imgui loader_textures
    ole32 windowscodecs advapi32 user32 gdi32 ws2_32)
target_compile_definitions(tmloader PRIVATE TMLOADER_VERSION="${PROJECT_VERSION}")
# Keep the executable where existing launchers expect it after add_subdirectory.
set_target_properties(tmloader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
# The protected original DLL executes legacy generated code; preserve the
# confirmed process-local compatibility settings, not system DEP/UAC changes.
target_link_options(tmloader PRIVATE /ENTRY:mainCRTStartup /NXCOMPAT:NO /DYNAMICBASE:NO
    /MANIFEST:EMBED "/MANIFESTUAC:level='asInvoker' uiAccess='false'")

foreach(target IN ITEMS loader_config loader_textures tmloader)
    get_target_property(sources ${target} SOURCES)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${sources})
endforeach()
