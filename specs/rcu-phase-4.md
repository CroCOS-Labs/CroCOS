---
kind: leaf
status: complete
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
| A CPU stalls, livelocks, or never enters the stress loop | The watchdog names it and every other CPU's iteration count, then **exits 9** (P4-DEC-008). Detection lands ~9-11 s in, well inside the 20 s shutdown | N/A — the detector working |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P4-ITEM-001 | Resolved 2026-08-01 → P4-DEC-006 | | | Does this workload actually drive `reclaimSlabPage`, or does it merely *happen* to expose stale mappings by another route? | Both, and the distinction turned out to matter. Instrumented rather than argued — see P4-DEC-006 for the numbers and for what remains unproven. |
| P4-ITEM-002 | Resolved 2026-08-01 → P4-DEC-010 | | | What is the retire-path cost of P1-DEC-018's per-retire `ensureTLBEntryFresh`? | Unmeasured. The userspace soak cannot answer it — its `ensureTLBEntryFresh` is a no-op. Needs either an in-kernel timing harness or an instruction-count comparison. |
| P4-ITEM-003 | Resolved 2026-08-01 → P4-DEC-009 | | | Should the stress also exercise `synchronize` / `barrier`, which no in-kernel path calls today? | The spike drives `ReadGuard`, `protect`, `retireDestroy` and `tryAdvance` only. `barrier`'s drive-to-completion loop and `synchronize`'s spin have never run on real hardware, and both are blocking primitives whose failure mode is a hang rather than a panic. |
| P4-ITEM-007 | Resolved 2026-08-01 → P4-DEC-011 (LTO taken) + P4-DEC-012 (duplicate CPU-id load deliberately NOT chased) | | | Should the two instruction-count reductions P4-DEC-010 uncovered be taken? | **(1) The current CPU's ID is computed TWICE per retire.** `retireNode` calls `kernel::getLogicalProcessorID()` to pass to the engine, and `ensureTLBEntryFresh` then independently calls `arch::getCurrentProcessorID()` — which is a straight wrapper around the same function (`kernel/arch/arch.cpp:51`), reached as an out-of-line cross-TU call costing ~12 of the retire path's 70 instructions, i.e. **~17%**. An overload taking a caller-supplied CPU id removes it. The prize is larger than retire: `ensureTLBEntryFresh` is on **every `SafePtr` dereference**, which is the hot path, so the same 12 instructions are paid there too — but for the same reason the signature change touches a lot of call sites. **(2) `-flto` is in `target_link_options` only, never in the compile flags** (`kernel/CMakeLists.txt`), so there is no cross-TU inlining and `ensureTLBEntryFresh` is a real call with prologue/epilogue from every caller. **(2) IS DONE — see P4-DEC-011**, which took it and cut retire 70 → 46 instructions. **(1) SURVIVES BUT SHRANK**: LTO inlined `getCurrentProcessorID` to `rdgsbase` + `movzbl`, so the id is still computed twice per retire but the redundant copy is now ~2 instructions rather than ~12 — the prize fell from ~17% of the retire path to ~4%, which is no longer obviously worth a signature change across every `SafePtr` call site. What remains is an optimization rather than correctness, and it should not be taken on QEMU evidence alone: P4-DEC-010's whole point is that the *cycle* cost is dominated by a cache miss and `invlpg`, which this environment cannot measure, so a real-silicon baseline should come first. |
| P4-ITEM-004 | Resolved 2026-08-01 → P4-DEC-013 | | | Is one `[[noreturn]]` per-CPU stress the right long-term shape, given vmsmalloc no longer has one? | Replacing rather than selecting was the right call for now (the user's: `VmsmallocStress` was always temporary). But the next subsystem wanting an in-kernel stress faces the same collision, and P4-I5 makes that a structural constraint rather than a convention. |
| P4-ITEM-005 | Resolved 2026-08-01 → P4-DEC-008 | | | Should the driver detect a hang, rather than relying on the shutdown timer to mask one? | A livelocked `barrier` or a stalled grace period currently looks identical to a slow boot: the timer fires, the kernel prints `Goodbye`, and CI sees success. A watchdog comparing per-CPU iteration counts across the run would distinguish them. |
| P4-ITEM-006 | Resolved 2026-08-01 → P4-DEC-007 | | | What is the machine-checkable pass condition? | **Cause identified 2026-08-01: the kernel has no failure exit path.** Four termination sites all write the same QEMU ACPI poweroff — `outw 0x2000 -> port 0x604` — `KernelMain.cpp:73` (the successful shutdown), `panic.h:32`, `amd64.cpp:127` (`pageFaultHandler`) and `amd64.cpp:153` (`unhandledExceptionHandler`). QEMU powers off cleanly and exits **0** through all of them, so a clean `Goodbye :)`, a debug `PANIC`, and a page fault on every CPU are indistinguishable to any automated caller. Note this is NOT the fault handler misbehaving: shutting down immediately on a fault is intended at this stage (spec author) — only the *status* it exits with is the problem, and the two are separable. **Scope is wider than this phase**: it applies to every in-kernel test the project has had, `VmsmallocStress` included, so any CI that checked this target's exit code was reading a constant. Interim pass condition: `Goodbye :)` present AND no `Pagefault` / `Stack trace` line — grepping for "Panic" is not sufficient, since release faults produce none. Likely fix, deferred until CI is real: `-device isa-debug-exit,iobase=0xf4,iosize=0x04` on the run targets with the three FAILURE paths writing there (QEMU exits `(v << 1) | 1`, always nonzero) while the success path keeps `0x604`; the failure handlers keep their trailing `cli; hlt` so an absent device halts rather than silently reporting success. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P4-DEC-001 | Settled | **`RcuStress` replaces `VmsmallocStress` outright** rather than being selectable alongside it. | `VmsmallocStress` was always intended as temporary scaffolding (spec author, 2026-08-01). Replacement avoids inventing a stress-selection mechanism that P4-I5 would then have to police, and the allocator coverage is carried forward by P4-DEC-002 instead of by keeping a second component alive. Decided 2026-08-01. |
| P4-DEC-002 | Settled | **Three node classes: 64 B and 512 B slab-backed, and 1024 B whole-page (DEC-029 bypass).** | Replacing `VmsmallocStress` would otherwise drop its 8-size-class plus whole-page sweep on the floor. Three classes recover the distinct *paths* (small slab, largest slab, whole-page bypass) at a fraction of the machinery, and RCU still sees one uniform retire/drain protocol. Cross-domain frees need no explicit hand-off: under RCU-DEC-006 stealing the CPU running a deleter is usually not the CPU that allocated the node, so vmsfree's DEC-019 gate is exercised for free. Decided 2026-08-01. |
| P4-DEC-003 | Settled | **The spike ran before the spec, and the spec records its finding rather than predicting it.** The finding is P1-DEC-018: `onPreTouch` covered reads but not writes. | Spec-first was the house style for Phases 1-3 and it worked; here it would have produced a document asserting that the veneer's freshness discipline was sound, because that is what every existing test showed. The bug was found on the first boot, in the one mechanism ITEM-021 named as most likely wrong, and its diagnosis needed a three-arm in-kernel experiment that no spec could have specified in advance. Recording that inversion is more useful than pretending to the usual order. Decided 2026-08-01. |
| P4-DEC-006 | Settled | **The hazard is genuinely exercised, and the framework's freshness discipline is load-bearing rather than decorative.** Resolves P4-ITEM-001. Measured by making `ensureTLBEntryFresh` report whether it actually invalidated, counting that at `KernelRcuHooks::onPreTouch` (once per retire, once per drained node — not hot, unlike `ensureTLBEntryFresh` itself, which is on every `SafePtr` dereference), and counting `reclaimSlabPage` calls. | **Debug run:** 605 reclaims, 686,913 `onPreTouch` calls, **97,706 of them (14.2%) actually invalidated**. **Release, ~20x volume:** 89,804 reclaims, 13.47M `onPreTouch`, 1.59M stale (11.8%). Three conclusions. (1) `reclaimSlabPage` **does** fire under this workload — the earlier suspicion that it might never run was wrong. (2) One in seven RCU `RetireHead` accesses happens through a mapping this CPU had stale, so `onPreTouch` is not a precaution against a theoretical case; remove it and the framework breaks immediately, which is precisely what P1-DEC-018 demonstrated. (3) `preTouch` ≈ 2x `retires` in every run, which validates the counter against the expected one-per-retire-plus-one-per-drain shape. **What is still NOT proven:** that a node *still sitting in a limbo bag* was on a page that `reclaimSlabPage` specifically reclaimed. Stale hits outnumber reclaims ~18:1, so the large majority come from ordinary slab mapping churn (dirty-bit propagation on any `freePage`/`allocPage`), not from the reclaim path. That also explains why P1-DEC-018 surfaced within ~1 iteration, which no reclaim-driven story accounted for. Proving the exact conjunction would need per-page provenance tracking; the two measurements together make it near-certain but they do not establish it. **Gating:** all of it sits behind `-DCROCOS_FRESHNESS_STATS=ON`, OFF by default — these counters answer a question you ask of a stress run, not something a shipping kernel should pay for on its retire and reclaim paths. Verified absent: `nm` finds zero counter symbols in a default image, and the liveness line prints `[stats=off]` rather than zeros. **The fields are ABSENT rather than zero on purpose:** a build reporting `stale=0` would read as a coverage regression under this phase's Verification Targets, which is the exact opposite of the truth. `ensureTLBEntryFresh`'s `bool` return is NOT gated — the branch already exists inside it, so reporting the outcome costs a register, and gating it would make the signature differ between builds for no gain. Decided 2026-08-01. |
| P4-DEC-005 | Settled | **The stress is run in a DEBUG kernel by default**, and the release configuration is validated separately rather than being the primary target. | Debug is where the detectors live: `CROCOS_RCU_DEBUG_CHECKS=1` makes every veneer and engine assert a named `PANIC`, which is how P1-DEC-018 was attributed in seconds rather than bisected. Release is still worth running — it builds and boots clean, and at ~20x the debug throughput (6.75M iterations/CPU against ~330K) it covers far more volume — but a release failure arrives as an anonymous fault, so it answers "does it survive" and not "what broke". Both configurations are exercised; only debug is diagnostic. Decided 2026-08-01. |
| P4-DEC-007 | Settled | **The kernel now reports a termination status to the host, and the exit code of `run` IS the machine-checkable pass condition.** Resolves P4-ITEM-006. The fix P4-ITEM-006 sketched and deferred was implemented rather than deferred further: a new arch-neutral `kernel::exitToHost(ExitStatus)` (`kernel/include/kexit.h`, AMD64 backend in `kernel/arch/amd64/hostExit.cpp`) replaces the open-coded `outw` at all four sites. `Success` keeps the ACPI poweroff at `0x604` and exits **0**; `Panic`, `PageFault` and `UnhandledException` write 1/2/3 to QEMU's `isa-debug-exit` at `0xf4`, which exits `(v << 1) \| 1` — **3, 5 and 7**, distinct and always odd, hence never confusable with success. `-device isa-debug-exit,iobase=0xf4,iosize=0x04` is on `run`, `run_numa` and `run_numa_hmat`, and deliberately NOT on `qmon`/`kdebug`. **Extended 2026-08-01 by P4-DEC-008 with `Hang` = 4 → exit 9.** | Measured, not assumed. Injecting each of the three faults into the shutdown callback in turn yields exactly **3 / 5 / 7**, and the three unmodified debug targets plus a Release build all still exit **0** with `Goodbye :)` and `corrupt=0`. **The exit code is a strictly stronger signal than the interim grep, not a restatement of it**: in all three injected runs `Goodbye :)` was still printed (the injection sat after the log line) and the status still reported failure — a case the old "`Goodbye` present" half of the grep would have called a pass. Three deliberate choices. (1) **The failure path never touches `0x604`**, since writing it would exit 0 and undo the entire point. (2) **The failure path keeps a trailing `cli; hlt`**, so on a target with no `isa-debug-exit` device — real hardware, or a hand-rolled QEMU line — a fault halts rather than falling through to the shutdown timer and reporting success; a hung run is a failure a human investigates, a false green is not. (3) **`qmon`/`kdebug` are excluded on purpose**: they are gdb stubs, where halting on a fault so it can be inspected beats exiting out from under the debugger. The status-drain `pause` loop that previously lived only in `panic` now covers all four paths, so a fault's serial output is no longer at risk of being cut off by the machine going away. **Scope is the whole project, not this phase**: every in-kernel test CroCOS has ever had, `VmsmallocStress` included, was previously unfalsifiable from outside, and all of them are now gateable. `-Werror` clean in Debug and Release; the userspace suite is unaffected at 1261/1261 (`panic.h` reaches the test runners, so its new call is excluded under `CROCOS_TESTING`). Decided 2026-08-01. |
| P4-DEC-008 | Settled | **A periodic timer watchdog detects hangs, and a detected hang is its own exit status: `ExitStatus::Hang` → exit 9.** Resolves P4-ITEM-005. Each CPU publishes `iteration + 1` to a cache-line-padded per-CPU slot every iteration; a self-re-enqueuing timer event samples every slot every 2 s (first sample at 3 s) and fails the run when any CPU's count is frozen across 4 consecutive samples. CPU 0 arms it. The report names the stalled CPU, its last iteration, and **every** CPU's count. | Chosen over three alternatives. A **shutdown-time comparison** was cheapest and needed no timer, but sees only stalls that persist to the end, reports after the full 20 s, and cannot fire while the stall is live. **Peer checking from the stress loop** needed no timer machinery and would let any live CPU catch any dead one, but wants a timestamp on the hot path and leaves an all-CPU stall undetected. **Self-timing** was rejected outright: a stuck CPU never reaches its own check, so it detects slowness and structurally cannot detect a hang. **Why `iteration + 1` rather than `iteration`:** it makes 0 mean "never entered the loop", which catches an AP that fails to reach `rcuStress` at all — a failure that was previously *completely* invisible, since a missing per-CPU liveness line is not something any automated check looks for. **Why a distinct status rather than `assert`:** exit 9 lets CI separate "stopped making progress" from "an invariant fired" without parsing the log. **Why 4 samples:** a healthy CPU turns over ~16K iterations/second, so four consecutive samples of *exactly zero* progress (6-8 s) is a very wide margin against QEMU/TCG vCPU starvation on an oversubscribed host — a flaky watchdog would be worse than none. **Why no stack trace:** the callback runs in the timer interrupt on a CPU that is by definition *not* the stalled one, so a trace would describe the healthy CPU and read as though it were the culprit. Getting the real one needs an async fire-and-forget IPI asking the stalled CPU to self-report — a natural second consumer for the IPI subsystem once it exists. Measured: an injected mid-run stall on CPU 3 exits **9** at 11.5 s naming `cpu=3 stalled at iteration 50000` with the other seven at ~250K; an injected never-start on CPU 5 exits **9** at 9.5 s naming `cpu=5 NEVER ENTERED`. No false positive on `run` / `run_numa` / `run_numa_hmat` or on a Release run at 20x volume. Kept private to `RcuStress.cpp` rather than generalised — see P4-ITEM-004. Decided 2026-08-01. |
| P4-DEC-009 | Settled | **`synchronize` and `barrier` are driven from all CPUs at ~1-in-1024 iterations each, for EXECUTION-ON-TARGET only — no semantic assertions — and their durations are measured.** Resolves P4-ITEM-003. Both are called from the loop body with no section held and from normal context, which is what their strict RCU-DEC-031 mask requires. A second random word selects them, so the two selectors do not overlap and "this iteration synchronizes" stays independent of "this iteration barriers". | **Liveness, not semantics, on purpose.** The torture suite already owns the semantics (`rcuTortureBarrierSemantics`, `BarrierTimesBound`, `DualBarriers`, `DeleterRetiresDuringBarrier`) and can assert exact residue and exactly-once destruction, which this phase structurally cannot. What was missing was that these two had **no in-kernel caller at all**: everything else the spike drives either returns promptly or panics, while these BLOCK, so their failure mode is a hang — and until P4-DEC-008 a hang was indistinguishable from a healthy run. Verifying destruction here would have needed a counting deleter, which deviates from RCU-DEC-039 ("a deleter may touch only state reachable solely from the retired object") to re-prove what userspace already proves; rejected on both counts. **`barrier` is the one that mattered:** its drive-to-completion predicate is non-monotone and its own comment records that BOTH obvious formulations of the loop livelock, so the argument had never faced seven other CPUs retiring continuously. It does now, ~2,700 times per 20 s run in debug and ~27,000 in release, with no livelock. **Rate tuned against measurement, not guessed:** at 1-in-4096 the means were 169 us (synchronize) / 339 us (barrier), ~0.2% of CPU time, with `reclaims`/`preTouch`/`stale` all inside noise of the P4-DEC-006 baseline; 1-in-1024 buys 4x the calls for well under 1% and `stale` held at 106,479 / 14.0% against the baseline's 97,706 / 14.2%, so the DEC-047 coverage this phase exists to provide was not traded away. **Durations are measured rather than assumed** because both calls freeze the caller's watchdog heartbeat for their whole span — deliberately, since a hung barrier is exactly what P4-DEC-008 should catch — so the margin had to be a number. Worst observed across ~15 runs is 331 ms against a threshold requiring 6 s of *sustained* zero progress, and a 331 ms stall structurally cannot span four consecutive 2 s samples. Verified the watchdog still fires on an injected stall with the blocking calls present. Decided 2026-08-01. |
| P4-DEC-010 | Settled | **The per-retire freshness call is 40 instructions on the fresh path and 49 on the stale path, against 70 for the whole retire — so it is ~59% of the retire path's INSTRUCTION count, amortised at the measured 14.2% stale rate.** Resolves P4-ITEM-002. Measured under `-icount shift=N`, which ties QEMU's virtual clock (and therefore the guest TSC) to instructions retired, behind a new `CROCOS_INSN_PROBE` build option — OFF by default, verified absent from a default image by `nm`. Mode 1 brackets the freshness call inside `onPreTouch`; mode 2 brackets the whole retire. Never both: nesting them would inflate the denominator by the inner probe's own cost. | **Why instructions and not wall-clock.** The two things that dominate this call on real silicon — a cache miss on the dirty-bitmap word, and `invlpg`'s serialisation — are exactly what TCG does not model, so a QEMU timing would have been confidently wrong. Instruction counting is the alternative P4-ITEM-002 itself named. **Why minima, not means:** interrupts are enabled on the measured paths, and a timer landing inside the bracket inflates that sample by the whole handler. This was not a theoretical worry — the first cut reported means, and at `shift=7` the empty-probe cost came out 2304 then 2539 ticks and drifted between prints against a true cost of 3 instructions. The contamination dilutes with sample count rather than averaging out, so the mean is simply the wrong statistic; the minimum over ~10^5 samples is the uncontaminated path. **Unit validated rather than assumed:** every minimum scales EXACTLY 2x from `shift=6` to `shift=7` (base 192→384, fresh 2752→5504, stale 3328→6656, retire 4672→9344), which pins ticks = 2^shift per instruction and makes the empty probe exactly 3 instructions. Cross-checked against the release disassembly by hand: `ensureTLBEntryFresh` is 28 instructions on the fresh path plus an out-of-line `arch::getCurrentProcessorID()`, and 40 − 28 = 12 for that callee including call/ret — consistent. **The headline number understates the real cost, and that direction matters.** Of the 70 retire instructions, the freshness call contributes the path's ONLY likely cache miss (the dirty word lives in the page-table region, a line nothing else in retire touches) and, on the stale path, its only serialising instruction (`invlpg`) plus a locked RMW. The other ~29 instructions are mostly register ops plus one usually-uncontended CAS. So in CYCLES on real hardware the share is very likely HIGHER than 59%, and that cannot be measured here — it needs real silicon. Decided 2026-08-01. |
| P4-DEC-011 | Settled | **LTO is enabled for RELEASE, and it cuts the retire path by a third: 70 → 46 instructions, with the freshness call 40 → 23 (fresh) and 49 → 27 (stale).** `-flto` moves into the compile flags; Debug deliberately stays non-LTO. Image size 739,264 → 513,480 bytes (−30.5%), and stress throughput +15.5% (7.80M vs 6.75M iterations/CPU). The freshness call is still ~51% of retire, down from ~59%. | User direction 2026-08-01, and the numbers justify it: `-flto` had been in `target_link_options` ALONE, which does nothing — with no LTO IR in the objects there is nothing for the link to optimize — so the kernel had **no cross-TU inlining at all**. **Two traps, both hit rather than predicted.** (1) The link line read `-flto -Os`. That `-Os` was inert while no object carried LTO IR, but with `-flto` at compile time the LINK-time level governs codegen for the whole program, so leaving it would have silently rebuilt all of Release at `-Os` instead of `-O2`. The Release link now restates `-O2` and `inline-unit-growth=500`, since under LTO the whole kernel is one unit and the default growth limit is far too tight. (2) **The build broke on `undefined reference to __stack_chk_guard`**: `-fstack-protector-strong`'s references are emitted during CODEGEN, which under LTO happens after the IPA pass that decides what to keep, so both symbols looked dead at IR level and were discarded. Fixed with `__attribute__((used))` on the definitions in `cxxcompat.cpp`. **The scare that was not one:** `.init_array` shrank from 44 entries to 9, which in this project is the signature of the silent-constructor-loss failure it has already shipped once. It is benign and the arithmetic closes exactly — 44 = 8 library + 36 kernel, 9 = 8 library + 1 merged: `CoreKernel`/`AllocKernel` are separate targets and are not LTO'd, so their 8 per-TU stubs survive individually, while LTO merged the kernel's own 36 into a single `_sub_I_65535_0.0`. Confirmed empirically by diffing the kernel boot output against a non-LTO build: **identical line for line**, modulo addresses. What LTO actually bought, read off the ISRA clone: `getCurrentProcessorID` inlined to `rdgsbase` + `movzbl` (2 instructions, replacing a ~12-instruction out-of-line call), `invlpg` inlined from a call/invlpg/ret to one instruction, and the entire prologue/epilogue eliminated. Gate: 1261/1261, all three debug targets exit 0, three Release-LTO runs exit 0 with `corrupt=0`. Decided 2026-08-01. |
| P4-DEC-012 | Settled | **The duplicate per-CPU-ID load is left in place.** Closes the remaining half of P4-ITEM-007. After P4-DEC-011 the redundant copy is `rdgsbase %rax` + `movzbl (%rax),%ecx` — 2 instructions, of which at most **1** is removable, against 46 for the whole retire (~2%) and ~23 for a `SafePtr` dereference (~4%). | Investigated on user direction ("if there's an elegant way … but if it's going to be tortured to write, let's not worry about chasing something so small") and both routes turned out tortured. **Reusing the value** — the literal ask — means `Hooks::onPreTouch` must take a CPU id, which changes the Phase-1 Hooks contract (RCU-DEC-017 / P1-DEC-009) and every implementor of it including `TestHooks` and `StallOnClaimHooks` in `tests/core`; and it buys nothing for `SafePtr::operator*`, the genuinely hot caller, which has no id to reuse. A spec-level interface change that skips the hot path. **Making the load one instruction** is the better idea and still fails on layering: the win needs explicit `%gs:`-relative addressing at the load site, GCC cannot fuse it on its own (`getCurrentCpuLocalBase` is inline asm, so the compiler cannot know that value IS the segment base for addressing purposes), and `arch/CpuLocalBase.h` states in its own header comment that the arch/portable split exists so "future ports add their backend without touching call sites" — an `#ifdef ARCH_AMD64` inline-asm block in the portable per-CPU header contradicts that head-on. Doing it properly means a new arch primitive threading the field offset down plus an ARMv8-shaped fallback, for one instruction. **And the premise is weak anyway:** P4-DEC-010 established that this call's cycle cost is dominated by the dirty-bitmap cache miss, so deleting one register-read instruction is unlikely to be measurable at all. **What WOULD be worth revisiting, and is a different change:** qualifying `CpuLocal` itself with GCC's `__seg_gs` named address space, so *every* field access through `cpuLocal()` becomes segment-relative with no base load — that reaches `interruptDepths` and the vmsmalloc `magazines`, which are far hotter than `logicalID`. It is viral through the type system (every `CpuLocal&` and `Magazine*` acquires the qualifier, and non-x86 needs a macro), so it is a real refactor to be justified by a real-silicon profile, not by this 1 instruction. Decided 2026-08-01. |
| P4-DEC-013 | Settled | **One in-kernel stress at a time is the right long-term shape, and P4-I5 stays a structural constraint rather than becoming a selection mechanism.** Closes P4-ITEM-004. | Spec author, 2026-08-01: there will only ever be one in-kernel stress live at a time, so the collision P4-ITEM-004 worried about — the next subsystem wanting its own `[[noreturn]]` per-CPU component — is not a real future. More importantly the question **dissolves rather than gets answered**: once the kernel carries a scheduler, stress workloads become ordinary threads and stop needing a boot-phase `[[noreturn]]` component at all, and eventually they move to userspace entirely. Building a stress-selection mechanism now would be designing infrastructure for a world the kernel is deliberately growing out of. P4-I5's assertion stays, because its value is not arbitration — it is that a SECOND registered component would silently never run, which is the failure mode this phase's table calls the worst available. Decided 2026-08-01. |
| P4-DEC-004 | Settled | **Liveness output is per-CPU counters every 64K iterations**, not per-operation tracing. | Enough to see all CPUs making progress and to spot a stalled one, cheap enough not to distort the workload, and small enough that a 20-second run does not drown the serial log. **Settled 2026-08-01 (was Provisional pending P4-ITEM-005): the hang detector did NOT reshape it.** P4-DEC-008 added a separate padded per-CPU heartbeat slot instead of promoting the log line into a detector, precisely so the human-facing output and the machine-facing signal could keep different rates — 64K iterations for the log, 2 s for the watchdog — and so the detector costs one uncontended relaxed store rather than a `klog` on the hot path. Decided 2026-08-01. |

## Hazards

- **Absence of a panic is a weak signal, and it is the only one this phase has.** Phase 3 can
  assert exactly-once destruction and exact residue; this phase cannot observe either. A defect
  that leaks, double-frees into a still-mapped page, or stalls reclamation without corrupting
  anything will look exactly like success. Treat green here as "no *loud* failure on the target",
  never as "validated".
- **A CPU wedged with interrupts MASKED defeats the watchdog.** P4-DEC-008 closed the general
  case — a livelock or a never-started AP now exits 9 rather than printing `Goodbye :)` and
  exiting 0 — but the detector is delivered as a timer interrupt, so a CPU stalled inside
  RCU-DEC-024's masked window cannot service it. If that CPU is the one holding the watchdog
  event, **the watchdog dies with it and the run reverts to the old behaviour exactly**:
  `Goodbye :)`, exit 0. This is stated rather than engineered around; closing it wants the
  watchdog event owned by a CPU that is not itself under stress, or a second detector on an
  independent path (peer checking, rejected in P4-DEC-008, is the obvious candidate).
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
- **The `syncUs` / `barrierUs` MAX is a host artifact and must not be read as an RCU latency
  figure.** Measured 2026-08-01 across `-smp 2 / 4 / 8`, two runs each. The **mean** is real and
  algorithmic — 41-48 us at `-smp 2`, 70-94 at 4, 144 at 8 — scaling ~2x per doubling of CPU
  count, which is EBR behaving as defined: a grace period waits for every reader to pass through,
  so more CPUs means longer. The **max** is not: run-to-run variance at *fixed* config is as large
  as variance across configs (`-smp 2` gave 2.4 ms then 20.8 ms; `-smp 8` gave 33 ms then 260 ms),
  and a 475x mean-to-max ratio with a stable mean is the signature of a participant frozen from
  outside, not of an algorithmic property — which would reproduce. The mechanism: macOS
  deschedules a QEMU vCPU thread **while that vCPU is inside a read section**, the slot stays
  active for the whole descheduling window, and every other CPU's `synchronize`/`barrier` waits on
  exactly it. Note `-smp 2` does not oversubscribe the 8-core host at all and still shows a 20 ms
  outlier. **None of this can occur in the kernel proper** — no scheduler, no preemption, and
  interrupts do not last 100 ms. Both figures are further inflated by TCG (~10-50x slower than
  native; the userspace soak measures read sections at 13.5 ns). Per this phase's second Non-Goal
  these are liveness diagnostics, not a benchmark; the soak runners are where latency claims live.
- **A residual, accepted flake risk follows from that.** A host stall long enough to freeze a CPU
  across four consecutive 2 s watchdog samples would produce a spurious exit 9. Worst observed is
  331 ms — a ~18x margin — and a 6 s macOS deschedule is implausible, so this is named rather than
  designed around.
- **`synchronize` spins rather than sleeps, and the spin is not free to the readers it waits on.**
  Each spin calls `tryAdvance`, which sweeps every slot, so a waiting CPU burns cycles and
  generates slot-array cache traffic that mildly slows the very readers whose exit it is waiting
  for. Inherent to drive-based EBR with nothing to block on — Linux's `synchronize_rcu` sleeps —
  and the correct choice today, but **worth revisiting when a scheduler lands**, alongside
  RCU-DEC-032's preemptible-drain work.
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
| A run's verdict is machine-checkable | **The exit status of the `run` target** (P4-DEC-007, P4-DEC-008): 0 clean, 3 panic, 5 page fault, 7 unhandled exception, 9 hang. Verified by injecting each in turn. The old grep — `Goodbye :)` present and no fault line — remains valid as a *secondary* check, but it is strictly weaker: the injected fault runs printed `Goodbye :)` and still exited nonzero |
| The per-retire freshness call's cost is known rather than assumed | `-icount` instruction probe (P4-DEC-010): 40 instructions fresh, 49 stale, against 70 for the whole retire. Unit validated by exact 2x scaling from `shift=6` to `shift=7`, and cross-checked against the release disassembly. **An instruction figure, not a cycle figure** — the cycle share is likely higher and needs real silicon |
| `synchronize` and `barrier` execute and RETURN on the target under sustained remote retire traffic | `syncs` / `barriers` in the liveness line (P4-DEC-009). **`barriers == 0` would mean the drive-to-completion loop — whose predicate is non-monotone and whose two obvious formulations both livelock — has still never run on hardware**; treat a drop to zero as a coverage regression. ~2,700 of each per debug run, ~27,000 per release run |
| Every CPU makes forward progress, and every CPU starts at all | The watchdog (P4-DEC-008). Verified by injecting a mid-run stall (exit 9, `cpu=3 stalled at iteration 50000`) and a never-start (exit 9, `cpu=5 NEVER ENTERED`), and by confirming no false positive across all three debug targets and a Release run |

## Testing Approach

`kernel/RcuStress.cpp`, registered as `[RcuStress]` in `kernel/general.icd`: `per_cpu = true`,
`phase = "smp_bringup"`, `depends_on = ["Shutdown"]`.

Each CPU loops: pick a node class and a shared cell by per-CPU xorshift; ~1 in 8 iterations
publishes a fresh node and `retireDestroy`s what it displaced, the rest take a `ReadGuard`,
`protect`-load the cell and verify magic + checksum through the `SafePtr`; ~1 in 64 calls
`tryAdvance` to pull grace-period progress from the mutation path (RCU-DEC-005); ~1 in 1024
calls `synchronize` and, independently, ~1 in 1024 calls `barrier` (P4-DEC-009), both with no
section held. Every iteration also publishes `iteration + 1` to a padded per-CPU heartbeat slot,
which the P4-DEC-008 watchdog samples on a timer.

**Reading the liveness line:** `iter` is per-CPU (a local counter); **every other field is a
GLOBAL total across all CPUs** — they are plain global atomics, and each CPU prints the shared
value at the moment it happens to reach a 64K boundary. That is why `reads + writes` sums to
`iter x CPU count` rather than to `iter`. An earlier draft of this section read them as per-CPU
and overstated read/retire volume eightfold.

**Running.**

```bash
# Coverage instrumentation (P4-DEC-006) is opt-in and off by default:
cmake -B build-stats -DCMAKE_BUILD_TYPE=Debug -DCROCOS_FRESHNESS_STATS=ON

# Retire-path instruction probe (P4-DEC-010), also off by default. Mode 1 is the
# freshness call, mode 2 the whole retire; never both. ONLY meaningful under
# -icount, which ties the guest TSC to instructions retired. Reported figures are
# MINIMA, and `probeBase` (an empty rdtsc pair, 3 instructions) is the offset to
# subtract. Divide ticks by 2^shift to get instructions.
cmake -B build-probe1 -DCMAKE_BUILD_TYPE=Release -DCROCOS_INSN_PROBE=1
qemu-system-x86_64 -icount shift=6 -kernel build-probe1/kernel/Kernel \
    -no-reboot -nographic -smp 8 -m 256M -cpu qemu64,+fsgsbase \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04

cmake --build cmake-build-debug --target run            # 8 CPUs, single domain
cmake --build cmake-build-debug --target run_numa       # 3 NUMA domains
cmake --build cmake-build-debug --target run_numa_hmat  # + HMAT latency/bandwidth
```

**The exit status is the verdict** (P4-DEC-007, P4-DEC-008) — `0` clean, `3` panic, `5` page
fault, `7` unhandled exception, `9` hang — so a CI gate is just the target's own exit code, with
no output parsing:

```bash
cmake --build cmake-build-debug --target run || echo "stress FAILED with status $?"
```

Observed on all three (debug): ~330-390K iterations **per CPU**, and — as **global** totals —
~2.3-2.7M protected reads, ~330-380K retires, ~2,700 `synchronize` and ~2,700 `barrier`
completions, `corrupt=0`, clean `Goodbye :)`, exit `0`.

Release is **LTO** as of P4-DEC-011; Debug is not, so the two configurations now differ in more
than assertions. Release (`-DCMAKE_BUILD_TYPE=Release`) builds and boots clean at roughly 20x the volume —
6.75M iterations per CPU, and globally 46.6M protected reads, 6.65M retires, ~27,000 of each
blocking primitive — with the debug-check symbols absent from the image. Per P4-DEC-005 that is a
volume run, not a diagnostic one.

`syncUs` / `barrierUs` print as `mean/max` microseconds. **Read the mean; the max is a host
artifact** — see the Hazards entry before quoting either number anywhere.

## References

- `specs/rcu.md` — parent; **ITEM-021** is this phase's charter.
- `specs/rcu-phase-1.md` — **P1-DEC-018**, the defect this phase found and the fix.
- `specs/rcu-phase-3.md` — the userspace gate this complements; its first Non-Goal deferred
  in-kernel stress to "a later phase, planned separately".
- `specs/vmsmalloc-phase-9.md` — the precedent, and the component this one replaces.
- `docs/vmsmalloc-stale-tlb-bug.md` — DEC-047, the bug class `onPreTouch` exists to guard.
