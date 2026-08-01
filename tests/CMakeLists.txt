# CroCOS Core Library Unit Tests
# Created by Spencer Martin on 7/24/25.
# 
# This CMakeLists.txt is designed to be used independently from the main build
# to test the Core library using the host compiler while maintaining freestanding mode

cmake_minimum_required(VERSION 3.20)

# Use Homebrew clang with C++26 support, fallback to system clang
find_program(HOMEBREW_CLANG /opt/homebrew/opt/llvm/bin/clang)
find_program(HOMEBREW_CLANGXX /opt/homebrew/opt/llvm/bin/clang++)

if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
    set(CMAKE_C_COMPILER ${HOMEBREW_CLANG})
    set(CMAKE_CXX_COMPILER ${HOMEBREW_CLANGXX})
    message(STATUS "Using Homebrew clang: ${HOMEBREW_CLANG}")
else()
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
    message(STATUS "Using system clang (fallback)")
endif()

project(CroCOSTests LANGUAGES C CXX)

# Don't use CMAKE_CXX_STANDARD since it might not recognize C++26
# We'll use explicit flags instead

# We'll instrument both Core library and test files together in the main executable

# Create the test harness library (uses standard library)
add_library(TestHarness STATIC)

target_sources(TestHarness PRIVATE
    harness/TestHarness.cpp
    harness/MemoryTracker.cpp
)

target_include_directories(TestHarness PUBLIC
    .
    harness
)

# Test harness can use standard library and exceptions
target_compile_options(TestHarness PRIVATE
    -std=c++2c
    -Wall
    -Wextra
    -g
)

# Link against C++ standard library for std::unordered_map, etc.
if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
target_link_libraries(TestHarness PUBLIC
    /opt/homebrew/opt/llvm/lib/c++/libc++.a
    /opt/homebrew/opt/llvm/lib/c++/libc++abi.a
)
else()
target_link_libraries(TestHarness PUBLIC
    c++
)
endif()

add_subdirectory(kernel)
add_subdirectory(liballoc)
add_subdirectory(core)

# Add a custom target to run all tests (core + kernel)
add_custom_target(run_all_tests
    USES_TERMINAL
    COMMAND core/CoreTestRunner
    COMMAND kernel/KernelTestRunner
    COMMAND liballoc/LibAllocTestRunner
    COMMAND kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunner
    COMMAND kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunnerTSan
    COMMAND kernel/rcu/KernelRcuIntegrationTestRunner
    COMMAND kernel/rcu/KernelRcuIntegrationTestRunnerTSan
    DEPENDS core/CoreTestRunner liballoc/LibAllocTestRunner kernel/KernelTestRunner
            kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunner
            kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunnerTSan
            kernel/rcu/KernelRcuIntegrationTestRunner
            kernel/rcu/KernelRcuIntegrationTestRunnerTSan
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running all CroCOS unit tests"
)