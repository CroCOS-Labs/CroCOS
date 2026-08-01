---
kind: leaf
status: drafting
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 9 — In-kernel stress test

> Stand up an in-kernel stress harness that exercises `vmsmalloc` / `vmsfree` (and
> indirectly `make<T>` / `destroy<T>`) on real kernel VAs under SMP — the missing
> validation gap Phase 8's userspace harness left behind. Modeled on the existing
> `kernel::naiveTest` (which stresses `VMSubstrate::allocPage` / `freePage`): a
> per-CPU init routine at `smp_bringup` phase that allocates, writes content,
> reads back, and frees in an infinite loop. Phase 9 adds cross-domain free
> hand-off, all-size-class coverage including DEC-029 whole-page, content-
> integrity verification, and periodic klog liveness output. The stress is its own
> success signal — if it crashes, hangs, panics, or reports corruption, the
> integration failed. Phase 9 is **the canonical validator for DEC-015's
> packed-tagged-head address math against real kernel VAs** (P8-DEC-001 / P8-ITEM-005
> deferred this from userspace).

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No userspace tests.** Phase 8 covers the userspace integration harness;
  Phase 9 is in-kernel. The two are complementary — Phase 8 catches issues
  cheaply via TSan; Phase 9 validates the same code paths against real kernel
  memory, real interrupts, and the production VMSubstrate.
- **No new vmsmalloc behavior.** Phase 9 ships tests + driver; the production
  `vmsmalloc.cpp` is unchanged. Any issue surfaced is a regression of Phases
  1–8 and gets fixed there.
- **No bounded run duration.** Like `naiveTest`, the Phase-9 driver runs in
  an infinite loop. The test passes as long as the kernel stays alive without
  panic, hang, or corruption. CI runs are time-boxed externally (e.g., QEMU
  killed after N minutes).
- **No fault injection.** Phase 9 does not artificially corrupt descriptor
  fields, simulate `allocPage` failures, or test out-of-memory paths. Negative
  testing of entry-point assertions lives in Phase 7's `AssertionsTest.cpp`.
- **No microbenchmarking.** Phase 9 is correctness-focused. Latency /
  throughput measurement is deferred to Phase 10 (magazine tuning policy)
  where the numbers feed the policy's parameter choice.
- **No formal TSan integration.** TSan is a userspace tool; Phase 9 runs in
  the kernel. Race detection in-kernel relies on the design discipline
  established by DEC-042 plus Phase 8's TSan validation of the same code.

## Consumer Contract

### New init entry

```toml
# kernel/general.icd — append:

[VmsmallocStress]
name = "Vmsmalloc Stress Test"
required = false                        # off by default; enable via build flag
per_cpu = true
phase = "smp_bringup"
depends_on = ["VMSubstrateSlab"]
routine = "kernel::vmsmallocStress"
```

Default `required = false` means the stress doesn't run on every boot — only
when enabled via a build-system flag (e.g., `cmake -DCROCOS_VMSMALLOC_STRESS=ON`).
The existing `naiveTest` entry is unchanged; Phase 9 adds the new entry
alongside.

The entry depends on `VMSubstrateSlab` (the init routine that brings vmsmalloc
online). Note: `naiveTest` depends on `Shutdown`; Phase 9's stress and
`naiveTest` are siblings, both per-CPU routines that run forever — they share
the system's CPUs by having each CPU pick exactly one to run (gated by a
build-time flag).

### Stress driver (`kernel/KernelMain.cpp` or new `kernel/VmsmallocStress.cpp`)

```cpp
namespace kernel {

    // Per-CPU stress driver — runs forever on each CPU.
    [[noreturn]] bool vmsmallocStress();

}  // namespace kernel
```

Body (sketch — full implementation in step 4 of Implementation Phases):

