# CroCOS Kernel Unit Tests
# Created by Spencer Martin on 7/27/25.
#
# This CMakeLists.txt tests kernel functionality using the host compiler
# with host-compatible compilation flags

cmake_minimum_required(VERSION 3.20)

# Create the core test library
add_library(CoreTests STATIC)

target_sources(CoreTests PRIVATE
    ArrayTest.cpp
    GraphTest.cpp
    HeapTest.cpp
    ObjectTest.cpp
    OptionalTest.cpp
    PermutationTest.cpp
    RingBufferTest.cpp
    HighReliabilityRingBufferTests.cpp
    SmartPointerTest.cpp
    TreeTest.cpp
    VariantTest.cpp
    VectorTest.cpp
    LinkedListTests.cpp
    AtomicLinkedListTest.cpp
    AtomicBitPoolTest.cpp
    SplitBitmapTest.cpp
    TreiberStackTest.cpp
    RcuEpochDomainTest.cpp
    RcuConcurrentTest.cpp
    #AtomicBitPoolGlobalLockMock.cpp
    #AtomicBitPoolConcurrentLinkedList.cpp
)

# Add the TestHarness from parent directory
target_include_directories(CoreTests PUBLIC
    ..
    ../harness
    ../../libraries/Core/include
)

# Add corelib source files that we're testing
target_sources(CoreTests PRIVATE
    ../../libraries/Core/PrintStream.cpp
    ../../libraries/Core/str.cpp
    ../../libraries/Core/atomic/atomic.cpp
    ../../libraries/Core/atomic/AtomicBitPool.cpp
    ../../libraries/Core/Object.cpp
)

# Core tests use host-compatible compilation
target_compile_options(CoreTests PRIVATE
    -std=c++2c
    -fexceptions                    # Allow exceptions for test framework
    -Wall
    -Wextra
    -Wno-c++26-extensions          # Allow C++26 extensions
    -Wno-vla
    -DCORE_KERNEL_TESTING          # Define to help resolve conflicts
    -DCORE_LINKED_WITH_KERNEL      # Match kernel compilation
    -DCORE_LIBRARY_TESTING
    -DCROCOS_TESTING
    -O0
    #-fsanitize=address  # Temporarily disabled - AddressSanitizer has locale issues with std::cout
    #-fsanitize=leak
    -g
)

# Force include test.h ONLY for the Core library source files being tested
# NOT for the test files themselves, so test infrastructure uses real allocators
set_source_files_properties(
    ../../libraries/Core/PrintStream.cpp
    ../../libraries/Core/str.cpp
    ../../libraries/Core/atomic/atomic.cpp
    ../../libraries/Core/Object.cpp
    PROPERTIES COMPILE_FLAGS "-include ${CMAKE_CURRENT_SOURCE_DIR}/../test.h -DCROCOS_TEST_INSTRUMENT_ALLOCATORS"
)

# AtomicBitPool.cpp uses memset; include string.h to ensure it is declared in
# the freestanding-style test build (no -ffreestanding, but no implicit includes).
set_source_files_properties(
    ../../libraries/Core/atomic/AtomicBitPool.cpp
    PROPERTIES COMPILE_FLAGS "-include ${CMAKE_CURRENT_SOURCE_DIR}/../test.h -DCROCOS_TEST_INSTRUMENT_ALLOCATORS -include string.h"
)

# Custom PRE_LINK command to instrument object files before linking
add_custom_command(TARGET CoreTests PRE_LINK
    COMMENT "Instrumenting corelib test object files for memory tracking before linking"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../instrument_objects.sh ${CMAKE_CURRENT_BINARY_DIR} CoreTests
)

# Create test executable
add_executable(CoreTestRunner)

target_sources(CoreTestRunner PRIVATE
    ../TestMain.cpp
)

target_link_libraries(CoreTestRunner PRIVATE
    CoreTests
    TestHarness
)

target_include_directories(CoreTestRunner PRIVATE
    ..
    ../harness
    ../../kernel/include
    ../../libraries/Core/include
)

# Test runner needs exceptions and standard library compatibility
target_compile_options(CoreTestRunner PRIVATE
    -std=c++2c
    -fexceptions
    -Wall
    -Wextra
    -DCORE_KERNEL_TESTING
    -O0
    -fsanitize=address
    -fsanitize=leak
    -g
)

# Force linker to keep custom test section
if(APPLE)
    target_link_options(CoreTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:CoreTests>
    )
