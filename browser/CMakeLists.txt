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

# Berkeley SoftFloat 3e, the x87 subset (third_party/softfloat/README.md)
file(GLOB softfloat_sources CONFIGURE_DEPENDS third_party/softfloat/source/*.c third_party/softfloat/source/8086/*.c)
add_library(softfloat STATIC ${softfloat_sources})
target_include_directories(softfloat PUBLIC third_party/softfloat third_party/softfloat/source/include
    third_party/softfloat/source/8086)
target_compile_options(softfloat PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>)

add_library(tmweb_core STATIC
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
set_target_properties(tmweb_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT EMSCRIPTEN)
    # The interpreter alone behind a C interface, for the differential test against Unicorn (tools/difftest.py)
    add_library(tmweb_capi SHARED tools/capi.cpp)
    target_link_libraries(tmweb_capi PRIVATE tmweb_core)
endif()

# The module's side of the page and the Node runner (host/host.h, web/)
add_library(tmweb_host OBJECT app/boot.cpp host/host.cpp)
target_link_libraries(tmweb_host PUBLIC tmweb_core)
target_link_libraries(tmweb_core PRIVATE softfloat)

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
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    include(FetchContent)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
        URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(Catch2)
    add_executable(tmweb_tests
        tests/cpu_test.cpp
        tests/host_test.cpp
        tests/f80_test.cpp
        tests/memory_test.cpp
        tests/threads_test.cpp
    )
    target_link_libraries(tmweb_tests PRIVATE tmweb_host Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(tmweb_tests)
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
