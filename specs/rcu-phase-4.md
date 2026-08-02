---
kind: leaf
status: drafting
parent: specs/rcu.md
---

# RCU Phase 4 — In-kernel stress

> The validator for everything the userspace harness mocks. Phases 1-3 ship 1261 green tests and
> yet, until this phase, several kernel-side mechanisms of the Phase-2 veneer had never executed
> on a real target even once. Written **after** a spike, so its Hazards are observations rather
> than predictions.

## Non-Goals

- **Not a replacement for the torture suite.** Phase 3 remains the correctness gate: it is
  deterministic, sanitizer-backed, and can assert exact residue and exactly-once destruction.
  This phase cannot do any of that — it has no ASan, no TSan, and no post-join quiescent point to
  measure from. The two are complementary, and a Phase-4 failure is usually a Phase-1/2 bug.
- **Not a performance benchmark.** The soak runners answer throughput and latency. This phase's
  only quantitative output is liveness counters, and their absolute values are meaningless
  (QEMU/TCG, and see the profiling caveat in `[[project_slab_abstraction_plan]]`).
- **Not a RadixVM workload.** Cells-and-nodes, deliberately, for the same reason Phase 3 was not
  a tree: a failure must implicate RCU, not an unwritten consumer.
- **Not fault injection.** No artificially corrupted descriptors, no simulated allocation
  failure. Negative testing of entry-point assertions lives in Phase 2's `AssertionsTest.cpp`.
- **Not a bounded run.** Like the `VmsmallocStress` it replaces, the driver loops until the
  shutdown timer fires. External time-boxing is the CI story.

## Consumer Contract

Ships a kernel component, not an API. Its deliverable is the claim Phase 3's Consumer Contract
could not make: **the framework has been exercised on the target, not only in the harness.**

Parent ITEM-021 is this phase's charter and should close against it.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 1 engine, Phase 2 veneer | Subject under test | Yes |
| `kernel::rcu::kernelDomain` + `rcu.icd` | The domain under stress; asserted `initialized()` on entry | Yes |
| `VMSubstrate::make<T>` / `destroy<T>` / `SafePtr` | Real allocation and real freshness | Yes |
| `[Shutdown]` at `smp_bringup` | Bounds the run via a timer callback | Yes |
| QEMU with `-smp 8` (`run`, `run_numa`, `run_numa_hmat`) | Multi-CPU, multi-domain targets | Yes |

## Invariants

- **P4-I1.** The structure under stress is **shared across CPUs**, never per-CPU. A per-CPU
  structure would never make one CPU read a node another CPU retired, which is the entire hazard.
- **P4-I2.** Every allocation is `VMSubstrate::make<T>` and every read of allocator-returned
  memory goes through `SafePtr<T>` (DEC-028). The one place this phase deliberately did *not*
  hold is what it found — see P4-DEC-003.
- **P4-I3.** Node classes span at least one slab size class and one DEC-029 whole-page
  allocation, so replacing `VmsmallocStress` does not silently narrow allocator coverage.
- **P4-I4.** The stress is its own success signal. No assertions about counts; a panic, a hang,
  or a corruption report is the failure.
- **P4-I5.** Exactly one per-CPU `[[noreturn]]` stress component is registered at `smp_bringup`.
  Two would mean the second never runs, silently.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| A protected read sees a torn or recycled node | `klog` the CPU/version/class, then `assert` → PANIC with a stack trace, and **QEMU exits 3** (P4-DEC-007) | N/A — the detector working |