```cpp
[[noreturn]] bool vmsmallocStress() {
    const auto myCpu = arch::getCurrentProcessorID();
    klog() << "vmsmallocStress: starting on CPU " << myCpu << "\n";

    // Per-class allocation tracking. Each CPU owns its own pool.
    static constexpr size_t kAllocsPerClass = 256;
    void* pools[kNumSizeClasses + 1 /* +1 for DEC-029 whole-page */]
               [kAllocsPerClass] = {};

    // Cross-domain free queue: another CPU may "hand off" a pointer
    // through here. Bounded SPSC-style ring per (donor → recipient) edge.
    static AtomicLinkedList<HandoffEntry> handoffInbox[kMaxCpus];

    uint64_t iteration = 0;
    while (true) {
        // 1. Drain handoff inbox: free anything other CPUs handed to us.
        while (auto* h = handoffInbox[myCpu].pop()) {
            verifyContent(h->ptr, h->expectedFill);
            vmsfree(h->ptr);
            // h is itself a vmsmalloc-allocated descriptor; free it after use.
        }

        // 2. Allocate fresh batch across all classes plus whole-page.
        for (size_t c = 0; c < kNumSizeClasses; c++) {
            const size_t size = kSlabSizeClasses[c];
            for (size_t i = 0; i < kAllocsPerClass; i++) {
                pools[c][i] = vmsmalloc(size);
                fillContent(pools[c][i], size, /*tag=*/myCpu, c, i, iteration);
            }
        }
        // DEC-029 whole-page allocations (size > 512 B):
        for (size_t i = 0; i < kAllocsPerClass; i++) {
            pools[kNumSizeClasses][i] = vmsmalloc(1024);
            fillContent(pools[kNumSizeClasses][i], 1024,
                        myCpu, kNumSizeClasses, i, iteration);
        }

        // 3. Verify content of every allocation before freeing
        //    (catches use-after-free / cross-allocation corruption).
        for (size_t c = 0; c <= kNumSizeClasses; c++) {
            const size_t size = (c < kNumSizeClasses)
                                ? kSlabSizeClasses[c] : 1024;
            for (size_t i = 0; i < kAllocsPerClass; i++) {
                verifyContent(pools[c][i], size, myCpu, c, i, iteration);
            }
        }

        // 4. Free locally OR hand off to another CPU (cross-domain stress).
        //    Hand off ~10% of allocations to drive the DEC-019 cross-domain
        //    gate on multi-NUMA configurations.
        for (size_t c = 0; c <= kNumSizeClasses; c++) {
            const size_t size = (c < kNumSizeClasses)
                                ? kSlabSizeClasses[c] : 1024;
            for (size_t i = 0; i < kAllocsPerClass; i++) {
                if (shouldHandoff(iteration, c, i)) {
                    const auto recipient = pickRecipientCpu(myCpu);
                    enqueueHandoff(handoffInbox[recipient], pools[c][i],
                                   /*expectedFill=*/...);
                } else {
                    vmsfree(pools[c][i]);
                }
                pools[c][i] = nullptr;
            }
        }

        // 5. Periodic liveness klog (every N iterations).
        if ((iteration & 0xFFFF) == 0) {
            klog() << "vmsmallocStress: CPU " << myCpu
                   << " iter=" << iteration << "\n";
        }
        iteration++;
    }
}
```

### Helper utilities

```cpp
namespace kernel::vmsmalloc::stress {

    // Fill the slot with a deterministic pattern that captures
    // (cpu, class, index, iteration). Catches:
    //   - cross-allocation corruption (some other allocation wrote to ours)
    //   - use-after-free (a freed slot still has its old pattern after realloc)
    //   - magazine state confusion (returned slot belongs to a different class)
    void fillContent(void* p, size_t size, uint32_t cpu,
                     uint32_t classIdx, uint32_t index, uint64_t iteration);

    // Verify the pattern. Asserts on mismatch.
    void verifyContent(void* p, size_t size, uint32_t cpu,
                       uint32_t classIdx, uint32_t index, uint64_t iteration);

    // Decide whether to hand off this allocation. ~10% rate; deterministic
    // function of (iteration, class, index) so behavior is reproducible.
    bool shouldHandoff(uint64_t iteration, size_t classIdx, size_t index);

    // Pick a recipient CPU for cross-domain hand-off. Prefers a CPU on a
    // *different* NUMA domain (to actually exercise the cross-domain gate
    // when running on `run_numa` / `run_numa_hmat` configurations).
    arch::ProcessorID pickRecipientCpu(arch::ProcessorID myCpu);

}  // namespace kernel::vmsmalloc::stress
```

