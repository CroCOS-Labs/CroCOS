---
kind: leaf
status: complete
parent: specs/rcu.md
---

# RCU Phase 3 — Torture Suite

> An rcutorture-shaped stress harness for the reclamation framework, using ASan-with-real-`free`
> as a use-after-grace-period oracle and ARMv8 TSan as the release gate.

## Non-Goals

- **Not an in-kernel stress.** Real vmsmalloc nodes, real `SafePtr` freshness, and real
  interrupt-context asserts across 8 QEMU CPUs are the job of a later phase, planned separately
  once the framework is proven. This phase is userspace only.
- **Not a RadixVM workload simulation.** The structure under torture is a deliberately simple
  array of versioned cells, not a radix tree. Consumer-shaped stress waits for the consumer.
- **Not a performance benchmark.** No throughput or latency numbers are gates here. Tuning
  (parent ITEM-005) waits for a representative workload.
- **Not a formal proof.** herd7 litmus files, if parent ITEM-002 resolves that way, are a Phase-1
  deliverable — TSan exercises fence pairings but does not prove them.

## Consumer Contract

This phase ships tests, not API. Its deliverable is a release gate: **the framework is not
considered usable by any consumer until this suite is green under both runners on ARMv8.**

**Outcome (2026-08-01).** `tests/kernel/rcu/TortureTest.cpp`, 14 scenarios. Gate green:
48/48 RCU (ASan+leak and TSan) and 441/441 Core (ASan and TSan) on ARMv8; kernel cross build
clean. The suite earned its keep on the first run by finding a real Phase-1 defect — see
P1-DEC-017: `drainAllQuiescent` claimed a slot's `openBagIndex` bag without moving the cursor,
so a deleter-retire (legal per RCU-DEC-038, and the shape RadixVM's node deleters have) tripped
`prepareOpenBag`'s `Free`-or-`Open` assert deterministically. It also caught two bugs in its own
scenarios, one of them via the ASan oracle doing exactly its job on a pointer read after
`barrier` had freed it.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 1 engine + `DebugIntrospection.h` | Subject under test; internals visibility | Yes |
| Phase 2 veneer + `tests/kernel/rcu/mocks/` | Harness scaffolding, `bindThreadToCpu`, mock clock | Yes |
| `TestHarness.h` — `TEST_WITH_TIMEOUT_NO_TRACKING`, `EXPECT_ASSERT_FAILURE` | Test registration and timeouts | Yes |
| Homebrew clang ≥ 22.x on macOS 26 | Working ASan/TSan on ARMv8 | Yes |

## Invariants

- **P3-I1.** In the ASan runner the terminal deleter calls the real `free()`, so any UAGP is a
  hard ASan trap rather than a silent read of poisoned bytes. This is the whole point of the
  phase and must not be weakened for convenience.
- **P3-I2.** One `std::thread` per logical CPU, each bound via `bindThreadToCpu`, so each owns a
  distinct slot. Phase 1's I5 (non-atomic `nesting`) depends on this and TSan will report it
  immediately if violated.
- **P3-I3.** Every torture scenario terminates deterministically. No test relies on a race
  happening; races are *induced* by injected stalls where the window matters.
- **P3-I4.** Both runners execute the same scenarios. They differ only in sanitizer.
- **P3-I5 (RCU-DEC-034/035; failure path added 2026-08-01).** Every *passing* scenario tears its
  domain down through the contract: join workers (the join is the happens-before edge
  RCU-DEC-035's corrected precondition requires), `drainAllQuiescent()`, then destroy. The
  destructor's quiescence assert and the harness leak tracker are two independent detectors of a
  broken teardown; neither may be suppressed to make a scenario pass. **A *failed* scenario skips
  teardown entirely and deliberately leaks its heap-allocated domain** (tracker-suppressed):
  running `drainAllQuiescent`/`~EpochDomain` on a domain wrecked mid-scenario (active slot,
  `Claimed` bag) throws from the epilogue — and a throwing assert escaping a `noexcept`
  destructor during unwinding is `std::terminate`, masking the summary, the exact failure mode
  P3-DEC-004 exists to prevent. Domains are therefore heap-allocated per scenario, destroyed
  only on the success path.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| UAGP occurs | ASan runner: hard trap with allocation/free stacks. TSan runner: likely a race report on the freed object. | N/A — this is the detector working |
| A reader thread stalls past the test timeout | Harness fails the test and calls `MemoryTracker::ignoreThread` so the stranded thread unwinds on its next allocation. | Yes |
| Assertion thrown inside a worker-thread lambda | **Uncaught → `std::terminate`**, killing the process and masking the summary. Every worker lambda must catch. | No — must be designed around |
| TSan in-suite timeout | Harness already applies `kTimeoutMultiplier = 10` under TSan. | Yes |
| Sanitizer fails at process startup on a macOS revision | Check Homebrew clang version first — 21.1.8 hangs ASan and SEGVs TSan on macOS 26. | Yes (toolchain) |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P3-ITEM-001 | Resolved 2026-08-01 → P1-DEC-009 (protocol-point callbacks on the existing `Hooks` policy) | | | How is the "stall a reader at an exact protocol point" hook implemented without leaving instrumentation in production code? | None of the three candidates: the engine's `Hooks` policy template (RCU-DEC-017) already is the injection mechanism, already provably zero-cost in release. The harness `Hooks` spins on atomic gates at named protocol points. |
| P3-ITEM-002 | Resolved 2026-08-01 → P3-DEC-007 | | | Does the quiet-system residue scenario (RCU-DEC-006) need a real idle thread, or is "thread A stops calling into RCU entirely while B keeps advancing" a faithful model? | Faithful — an idle kernel CPU makes zero RCU calls, so the domain-observable behavior is identical. The stealing-not-owner-drain concern becomes an assertion, not a harness feature; see P3-DEC-007. |
| P3-ITEM-003 | Resolved 2026-08-01 → P3-DEC-008 | | | Should the versioned-cell structure be swapped for a small linked list to exercise multi-node traversal within one section? | Yes, and it cost three lines. Each cell publishes a CHAIN of `kChainDepth = 3` payloads; readers walk all of it inside one section. The chain is immutable once published and retired as a unit, so its links need no atomics. |
| P3-ITEM-004 | Resolved 2026-08-01 | | | Should a `run_rcu_tests` target be added to `run_all_tests`, given that `run_all_tests` already omits `CoreTestRunnerTSan` and `LibAllocTestRunnerTSan`? | Moot for RCU: `run_all_tests` already ran BOTH RCU runners, so the torture scenarios joined the aggregate target by adding the source file; `run_rcu_tests` / `run_rcu_tests_tsan` now exist as this spec's names, aliasing the Phase-2 pair. The anticipated cleanup was then done too — `CoreTestRunnerTSan` and `LibAllocTestRunnerTSan` are now in `run_all_tests`, so every ASan runner is paired with its TSan sibling: 9 runners, 1259 tests, all green. This mattered beyond tidiness — `CoreTestRunnerTSan` is the only coverage of `Core::rcu::EpochDomain`'s concurrency tests under the ARMv8 gate, so the engine's weak-memory validation had been opt-in. **Caveat carried forward:** that runner is the known post-rebuild flake; a red first run after a full rebuild should be re-run before it is believed. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P3-DEC-001 | Settled | **The ASan runner's deleter really calls `free()`.** No pooling, no deferred recycling, no poison-only mode. | This is the strongest UAGP oracle available and the only reason heap-backed mocks matter. A pooled deleter would convert every UAGP into a silent read of recycled-but-valid memory — exactly the failure mode vmsmalloc's immortal slabs already produce in the kernel, and precisely what the userspace harness exists to *avoid*. Decided 2026-08-01. |
| P3-DEC-002 | Settled | **Sibling ASan and TSan executables running identical scenarios.** | TSan and ASan are mutually exclusive at link time; the whole tree already solves this by building sibling library+runner pairs rather than toggling flags. Decided 2026-08-01. |
| P3-DEC-003 | Settled | **ARMv8 TSan is the release gate; ASan/leak runs alongside.** | `[[project_armv8_dev_tsan]]`. The two fences are the entire safety argument and x86 TSO would mask a downgrade. Decided 2026-08-01. |
| P3-DEC-004 | Settled | **Every worker lambda catches assertion exceptions and reports via a shared failure flag.** | An assert thrown inside a `std::thread` lambda is uncaught and calls `std::terminate`, killing the process and destroying the test summary. vmsmalloc Phase 8 hit this and it cost real debugging time. Decided 2026-08-01. |
| P3-DEC-005 | Settled | **Stalls are injected, never awaited.** No scenario sleeps hoping a race occurs. | P3-I3. A torture suite that relies on timing luck reports green on a broken build. HAZARD-1's window in particular is far too narrow to hit by chance. Decided 2026-08-01. |
| P3-DEC-007 | Settled (**assertion corrected same day — final review F4: the original "every retiree on B's thread, residue → 0" fails on a correct build**) | **The quiet-system residue scenario models idleness as "thread A stops calling into the domain" — no real idle thread — with choreographed sealing and a residue-exact assertion: (1) A retires a batch smaller than the advance threshold (so A never self-drains); (2) B advances the epoch (via its own retire + `tryAdvance`) *between* A's batch and A's final retire, so A's final `maybeRotate` seals the batch bag; (3) A quiesces. Assert: every *sealed*-bag retiree of A is destroyed **on B's thread** (deleters record their executor), A's slot never re-enters, and the terminal residue is **exactly** A's open-bag contents — the final retiree — per ITEM-014.** | Resolves P3-ITEM-002. An idle kernel CPU makes zero RCU calls, so the model is observationally exact. The original unqualified assertion demanded reclamation the design explicitly disclaims: A's final `Open` bag is unstealable (I13; the RCU-DEC-031 amendment), so "residue → 0" deterministically fails on a *correct* implementation, and "every retiree on B's thread" is over-strict, since A self-draining its own expired bags pre-quiesce is legal (own-slot-first, P1-DEC-008) — the choreography (batch < threshold; the B-driven advance forcing the seal) is what makes the corrected assertion deterministic (P3-I3). Still fails if stealing is removed — the parent Hazards' requirement that the suite be able to fail — and now also fails if residue exceeds the documented bound. Decided 2026-08-01. |
| P3-DEC-006 | Provisional | **Scenario set: stutter, ratio sweep, forced stall, quiet-system residue, deleter-retires, nested-section, batch-bound churn, barrier semantics, barrier×bound, dual barriers, deleter-retires-during-barrier, escaped-SafePtr, contended teardown, failed-scenario survival.** (Additions 2026-08-01: first two with RCU-DEC-031/033; last six from subagent review.) **Known-untestable:** migration between `retire` and `barrier` (RCU-DEC-040) is unrepresentable under P3-I2's one-thread-per-slot binding — recorded so it is not presumed covered. | Covers each parent Verification Target that is not already a Phase-1 unit test. Provisional — adversarial review may add scenarios, and the vmsmalloc precedent was that the shipped corpus was smaller and more focused than the spec's. Decided 2026-08-01. |

| P3-DEC-008 | Settled | **Each cell publishes a chain of `kChainDepth = 3` payloads, walked inside one section.** | Resolves P3-ITEM-003. A single cell holds one pointer per section, which does not exercise the "hold a pointer across several dereferences" shape a radix walk has — the shape R5 exists for. The chain is immutable once published and retired as a unit, so its links are plain pointers and its retire is a loop over `RetireHead`s that never alias the chain links. Decided 2026-08-01. |
| P3-DEC-009 | Settled | **The escaped-`SafePtr` scenario is compiled unconditionally but armed only by `CROCOS_RCU_ESCAPE_DEMO=1` in the environment.** | The scenario's PASSING behaviour is an ASan abort — which is the oracle working, and also a dead process with no test summary. A release gate that always aborts is not a gate. Compiling it unconditionally keeps it from rotting; the disarmed path asserts its own guard so the test is visibly present and visibly off in every ordinary run. Decided 2026-08-01. |
| P3-DEC-010 | Settled | **The klog sink gains a capture surface (`tests/kernel/vmsmalloc/mocks/MockKlog.h`), armed per test.** | The forced-stall scenario must assert that RCU-DEC-013's diagnostic *fires*, per the parent Hazards' "a torture suite that cannot fail is worse than none". The diagnostic's only observable is a `klog` line from `~ReadGuard`, and the shared mock discarded unconditionally — so without this the second half of that assertion could only be trusted, not tested. The armed flag is checked before the mutex so the default path stays one relaxed load and vmsmalloc's timed concurrent tests pay nothing. Decided 2026-08-01. |
| P3-DEC-011 | Settled | **Torture nodes are `new`/`delete`, NOT `retireDestroy`.** | P3-DEC-001's oracle requires a real `free`. `retireDestroy` bottoms out in the mock `VMSubstrate`, which recycles — exactly the Hazards' "mock divergence" case, and it would convert every UAGP into a silent read of recycled-but-valid memory. Phase 2 already covers the `retireDestroy` path; this phase deliberately trades it for the detector. Decided 2026-08-01. |

## Hazards

- **A torture suite that cannot fail is worse than none.** The forced-stall scenario must assert
  that reclamation *does* stall and that the diagnostic *does* fire — testing the liveness
  diagnostics rather than trusting them. Same for the residue scenario: it must fail if stealing
  is removed.
- **Mock divergence.** `MockVMSubstrate` recycles memory; if the RCU harness's mock keeps a free
  list instead of really freeing, P3-DEC-001 is silently defeated. The RCU harness's allocation
  path must be genuinely heap-backed even though vmsmalloc's deliberately is not.
- **The all-zero interrupt-depth mock.** vmsmalloc's harness returns all zeros
  (`MockKernelEnv.cpp:68-69`), so DEC-014 checks never fire there. Copying that mock verbatim
  would make Phase 2's context assertions untestable — the settable variant is a Phase-2
  deliverable this phase depends on.
- **TSan and `nesting`.** Phase 1's non-atomic `nesting` (I5) is correct only under one-thread-
  per-slot. Any scenario that drives one slot from two threads will produce a true-positive TSan
  report that looks like an engine bug.
- **Leak tracker vs. `std::thread`.** Concurrent tests must use `TEST_WITH_TIMEOUT_NO_TRACKING` or
  the tracker attributes thread machinery to the test.

## Verification Targets

| Property | Method |
|---|---|
| No use-after-grace-period under sustained read/write churn | ASan runner, real `free` deleter (P3-DEC-001) |
| No data races in the protocol | TSan runner on ARMv8 (release gate) |
| Full drain occurs when all readers pause | Stutter scenario: assert live-object count returns to zero |
| Reclamation stalls while a reader holds a section, and the diagnostic fires | Forced-stall scenario asserting both |
| Residue → 0 on a quiet system with no scheduler/IPI input (RCU-DEC-006) | Quiet-system scenario: one thread retires then stops touching RCU; another advances; assert full reclamation |
| HAZARD-1 interlock holds under contention | Injected-stall scenario at the push/steal window, under TSan |
| Deleters that retire are safe | Deleter-retires scenario |
| Nested sections, incl. simulated interrupt nesting | Nested-section scenario |
| Behaviour is stable across reader/writer ratios | Ratio sweep |
| Batch-bound re-seal is exact: nothing lost, nothing double-run, nothing early | Batch-bound churn scenario with per-object run counters, both runners |
| `barrier`'s amended contract (RCU-DEC-031): own retirees complete, remote open bags untouched | Barrier-semantics scenario |
| Teardown contract end-to-end (P3-I5) | Every scenario's epilogue; destructor assert + leak tracker |

## Testing Approach

`tests/kernel/rcu/TortureTest.cpp` alongside Phase 2's integration and assertion tests, sharing
the same mocks and both runners.

**Structure under torture.** A fixed array of cells, each holding an `Atomic<Cell*>` to a
heap-allocated versioned payload (`{ magic, version, ownerThread, filler }`). Readers enter a
section, `protect`-load a cell, verify magic and self-consistency, and exit. Writers allocate a
new payload, swap it in, and retire the old one. Any reader that observes a freed payload trips
ASan; any reader that observes a torn or stale-magic payload trips an assertion.

**Scenarios** (P3-DEC-006):

| Scenario | Shape | What fails if broken |
|---|---|---|
| Stutter | All readers pause simultaneously; writers keep retiring; then everyone resumes | Drain never completes → live count stays non-zero |
| Ratio sweep | Reader:writer ratios from all-readers to all-writers | Contention-dependent bugs in the bag machine |
| Forced stall | One reader holds a section for a long, *bounded* interval | Reclamation must stall and the diagnostic must fire |
| Quiet-system residue | Thread A retires sub-threshold, B forces an epoch advance, A's final retire seals the batch bag, A quiesces (P3-DEC-007 choreography) | A's sealed-bag retirees destroyed on B's thread; terminal residue exactly A's open-bag contents — fails if stealing (RCU-DEC-006) is broken *or* if residue exceeds the ITEM-014 bound |
| Deleter retires | Deleters retire a second object | Reentrancy into `retire` during a drain |
| Nested sections | Sections nested 2-3 deep, plus a simulated interrupt-nested entry | Nesting bookkeeping (I5) |
| Batch-bound churn | `drainBatchBound = 1` under full read/write contention, so every drain re-seals mid-bag | The `Claimed → Sealed` exit (RCU-DEC-033, amended I13): remainder lost, double-run, or drained early |
| Barrier semantics | Thread A retires into its open bag and calls `barrier`; thread B retires and goes quiet unsealed | `barrier` must destroy all of A's retirees, must *not* touch B's open bag, and must drain B's sealed bags |
| Barrier × bound | `drainBatchBound = 1` while one thread runs `barrier` under full churn | The `kBagReseal` hand-off of `≤ e0` remainders mid-barrier; RCU-DEC-036's bounded-conversion termination argument |
| Dual barriers | Two threads call `barrier` concurrently on different slots | Hold-and-wait deadlock (should be impossible — claims are never held while waiting); both must return |
| Deleter-retires during barrier | Deleters retire fresh objects while their thread's `barrier` sweeps run them | RCU-DEC-036's mandatory rotate: without it these push into a `Sealed` bag (I11-fatal); also the `!inDrain` assert boundary |
| Escaped `SafePtr` | A reader deliberately keeps a protected pointer past `readUnlock` and dereferences | ASan trap — the parent-Hazards-requested oracle for the escape bug class |
| Contended teardown | Full churn, then join everything, then `drainAllQuiescent()` with pending deleter-retire chains and remote open bags | The teardown epilogue under realistic debris, not just the phase-1 quiet unit test |
| Failed-scenario survival | A scenario is *forced* to fail (injected assert); the suite continues | P3-I5's failure path: no `std::terminate`, summary intact, domain leaked deliberately |

**Build.** Copy `tests/kernel/vmsmalloc/CMakeLists.txt` as the template: mocks first on the
include path, the five-define set plus `CROCOS_RCU_TEST_HARNESS`, force-included
`assert_support.h`, ASan at `-O0 -g` and TSan at `-O1 -g`, `-Wl,-undefined,dynamic_lookup` on
Apple, and `run_rcu_tests` / `run_rcu_tests_tsan` custom targets.

**Running.**

```bash
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Debug
cmake --build tests/build --target run_rcu_tests
cmake --build tests/build --target run_rcu_tests_tsan     # release gate
```

Per the standing verification policy, re-run these directly rather than trusting a summary that
says they passed.

## Implementation Phases

1. **Harness scaffolding** — CMakeLists, both runners, mocks wired, one trivial test green in
   both.
2. **Cell structure + reader/writer bodies**, with P3-DEC-004's exception discipline in place
   from the start.
3. **Stutter and ratio-sweep scenarios** — the baseline churn coverage.
4. **Injected-stall infrastructure** — the harness `Hooks` with atomic-gated protocol points
   (P1-DEC-009) — then the forced-stall and HAZARD-1 scenarios.
5. **Quiet-system residue scenario** — the RCU-DEC-006 gate. Verify it fails when stealing is
   disabled.
6. **Deleter-retires, nested-section, batch-bound churn, and barrier-semantics scenarios.**
7. **Green on both runners on ARMv8**; register in `run_all_tests` (and fix the pre-existing
   `CoreTestRunnerTSan` / `LibAllocTestRunnerTSan` omissions per P3-ITEM-004).

## Beyond the gate — the endurance/performance soak

`tests/kernel/rcu/SoakTest.cpp` is **not part of this phase's gate and not part of
`run_all_tests`**. It ships in its own executables (`KernelRcuSoakRunner{,TSan}`,
both `EXCLUDE_FROM_ALL`) behind `run_rcu_soak` / `run_rcu_soak_tsan`, and the test is
disarmed unless `CROCOS_RCU_SOAK_SECONDS` is set. An endurance test inside the
correctness gate is a gate people stop running.

It exists for two questions this phase's scenarios deliberately do not answer:

- **Memory overhead** — peak/p50/p90/p99 limbo occupancy (objects retired but not
  yet destroyed) under sustained churn with threads periodically going quiet.
  Measured at the *test's* accounting layer, not by sampling the engine:
  `totalResidue` on a live domain walks bag node lists concurrently with a drainer
  holding one `Claimed`, which is a real UAF risk under ASan and a true positive
  under TSan. Both `DebugIntrospection` headers already say snapshots assume a
  quiescent domain; the test-side counter measures the same quantity safely.
- **Read/update throughput and latency.** Three runners ship: ASan (UAGP oracle),
  TSan (race oracle), and **Perf** (`-O2 -g -fno-omit-frame-pointer`, no
  sanitizer) — the only one whose numbers mean anything. ASan instruments every
  load and store and TSan additionally serialises through shadow memory, so both
  inflate latency by roughly an order of magnitude. Perf is *not* a correctness
  runner: the assertions still hold there, but with no sanitizer behind them a
  use-after-grace-period reads recycled bytes silently. Run an oracle first.

  The report calibrates its own instrument before using it — `Clock::now()` pair
  cost at p50/p90 plus the observed clock tick — because on an uninstrumented
  build a read section costs the same order as the measurement. On Apple Silicon
  the tick is 41 ns, which quantises every latency figure and is the number that
  bounds their precision. Calibration is reported, never subtracted.

  First measured baseline (2026-08-01, M1, 60 s x 8 threads, uninstrumented):
  9.1 M reads/s, 1.3 M updates/s, 3.9 M retires/s; read latency
  p50 192 / p90 768 / p99 1024 ns; update p50 768 / p90 2048 / p99 8192 ns.

Threads nap *outside* any section — the RCU-DEC-006 quiet-CPU case at scale, not
the forced-stall case. It feeds parent ITEM-005, which cannot be answered until a
RadixVM-shaped workload exists but now has a measurement rig waiting for one.

One harness change was required: `kernel::timing::test::setMonoStep(0)`. The mock
`monoTimens` auto-advances a *shared* counter per call, so with N threads looping
sections a section's measured "elapsed" tracks total call volume rather than its
own duration — every `~ReadGuard` trips RCU-DEC-013's 100 ms threshold, and each
warning takes `AtomicPrintStream`'s **process-wide spinlock**, serialising exactly
the threads being measured. Freezing the step removes a harness artifact, not a
real signal.

## References

- `specs/rcu.md` — parent; Verification Targets and Testing Approach; RCU-DEC-031/033/034/035
  (barrier contract, batch bound, teardown).
- `specs/rcu-phase-1.md`, `specs/rcu-phase-2.md` — subjects under test.
- `specs/vmsmalloc-phase-8.md` — the harness pattern, P8-DEC-002 (build production source
  unchanged), P8-DEC-004 (ARMv8 TSan gate).
- `tests/kernel/vmsmalloc/` — the concrete template: `CMakeLists.txt`, `mocks/`,
  `ConcurrentTest.cpp`.
- `tests/harness/TestHarness.cpp:66-75` — TSan timeout scaling.
- P. E. McKenney, rcutorture — scenario taxonomy (stutter, ratio sweep, stall injection).
