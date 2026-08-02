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

# Mocks + production code under test. Shared by the correctness runners and the
# soak runners, which differ only in which test TUs they link.
set(RCU_SUPPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TestMain.cpp
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

set(RCU_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/VeneerTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AssertionsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConcurrentTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TortureTest.cpp
    ${RCU_SUPPORT_SOURCES}
)

# The soak lives in its OWN executables, not merely disarmed inside the
# correctness runners. It is an endurance/performance harness, so it must not
# appear in run_all_tests at all — not as a test, not as a skipped line, not as a
# row in the total.
set(RCU_SOAK_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/SoakTest.cpp
    ${RCU_SUPPORT_SOURCES}
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

# ─── Endurance / performance soak ───────────────────────────────────────────
#
# Separate executables, deliberately absent from run_all_tests. The test is
# disarmed unless CROCOS_RCU_SOAK_SECONDS is set, so running the binary bare
# prints usage rather than burning a minute; the targets below supply the env.
#
#   cmake --build tests/build --target run_rcu_soak        # ASan  — UAGP oracle
#   cmake --build tests/build --target run_rcu_soak_tsan   # TSan  — race oracle
#   cmake --build tests/build --target run_rcu_soak_perf   # -O2   — REAL numbers
#   cmake -S tests -B tests/build -DRCU_SOAK_SECONDS=300 -DRCU_SOAK_THREADS=8
#
# The Perf variant is the only one whose throughput and latency figures mean
# anything: ASan instruments every load and store, and TSan additionally
# serialises through shadow memory, so both inflate latency by roughly an order
# of magnitude. It is NOT a correctness runner — the soak's assertions still hold
# there, but with no sanitizer behind them a use-after-grace-period reads
# recycled bytes silently instead of trapping. Run one of the other two first.
set(RCU_SOAK_SECONDS   60 CACHE STRING "RCU soak duration, seconds")
set(RCU_SOAK_THREADS    8 CACHE STRING "RCU soak thread count (clamped to 4..8)")
set(RCU_SOAK_QUIET_PCT 12 CACHE STRING "RCU soak: percent chance a thread naps between bursts")

foreach(variant ASan TSan Perf)
    # Cleared every iteration: CMake variables outlive the loop body, and a
    # CROCOS_SOAK_POOLED that leaked into a sanitizer runner would silently
    # disable that runner's UAGP oracle while it still reported green.
    set(soakExtraDefs "")
    if(variant STREQUAL "ASan")
        set(soakTarget KernelRcuSoakRunner)
        set(soakFlags  -O0 -g -fsanitize=address -fsanitize=leak)
        set(soakLink   -fsanitize=address)
        set(soakRun    run_rcu_soak)
    elseif(variant STREQUAL "TSan")
        set(soakTarget KernelRcuSoakRunnerTSan)
        set(soakFlags  -O1 -g -fsanitize=thread)
        set(soakLink   -fsanitize=thread)
        set(soakRun    run_rcu_soak_tsan)
    else()
        set(soakTarget KernelRcuSoakRunnerPerf)
        # -fno-omit-frame-pointer so samply / Instruments / perf can walk stacks;
        # -g so those stacks have names. Neither costs measurable throughput, and
        # a profiling build without them is a profiling build you cannot read.
        set(soakFlags  -O2 -g -fno-omit-frame-pointer)
        set(soakLink   "")
        set(soakRun    run_rcu_soak_perf)
        # PERF RUNNER ONLY, both of them:
        #
        #   CROCOS_RCU_RELEASE_CHECKS compiles out the veneer's debug checks so
        #   this measures the release read path ("one store plus one fence")
        #   rather than the debug one every other harness build gets.
        #
        #   CROCOS_SOAK_POOLED swaps the test's new/delete for a per-thread free
        #   list. NEVER set it on a sanitizer runner: P3-DEC-001 turns on the
        #   deleter really calling free(), which is what makes a UAGP a hard trap
        #   instead of a silent read of recycled bytes.
        set(soakExtraDefs CROCOS_RCU_RELEASE_CHECKS CROCOS_SOAK_POOLED)
    endif()

    add_executable(${soakTarget} EXCLUDE_FROM_ALL ${RCU_SOAK_SOURCES})
    target_include_directories(${soakTarget} PRIVATE ${RCU_INCLUDES})
    target_compile_definitions(${soakTarget} PRIVATE ${RCU_DEFINES} ${soakExtraDefs})
    target_compile_options(${soakTarget} PRIVATE ${RCU_COMMON_FLAGS} ${soakFlags})
    target_link_libraries(${soakTarget} PRIVATE TestHarness)
    if(NOT soakLink STREQUAL "")
        target_link_options(${soakTarget} PRIVATE ${soakLink})
    endif()
    if(APPLE)
        target_link_options(${soakTarget} PRIVATE
            -Wl,-force_load,$<TARGET_FILE:TestHarness>)
        if(variant STREQUAL "TSan")
            target_link_options(${soakTarget} PRIVATE -Wl,-undefined,dynamic_lookup)
        endif()
    else()
        target_link_options(${soakTarget} PRIVATE -Wl,--keep-section=.crocos_unit_tests)
    endif()

    add_custom_target(${soakRun}
        USES_TERMINAL
        COMMAND ${CMAKE_COMMAND} -E env
            CROCOS_RCU_SOAK_SECONDS=${RCU_SOAK_SECONDS}
            CROCOS_RCU_SOAK_THREADS=${RCU_SOAK_THREADS}
            CROCOS_RCU_SOAK_QUIET_PCT=${RCU_SOAK_QUIET_PCT}
            $<TARGET_FILE:${soakTarget}> rcuSoak
        DEPENDS ${soakTarget}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "RCU endurance soak, ${variant}, ${RCU_SOAK_SECONDS}s x ${RCU_SOAK_THREADS} threads")
endforeach()