### Build-system integration

- Add `CROCOS_VMSMALLOC_STRESS` CMake option (default OFF).
- When ON, `general.icd`'s `[VmsmallocStress]` entry's `required = true` is
  set (via a generator step that toggles the field), and the existing
  `[Test]` (naiveTest) entry's `required` is set to `false` (so the two
  don't race for CPU time — they're alternatives, not parallel).
- New CMake target `run_vmsmalloc_stress`: identical to `run` but with
  `CROCOS_VMSMALLOC_STRESS=ON`.
- New CMake target `run_vmsmalloc_stress_numa`: with `CROCOS_VMSMALLOC_STRESS=ON`
  on the `run_numa` topology (3 NUMA domains).

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phases 1–7 + Phase 4.5 — vmsmalloc fully online | The stress calls real `vmsmalloc` / `vmsfree`. All prior phases must be merged. | Yes — all prior phases. |
| `kernel/general.icd` | New `[VmsmallocStress]` init entry added by Phase 9. | Refactored in Phase 9. |
| `kernel/KernelMain.cpp` or new `kernel/VmsmallocStress.cpp` | Hosts the `vmsmallocStress` per-CPU routine and helper functions. | New file in Phase 9 (or extend existing). |
| `kernel::klog()` (existing kernel logging) | Periodic liveness output + diagnostic messages on assertion failure. | Yes — live. |
| `arch::getCurrentProcessorID()` | Identifies the running CPU in the per-CPU routine. (After Phase 4.5 this reads through `cpuLocal().logicalID`.) | Yes — live post-Phase-4.5. |
| `kernel::mm::NUMAPolicy::domainFor` | Used by `pickRecipientCpu` to find a cross-domain recipient. | Yes — live. |
| `Core::AtomicLinkedList<T>` | Lock-free SPSC inbox for cross-CPU hand-off. | Yes — live per CLAUDE.md's Core library inventory. |
| `make<T>` / `destroy<T>` (or raw `vmsmalloc` / `vmsfree`) | Phase 9 uses raw `vmsmalloc` / `vmsfree` because the content-pattern bookkeeping wants exact size control. A side variant could exercise `make<T>` for a representative type — possible follow-up. | Yes — live post-Phase-7. |
| `kernel::assert` / `klog` (existing panic + log) | Stress driver's content-mismatch checks invoke `assert`. | Yes — live. |
| QEMU run configurations (`run`, `run_numa`, `run_numa_hmat`) | The stress runs under each to validate single-domain and multi-domain behavior. | Yes — live. |

## Invariants

- **`vmsmallocStress` is a per-CPU init routine** registered at the
  `smp_bringup` phase. Each CPU runs its own instance of the body
  independently; no shared state between CPUs except the cross-domain
  hand-off inbox.
- **Content pattern catches corruption** — every allocated slot's bytes
  encode `(cpu, classIdx, index, iteration)`. After realloc / free /
  re-alloc cycles, any cross-allocation write or stale-content read
  triggers `verifyContent`'s assertion.
- **Cross-domain hand-off rate is bounded and reproducible** — `shouldHandoff`
  returns true ~10% of the time, deterministically from
  `(iteration, classIdx, index)`. Tunable via build-time constant.
- **The stress is its own success signal** — no explicit "test passed"
  output. If the kernel survives without panic for N minutes (CI
  externally bounds the run), the integration is verified.
