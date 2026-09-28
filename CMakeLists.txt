cmake_minimum_required(VERSION 3.29)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" TMLOADER_VERSION LIMIT_COUNT 1)
project(tmloader VERSION ${TMLOADER_VERSION} LANGUAGES CXX)
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
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX DIRECTINPUT_VERSION=0x0800
    _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS
    TMLOADER_VERSION="${PROJECT_VERSION}")
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

include(CTest)
include(cmake/Dependencies.cmake)

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
