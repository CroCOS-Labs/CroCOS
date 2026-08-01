---
kind: leaf
status: drafting
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 8 — Userspace integration test harness

> Stand up a userspace test harness that exercises the full `vmsmalloc` / `vmsfree`
> integration — magazine state machine, cross-domain gate, becameFull pop, eager-free,
> ABA-safety, lazy freshness — under both ASan/leak and TSan. The harness mocks
> `kernel::mm::VMSubstrate`, `kernel::CpuLocal` (per-thread storage), and
> `NUMAPolicy` (configurable topology) so the same `vmsmalloc.cpp` source file used
> in the kernel build links cleanly into a userspace test binary. **ARMv8 + TSan on
> the M1 dev machine is the release gate** (per the project's existing TSan-on-ARMv8
> default); the AMD64 Linux TSan path is the cross-check. Phase 8 builds on Phase 1's
> bookkeeper TSan + Phase 4's Treiber TSan, which exercise the lower-level primitives
> in isolation; Phase 8 exercises them composed under realistic alloc/free pressure.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No in-kernel stress test.** Phase 9 covers the in-kernel `naiveTest`-style
  exercise across all size classes. Phase 8 stays in userspace.
- **No new vmsmalloc behavior.** Phase 8 ships tests + mocks; the production
  `vmsmalloc.cpp` source compiled by the harness is the same source the kernel
  build uses. Any vmsmalloc-side change discovered necessary during Phase 8 work
  is a regression of Phases 1-7 and gets fixed there.
- **No exercising the kernel's actual VMSubstrate.** The harness mocks
  `allocPage` / `freePage` / `ensureTLBEntryFresh` / `reservePerDomainStaticBuffer`
  / `cpuLocalPageFor` etc. with userspace-friendly bodies that don't require
  page tables or interrupts.
- **No magazine tuning policy.** `currentK` / `maxChainLength` stays at
  `kInitialK` for the lifetime of the test; Phase 10 adds the policy logic.
  Phase 8 can artificially set `maxChainLength` to small values (e.g., 2) to
  drive specific code paths (flush, refill churn).
- **No RadixVM workload simulation.** Phase 8 exercises vmsmalloc as an
  allocator. RadixVM-specific stress patterns (the consumer's actual
  alloc/free sequencing) wait for RadixVM itself.
- **No fault injection.** Phase 8 does not artificially corrupt descriptor
  fields, simulate `allocPage` failures, or test out-of-memory paths.
  Negative tests for entry-point assertions live in Phase 7's
  `AssertionsTest.cpp`.
- **No coverage of the Phase-7 interrupt-context predicate.** That fires only
  when actual interrupts are nested; the userspace harness simulates kernel
  context by stubbing `kernel::interrupts::currentCpuInterruptDepths()` to
  return all-zeros. The "called in IRQ context" assertion never fires in
  userspace tests by design.

## Consumer Contract

### Harness layout (`tests/kernel/vmsmalloc/`)

```
tests/kernel/vmsmalloc/
├── CMakeLists.txt
├── mocks/
│   ├── MockVMSubstrate.h         (declares the mock API surface)
│   ├── MockVMSubstrate.cpp       (implementation: malloc-backed pages, mock dirty bitmap)
│   ├── MockCpuLocal.h            (thread_local CpuLocal storage + setup helpers)
│   ├── MockCpuLocal.cpp
│   ├── MockNUMAPolicy.h          (configurable topology)
│   └── MockNUMAPolicy.cpp
├── SlabLayoutTest.cpp            (Phase 2, existing)
├── AssertionsTest.cpp            (Phase 7, existing)
├── MakeStaticAssertsTest.cpp     (Phase 7, existing)
├── IntegrationTest.cpp           (Phase 8, single-threaded scenarios)
├── ConcurrentTest.cpp            (Phase 8, multi-thread + TSan-validated)
└── DebugIntrospection.h          (test-only accessors into vmsmalloc internals)
```

### Mock surfaces

**`MockVMSubstrate`** (replaces `kernel/include/mem/VMSubstrate.h`):

```cpp
namespace kernel::mm::VMSubstrate {
    // Page-allocation primitives. malloc-backed; returns 4 KiB-aligned page.
    void* allocPage();
    void  freePage(void* p);
    void* mapMMIOPage(kernel::mm::phys_addr) { /* unused in test, asserts */ }

    // Freshness: no-op in userspace (no TLB, no dirty bitmap). The call is
    // exercised to confirm the source-order positioning in vmsmalloc.cpp is
    // correct, but the body returns immediately.
    void ensureTLBEntryFresh(void*) noexcept {}

    // Static-buffer + arena-resident page primitives (Phase 3 surface).
    // Test setup pre-allocates buffers; these return them.
    void* reservePerDomainStaticBuffer(size_t byteSize, kernel::mm::DomainID d);
    void* cpuLocalPageFor(arch::ProcessorID i);

    // Arena virtual-base accessor (used by vmsmalloc for the VA range check).
    kernel::mm::virt_addr arenaVirtualBase(size_t index);

    // Test-only helpers (not exported from kernel/include/mem/VMSubstrate.h).
    namespace test {
        void initialize(size_t cpuCount, size_t domainCount);
        void shutdown();  // releases all malloc'd pages
        size_t activePageCount();  // live pages — for leak verification
    }
}
```