| An engine or veneer debug assert fires | PANIC, **exit 3**. This is the intended channel: it is the first time these asserts run as `PANIC` rather than as a thrown C++ exception | N/A |
| Release build hits the same defect | The debug assert is *gone*. A null-deleter defect becomes a call through a null function pointer — see P4-DEC-003. Still reported, as **exit 5**, but anonymously | No — argues for running the stress in debug |
| Component silently unregistered | `.icd` glob is configure-time; adding or renaming one without re-running CMake configure leaves the kernel booting happily with no stress at all | Yes (re-configure) |
| `depends_on` names a component in another phase | Reported as "Component X is in dependency cycle" — misleading; the name is simply unresolvable, since `compute_component_order` runs per phase | Yes |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P4-ITEM-001 | Resolved 2026-08-01 → P4-DEC-006 | | | Does this workload actually drive `reclaimSlabPage`, or does it merely *happen* to expose stale mappings by another route? | Both, and the distinction turned out to matter. Instrumented rather than argued — see P4-DEC-006 for the numbers and for what remains unproven. |
| P4-ITEM-002 | Open | No | | What is the retire-path cost of P1-DEC-018's per-retire `ensureTLBEntryFresh`? | Unmeasured. The userspace soak cannot answer it — its `ensureTLBEntryFresh` is a no-op. Needs either an in-kernel timing harness or an instruction-count comparison. |
| P4-ITEM-003 | Open | No | | Should the stress also exercise `synchronize` / `barrier`, which no in-kernel path calls today? | The spike drives `ReadGuard`, `protect`, `retireDestroy` and `tryAdvance` only. `barrier`'s drive-to-completion loop and `synchronize`'s spin have never run on real hardware, and both are blocking primitives whose failure mode is a hang rather than a panic. |
| P4-ITEM-004 | Open | No | | Is one `[[noreturn]]` per-CPU stress the right long-term shape, given vmsmalloc no longer has one? | Replacing rather than selecting was the right call for now (the user's: `VmsmallocStress` was always temporary). But the next subsystem wanting an in-kernel stress faces the same collision, and P4-I5 makes that a structural constraint rather than a convention. |
| P4-ITEM-005 | Open | No | | Should the driver detect a hang, rather than relying on the shutdown timer to mask one? | A livelocked `barrier` or a stalled grace period currently looks identical to a slow boot: the timer fires, the kernel prints `Goodbye`, and CI sees success. A watchdog comparing per-CPU iteration counts across the run would distinguish them. |
| P4-ITEM-006 | Resolved 2026-08-01 → P4-DEC-007 | | | What is the machine-checkable pass condition? | **Cause identified 2026-08-01: the kernel has no failure exit path.** Four termination sites all write the same QEMU ACPI poweroff — `outw 0x2000 -> port 0x604` — `KernelMain.cpp:73` (the successful shutdown), `panic.h:32`, `amd64.cpp:127` (`pageFaultHandler`) and `amd64.cpp:153` (`unhandledExceptionHandler`). QEMU powers off cleanly and exits **0** through all of them, so a clean `Goodbye :)`, a debug `PANIC`, and a page fault on every CPU are indistinguishable to any automated caller. Note this is NOT the fault handler misbehaving: shutting down immediately on a fault is intended at this stage (spec author) — only the *status* it exits with is the problem, and the two are separable. **Scope is wider than this phase**: it applies to every in-kernel test the project has had, `VmsmallocStress` included, so any CI that checked this target's exit code was reading a constant. Interim pass condition: `Goodbye :)` present AND no `Pagefault` / `Stack trace` line — grepping for "Panic" is not sufficient, since release faults produce none. Likely fix, deferred until CI is real: `-device isa-debug-exit,iobase=0xf4,iosize=0x04` on the run targets with the three FAILURE paths writing there (QEMU exits `(v << 1) | 1`, always nonzero) while the success path keeps `0x604`; the failure handlers keep their trailing `cli; hlt` so an absent device halts rather than silently reporting success. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P4-DEC-001 | Settled | **`RcuStress` replaces `VmsmallocStress` outright** rather than being selectable alongside it. | `VmsmallocStress` was always intended as temporary scaffolding (spec author, 2026-08-01). Replacement avoids inventing a stress-selection mechanism that P4-I5 would then have to police, and the allocator coverage is carried forward by P4-DEC-002 instead of by keeping a second component alive. Decided 2026-08-01. |
| P4-DEC-002 | Settled | **Three node classes: 64 B and 512 B slab-backed, and 1024 B whole-page (DEC-029 bypass).** | Replacing `VmsmallocStress` would otherwise drop its 8-size-class plus whole-page sweep on the floor. Three classes recover the distinct *paths* (small slab, largest slab, whole-page bypass) at a fraction of the machinery, and RCU still sees one uniform retire/drain protocol. Cross-domain frees need no explicit hand-off: under RCU-DEC-006 stealing the CPU running a deleter is usually not the CPU that allocated the node, so vmsfree's DEC-019 gate is exercised for free. Decided 2026-08-01. |
| P4-DEC-003 | Settled | **The spike ran before the spec, and the spec records its finding rather than predicting it.** The finding is P1-DEC-018: `onPreTouch` covered reads but not writes. | Spec-first was the house style for Phases 1-3 and it worked; here it would have produced a document asserting that the veneer's freshness discipline was sound, because that is what every existing test showed. The bug was found on the first boot, in the one mechanism ITEM-021 named as most likely wrong, and its diagnosis needed a three-arm in-kernel experiment that no spec could have specified in advance. Recording that inversion is more useful than pretending to the usual order. Decided 2026-08-01. |
| P4-DEC-006 | Settled | **The hazard is genuinely exercised, and the framework's freshness discipline is load-bearing rather than decorative.** Resolves P4-ITEM-001. Measured by making `ensureTLBEntryFresh` report whether it actually invalidated, counting that at `KernelRcuHooks::onPreTouch` (once per retire, once per drained node — not hot, unlike `ensureTLBEntryFresh` itself, which is on every `SafePtr` dereference), and counting `reclaimSlabPage` calls. | **Debug run:** 605 reclaims, 686,913 `onPreTouch` calls, **97,706 of them (14.2%) actually invalidated**. **Release, ~20x volume:** 89,804 reclaims, 13.47M `onPreTouch`, 1.59M stale (11.8%). Three conclusions. (1) `reclaimSlabPage` **does** fire under this workload — the earlier suspicion that it might never run was wrong. (2) One in seven RCU `RetireHead` accesses happens through a mapping this CPU had stale, so `onPreTouch` is not a precaution against a theoretical case; remove it and the framework breaks immediately, which is precisely what P1-DEC-018 demonstrated. (3) `preTouch` ≈ 2x `retires` in every run, which validates the counter against the expected one-per-retire-plus-one-per-drain shape. **What is still NOT proven:** that a node *still sitting in a limbo bag* was on a page that `reclaimSlabPage` specifically reclaimed. Stale hits outnumber reclaims ~18:1, so the large majority come from ordinary slab mapping churn (dirty-bit propagation on any `freePage`/`allocPage`), not from the reclaim path. That also explains why P1-DEC-018 surfaced within ~1 iteration, which no reclaim-driven story accounted for. Proving the exact conjunction would need per-page provenance tracking; the two measurements together make it near-certain but they do not establish it. **Gating:** all of it sits behind `-DCROCOS_FRESHNESS_STATS=ON`, OFF by default — these counters answer a question you ask of a stress run, not something a shipping kernel should pay for on its retire and reclaim paths. Verified absent: `nm` finds zero counter symbols in a default image, and the liveness line prints `[stats=off]` rather than zeros. **The fields are ABSENT rather than zero on purpose:** a build reporting `stale=0` would read as a coverage regression under this phase's Verification Targets, which is the exact opposite of the truth. `ensureTLBEntryFresh`'s `bool` return is NOT gated — the branch already exists inside it, so reporting the outcome costs a register, and gating it would make the signature differ between builds for no gain. Decided 2026-08-01. |
| P4-DEC-005 | Settled | **The stress is run in a DEBUG kernel by default**, and the release configuration is validated separately rather than being the primary target. | Debug is where the detectors live: `CROCOS_RCU_DEBUG_CHECKS=1` makes every veneer and engine assert a named `PANIC`, which is how P1-DEC-018 was attributed in seconds rather than bisected. Release is still worth running — it builds and boots clean, and at ~20x the debug throughput (6.75M iterations/CPU against ~330K) it covers far more volume — but a release failure arrives as an anonymous fault, so it answers "does it survive" and not "what broke". Both configurations are exercised; only debug is diagnostic. Decided 2026-08-01. |
| P4-DEC-007 | Settled | **The kernel now reports a termination status to the host, and the exit code of `run` IS the machine-checkable pass condition.** Resolves P4-ITEM-006. The fix P4-ITEM-006 sketched and deferred was implemented rather than deferred further: a new arch-neutral `kernel::exitToHost(ExitStatus)` (`kernel/include/kexit.h`, AMD64 backend in `kernel/arch/amd64/hostExit.cpp`) replaces the open-coded `outw` at all four sites. `Success` keeps the ACPI poweroff at `0x604` and exits **0**; `Panic`, `PageFault` and `UnhandledException` write 1/2/3 to QEMU's `isa-debug-exit` at `0xf4`, which exits `(v << 1) \| 1` — **3, 5 and 7**, distinct and always odd, hence never confusable with success. `-device isa-debug-exit,iobase=0xf4,iosize=0x04` is on `run`, `run_numa` and `run_numa_hmat`, and deliberately NOT on `qmon`/`kdebug`. | Measured, not assumed. Injecting each of the three faults into the shutdown callback in turn yields exactly **3 / 5 / 7**, and the three unmodified debug targets plus a Release build all still exit **0** with `Goodbye :)` and `corrupt=0`. **The exit code is a strictly stronger signal than the interim grep, not a restatement of it**: in all three injected runs `Goodbye :)` was still printed (the injection sat after the log line) and the status still reported failure — a case the old "`Goodbye` present" half of the grep would have called a pass. Three deliberate choices. (1) **The failure path never touches `0x604`**, since writing it would exit 0 and undo the entire point. (2) **The failure path keeps a trailing `cli; hlt`**, so on a target with no `isa-debug-exit` device — real hardware, or a hand-rolled QEMU line — a fault halts rather than falling through to the shutdown timer and reporting success; a hung run is a failure a human investigates, a false green is not. (3) **`qmon`/`kdebug` are excluded on purpose**: they are gdb stubs, where halting on a fault so it can be inspected beats exiting out from under the debugger. The status-drain `pause` loop that previously lived only in `panic` now covers all four paths, so a fault's serial output is no longer at risk of being cut off by the machine going away. **Scope is the whole project, not this phase**: every in-kernel test CroCOS has ever had, `VmsmallocStress` included, was previously unfalsifiable from outside, and all of them are now gateable. `-Werror` clean in Debug and Release; the userspace suite is unaffected at 1261/1261 (`panic.h` reaches the test runners, so its new call is excluded under `CROCOS_TESTING`). Decided 2026-08-01. |
| P4-DEC-004 | Provisional | **Liveness output is per-CPU counters every 64K iterations**, not per-operation tracing. | Enough to see all CPUs making progress and to spot a stalled one, cheap enough not to distort the workload, and small enough that a 20-second run does not drown the serial log. Provisional because P4-ITEM-005 may want it shaped into a real hang detector. Decided 2026-08-01. |

## Hazards

- **Absence of a panic is a weak signal, and it is the only one this phase has.** Phase 3 can
  assert exactly-once destruction and exact residue; this phase cannot observe either. A defect
  that leaks, double-frees into a still-mapped page, or stalls reclamation without corrupting
  anything will look exactly like success. Treat green here as "no *loud* failure on the target",
  never as "validated".
- **The shutdown timer masks hangs** (P4-ITEM-005). A livelock and a healthy run both end in
  `Goodbye :)` — and, since P4-DEC-007, both still exit **0**, because a stalled CPU is not a
  fault and nothing writes a failure status. The exit code makes a *loud* failure checkable; it
  does nothing for a quiet one. This is the one hazard P4-DEC-007 did not touch, and it is what
  makes P4-ITEM-005's watchdog the natural successor: a watchdog that panics on a stalled CPU now
  has a channel to report through, which it did not before.
- **Release turns a NAMED panic into an ANONYMOUS fault.** Measured 2026-08-01 by reintroducing
  P1-DEC-018 in a Release build. Debug reports `Panic: Assert failed: rcu: retired node has no
  deleter` with the file and line — it names the invariant. Release reports
  `Pagefault at 0x0 accessing 0x0` with a raw stack trace on several CPUs, never reaches the
  shutdown, and prints no `Goodbye :)`; nothing in that output connects it to RCU, to a deleter,
  or to a retire. The defect is loud in release; it is merely unattributable — which is why
  P4-DEC-005 keeps debug as the diagnostic configuration. (An earlier draft of this hazard claimed
  a release stress "would have found nothing and reported success" — that was wrong, and measuring
  it is what corrected it. A second clause, that `run` exits 0 in the fatal case too, was true when
  written and was **fixed** rather than corrected: see P4-DEC-007. The two statuses now differ,
  5 against 0, but the *attribution* problem this hazard describes is untouched — an exit code of
  5 says a page fault happened, not that RCU caused it.)
- **A failure status needs the `isa-debug-exit` device, and a hand-rolled QEMU line will not have
  one.** The `run`, `run_numa` and `run_numa_hmat` targets pass it; anyone invoking
  `qemu-system-x86_64` directly, or running on real hardware, gets a failure path that halts at
  `cli; hlt` instead of exiting. That is the deliberate choice (P4-DEC-007) — a hang is
  investigable, a false green is not — but it means **a CI job that builds its own QEMU command
  line inherits the old unfalsifiable behaviour in the worst way: as an apparent timeout.**
- **The `.icd` glob is configure-time.** Adding or renaming a component without re-running CMake
  configure leaves it silently unregistered — per the Phase-2 failure table, the worst mode
  available, because the kernel boots perfectly and simply does nothing.
- **`depends_on` is intra-phase.** A cross-phase dependency is reported as a dependency cycle,
  which sends you looking for the wrong thing entirely.

## Verification Targets

| Property | Method |
|---|---|
| The veneer's masked window, `SafePtr`, real `assert`→`PANIC`, and `.icd` AP ordering all execute | Boot on `run`; the driver reaches its first liveness line on every CPU |
| `onPreTouch` → `ensureTLBEntryFresh` covers every `RetireHead` access | P1-DEC-018's three-arm experiment; regression-guarded by the Phase-1 unit test `rcuOnPreTouchFiresOncePerRetireAndPerDrain` |
| Stealing works across real arenas and NUMA domains | `run_numa` / `run_numa_hmat` clean, with retires >> 0 on every CPU |
| All three allocation paths survive sustained retire/drain | `corrupt=0` with each node class in rotation |
| No torn or recycled node is ever observed by a protected read | Magic + checksum verified inside every section; mismatch panics |
| The DEC-047 hazard is actually presented to `onPreTouch`, not merely walked past | `reclaims` and `stale` counters in the liveness line (P4-DEC-006). **`stale == 0` would mean the workload proves nothing about freshness, however green it looks** — treat a drop to zero as a coverage regression, not an improvement |
| A run's verdict is machine-checkable | **The exit status of the `run` target** (P4-DEC-007): 0 clean, 3 panic, 5 page fault, 7 unhandled exception. Verified by injecting each fault in turn. The old grep — `Goodbye :)` present and no fault line — remains valid as a *secondary* check, but it is strictly weaker: the injected runs printed `Goodbye :)` and still exited nonzero |

## Testing Approach

`kernel/RcuStress.cpp`, registered as `[RcuStress]` in `kernel/general.icd`: `per_cpu = true`,
`phase = "smp_bringup"`, `depends_on = ["Shutdown"]`.

Each CPU loops: pick a node class and a shared cell by per-CPU xorshift; ~1 in 8 iterations
publishes a fresh node and `retireDestroy`s what it displaced, the rest take a `ReadGuard`,
`protect`-load the cell and verify magic + checksum through the `SafePtr`; ~1 in 64 calls
`tryAdvance` to pull grace-period progress from the mutation path (RCU-DEC-005).

**Running.**

```bash
# Coverage instrumentation (P4-DEC-006) is opt-in and off by default:
cmake -B build-stats -DCMAKE_BUILD_TYPE=Debug -DCROCOS_FRESHNESS_STATS=ON

cmake --build cmake-build-debug --target run            # 8 CPUs, single domain
cmake --build cmake-build-debug --target run_numa       # 3 NUMA domains
cmake --build cmake-build-debug --target run_numa_hmat  # + HMAT latency/bandwidth
```

**The exit status is the verdict** (P4-DEC-007) — `0` clean, `3` panic, `5` page fault, `7`
unhandled exception — so a CI gate is just the target's own exit code, with no output parsing:

```bash
cmake --build cmake-build-debug --target run || echo "stress FAILED with status $?"
```

Observed on all three (debug): ~330K iterations per CPU, ~2.5M protected reads and ~350K retires
per CPU, `corrupt=0`, clean `Goodbye :)`, exit `0`.

Release (`-DCMAKE_BUILD_TYPE=Release`) builds and boots clean at roughly 20x the volume —
6.75M iterations per CPU, 46.6M protected reads, 6.65M retires — with the debug-check symbols
absent from the image. Per P4-DEC-005 that is a volume run, not a diagnostic one.

## References

- `specs/rcu.md` — parent; **ITEM-021** is this phase's charter.
- `specs/rcu-phase-1.md` — **P1-DEC-018**, the defect this phase found and the fix.
- `specs/rcu-phase-3.md` — the userspace gate this complements; its first Non-Goal deferred
  in-kernel stress to "a later phase, planned separately".
- `specs/vmsmalloc-phase-9.md` — the precedent, and the component this one replaces.
- `docs/vmsmalloc-stale-tlb-bug.md` — DEC-047, the bug class `onPreTouch` exists to guard.