- **No allocation outlives one iteration** — every allocation in iteration
  N is either freed by the same CPU before iteration N+1 begins, or
  handed off to another CPU's inbox (which frees on its next iteration's
  drain step).
- **Periodic liveness klog confirms forward progress** — every 64K
  iterations, each CPU emits `iter=<count>`. Absence of this output for
  > 30 seconds indicates a hang or deadlock.
- **Phase 9 is the canonical validator of DEC-015's address math against
  real kernel VAs** — the encoding's pack/unpack runs against actual
  VMSubstrate-arena VAs (not the userspace mmap region of Phase 8).
- **Mutual exclusion with `naiveTest`** — at most one of `[Test]` and
  `[VmsmallocStress]` has `required = true` on any given build; both
  routines are infinite-loop per-CPU and would otherwise compete for CPU
  time without coordination.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `verifyContent` mismatch | Assertion fires; kernel panics with the (cpu, class, index, iteration) at the failure site. Indicates cross-allocation corruption, use-after-free, or magazine state corruption. | No (must fix) |
| `vmsmalloc` returns null | Cannot happen — DEC-012 says vmsmalloc panics on `allocPage` failure rather than returning null. If observed, indicates a contract violation. | No (must fix) |
| Hang (no liveness klog for > 30s on any CPU) | External QEMU watchdog or CI timeout detects. Indicates deadlock — most likely a magazine-state bug that wedges the fast path or refill. | No (must fix) |
| Assertion fires in `vmsfree` (DEC-026 validation chain) | Indicates Phase 9 passed a corrupted pointer to `vmsfree`. Most likely a magazine state corruption that returned a non-slab pointer. Same diagnostic as content mismatch — magazine state corruption. | No (must fix) |
| Cross-domain inbox unbounded growth | If a CPU's hand-off rate to another CPU exceeds the recipient's drain rate, the inbox grows. The 10% hand-off rate per iteration plus the recipient draining at the start of each iteration is balanced by construction. If observed, indicates a bug in the recipient's drain step. | No (must fix) |
| Per-CPU pool ran out of stack / static space | `pools` is `static` (BSS-allocated), so each CPU's pool is shared global state — but Phase 9 declares the pool per-CPU via the `per_cpu = true` ICD attribute, which means each CPU's `vmsmallocStress` invocation has its own stack-local `pools` array. Stack size is bounded; if `pools` overflows the per-CPU init stack, the kernel's stack-guard fires. Mitigation: shrink `kAllocsPerClass` or move `pools` to a `static thread_local` (per-CPU via `cpuLocal()` if exposed). | No (must fix or tune) |
| `naiveTest` and `vmsmallocStress` both enabled | Cannot happen by construction — the build-system flag is mutually exclusive. If both `[Test]` and `[VmsmallocStress]` have `required = true`, the init registry should warn or the build should fail. (Audit during Phase 9 implementation.) | No (build config bug) |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P9-ITEM-001 | Resolved 2026-05-27 (mutually exclusive via build flag) | | | Should `vmsmallocStress` and `naiveTest` co-exist or be mutually exclusive? | Resolved: mutually exclusive via the `CROCOS_VMSMALLOC_STRESS` build flag. Both are infinite-loop per-CPU routines; co-existing would mean each only gets half the CPU time, muddying the test signal. If a future scenario needs both running simultaneously (e.g., vmsmalloc under PageAllocator pressure), they can be split across CPU subsets. |
| P9-ITEM-002 | Resolved 2026-05-27 (repeat pattern) | | | Repeat the content pattern across the slot or only at offset 0? | Resolved: repeat. Every 8-byte chunk of the slot holds the same packed value. Catches partial corruption (a wild write landing in the middle of a slot) at negligible cost. |
| P9-ITEM-003 | Resolved 2026-05-27 (no — wrappers don't need separate testing) | | | Should `vmsmallocStress` use `make<T>` / `destroy<T>` as well as raw `vmsmalloc` / `vmsfree`? | Resolved: no. `make<T>` / `destroy<T>` are very lightweight wrappers over `vmsmalloc` / `vmsfree` (the templates do placement-new, the static_assert, and the `SafePtr` construction — none of which add behavior the stress test would exercise productively). Raw `vmsmalloc` / `vmsfree` is the entire stress surface. |
| P9-ITEM-004 | Resolved 2026-05-27 (fall back to local free) | | | How does the cross-domain hand-off behave on single-CPU / single-domain QEMU? | Resolved: `pickRecipientCpu` returns the current CPU's logicalID when no cross-domain target is available, which degenerates to a local free. The cross-domain gate isn't exercised on `make run` (single domain); `make run_numa` and `make run_numa_hmat` exercise it. |
| P9-ITEM-005 | Resolved 2026-05-27 (per_cpu = true) | | | Should the stress run on the BSP only or on all CPUs? | Resolved: all CPUs, matching `naiveTest`'s `per_cpu = true` pattern. Maximizes SMP contention and exercises both same-domain and cross-domain code paths simultaneously. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P9-DEC-001 | Settled | **`vmsmallocStress` is registered as a separate ICD entry from `naiveTest`**, gated by `CROCOS_VMSMALLOC_STRESS` build flag (default OFF). The two are mutually exclusive — never both `required = true` simultaneously. | Sibling stress routines that compete for CPU time would muddle the test results. Build-time gating is the simplest mutex; future runs that want both can split the CPU set. The default-OFF behavior preserves `naiveTest` as the default boot stress, which is currently CroCOS's smoke test. |
| P9-DEC-002 | Settled | **Content pattern is `(cpu << 48) \| (classIdx << 40) \| (index << 24) \| (iteration & 0xFFFFFF)` packed into 8 B, repeated across the slot's `size` bytes.** Each 8-byte chunk of the slot carries the same value. `verifyContent` reads every 8 B and asserts equality. Slot sizes < 8 B (none in DEC-003 today, but reserved) would need a smaller pattern; current schema's smallest class is 8 B which fits exactly one pattern repetition. | Catches the widest class of corruption: cross-allocation writes (different (cpu, class) values), use-after-free (stale (iteration) value), magazine state confusion (slot returned for class A actually belongs to class B — pattern matches the wrong class). The 24-bit iteration counter wraps every 16M iterations — beyond any single stress test's run. Cost: O(size) writes/reads per slot; negligible. |
| P9-DEC-003 | Settled | **Cross-domain hand-off uses `Core::AtomicLinkedList<HandoffEntry>` per-recipient-CPU**, where `HandoffEntry` is itself a small struct allocated via `vmsmalloc` (recursive use of the allocator under stress). The recipient drains its inbox at the start of each iteration, freeing both the handed-off allocation AND the `HandoffEntry` descriptor. | Exercises both directions of the cross-domain gate: the donor's `vmsfree(ptr)` on a slab owned by the recipient's domain (cross-domain push), and the recipient's allocation/free of `HandoffEntry` descriptors (same-domain operations under cross-domain free pressure). Using `vmsmalloc` for the `HandoffEntry` is a small recursive-stress bonus. `AtomicLinkedList` is already in Core (per CLAUDE.md). |
| P9-DEC-004 | Settled | **Hand-off rate is ~10% of allocations, deterministically chosen by `(iteration, class, index)`.** The function: `((iteration * 31 + class * 17 + index * 7) % 100) < 10` — pseudo-random but reproducible. | 10% is high enough to drive measurable cross-domain push pressure on the home-domain stack but low enough to not dominate the local free path. Deterministic so re-running with the same iteration count reproduces the failure trace. Tunable via a constexpr if Phase 10 wants different ratios for measurement. |
| P9-DEC-005 | Settled | **Phase 9's per-CPU pool is a stack-local array** in `vmsmallocStress`'s body, sized to `(kNumSizeClasses + 1) * kAllocsPerClass * sizeof(void*)`. For `kNumSizeClasses = 8`, `kAllocsPerClass = 256`: 9 * 256 * 8 B = 18 KiB per-CPU stack. CroCOS's per-CPU init stack is well above this (verify at step 1). | Simpler than a separate per-CPU global. Stack-local means no concurrent-access concerns. 18 KiB per CPU is a one-time cost; bounded. If a future schema grows `kAllocsPerClass`, revisit. |
| P9-DEC-006 | Settled | **Periodic liveness klog every 65536 iterations** (`(iteration & 0xFFFF) == 0`). | Loud enough to detect hangs in tens of seconds (each iteration is many vmsmalloc / vmsfree calls; one iteration = ~10^4 ops, 64K iterations ≈ 6×10^8 ops ≈ several seconds on real hardware, less under QEMU). Quiet enough to not flood the serial console. |

## Hazards

- **Phase 9 stress depends on every prior phase being correct.** A failure
  in Phase 9 is hard to localize — could be Phase 1 (bookkeeper), Phase 4
  (Treiber stack), Phase 5/6 (magazine state machine), Phase 4.5 (CpuLocal),
  Phase 3 (arena metadata), Phase 7 (entry asserts). The diagnostic
  message at content-mismatch should include enough context (cpu, class,
  index, iteration, pointer value) to bisect. Phase 8's userspace harness
  is a much faster bisection tool than Phase 9 for narrowing down the
  failure to a specific Phase.
- **Cross-domain hand-off depends on multi-NUMA topology.** On single-CPU
  QEMU (`make run`), `pickRecipientCpu` always returns the current CPU,
  meaning hand-off degenerates to local free. The cross-domain gate is
  *not exercised* in single-domain configurations. `make run_numa` /
  `make run_numa_hmat` are required to exercise that path.
- **Stack-local `pools` array growth.** 18 KiB per CPU is fine today, but
  if Phase 10's tuning policy or future RadixVM workload measurements
  motivate `kAllocsPerClass = 1024` or `kNumSizeClasses = 16`, the array
  becomes 128+ KiB and likely overflows the per-CPU init stack. Defense:
  static_assert `sizeof(pools) <= 32 KiB` and revisit if it bumps.
- **`AtomicLinkedList` for the inbox** is multi-producer single-consumer
  semantics (multiple CPUs hand off; one CPU drains). Confirm `Core::AtomicLinkedList`
  supports this concurrency model at step 1; if it's SPSC-only, switch to
  a Treiber stack inbox.
- **Hand-off iteration deterministic but architecture-time-sensitive.**
  The pseudo-random hand-off function uses iteration count, which depends
  on per-CPU execution speed — different CPUs reach different iteration
  counts at different wall-clock times. Cross-CPU hand-off timing is
  therefore non-deterministic, even though each CPU's own hand-off
  pattern is reproducible. Acceptable for stress testing; a fully
  reproducible scenario would need synchronized iteration counts (out of
  scope).
- **`vmsmalloc` inside the stress loop must not panic.** A panic crashes
  the kernel and the stress run terminates with no useful state. The
  stress's content-mismatch assertions panic deliberately; any unexpected
  panic during the stress is a bug. Capture the panic message + serial
  log for diagnosis.
- **DEC-015 address-math validation depends on the encoding's offset
  range fitting the actual VMSubstrate VA window.** Phase 9 is where this
  invariant is empirically validated (Phase 8 deferred it). If the
  in-kernel boot trips the `vmsmallocInit` runtime check that the
  VMSubstrate window fits in the encoded offset bits, the stress is
  unrunnable — fix the encoding or the VA window.
- **`naiveTest` mutual exclusion is a build-system invariant.** A
  mis-configured CI run with both enabled would result in two infinite
  loops competing for each CPU's init slot — likely the first-registered
  one wins and the second never runs. Audit the init-registry generator
  at step 1.

## Verification Targets

| Property | Method |
|---|---|
| `make run_vmsmalloc_stress` boots and runs for N minutes without panic / hang | QEMU run + serial-log inspection |
| `make run_vmsmalloc_stress_numa` boots and runs for N minutes; cross-domain hand-off klog confirms domain edges are exercised | QEMU + serial-log inspection; debug klog at hand-off site (release builds suppress) |
| `make run_vmsmalloc_stress_numa_hmat` boots and runs N minutes | Same |
| Periodic liveness klog appears at expected cadence (~every few seconds per CPU) | Serial-log inspection |
| Content-pattern verification passes on every allocation across N minutes | Stress assertion silence — no `kassert` fires from `verifyContent` |
| `make run` (default config) unchanged — `naiveTest` continues to pass | `cmake --build cmake-build-debug --target run` boots; `naiveTest` runs |
| Build-system mutex: `CROCOS_VMSMALLOC_STRESS=ON` flips `[VmsmallocStress]` to `required = true` and `[Test]` to `required = false` | Inspect generated init-registry source |
| `vmsmallocStress` symbol is present only when the flag is on (or always present but only registered when required) | Inspect kernel binary's symbol table |

## Testing Approach

- **Primary signal: in-kernel boot in QEMU under `CROCOS_VMSMALLOC_STRESS=ON`.**
  The stress runs forever; CI bounds the run to N minutes (e.g., 5 min)
  via external `timeout` or QEMU's own time-limit mechanism.
- **Three QEMU configurations:** `run`, `run_numa`, `run_numa_hmat` — all
  must boot and run for the full N minutes without failure. `run_numa` /
  `run_numa_hmat` exercise the cross-domain gate; `run` exercises the
  same-domain path only.
- **Liveness klog cadence** confirms forward progress. Absence of klog
  output for > 30s on any CPU indicates a hang; the test fails by timeout.
- **No formal pass/fail output** — the absence of assertions and panics IS
  the pass signal. CI parses the serial log for "PANIC" or "kassert" lines;
  presence == fail.
- **Manual bisection on failure:** if Phase 9 fails after Phase 1–8 passed
  individually, suspect interaction between phases. Drop hand-off rate to
  0% to isolate to same-domain bugs; drop multi-class to single-class to
  isolate magazine state issues; reduce CPU count to isolate cross-CPU
  races.

## Implementation Phases

<!-- Concrete ordered steps for Phase 9 itself. -->

1. **Confirm starting state.**
   - Phases 1–8 + Phase 4.5 are merged. `vmsmalloc` / `vmsfree` work in the
     kernel boot (Phase 5/6 boot smoke succeeds).
   - Confirm `kernel::naiveTest` at `KernelMain.cpp:54` is the existing
     stress-test pattern Phase 9 emulates.
   - Confirm `kernel/general.icd` schema for `[Section]` entries and the
     `per_cpu = true` / `required = true` / `phase = "smp_bringup"` attributes.
   - Confirm `Core::AtomicLinkedList<T>` supports multi-producer
     single-consumer (the hand-off inbox use case). If SPSC only, plan a
     Treiber-stack-based alternative.

2. **Add the CMake option + ICD generator hook.**
   - `CMakeLists.txt`: `option(CROCOS_VMSMALLOC_STRESS "Run vmsmalloc stress
     test instead of naiveTest" OFF)`.
   - When ON, the `gen_init_registry.py` step receives a flag (e.g., via
     command-line argument) that flips `[Test]` to `required = false` and
     `[VmsmallocStress]` to `required = true`.
   - Add CMake targets `run_vmsmalloc_stress`, `run_vmsmalloc_stress_numa`,
     `run_vmsmalloc_stress_numa_hmat` — all set `CROCOS_VMSMALLOC_STRESS=ON`
     and pass through to QEMU.

3. **Add `[VmsmallocStress]` entry to `kernel/general.icd`.**
   - Per the Consumer Contract section. Default `required = false`.

4. **Create `kernel/VmsmallocStress.cpp`** (new TU) or extend
   `kernel/KernelMain.cpp`.
   - Implement `vmsmallocStress` per the body sketch in the Consumer
     Contract.
   - Implement `fillContent`, `verifyContent`, `shouldHandoff`,
     `pickRecipientCpu`, `enqueueHandoff` helpers.
   - Implement the per-recipient `AtomicLinkedList<HandoffEntry>` inbox
     storage (static-global per CPU).

5. **Validate on single-CPU `make run_vmsmalloc_stress`.**
   - Kernel boots; the BSP's `vmsmallocStress` starts; liveness klog
     appears at expected cadence; no panics.
   - Run for 5 minutes; CI-style "no failures" pass.

6. **Validate on multi-domain `make run_vmsmalloc_stress_numa` and `_numa_hmat`.**
   - All CPUs start their stress instance; cross-domain hand-off klog
     confirms cross-edge exercise; no panics over 5 minutes.

7. **Audit and document.**
   - Confirm `make run` (default config, `naiveTest`) unchanged.
   - Confirm `CROCOS_VMSMALLOC_STRESS=OFF` produces a kernel binary
     identical (modulo build-flag difference) to the pre-Phase-9 binary.
   - Update `[[project_slab_abstraction_plan]]` memory: Phase 9 status →
     drafted / implemented.

8. **Optional follow-ups (under user latitude).**
   - Add latency counters per class (Phase 10 will use these as input to
     the tuning policy).
   - Add a `vmsmallocStressShutdownAfter(N seconds)` variant for bounded
     CI runs that exit cleanly.
   - Once Phase 10 lands, add a per-domain `currentK` snapshot to the
     liveness klog to confirm the tuning policy is reacting.

## References

- `kernel/KernelMain.cpp:54` — `naiveTest` template Phase 9 emulates.
- `kernel/general.icd:83-90` — existing `[Test]` entry as the ICD pattern.
- `kernel/include/mem/VMSubstrate.h` — `vmsmalloc` / `vmsfree` /
  `make<T>` / `destroy<T>` public surface (Phase 7 amended).
- `kernel/mm/vmsmalloc.cpp` — production source under stress.
- `kernel/mm/VMSubstrateSlab.h` — `kSlabSizeClasses`, `kNumSizeClasses`
  (Phase 2 output).
- `libraries/Core/include/core/atomic/AtomicLinkedList.h` —
  multi-producer single-consumer inbox primitive (per CLAUDE.md Core
  inventory).
- `kernel/include/arch/amd64/amd64.h` — `arch::getCurrentProcessorID()`
  (post-Phase-4.5 reads through `cpuLocal().logicalID`).
- `kernel/include/mem/NUMA.h` — `NUMAPolicy::domainFor` for
  cross-domain recipient selection.
- Parent spec `specs/vmsmalloc.md`:
  - DEC-012 — `allocPage` failure panics (no null return) — Phase 9 relies
    on this.
  - DEC-014 amended (ITEM-052) — stress runs in process / kernel-thread
    context; #PF reentry is conditionally legal but Phase 9 doesn't
    deliberately trigger it.
  - DEC-015 — packed-tagged-head encoding; Phase 9 is the canonical
    validator for the address-math invariant against real kernel VAs.
  - DEC-018 — slab home-domain recording; Phase 9 verifies via
    cross-domain hand-off behavior.
  - DEC-019 / DEC-034 — cross-domain gate; Phase 9 drives the gate via
    deterministic 10% hand-off rate.
  - DEC-029 — whole-page allocations included in Phase 9's class corpus.
- Phase 8 spec `specs/vmsmalloc-phase-8.md` — userspace harness; Phase 8
  catches most issues cheaply via TSan, Phase 9 validates the same code
  against real kernel memory + interrupts + production VMSubstrate.
- Memory: `[[project_slab_abstraction_plan]]` — phase plan; updated on
  Phase 9 completion.
