cmake_minimum_required(VERSION 3.24)

project(EngineX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ENGINEX_ENABLE_TESTS "Build the EngineX test suite" ON)
option(ENGINEX_WERROR "Treat warnings in EngineX-owned code as errors" ON)

include(cmake/Warnings.cmake)
if(APPLE)
    # CMake repeats static libraries for transitive deps; Xcode 15+ ld warns about each repeat.
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" ENGINEX_HAS_NO_WARN_DUP)
    if(ENGINEX_HAS_NO_WARN_DUP)
        add_link_options("LINKER:-no_warn_duplicate_libraries")
    endif()
endif()
include(cmake/Sanitizers.cmake)
include(FetchContent)

# ---------------------------------------------------------------------------
# Pinned third-party dependencies (URL + SHA-256; no moving branches).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# Build metadata recorded in benchmark output.
find_package(Git QUIET)
set(ENGINEX_GIT_SHA "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ENGINEX_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT ENGINEX_GIT_SHA)
        set(ENGINEX_GIT_SHA "unknown")
    endif()
endif()
configure_file(cmake/build_info.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/enginex/build_info.hpp
               @ONLY)

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
# enginex_core: pure matching, validation, sequencing, canonical state. No I/O, no JSON.
add_library(enginex_core
    src/types.cpp
    src/order_book.cpp
    src/engine.cpp
    src/state.cpp
    src/canonical.cpp
    src/sha256.cpp)
target_include_directories(enginex_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
enginex_set_warnings(enginex_core)

# enginex_codec: JSON at the command/transport boundary (simulate, wire protocol).
add_library(enginex_codec src/json_codec.cpp src/simulate.cpp)
target_link_libraries(enginex_codec PUBLIC enginex_core nlohmann_json::nlohmann_json)
enginex_set_warnings(enginex_codec)

# enginex_journal: binary command journal, recovery and the durable state machine (R2).
add_library(enginex_journal src/crc32c.cpp src/journal.cpp src/durable.cpp)
target_link_libraries(enginex_journal PUBLIC enginex_core)
enginex_set_warnings(enginex_journal)

# enginex_net: wire protocol, bounded queues, TCP server and client (R3).
find_package(Threads REQUIRED)
add_library(enginex_net src/protocol.cpp src/socket.cpp src/server.cpp src/client.cpp)
target_link_libraries(enginex_net PUBLIC enginex_codec enginex_journal Threads::Threads)
enginex_set_warnings(enginex_net)

# enginex_itch: Nasdaq TotalView-ITCH 5.0 decoding and order-book reconstruction from historical
# market data. Independent of the synthetic exchange (it only reuses SHA-256 from the core).
add_library(enginex_itch src/itch/codec.cpp src/itch/book.cpp src/itch/replay.cpp src/itch/synthetic.cpp)
target_link_libraries(enginex_itch PUBLIC enginex_core)
enginex_set_warnings(enginex_itch)
option(ENGINEX_ITCH_STD_INDEX "Benchmark baseline: std::unordered_map order index" OFF)
if(ENGINEX_ITCH_STD_INDEX)
    target_compile_definitions(enginex_itch PRIVATE ENGINEX_ITCH_STD_INDEX)
endif()

# enginex_bench_lib: workload generation and the in-process measurement harness.
add_library(enginex_bench_lib benchmarks/scenarios.cpp benchmarks/harness.cpp src/metrics.cpp)
target_include_directories(enginex_bench_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
target_link_libraries(enginex_bench_lib PUBLIC enginex_core enginex_journal enginex_net)
enginex_set_warnings(enginex_bench_lib)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(enginex src/main.cpp src/cli_simulate.cpp src/cli_benchmark.cpp src/cli_journal.cpp
    src/cli_net.cpp src/cli_itch.cpp)
target_link_libraries(enginex PRIVATE enginex_net enginex_bench_lib enginex_itch)
enginex_set_warnings(enginex)

add_executable(enginex_bench benchmarks/bench_main.cpp)
target_link_libraries(enginex_bench PRIVATE enginex_bench_lib)
enginex_set_warnings(enginex_bench)

# ITCH decode / book / replay measurements (counts heap allocations via operator new).
add_executable(enginex_itch_bench benchmarks/itch_bench.cpp src/metrics.cpp)
target_link_libraries(enginex_itch_bench PRIVATE enginex_itch)
enginex_set_warnings(enginex_itch_bench)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ENGINEX_ENABLE_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()