else()
    target_link_options(CoreTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

target_compile_options(CoreTestRunner PRIVATE -fsanitize=address)
target_link_options(CoreTestRunner PRIVATE -fsanitize=address)

# On macOS, the linker automatically creates section boundary symbols
# On Linux, we need to tell the linker to keep our custom section
if(NOT APPLE)
    target_link_options(CoreTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# Run both ASan and TSan core test runners (mirrors the Phase-1
# LibAlloc/run_liballoc_tests target).
add_custom_target(run_core_tests
    COMMAND CoreTestRunner
    COMMAND CoreTestRunnerTSan
    DEPENDS CoreTestRunner CoreTestRunnerTSan
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS corelib unit tests (ASan + TSan)"
)

# ============================================================
# TSan-instrumented parallel runner (vmsmalloc Phase-4 release gate for
# ARMv8 weak-memory correctness — parent-spec DEC-042 #4).
#
# TSan and ASan are mutually exclusive at link time, so we build a sibling
# library + runner pair instead of toggling flags on the ASan target.
# Mirrors the Phase-1 LibAllocTestRunnerTSan pattern.
# ============================================================

add_library(CoreTestsTSan STATIC)

target_sources(CoreTestsTSan PRIVATE
    ArrayTest.cpp
    GraphTest.cpp
    HeapTest.cpp
    ObjectTest.cpp
    OptionalTest.cpp
    PermutationTest.cpp
    RingBufferTest.cpp
    HighReliabilityRingBufferTests.cpp
    SmartPointerTest.cpp
    TreeTest.cpp
    VariantTest.cpp
    VectorTest.cpp
    LinkedListTests.cpp
    AtomicLinkedListTest.cpp
    AtomicBitPoolTest.cpp
    SplitBitmapTest.cpp
    TreiberStackTest.cpp
    RcuEpochDomainTest.cpp
    RcuConcurrentTest.cpp
)

target_include_directories(CoreTestsTSan PUBLIC
    ..
    ../harness
    ../../libraries/Core/include
)

target_sources(CoreTestsTSan PRIVATE
    ../../libraries/Core/PrintStream.cpp
    ../../libraries/Core/str.cpp
    ../../libraries/Core/atomic/atomic.cpp
    ../../libraries/Core/atomic/AtomicBitPool.cpp
    ../../libraries/Core/Object.cpp
)

target_compile_options(CoreTestsTSan PRIVATE
    -std=c++2c
    -fexceptions
    -Wall
    -Wextra
    -Wno-c++26-extensions
    -Wno-vla
    -DCORE_KERNEL_TESTING
    -DCORE_LINKED_WITH_KERNEL
    -DCORE_LIBRARY_TESTING
    -DCROCOS_TESTING
    -O1
    -g
    -fsanitize=thread
)

# Force include test.h ONLY for the Core library source files being tested
# (mirrors the ASan library above).
set_source_files_properties(
    ../../libraries/Core/PrintStream.cpp
    ../../libraries/Core/str.cpp
    ../../libraries/Core/atomic/atomic.cpp
    ../../libraries/Core/Object.cpp
    PROPERTIES COMPILE_FLAGS "-include ${CMAKE_CURRENT_SOURCE_DIR}/../test.h -DCROCOS_TEST_INSTRUMENT_ALLOCATORS"
)

set_source_files_properties(
    ../../libraries/Core/atomic/AtomicBitPool.cpp
    PROPERTIES COMPILE_FLAGS "-include ${CMAKE_CURRENT_SOURCE_DIR}/../test.h -DCROCOS_TEST_INSTRUMENT_ALLOCATORS -include string.h"
)

add_custom_command(TARGET CoreTestsTSan PRE_LINK
    COMMENT "Instrumenting corelib TSan test object files for memory tracking before linking"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../instrument_objects.sh ${CMAKE_CURRENT_BINARY_DIR} CoreTestsTSan
)

add_executable(CoreTestRunnerTSan)

target_sources(CoreTestRunnerTSan PRIVATE
    ../TestMain.cpp
)

target_link_libraries(CoreTestRunnerTSan PRIVATE
    CoreTestsTSan
    TestHarness
)

target_include_directories(CoreTestRunnerTSan PRIVATE
    ..
    ../harness
    ../../kernel/include
    ../../libraries/Core/include
)

target_compile_options(CoreTestRunnerTSan PRIVATE
    -std=c++2c
    -fexceptions
    -Wall
    -Wextra
    -DCORE_KERNEL_TESTING
    -O1
    -g
    -fsanitize=thread
)

target_link_options(CoreTestRunnerTSan PRIVATE -fsanitize=thread)

if(APPLE)
    target_link_options(CoreTestRunnerTSan PRIVATE
        -Wl,-force_load,$<TARGET_FILE:CoreTestsTSan>
    )
else()
    target_link_options(CoreTestRunnerTSan PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# Add a custom target to run both ASan and TSan core tests
add_custom_target(run_core_tests_tsan
    COMMAND CoreTestRunnerTSan
    DEPENDS CoreTestRunnerTSan
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS corelib unit tests (TSan)"
)
