
# CroCOS Kernel Unit Tests
# Created by Spencer Martin on 7/27/25.
# 
# This CMakeLists.txt tests kernel functionality using the host compiler
# with host-compatible compilation flags

cmake_minimum_required(VERSION 3.20)

# Create the kernel test library 
add_library(KernelTests STATIC)

target_sources(KernelTests PRIVATE
    ArchMocks.cpp
    InterruptGraphTests.cpp
    PageAllocatorTests.cpp
    NUMATests.cpp
    vmsmalloc/SlabLayoutTest.cpp
)

# Add the TestHarness from parent directory
target_include_directories(KernelTests PUBLIC
    ../harness
    ../../kernel/include
    ../../kernel/mm                  # implementation-internal headers (VMSubstrateSlab.h)
    ../../libraries/Core/include
    ../../libraries/LibAlloc/include
)

# Add kernel source files that we're testing
target_sources(KernelTests PRIVATE
        ../../kernel/interrupts/InterruptGraphManager.cpp
        ../../kernel/interrupts/ExplicitTypes.cpp
        ../../kernel/mm/PageAllocator.cpp
        ../../kernel/mm/MemTypes.cpp
        ../../kernel/mm/NUMA.cpp
        ../../kernel/arch/arch.cpp
)

# Add Core library sources needed for kernel code
target_sources(KernelTests PRIVATE
    ../../libraries/Core/PrintStream.cpp
    ../../libraries/Core/str.cpp
    ../../libraries/Core/Object.cpp
    ../../libraries/Core/atomic/atomic.cpp
    ../../libraries/Core/atomic/AtomicBitPool.cpp
)

# Kernel tests use host-compatible compilation but with freestanding mode for tested code
target_compile_options(KernelTests PRIVATE
    -std=c++2c
    -fexceptions                    # Allow exceptions for test framework
    -Wall
    -Wextra
    -Wno-c++26-extensions          # Allow C++26 extensions
    -DCORE_KERNEL_TESTING          # Define to help resolve conflicts
    -DCORE_LINKED_WITH_KERNEL      # Match kernel compilation
    -DCORE_LIBRARY_TESTING
    -DCROCOS_TESTING
    -O0
    -fsanitize=address
    -fsanitize=leak
    -g
)

# Force include test.h ONLY for the library/kernel source files being tested
# NOT for the test files themselves, so test infrastructure uses real allocators
set_source_files_properties(
    ../../kernel/interrupts/InterruptGraphManager.cpp
    ../../kernel/interrupts/ExplicitTypes.cpp
    ../../libraries/Core/PrintStream.cpp
    ../../libraries/Core/str.cpp
    ../../libraries/Core/Object.cpp
    ../../libraries/Core/atomic/atomic.cpp
    ../../kernel/mm/PageAllocator.cpp
    ../../kernel/mm/NUMA.cpp
    ../../kernel/arch/arch.cpp
    PROPERTIES COMPILE_FLAGS "-include ${CMAKE_CURRENT_SOURCE_DIR}/../test.h -DCROCOS_TEST_INSTRUMENT_ALLOCATORS"
)

# Custom PRE_LINK command to instrument object files before linking
add_custom_command(TARGET KernelTests PRE_LINK
    COMMENT "Instrumenting kernel test object files for memory tracking before linking"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../instrument_objects.sh ${CMAKE_CURRENT_BINARY_DIR} KernelTests
)

# Create test executable
add_executable(KernelTestRunner)

target_sources(KernelTestRunner PRIVATE
    ../TestMain.cpp
)

target_link_libraries(KernelTestRunner PRIVATE
    KernelTests
    TestHarness
)

target_include_directories(KernelTestRunner PRIVATE
    ..
    ../harness
    ../../kernel/include
    ../../libraries/Core/include
)

# Test runner needs exceptions and standard library compatibility
target_compile_options(KernelTestRunner PRIVATE
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
    target_link_options(KernelTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:KernelTests>
    )
else()
    target_link_options(KernelTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

target_compile_options(KernelTestRunner PRIVATE -fsanitize=address)
target_link_options(KernelTestRunner PRIVATE -fsanitize=address)

# On macOS, the linker automatically creates section boundary symbols
# On Linux, we need to tell the linker to keep our custom section
if(NOT APPLE)
    target_link_options(KernelTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# vmsmalloc Phase 8 — standalone integration harness (own runner binaries).
add_subdirectory(vmsmalloc)
add_subdirectory(rcu)

# Add a custom target to run kernel tests
add_custom_target(run_kernel_tests
    COMMAND KernelTestRunner
    DEPENDS KernelTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS Kernel unit tests"
)