**`MockCpuLocal`** (replaces `kernel/include/cpu_local.h` + the arch backend):

```cpp
namespace arch {
    // Backed by thread_local in the userspace test.
    void  setCurrentCpuLocalBase(void* ptr) noexcept;
    void* getCurrentCpuLocalBase() noexcept;
}

namespace kernel {
    bool cpuLocalReady;  // test setup sets to true after mock-VMSubstrate init
    CpuLocal& cpuLocal() noexcept;  // reads thread_local pointer; asserts cpuLocalReady

    namespace test {
        // Each test thread calls this on entry to bind itself to a CpuLocal.
        void bindThreadToCpu(arch::ProcessorID i);
    }
}
```

**`MockNUMAPolicy`** (replaces `kernel/include/mem/NUMA.h`'s `NUMAPolicy`):

```cpp
namespace kernel::mm::NUMAPolicy {
    // Test-configurable topology: cpuCount, domainCount, cpu→domain mapping.
    DomainID domainFor(arch::ProcessorID i);
    size_t   domainCount();
    size_t   cpuCount();

    namespace test {
        // Set the topology once before any vmsmalloc call.
        void configure(size_t cpus, size_t domains,
                       function<DomainID(arch::ProcessorID)> mapping);
    }
}
```

### Debug introspection (`DebugIntrospection.h`)

Test-only accessors that read vmsmalloc's anonymous-namespace state:

```cpp
namespace kernel::mm::VMSubstrate::test {
    // Snapshot of a per-CPU magazine for class c.
    struct MagazineSnapshot { vmsmalloc::SlabDescriptorBase* head; uint32_t depth; };
    MagazineSnapshot magazineSnapshot(arch::ProcessorID i, size_t c);

    // Snapshot of a per-domain shared stack for class c.
    struct PartialStackSnapshot { vmsmalloc::SlabDescriptorBase* topHead; uint32_t topDepth; size_t chainCount; };
    PartialStackSnapshot partialStackSnapshot(kernel::mm::DomainID d, size_t c);

    // Snapshot of tuning counters.
    struct TuningSnapshot { uint32_t currentK, overflowCount, starvationCount; };
    TuningSnapshot tuningSnapshot(kernel::mm::DomainID d, size_t c);

    // Force-set maxChainLength for testing flush behavior.
    void setMaxChainLength(kernel::mm::DomainID d, size_t c, uint32_t k);
}
```

Exposed via a single `#ifdef CROCOS_VMSMALLOC_TEST_HARNESS` block in
`kernel/mm/vmsmalloc.cpp` that adds these accessor function bodies as friends
of the anonymous namespace. The accessors are *not* compiled into the kernel
build.

### Tests (the actual exercises)

**`IntegrationTest.cpp`** (single-threaded scenarios):

1. `alloc_free_alloc_returns_same_slot`: one class, alloc → free → alloc, assert same address.
2. `fill_slab_then_one_more_allocates_fresh_slab`: alloc N times (N = slotCount(c)) → assert all in one slab; alloc N+1 → assert in a new slab.
3. `becameFull_pop_advances_magazine_head`: fill m.head; alloc the becameFull slot; assert m.head now points to the next chain element (or null if singleton).
4. `eager_free_walks_empty_heads`: pre-build a chain in the shared stack with the first slab Empty and others Partial; pop refills the magazine; the first iteration's eager-free runs `freePage` on the Empty head.
5. `dec_029_whole_page_bypass`: `vmsmalloc(1024)` returns a page-aligned pointer; `vmsfree(p)` calls the mock's `freePage`.
6. `same_domain_free_extends_magazine`: alloc → fill slab to Full → another alloc creates new slab — old becomes orphaned-Full; free a slot in the orphaned slab → assert it's pushed onto local magazine.
7. `flush_at_max_chain_length`: set `maxChainLength = 2`; drive multiple `becameAvailable` pushes; on the 2nd push, magazine flushes via `pushChain`; assert shared stack has a depth-2 chain.
8. `refill_pop_walks_chain`: shared stack has a depth-3 chain; magazine empty; one allocation triggers refill; assert m.depth == 3 after.
9. `dec_036_eager_free_floor`: chain of depth 1 with the head Empty; eager-free does NOT fire (floor of 1).
10. `dec_039_pre_read_on_becameFull`: instrument the test to interleave allocSlot returning becameFull with a hypothetical same-domain freer's chainNext write. Single-threaded with a hook that fires between the pre-read and the allocSlot call — confirms the pre-read snapshot is what gets used.

**`ConcurrentTest.cpp`** (multi-thread + TSan-validated):

11. `concurrent_alloc_same_class_single_domain`: N threads alloc M times each on class c; each thread's allocations are unique; total count == N*M; no descriptor corruption.
12. `concurrent_alloc_free_balanced`: N threads alternate alloc/free; final state has same number of live allocations as the test driver tracks; no double-free, no leaks.
13. `cross_domain_free_routes_to_home`: thread A (domain 0) allocs; thread B (domain 1) frees. Snapshot confirms the slab landed on domain 0's `partialFor` stack, not domain 1's. Magazine on B is unchanged.
14. `tsan_dec_039_same_domain_race`: stress driver designed to maximize the same-domain becameFull race window. Owner CPU alloc'ing slab S to Full; same-domain freer immediately freeing a slot in S to drive becameAvailable. Many iterations under TSan. Pass = TSan-clean.
15. `tsan_dec_040_freshness_at_first_touch`: drive a slab through { allocator owns → flushed → re-popped → fields read }. Confirms the freshness call site fires (in userspace it's a no-op but the call is exercised, and TSan validates the surrounding acquire ordering).
16. `tsan_treiber_aba`: rapid push-pop-push of same chain head while another thread pops in between. Builds on Phase-4's TreiberStack ABA stress, but exercised through vmsmalloc's actual `partial[d][c]`.
17. `tsan_flush_collision`: two same-domain threads flush concurrently. CAS retries bump `overflowCount`. No corruption.
18. `tsan_cross_domain_push_extending_top`: cross-domain singleton frees hammer one home-domain stack; the `push(element)` extend-or-singleton logic should produce a chain whose depth grows up to `maxChainLength`, then a new singleton.
19. `stress_long_running_balanced`: 4 threads × 1M ops each, alternating alloc/free across all 8 classes. Compares the multiset of allocated addresses to the multiset of freed addresses at end (must match).
20. `stress_long_running_unbalanced`: 4 producer threads, 4 consumer threads, queue-based hand-off, sustained for 10M ops. Validates no corruption under sustained pressure.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phases 1–7 + Phase 4.5 — vmsmalloc source code | The harness compiles `kernel/mm/vmsmalloc.cpp` (and the other vmsmalloc TUs) against mocked headers. Phases must all be merged. | Yes — all prior phases. |
| Phase 1 TSan runner (`LibAllocTestRunnerTSan`) | Precedent for the parallel TSan binary pattern; Phase 8 mirrors the CMakeLists setup. | Yes — Phase 1 output. |
| Phase 4 TSan runner (`CoreTestRunnerTSan`) | Same precedent for the concurrent-stress + named memory-ordering pattern. | Yes — Phase 4 output. |
| `tests/harness/TestHarness.h` | Test registration macros, `EXPECT_*` family, assertion-trap exception (P1-ITEM-002). | Yes — live. |
| C++ standard library `<thread>`, `<atomic>`, `<chrono>`, `<unordered_set>` | Concurrent stress driver. | Yes — host stdlib. |
| Apple Clang TSan / ARMv8 LSE atomics on the M1 dev machine | Default release gate per `[[project_armv8_dev_tsan]]`. | Yes — live. |
| `kernel/mm/vmsmalloc.cpp` (and its includes) | Source under test. Phase 8 adds `#ifdef CROCOS_VMSMALLOC_TEST_HARNESS` blocks for the debug-introspection accessors. | Yes — Phases 5/6/7 output. |

## Invariants

- **The harness compiles the same `vmsmalloc.cpp` source the kernel uses.** No
  fork, no separate "test version" of the allocator. Includes resolve to mock
  headers via the build system's include-path ordering.
- **Mocks are state-resettable.** `MockVMSubstrate::test::shutdown()` releases
  every allocated page; subsequent `initialize()` starts clean. Each test
  binary run starts with no pre-existing state.
- **Per-thread CpuLocal isolation.** Each `std::thread` in a concurrent test
  binds to its own `CpuLocal` via `kernel::test::bindThreadToCpu(i)` at
  thread start. `cpuLocal()` on that thread returns its bound CpuLocal.
- **TSan-clean is the release gate.** A test failure under TSan blocks
  merging — even if the ASan/leak run passes. ARMv8 (M1) is the canonical
  TSan target; AMD64 Linux TSan is the cross-check.
- **Mock `ensureTLBEntryFresh` is a no-op.** Userspace has no TLB / dirty
  bitmap. The call site in vmsmalloc.cpp is exercised but the body returns
  immediately. The harness does NOT validate the freshness invariant
  directly — that's a property of the real VMSubstrate that's tested
  separately. What Phase 8 validates is that vmsmalloc places the call
  correctly in source order (e.g., before reading any descriptor field), via
  the source-code review during merge.
- **Magazine state introspection is read-only in tests.** `DebugIntrospection`
  snapshots are taken when the system is quiescent (after thread joins, or
  between operations on a single-threaded test). Reading mid-operation is
  not synchronized.
- **Address space contiguity assumption.** The DEC-015 packed-tagged-head
  encoding requires the VMSubstrate VA range to fit in a single PDPT
  (512 GiB on AMD64). In userspace, `MockVMSubstrate::allocPage` returns
  pages from `malloc`, which can land anywhere. The harness pre-allocates a
  contiguous 512 GiB-aligned region at test setup (using `mmap` with `MAP_ANONYMOUS`
  + `MAP_NORESERVE` + alignment) and serves pages from within it, so the
  encoding's bit budget is honored. P8-DEC-001 pins this.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| TSan reports a race | Test fails; merge blocked. The race location identifies whether vmsmalloc, Phase 4's Treiber, Phase 1's SplitBitmap, or the harness itself is at fault — adjudication during fix. | No (must be fixed) |
| ASan/leak reports a leak | Test fails. Likely cause: a `vmsmalloc` call that didn't get a matching `vmsfree`, or a mock that didn't clean up. | No (must be fixed) |
| Concurrent stress's alloc-set vs. free-set mismatch | Test fails. Indicates a vmsmalloc bug (double-free, corruption, or path mis-routing). | No |
| Mock `MockVMSubstrate::allocPage` runs out of preallocated VA | Test fails with "out of mock-VMSubstrate VA". Increase the preallocated region in P8-DEC-001 or fix a leak in vmsmalloc. | No (configure mock or fix bug) |
| TSan flags a benign race that's actually correct under DEC-042's memory model | False positive; document with a TSan suppression file entry citing the relevant DEC-042 item. Suppression entries are reviewed during PR. | Yes (suppress with justification) |
| ARMv8 TSan path passes but AMD64 TSan path fails (or vice versa) | Architecture-specific bug. Investigate the failing path — TSan's diagnostics include the access instructions. | No (must be fixed) |
| Boot smoke tests fail in the kernel build after Phase-8-introduced `#ifdef`s in `vmsmalloc.cpp` | Indicates the introspection accessors leaked into the non-test build. Audit the `#ifdef` placements. | No (must be fixed) |
| Test takes longer than ~30 seconds to complete | Acceptable for the stress variants (#19/#20). For unit-style tests, indicates either bad-time-complexity in the implementation or an excessive workload — investigate. | Maybe (depends on test) |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P8-ITEM-001 | Resolved 2026-05-27 (use real bookkeeper) | | | Should the harness mock `LibAlloc::SlabBookkeeper`? | Resolved: no. The bookkeeper is exhaustively tested in Phase 1 (with its own TSan runner). Phase 8 exercises vmsmalloc *as composed*; mocking the bookkeeper would defeat that integration goal. |
| P8-ITEM-002 | Resolved 2026-05-27 (thread_local + explicit bind) | | | How does the harness simulate "calling CPU"? | Resolved: `thread_local` storage for the CpuLocal pointer, with `kernel::test::bindThreadToCpu(i)` setting it once at thread start. Each `std::thread` models one CroCOS CPU. The existing stress-test mocks at `stress_tests/StressMocks.cpp` offer borrowable patterns for klog stubs and the mutex-protected `std::thread::id → ProcessorID` map (if explicit binding ever proves too rigid). |
| P8-ITEM-003 | Resolved 2026-05-27 (ARMv8 primary; AMD64 deferred as adjudication tool) | | | TSan-on-M1 vs. TSan-on-AMD64 differences? | Resolved: **ARMv8 M1 TSan is the sole primary release gate.** AMD64 TSan is a cross-check tool used only when investigating an ambiguous TSan flag, not a routine release gate. Two AMD64 options (priority order): **(a) Rosetta 2 cross-compile** (`clang -arch x86_64 -fsanitize=thread`, run under Rosetta 2 on the M1) — cheap if it works; Rosetta preserves x86 TSO semantics by inserting fences during ARMv8 translation. **(b) SSH-accessible Linux box (Surface Pro)** — user-owned, can be spun up when needed. Memory-model rationale: ARMv8's weak ordering is a strict superset of AMD64's TSO; if code is race-free under ARMv8 TSan, it's race-free under AMD64 TSan modulo toolchain differences. AMD64 TSan's real value is adjudicating false positives, not catching new bugs. Validate option (a) viability at step 1 of Phase 8 implementation. |
| P8-ITEM-004 | Resolved 2026-05-27 (random by default, fixed via flag) | | | RNG seed default: fixed or random? | Resolved: random by default — the seed is generated at test start and logged via klog at the very first line of each test run, so any failure is reproducible by re-running with `--random-seed=<value>`. Optional `--random-seed=N` flag sets a fixed seed for reproducing a specific failure or for deterministic CI baseline. CI logs preserve the seed. |
| P8-ITEM-005 | Resolved 2026-05-27 (drop the 512 GiB requirement) | | | Should the harness specifically verify DEC-015's single-PDPT constraint? | Resolved: no. The harness allocates a moderately-sized aligned region (tens to hundreds of MiB — sufficient for the test corpus). The implementation's runtime check at `vmsmallocInit` is the canonical authority for the DEC-015 bit budget; Phase 9 (in-kernel stress) is where the encoding's address math is validated against real kernel VAs. Phase 8 stays memory-layout-agnostic, which encourages cleaner vmsmalloc abstractions. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P8-DEC-001 | Settled (amended 2026-05-27 — smaller region, memory-layout-agnostic) | **MockVMSubstrate pre-allocates a modest page-aligned `mmap` region (`MAP_ANONYMOUS \| MAP_NORESERVE`) sized to the test corpus** — default ~64 MiB (16K pages), tunable per test. `allocPage` returns the next free page-aligned VA within the region; `freePage` marks it free (a simple bitmap suffices). `MAP_NORESERVE` defers physical backing until pages are touched, so the reservation is cheap. **The 512 GiB DEC-015 ceiling is NOT verified in userspace** (P8-ITEM-005 resolution) — the implementation's runtime check at `vmsmallocInit` is the canonical authority; the harness uses a region well under any plausible bit-budget ceiling and stays memory-layout-agnostic. | The 512 GiB-aligned region from the earlier draft was over-engineered for the test goal. A modest aligned region is enough to honor the encoding's actual needs (descriptors must lie within `[vmsBase, vmsBase + vmsSize)`, which the harness ensures by construction) without imposing precise layout requirements on the harness. The result is cleaner: vmsmalloc's encoding-policy abstraction is exercised end-to-end without the harness having to mimic kernel-VA-layout specifics. |
| P8-DEC-002 | Settled | **The same `kernel/mm/vmsmalloc.cpp` source is compiled by both the kernel build and the Phase-8 userspace test binary.** Distinguished by `#ifdef CROCOS_VMSMALLOC_TEST_HARNESS` for the introspection accessors only — production code paths are identical in both builds. Include-path ordering in CMake makes test includes resolve to `tests/kernel/vmsmalloc/mocks/` first. | A fork between the kernel and test source would defeat the integration-test goal. Conditional compilation for the introspection accessors only is the minimum necessary deviation. The mock headers shadow `kernel/include/mem/VMSubstrate.h` etc. via include-path ordering, which is the standard CMake pattern. |
| P8-DEC-003 | Settled | **Each `std::thread` binds to a `CpuLocal` via `kernel::test::bindThreadToCpu(i)` at thread start.** The binding writes the per-CPU `CpuLocal` pointer into the thread's TLS (backing `arch::setCurrentCpuLocalBase`). `cpuLocal()` returns the per-thread instance via `arch::getCurrentCpuLocalBase()`. The test driver may bind any number of `std::thread`s to any logical CPU IDs; topology is configurable via `MockNUMAPolicy::test::configure`. | Models the kernel's "one thread per CPU" execution model directly. `thread_local` storage in the mock provides the per-thread `CpuLocal` automatically. Test cases that need to simulate a CPU-pinned thread call `bindThreadToCpu` once at thread start and stay pinned for the thread's lifetime. |
| P8-DEC-004 | Settled | **Phase 8 introduces a parallel TSan test binary `KernelVmsmallocIntegrationTestRunnerTSan`** alongside the existing ASan/leak runner. Sanitizers are mutually exclusive; the TSan runner builds the same sources with `-fsanitize=thread -fsanitize=undefined` instead of `-fsanitize=address -fsanitize=leak`. Custom CMake target `run_vmsmalloc_integration_tests_tsan` runs it. Mirrors Phase 1's `LibAllocTestRunnerTSan` and Phase 4's `CoreTestRunnerTSan` patterns. | Established precedent. ARMv8 + TSan on M1 is the canonical release gate per the project's TSan-on-ARMv8 default; AMD64 Linux TSan is the cross-check (P8-ITEM-003 hazard handling). The ASan/leak run catches allocation-hygiene bugs; TSan catches concurrency bugs. Both are required to pass. |
| P8-DEC-005 | Settled | **Debug introspection (`DebugIntrospection.h`) is gated by `#ifdef CROCOS_VMSMALLOC_TEST_HARNESS` in `vmsmalloc.cpp`.** The introspection accessors are defined as friend functions of the anonymous-namespace symbols they access. The header itself is included only from test code; production builds compile vmsmalloc.cpp without the ifdef defined. | Keeps the introspection surface out of the kernel binary while giving tests structured read access to magazine/partial-stack/tuning state. A simpler "always expose accessors" alternative was considered and rejected because it would pollute the production symbol table with test-only entry points. |
| P8-DEC-006 | Settled | **The harness validates the alloc-set vs. free-set balance at test end, not on every operation.** Each producer/consumer thread maintains its own thread-local `unordered_set<void*>` of "currently allocated by me" pointers; on test completion all sets are joined and compared against the test driver's expected count. Per-operation invariant checks (e.g., "does the returned pointer pass the descriptor magic test?") are also done, but the multiset balance is the integration-level test. | Per-operation full-system checks would slow the stress test substantially (each `vmsmalloc` followed by a snapshot of every magazine + every shared stack is expensive). End-of-test balance is cheap and catches the most important integration bug (any imbalance indicates a corruption, double-free, or lost allocation). |
| P8-DEC-007 | Settled (added 2026-05-27) | **The harness uses the same DEC-015 packed-tagged-head encoding as production (27-bit offset + 37-bit counter).** The test region's smaller size (default ~64 MiB = 16K pages, fitting in 14 bits) leaves the upper offset bits unused-zero — no test-only encoding code path, no spec divergence from kernel behavior. The encoding is policy-parameterized in Phase 4; if a future need arises to tune the offset/counter split (e.g., a region size that wants more counter bits), the policy can be re-parameterized without changing call sites. | Per user direction 2026-05-27. Phase 8's value is testing production code paths under stress; a test-only encoding variant would weaken the integration-test claim. The smaller test region under-uses offset bits but the math works identically (unused bits are always zero). No code change needed beyond the smaller `vmsSize`. |

## Hazards

- **Mock VA region must be pre-faulted / committed-on-demand correctly.**
  `MAP_NORESERVE` lets us reserve 512 GiB cheaply, but the first touch of each
  page commits a real physical page. Under stress, the test process's
  resident-set size grows. Defense: the test driver tracks active page count
  via `MockVMSubstrate::test::activePageCount()` and asserts it stays under
  a configured ceiling. Tests that need >10K pages live use bounded loops
  with explicit `vmsfree` cleanup.
- **Apple Clang TSan false positives on 16-byte atomic ops** (Phase 4 Hazard
  carried forward). When investigating a TSan flag, first reproduce on
  Linux AMD64 TSan. If only ARMv8 fails, suspect the toolchain before the
  code; document via suppression file with justification.
- **`thread_local CpuLocal` lifetime.** The `thread_local` instance is
  destroyed at thread exit. If a `std::thread` is still pushing to a
  shared stack when its destructor runs, the next thread to read that
  shared-stack entry may observe a half-destroyed chain. Defense: all
  tests `.join()` worker threads before reading `DebugIntrospection`
  snapshots; no thread destroys its `CpuLocal` while another thread is
  reading the stack it published to.
- **DEC-015 single-PDPT encoding depends on the mock VA window staying
  within 512 GiB.** P8-DEC-001's `mmap` region provides this; a future
  refactor that splits the region across multiple `mmap` calls would
  violate the encoding. Static_assert at the mock's `initialize()`
  guards.
- **Phase 8's stress tests are time-sensitive.** A 1M-op stress can take
  10-30 seconds. CI runtime budget is bounded; if the test corpus grows
  too large, split into "quick" and "long" categories with the long set
  gated by `CTEST_LABEL` for opt-in nightly runs.
- **`MockVMSubstrate::ensureTLBEntryFresh` is a no-op.** The harness
  cannot validate the freshness invariant directly. P5-ITEM-002 already
  defers full freshness-cost validation to Phase 9 (in-kernel stress);
  Phase 8 covers source-position correctness via code review and
  exercise that the call sites compile / run without errors.
- **Concurrent stress drivers must not leak via thread cancellation.**
  Each thread completes its operation budget before joining; no thread
  is forcibly stopped mid-operation. Defense: the test driver uses a
  shared atomic "stop" flag that worker threads poll between operations,
  not between sub-steps of one operation.
- **TSan suppressions are PR-reviewed.** A suppression added during a
  Phase-8 development session must justify the suppression in a comment
  citing the relevant parent-spec DEC-042 item (or other authority).
  Suppressions accumulate over time; periodic audit needed.

## Verification Targets

| Property | Method |
|---|---|
| `IntegrationTest.cpp` single-threaded scenarios all pass under ASan/leak | `cmake --build build --target run_vmsmalloc_integration_tests` |
| `IntegrationTest.cpp` single-threaded scenarios all pass under TSan | `cmake --build build --target run_vmsmalloc_integration_tests_tsan` |
| `ConcurrentTest.cpp` multi-thread scenarios all pass under both ASan/leak and TSan | Same targets, full corpus |
| ARMv8 (M1) TSan pass: `run_vmsmalloc_integration_tests_tsan` exits 0 on the dev machine | `[[project_armv8_dev_tsan]]` — primary release gate |
| AMD64 TSan adjudication available (Rosetta cross-compile or Linux SSH box) | Used only when investigating ambiguous ARMv8 TSan flags; not a routine merge gate (P8-ITEM-003 amended 2026-05-27) |
| No leaks under ASan/leak (active page count == 0 at test shutdown) | `MockVMSubstrate::test::activePageCount() == 0` assertion at end of every test |
| Alloc-set vs free-set balance holds in concurrent stress | `unordered_set` multiset comparison at end of test #11, #12, #19, #20 |
| `DebugIntrospection` snapshots after each integration test match expected magazine / partial-stack state | Per-test assertions on `magazineSnapshot` / `partialStackSnapshot` |
| `#ifdef CROCOS_VMSMALLOC_TEST_HARNESS` leaks no symbols into the kernel build | Grep the kernel's compiled object file for any test-only symbol; expect none |
| Test corpus completes within the CI time budget (~5 min total for ASan + TSan runs) | Wall-clock measurement during CI |
| `naiveTest` regression: unchanged | `cmake --build cmake-build-debug --target run` continues to pass |

## Testing Approach

- **`IntegrationTest.cpp`** is the primary correctness gate — 10 single-threaded
  scenarios covering every Phase-5/6/7 code path. Each test sets up a fresh
  MockVMSubstrate, exercises one scenario, snapshots state, asserts. Runs
  under both ASan and TSan (identical results expected for single-threaded
  paths).
- **`ConcurrentTest.cpp`** is the concurrency-safety gate — 10 multi-thread
  scenarios. The DEC-039 / DEC-040 / DEC-041 / DEC-042 invariants are
  validated by TSan reporting zero races on the relevant code paths.
- **Stress tests #19 and #20** are the long-running endurance gates. Run
  on every CI invocation but with bounded operation counts (1M / 10M); a
  separate nightly variant could run 100M+ for deeper coverage if budget
  allows.
- **Reproducibility:** the RNG seed is **randomized by default** at test
  start (generated from `std::random_device` or system clock); the first
  klog line of each test run prints the seed verbatim so any failure is
  reproducible. `--random-seed=N` flag fixes the seed for re-running a
  specific failure or for deterministic CI baseline. CI logs preserve
  the seed.
- **ARMv8 + TSan on M1 is the sole primary release gate.** AMD64 TSan
  is a deferred / on-demand cross-check, not a routine merge requirement
  (P8-ITEM-003 amended). Rationale: ARMv8's weak memory model is a strict
  superset of AMD64's TSO; ARMv8 TSan catches strictly more bugs. AMD64
  TSan's value is adjudicating ambiguous flags (Apple Clang's occasional
  16-byte-atomic false positives, Phase 4 hazard) — used when we hit one,
  not preemptively.
- **AMD64 cross-check options** when needed: (a) **Rosetta 2** —
  `clang -arch x86_64 -fsanitize=thread` cross-compiled on the M1, run
  under Rosetta 2; cheap and local. Viability validated at step 1 of
  Phase 8 implementation. (b) **SSH-accessible Linux box** —
  user-maintained Surface Pro; spun up when needed.

## Implementation Phases

<!-- Concrete ordered steps for Phase 8 itself. -->

1. **Confirm starting state.**
   - Phases 1–7 + Phase 4.5 are merged. `kernel/mm/vmsmalloc.cpp` exists
     with `vmsmalloc` + `vmsfree` bodies.
   - Confirm Phase 1's `LibAllocTestRunnerTSan` and Phase 4's
     `CoreTestRunnerTSan` build and run on the M1 dev machine.
   - Confirm `tests/harness/TestHarness.h` exposes the assertion-trap
     exception mechanism (P1-ITEM-002 resolved).
   - **Validate Rosetta-AMD64-TSan viability** (P8-ITEM-003 resolution):
     `clang -arch x86_64 -fsanitize=thread -E -x c++ - < /dev/null` exits
     0; compile a trivial concurrent test (two threads incrementing a
     shared counter, one with a sync error) and run under Rosetta;
     confirm TSan reports the race. If yes, Rosetta is the AMD64
     cross-check path. If no, fall back to the SSH Linux box when
     adjudication is needed.

2. **Stand up the mock-VMSubstrate VA region.**
   - Create `tests/kernel/vmsmalloc/mocks/MockVMSubstrate.{h,cpp}`.
   - `initialize(cpuCount, domainCount)` mmaps a page-aligned anonymous
     `MAP_NORESERVE` region (default ~64 MiB, configurable per test);
     records `vmsBase` and `vmsSize`. Allocates per-domain static buffer
     pool (small fraction of the region) and the per-CPU CpuLocal pool.
   - `allocPage` / `freePage` use a simple bitmap over the region's
     allocatable subrange (excluding the static-buffer and per-CPU regions).
   - `ensureTLBEntryFresh` is a no-op (per P8 spec).
   - `shutdown()` munmaps the region and zeroes all state.
   - **Borrow patterns from `stress_tests/StressMocks.cpp` where relevant** —
     particularly the `klog()` stubs (`NullStream` / `StderrStream`) and the
     `kernel::print_stacktrace` no-op stubs. These are kernel-symbol
     dependencies that vmsmalloc.cpp's includes pull in transitively, and
     the existing stress mocks already solve them cleanly.

3. **Stand up MockCpuLocal + MockNUMAPolicy.**
   - `MockCpuLocal.{h,cpp}`: `thread_local void* cpuLocalBase = nullptr;`
     `arch::setCurrentCpuLocalBase(ptr) { cpuLocalBase = ptr; }`
     `arch::getCurrentCpuLocalBase() { return cpuLocalBase; }`
     `kernel::cpuLocalReady` flag. `kernel::test::bindThreadToCpu(i)`
     looks up the per-CPU `CpuLocal` page from `MockVMSubstrate` and
     installs the pointer.
   - `MockNUMAPolicy.{h,cpp}`: configurable topology with
     `test::configure(cpus, domains, mapping)`.

4. **Add the `DebugIntrospection` API to `vmsmalloc.cpp`.**
   - Wrap the new accessor definitions in `#ifdef CROCOS_VMSMALLOC_TEST_HARNESS`.
   - Each accessor is a function in the `kernel::mm::VMSubstrate::test`
     namespace that reads the anonymous-namespace state.
   - Add corresponding header at
     `tests/kernel/vmsmalloc/DebugIntrospection.h`.

5. **Write `IntegrationTest.cpp` with the 10 single-threaded scenarios.**
   - Each test: `MockVMSubstrate::test::initialize(1, 1);`
     `MockNUMAPolicy::test::configure(...)`; `bindThreadToCpu(0);`
     exercise the scenario; assert via `DebugIntrospection` snapshots and
     direct memory inspection. `MockVMSubstrate::test::shutdown();`
   - Follow the CLAUDE.md test-file convention: first two `#include`s are
     `"../test.h"` and `<harness/TestHarness.h>`.

6. **Write `ConcurrentTest.cpp` with the 10 multi-thread scenarios.**
   - Use `std::thread` for concurrent operations. Each thread calls
     `bindThreadToCpu(i)` at start.
   - The stress drivers (tests #19, #20) use producer-consumer queues
     for hand-off; a shared atomic stop flag for clean shutdown.

7. **Set up the CMake build.**
   - Add `tests/kernel/vmsmalloc/CMakeLists.txt` (extending the existing
     test-build pattern).
   - Two target binaries:
     - `KernelVmsmallocIntegrationTestRunner` (ASan + leak).
     - `KernelVmsmallocIntegrationTestRunnerTSan` (TSan + UBSan).
   - Both compile `kernel/mm/vmsmalloc.cpp` (and other vmsmalloc sources)
     against the mock headers via include-path ordering.
   - Custom targets `run_vmsmalloc_integration_tests` /
     `run_vmsmalloc_integration_tests_tsan`.

8. **Build, run both variants, iterate.**
   - `cmake --build build --target run_vmsmalloc_integration_tests` exits 0.
   - `cmake --build build --target run_vmsmalloc_integration_tests_tsan`
     exits 0 on the M1 dev machine.
   - If TSan reports a race, cross-check on AMD64 Linux TSan; if both
     report, fix vmsmalloc; if only ARMv8 reports, investigate
     suppression viability.

9. **Audit and document.**
   - Confirm the `#ifdef CROCOS_VMSMALLOC_TEST_HARNESS` doesn't leak symbols
     into the kernel build (grep object file).
   - TSan suppressions (if any) reviewed for justification.
   - Update `[[project_slab_abstraction_plan]]` memory: Phase 8 status →
     drafted / implemented.

10. **Optional follow-ups (under user latitude).**
    - Add a "long" stress variant that runs for 5 minutes; gated by a
      `CTEST_LABEL` for opt-in nightly CI.
    - Add a debug harness `printCounter` helper that dumps the per-CPU /
      per-domain counters periodically during a stress run, for diagnosing
      tuning policy regressions in Phase 10.
    - Integrate Phase 8's stress runner into the project's CI matrix
      (currently CroCOS's CI configuration is out of scope for this spec).

## References

- `kernel/mm/vmsmalloc.cpp` — Phases 5/6/7 output; Phase 8 compiles this
  against mock headers.
- `kernel/include/mem/VMSubstrate.h` — replaced by `MockVMSubstrate.h` in
  the test build.
- `kernel/include/cpu_local.h` — replaced by `MockCpuLocal.h` in the test
  build (Phase 4.5 output).
- `kernel/include/mem/NUMA.h` — replaced by `MockNUMAPolicy.h` in the test
  build.
- `kernel/include/interrupts/InterruptContextDepths.h` — the struct is
  used as-is; `currentCpuInterruptDepths()` returns a thread-local
  all-zeros instance in the harness.
- `tests/kernel/vmsmalloc/SlabLayoutTest.cpp` — Phase 2 output (existing,
  no change in Phase 8).
- `tests/kernel/vmsmalloc/AssertionsTest.cpp` — Phase 7 output (existing).
- `tests/kernel/vmsmalloc/MakeStaticAssertsTest.cpp` — Phase 7 output
  (existing).
- `tests/kernel/vmsmalloc/CMakeLists.txt` — extended by Phase 8 to add the
  two test runner binaries.
- `tests/harness/TestHarness.h` — test registration macros and
  assertion-trap mechanism.
- Phase 1 spec `specs/vmsmalloc-phase-1.md` — `LibAllocTestRunnerTSan`
  precedent for the parallel TSan binary pattern.
- Phase 4 spec `specs/vmsmalloc-phase-4.md` — `CoreTestRunnerTSan`
  precedent + DEC-042 memory-ordering rules that Phase 8 validates
  empirically.
- Parent spec `specs/vmsmalloc.md`:
  - DEC-015 — single-PDPT encoding constraint (Phase 8 honors via
    P8-DEC-001's `mmap` region).
  - DEC-034, DEC-037, DEC-039, DEC-040, DEC-041, DEC-042 — concurrency
    invariants that Phase 8 validates under TSan.
- Memory: `[[project_armv8_dev_tsan]]` — ARMv8 + TSan as the default
  release gate; Phase 8 inherits.
- Memory: `[[project_slab_abstraction_plan]]` — phase plan; updated on
  Phase 8 completion.
