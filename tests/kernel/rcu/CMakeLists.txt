# RCU Phase 2 — userspace integration test harness.
#
# Compiles the production kernel/rcu/RCU.cpp (and, underneath it, the production
# kernel/mm/vmsmalloc.cpp) against mock headers, so retireDestroy round-trips
# through a real allocator rather than a stand-in.
#
# The mock set is deliberately SHARED with tests/kernel/vmsmalloc rather than
# duplicated: mem/VMSubstrate.h, mem/NUMA.h, kassert.h, MockCpuLocal.h and the
# per-thread interrupt depths all come from there. Two mocks are new here
# (rcu/mocks/): timing/timing.h and arch::InterruptDisabler, neither of which
# had a userspace body anywhere under tests/ before this phase.
#
# Include-path order matters twice over: rcu/mocks first so timing/timing.h
# shadows the real one, then vmsmalloc/mocks so mem/ and kassert.h shadow
# theirs, and only then kernel/include.

set(RCU_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(VMS_MOCKS ${CMAKE_CURRENT_SOURCE_DIR}/../vmsmalloc/mocks)

set(RCU_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TestMain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VeneerTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AssertionsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConcurrentTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TortureTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/MockRcuEnv.cpp
    ${VMS_MOCKS}/MockVMSubstrate.cpp
    ${VMS_MOCKS}/MockKernelEnv.cpp
    ${RCU_ROOT}/kernel/rcu/RCU.cpp
    ${RCU_ROOT}/kernel/mm/vmsmalloc.cpp
    ${RCU_ROOT}/libraries/Core/PrintStream.cpp
    ${RCU_ROOT}/libraries/Core/str.cpp
    ${RCU_ROOT}/libraries/Core/Object.cpp
    ${RCU_ROOT}/libraries/Core/atomic/atomic.cpp
    ${RCU_ROOT}/libraries/Core/atomic/AtomicBitPool.cpp
)

set(RCU_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks           # shadow timing/timing.h; MockRcuEnv.h
    ${VMS_MOCKS}                                # shadow mem/*, kassert.h; MockCpuLocal.h
    ${CMAKE_CURRENT_SOURCE_DIR}                 # this directory's test headers
    ${RCU_ROOT}/kernel/include
    ${RCU_ROOT}/kernel/mm                       # VMSubstrateSlab.h (real)
    ${RCU_ROOT}/libraries/Core/include
    ${RCU_ROOT}/libraries/LibAlloc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../harness   # TestHarness.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../..           # tests/ — assert_support.h, test.h
)

set(RCU_DEFINES
    CROCOS_TESTING
    CORE_LIBRARY_TESTING
    CORE_KERNEL_TESTING
    CORE_LINKED_WITH_KERNEL
    CROCOS_RCU_TEST_HARNESS
)

set(RCU_COMMON_FLAGS
    -std=c++2c -fexceptions -Wall -Wextra -Wno-c++26-extensions
    # Force-include assert_support.h: the throwing assert macro references
    # CroCOSTest, which neither the mocks nor RCU.cpp nor vmsmalloc.cpp include.
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../../assert_support.h)

# ─── ASan + leak runner ─────────────────────────────────────────────────────
add_executable(KernelRcuIntegrationTestRunner ${RCU_SOURCES})
target_include_directories(KernelRcuIntegrationTestRunner PRIVATE ${RCU_INCLUDES})
target_compile_definitions(KernelRcuIntegrationTestRunner PRIVATE ${RCU_DEFINES})
target_compile_options(KernelRcuIntegrationTestRunner PRIVATE
    ${RCU_COMMON_FLAGS} -O0 -g -fsanitize=address -fsanitize=leak)
target_link_libraries(KernelRcuIntegrationTestRunner PRIVATE TestHarness)
target_link_options(KernelRcuIntegrationTestRunner PRIVATE -fsanitize=address)
if(APPLE)
    target_link_options(KernelRcuIntegrationTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:TestHarness>)
else()
    target_link_options(KernelRcuIntegrationTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests)
endif()

add_custom_target(run_rcu_integration_tests
    COMMAND KernelRcuIntegrationTestRunner
    DEPENDS KernelRcuIntegrationTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running RCU veneer + torture tests (ASan/leak)")

# specs/rcu-phase-3.md's names. Phase 3 makes this runner "all RCU tests" rather
# than "the Phase 2 veneer tests", so these are the ones to reach for; the
# run_rcu_integration_tests pair is kept because it is what already exists in
# muscle memory and in this tree's docs.
add_custom_target(run_rcu_tests DEPENDS run_rcu_integration_tests)

# ─── TSan runner (ARMv8/M1 = primary release gate) ──────────────────────────
add_executable(KernelRcuIntegrationTestRunnerTSan ${RCU_SOURCES})
target_include_directories(KernelRcuIntegrationTestRunnerTSan PRIVATE ${RCU_INCLUDES})
target_compile_definitions(KernelRcuIntegrationTestRunnerTSan PRIVATE ${RCU_DEFINES})
target_compile_options(KernelRcuIntegrationTestRunnerTSan PRIVATE
    ${RCU_COMMON_FLAGS} -O1 -g -fsanitize=thread)
target_link_libraries(KernelRcuIntegrationTestRunnerTSan PRIVATE TestHarness)
target_link_options(KernelRcuIntegrationTestRunnerTSan PRIVATE -fsanitize=thread)
if(APPLE)
    target_link_options(KernelRcuIntegrationTestRunnerTSan PRIVATE
        -Wl,-force_load,$<TARGET_FILE:TestHarness>
        -Wl,-undefined,dynamic_lookup)
else()
    target_link_options(KernelRcuIntegrationTestRunnerTSan PRIVATE
        -Wl,--keep-section=.crocos_unit_tests)
endif()

add_custom_target(run_rcu_integration_tests_tsan
    COMMAND KernelRcuIntegrationTestRunnerTSan
    DEPENDS KernelRcuIntegrationTestRunnerTSan
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running RCU veneer + torture tests (TSan — the release gate)")

add_custom_target(run_rcu_tests_tsan DEPENDS run_rcu_integration_tests_tsan)
