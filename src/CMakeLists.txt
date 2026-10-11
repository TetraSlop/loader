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
    ui/imgui_window.cpp
    ui/imgui_window.h
    ui/settings_ui.cpp
    ui/window.cpp
    ui/window.h)
target_link_libraries(tmloader PRIVATE
    loader_config loader_imgui nlohmann_json::nlohmann_json
    ole32 windowscodecs advapi32 user32 gdi32 shell32 ws2_32 dxguid)
# Keep tmloader.exe at <build>/<config>/ rather than under src/.
set_target_properties(tmloader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
# The game DLL executes generated code and expects fixed addresses. CMake embeds
# the default asInvoker manifest for every generator.
target_link_options(tmloader PRIVATE /ENTRY:mainCRTStartup /NXCOMPAT:NO /DYNAMICBASE:NO)
set(loader_targets loader_common loader_config tmloader)

# Optional features. Each lives in its own folder and is switched in the
# top-level CMakeLists.txt; docs/architecture.md lists what refers to it.
if(TMLOADER_HD_TEXTURES)
    add_library(loader_textures STATIC
        textures/texture_draw.cpp
        textures/texture_pack.cpp
        textures/texture_pack.h
        textures/texture_replacement.h
        textures/texture_replacements.cpp
        textures/texture_replacements.h)
    target_link_libraries(loader_textures PUBLIC loader_config PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(tmloader PRIVATE loader_textures)
    list(APPEND loader_targets loader_textures)
endif()

if(TMLOADER_DISCORD_PRESENCE)
    add_library(loader_presence STATIC
        presence/discord_presence.cpp
        presence/discord_presence.h
        presence/game_phase.cpp
        presence/game_phase.h
        presence/presence.cpp
        presence/presence.h)
    target_link_libraries(loader_presence PUBLIC loader_config PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(tmloader PRIVATE loader_presence)
    list(APPEND loader_targets loader_presence)
endif()

if(TMLOADER_HOSTED)
    add_library(loader_hosted STATIC
        hosted/auth.cpp
        hosted/auth.h
        hosted/http.cpp
        hosted/http.h
        hosted/session.cpp
        hosted/session.h
        hosted/tunnel.cpp
        hosted/tunnel.h)
    target_link_libraries(loader_hosted PUBLIC loader_config winhttp crypt32 ws2_32 PRIVATE nlohmann_json::nlohmann_json)
    target_sources(tmloader PRIVATE
        ui/image.cpp
        ui/image.h
        ui/launcher.cpp
        ui/launcher.h)
    target_link_libraries(tmloader PRIVATE loader_hosted)
    list(APPEND loader_targets loader_hosted)
endif()

foreach(target IN LISTS loader_targets)
    get_target_property(sources ${target} SOURCES)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${sources})
endforeach()
