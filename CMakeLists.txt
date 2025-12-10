cmake_minimum_required(VERSION 3.20)
project(itch_feed_handler 
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Zero-Copy NASDAQ TotalView-ITCH 5.0 Feed Handler"
)

# ============================================================================
# C++20 Strict Configuration
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Compiler Flags for Low-Latency
# ============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -O3
        -march=native
        -fno-exceptions       # Exceptions are too slow for HFT
        -fno-rtti             # RTTI has overhead
    )
endif()

# ============================================================================
# Include Directories
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Fetch Dependencies (Google Test & Google Benchmark)
# ============================================================================
include(FetchContent)

# Google Test
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

# Google Benchmark
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
)

# Disable benchmark tests to speed up build
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest googlebenchmark)

# ============================================================================
# Source Files (library)
# ============================================================================
# Header-only library for now, will add implementation files later
add_library(itch_parser INTERFACE)
target_include_directories(itch_parser INTERFACE ${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Main Executable (PCAP Driver)
# ============================================================================
add_executable(itch_driver
    src/main.cpp
)
target_link_libraries(itch_driver
    PRIVATE
        itch_parser
)
# ============================================================================
# Benchmarks
# ============================================================================
add_executable(itch_benchmark
    benchmarks/hello_benchmark.cpp
    benchmarks/itch_bench.cpp
)
target_link_libraries(itch_benchmark 
    PRIVATE 
        itch_parser
        benchmark::benchmark
        benchmark::benchmark_main
)

# ============================================================================
# Tests
# ============================================================================
enable_testing()

add_executable(itch_tests
    tests/hello_test.cpp
    tests/message_test.cpp
    tests/parser_test.cpp
)
target_link_libraries(itch_tests 
    PRIVATE 
        itch_parser
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(itch_tests)

# ============================================================================
# Custom Targets
# ============================================================================
add_custom_target(run_benchmarks
    COMMAND itch_benchmark
    DEPENDS itch_benchmark
    COMMENT "Running benchmarks..."
)

add_custom_target(run_tests
    COMMAND ctest --output-on-failure
    DEPENDS itch_tests
    COMMENT "Running tests..."
)
