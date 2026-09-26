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

add_library(loader_config STATIC src/config.cpp src/endpoints.cpp src/validation.cpp)
target_include_directories(loader_config PUBLIC src)
target_link_libraries(loader_config PUBLIC bcrypt ws2_32)
add_executable(tmloader WIN32 src/main.cpp src/loader.cpp src/runtime.cpp src/log.cpp
    src/patch.cpp src/files.cpp src/graphics.cpp src/input.cpp src/network.cpp)
target_link_libraries(tmloader PRIVATE loader_config ole32 advapi32 user32 ws2_32)
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
    add_test(NAME cli_help COMMAND tmloader --help)
endif()
