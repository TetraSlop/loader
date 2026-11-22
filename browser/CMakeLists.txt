# The browser client: an x86-32 interpreter with host stand-ins, built with Emscripten 5.0.7 for Node and the
# browser, and natively for fast tests.
#   Wasm:   emcmake cmake -G Ninja -S browser -B build/browser-wasm && cmake --build build/browser-wasm
#   Native: cmake -G Ninja -S browser -B build/browser && cmake --build build/browser && ctest --test-dir build/browser
cmake_minimum_required(VERSION 3.29)
project(tmweb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# tools/capi.cpp is a shared library over the static ones
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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

# Berkeley SoftFloat 3e, the x87 subset (third_party/softfloat/README.md)
file(GLOB softfloat_sources CONFIGURE_DEPENDS third_party/softfloat/source/*.c third_party/softfloat/source/8086/*.c)
add_library(softfloat STATIC ${softfloat_sources})
target_include_directories(softfloat PUBLIC third_party/softfloat third_party/softfloat/source/include
    third_party/softfloat/source/8086)
target_compile_options(softfloat PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>)

add_library(tmweb_core STATIC
    com/proxies.cpp
    core/boundary.cpp
    core/cpu.cpp
    core/machine.cpp
    core/memory.cpp
    core/threads.cpp
    core/x87.cpp
    f80/f80.cpp
    f80/f80_transcendental.cpp
)
target_include_directories(tmweb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT EMSCRIPTEN)
    # The interpreter alone behind a C interface, for the differential test against Unicorn (tools/difftest.py)
    add_library(tmweb_capi SHARED tools/capi.cpp)
    target_link_libraries(tmweb_capi PRIVATE tmweb_core)
endif()

# Win32 stand-ins and the boot sequence
add_library(tmweb_hle STATIC
    hle/advapi32.cpp
    hle/boot.cpp
    hle/files.cpp
    hle/image.cpp
    hle/kernel32.cpp
    hle/process.cpp
    hle/small.cpp
    hle/user32.cpp
)
target_link_libraries(tmweb_hle PUBLIC tmweb_core)

# The module's side of the page and the Node runner (host/host.h, web/)
add_library(tmweb_host OBJECT app/boot.cpp host/host.cpp)
target_link_libraries(tmweb_core PRIVATE softfloat)

# Kestrel's Direct3D 8 on WebGL2, DirectInput and DirectSound (compat/, see README.md). They are written against
# the Windows headers, which mingw-w64 supplies, so they build for Wasm only.
if(EMSCRIPTEN)
    include(FetchContent)
    FetchContent_Declare(mingw_w64
        URL https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/mingw-w64-v14.0.0.tar.bz2/download
        URL_HASH SHA256=6eaf921d9eb987d3820b364ea9775bc19b965ec81490b6fdd716526c28e1995c
        DOWNLOAD_NAME mingw-w64-v14.0.0.tar.bz2
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(mingw_w64)
    set(MINGW_HEADERS ${mingw_w64_SOURCE_DIR}/mingw-w64-headers)

    # mingw-w64's own build makes _mingw.h from this template, by default for the UCRT and Windows 10. Each Windows
    # header compat/ names is read through a wrapper that opens it between mingw_begin.h and mingw_end.h.
    set(WIN32_DIR ${CMAKE_CURRENT_BINARY_DIR}/win32)
    set(DEFAULT_MSVCRT_VERSION 0xE00)
    set(DEFAULT_WIN32_WINNT 0xa00)
    configure_file(${MINGW_HEADERS}/crt/_mingw.h.in ${WIN32_DIR}/_mingw.h @ONLY)
    foreach(header dinput.h dsound.h mmsystem.h objbase.h unknwn.h windows.h winerror.h)
        file(CONFIGURE OUTPUT ${WIN32_DIR}/${header}
            CONTENT "#include <mingw_begin.h>
#include_next <${header}>
#include <mingw_end.h>
")
    endforeach()

    file(GLOB COMPAT_SOURCES CONFIGURE_DEPENDS compat/dx/*.cpp compat/gl/*.cpp compat/win32/*.cpp)
    add_library(tmweb_compat STATIC ${COMPAT_SOURCES})
    target_include_directories(tmweb_compat SYSTEM PUBLIC ${WIN32_DIR} compat/win32 compat/dx compat)
    # Kestrel's style, not ours to change
    target_compile_options(tmweb_compat PRIVATE -Wno-unused-parameter)
    # After the C library's own headers, so mingw-w64 only fills in what musl lacks
    target_compile_options(tmweb_compat PUBLIC
        "SHELL:-idirafter ${MINGW_HEADERS}/include"
        "SHELL:-idirafter ${MINGW_HEADERS}/crt"
        # MSVC code: __declspec, anonymous structs
        -fms-extensions
        -fdeclspec
        -fno-strict-aliasing
        # Wasm has no __stdcall, which the COM interfaces declare
        -Wno-ignored-attributes
        # mingw-w64's headers
        -Wno-pragma-pack
        -Wno-unknown-pragmas)
    # INITGUID defines each GUID where a header declares it, as a selectany constant the linker folds
    target_compile_definitions(tmweb_compat PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN INITGUID)
    target_link_options(tmweb_compat PUBLIC -lGL -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
        --js-library=${CMAKE_CURRENT_SOURCE_DIR}/compat/gl/gl.js)

    # d3d8.dll, dinput8.dll and dsound.dll for the guest, on compat's objects
    add_library(tmweb_directx STATIC com/directx.cpp)
    target_link_libraries(tmweb_directx PUBLIC tmweb_core tmweb_compat)
endif()
target_link_libraries(tmweb_host PUBLIC tmweb_hle)

add_executable(tmweb app/main.cpp)
target_link_libraries(tmweb PRIVATE tmweb_core)
if(EMSCRIPTEN)
    # Node reads the user's game files straight from disk; the guest arena needs 384 MiB of linear memory
    target_link_options(tmweb PRIVATE -sNODERAWFS=1 -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=448MB -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1)
endif()

if(EMSCRIPTEN)
    # The module the page's worker and the Node runner load (web/module.js): no main, the host drives it. The page
    # copies the game files into MEMFS; the Node runner mounts the runtime folder with NODEFS.
    set(HOST_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/web/host.js)
    add_executable(tmweb-web)
    target_link_libraries(tmweb-web PRIVATE tmweb_host)
    set_target_properties(tmweb-web PROPERTIES SUFFIX ".mjs")
    set_property(TARGET tmweb-web APPEND PROPERTY LINK_DEPENDS ${HOST_LIBRARY})
    target_link_options(tmweb-web PRIVATE --no-entry --js-library=${HOST_LIBRARY}
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createTmweb -sENVIRONMENT=worker,node
        -sFORCE_FILESYSTEM=1 -lnodefs.js -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=448MB -sMAXIMUM_MEMORY=2GB
        -sSTACK_SIZE=4MB
        -sEXPORTED_FUNCTIONS=_tmweb_boot,_tmweb_run,_tmweb_instructions,_tmweb_input,_tmweb_sound_take,_tmweb_sound_frames,_tmweb_socket_event,_malloc,_free
        -sEXPORTED_RUNTIME_METHODS=FS,NODEFS,HEAPU8,HEAPF32,UTF8ToString,stringToUTF8,lengthBytesUTF8)
endif()

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
        URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_executable(tmweb_tests
        tests/boundary_com_test.cpp
        tests/boot_test.cpp
        tests/boundary_test.cpp
        tests/cpu_test.cpp
        tests/guest_test.cpp
        tests/hle_test.cpp
        tests/host_test.cpp
        tests/f80_test.cpp
        tests/memory_test.cpp
        tests/proxies_test.cpp
        tests/threads_test.cpp
        tests/x87_test.cpp
    )
    target_link_libraries(tmweb_tests PRIVATE tmweb_host Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(tmweb_tests)

    # The guest and boot tests again with Unicorn as the CPU, a reference for the interpreter. Off in CI. The boot
    # test reads the user's TM_native.dll from TMWEB_RUNTIME (the runtime folder) and skips without it.
    option(TMWEB_UNICORN "Build the tests that run guest code on Unicorn" OFF)
    if(TMWEB_UNICORN)
        set(UNICORN_ARCH "x86" CACHE STRING "" FORCE)
        set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
        set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(unicorn
            URL https://github.com/unicorn-engine/unicorn/archive/refs/tags/v2.1.4.tar.gz
            URL_HASH SHA256=ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(unicorn)
        add_executable(tmweb_unicorn_tests
            tests/boot_test.cpp
            tests/guest_test.cpp
            tests/unicorn_runner.cpp
        )
        target_compile_definitions(tmweb_unicorn_tests PRIVATE TMWEB_ON_UNICORN)
        target_link_libraries(tmweb_unicorn_tests PRIVATE tmweb_hle unicorn Catch2::Catch2WithMain)
        catch_discover_tests(tmweb_unicorn_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()

# f80's results hashed: native and Wasm (run by Node) must print the same digest as kExpected
if(BUILD_TESTING)
    add_executable(f80_digest tests/f80_digest.cpp)
    target_link_libraries(f80_digest PRIVATE tmweb_core)
    if(EMSCRIPTEN)
        target_link_options(f80_digest PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1)
    endif()
    add_test(NAME f80_digest COMMAND f80_digest --check)
endif()

# f80 against the host x87: Windows x86 only, run by hand (browser/README.md)
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(f80_hostdiff tests/f80_hostdiff.cpp)
    target_link_libraries(f80_hostdiff PRIVATE tmweb_core)
endif()

if(BUILD_TESTING AND EMSCRIPTEN)
    # What only builds for Wasm, under Node: node build/browser-wasm/tmweb_wasm_tests.js. One guest arena per
    # process, so the tests share one machine.
    add_executable(tmweb_wasm_tests tests/directx_test.cpp)
    target_link_libraries(tmweb_wasm_tests PRIVATE tmweb_directx Catch2::Catch2WithMain)
    target_link_options(tmweb_wasm_tests PRIVATE -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=448MB -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1)
    add_test(NAME tmweb_wasm_tests COMMAND node $<TARGET_FILE:tmweb_wasm_tests>)
endif()
