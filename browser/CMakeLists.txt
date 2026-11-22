# The browser client: an x86-32 interpreter with host stand-ins, built with Emscripten 5.0.7 for Node and the
# browser, and natively for fast tests.
#   Wasm:   emcmake cmake -G Ninja -S browser -B build/browser-wasm && cmake --build build/browser-wasm
#   Native: cmake -G Ninja -S browser -B build/browser && cmake --build build/browser && ctest --test-dir build/browser
cmake_minimum_required(VERSION 3.29)
project(tmweb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra)
endif()
if(EMSCRIPTEN)
    # Guest faults unwind as C++ exceptions
    add_compile_options(-fwasm-exceptions)
    add_link_options(-fwasm-exceptions)
endif()

add_library(tmweb_core STATIC
    core/boundary.cpp
    core/cpu.cpp
    core/machine.cpp
    core/memory.cpp
    core/threads.cpp
    core/x87.cpp
    f80/f80.cpp
)
target_include_directories(tmweb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(tmweb app/main.cpp)
target_link_libraries(tmweb PRIVATE tmweb_core)
if(EMSCRIPTEN)
    # Node reads the user's game files straight from disk; the guest arena needs 384 MiB of linear memory
    target_link_options(tmweb PRIVATE -sNODERAWFS=1 -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=448MB -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1)
endif()

include(CTest)
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    include(FetchContent)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
        URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(Catch2)
    add_executable(tmweb_tests
        tests/cpu_test.cpp
        tests/memory_test.cpp
    )
    target_link_libraries(tmweb_tests PRIVATE tmweb_core Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(tmweb_tests)
endif()
