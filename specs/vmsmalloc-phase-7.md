---
kind: leaf
status: drafting
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 7 — Assertion paths + interrupt-context tracking

> Pin the entry-point assertions and compile-time alignment checks that Phase 5 and Phase 6
> deferred; design + implement the consumer side of the per-CPU interrupt-context tracking
> subsystem that DEC-014's debug-only context check requires. The `kernel::CpuLocal`
> infrastructure and `InterruptContextDepths` *struct* are defined in **Phase 4.5** (which
> lands before Phases 5 and 6 because they consume `cpuLocal().magazines[c]`); Phase 7
> extends `InterruptContextDepths.h` with the consumer logic (kind enum, `kindForVector`,
> `InterruptContextGuard` RAII) and integrates the guard into `dispatchInterrupt`. All
> runtime asserts are **debug-only**. Scope:
> (a) `vmsmalloc(0)` / `vmsmalloc(size > pageSize)` / `vmsfree(nullptr)` asserts (DEC-023 +
> DEC-004 hoist); (b) the DEC-014 "not in IRQ/NMI/#GP/#MC/#UD/#DF context" assert with #PF
> permitted per ITEM-052 — built on top of Phase 4.5's `InterruptContextDepths`; (c) DEC-030's
> preempt-disable / CPU-pinned placeholder predicates (split, both stubbed `return true`
> pending a future scheduler); (d) DEC-025's `static_assert(alignof(T) <= slotAlignment(class))`
> on `VMSubstrate::make<T>`; (e) the DEC-028-amendment convention-internal comment at
> `vmsmalloc` / `vmsfree`'s public declarations; (f) the interrupt-context-tracking
> *consumer* subsystem: `InterruptKind` enum, `kindForVector` constexpr mapping, the
> `InterruptContextGuard` RAII type, and the `dispatchInterrupt` integration that fires
> the guard. Harness-based negative tests cover the runtime asserts via the exception-based
> assertion trap (P1-ITEM-002 resolved).

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No new validation logic** beyond what the parent spec's decisions specify. Phase 7's
  scope is *wiring* the asserts that already-resolved decisions call for; not inventing new
  checks.
- **No release-build assert behavior.** Per user direction 2026-05-27, all runtime asserts
  in Phase 7 are debug-only — including the `size > 0`, `size <= pageSize`, `p != nullptr`
  checks that an earlier draft of P7-DEC-001 had as release-active. Release builds trust
  the caller-side contract; bad inputs in release produce silent misbehavior (e.g.,
  `vmsmalloc(8000)` returns a 4 KiB page, the caller writes past page bounds). Acceptable
  per the kernel's "trust callers, check in debug" stance. The pre-existing Phase-5
  `size <= pageSize` check on the DEC-029 bypass branch moves to debug-only for
  consistency.
- **No preemptive-scheduler primitives.** DEC-030's `preemptionDisabled()` / `cpuPinned()`
  predicates are introduced as two separate stubs (P7-DEC-003 amended). CroCOS has no
  scheduler today, so both stubs `return true` unconditionally; Phase 7 wires the call
  sites so a future scheduler implementation only needs to fill in the predicate bodies.
  Splitting the predicate (vs. combining into one) names the two distinct caller
  obligations DEC-030 imposes; future scheduler design can unify them transparently if it
  chooses.
- **No general-purpose interrupt-context API.** The interrupt-context tracking subsystem
  Phase 7 introduces is a vmsmalloc-driven addition — Phase 7's only consumer is the
  `inForbiddenContextForVmsmalloc()` predicate. The per-CPU depth counters are designed to
  be reusable by future kernel code (e.g., a scheduler's preempt-context check) but
  Phase 7 doesn't introduce additional consumers or test cases for them.
- **No CpuLocal infrastructure changes** — that's Phase 4.5's scope. Phase 7 *consumes*
  `kernel::cpuLocal()` (introduced by 4.5) and the `InterruptContextDepths` struct
  (defined as struct only in 4.5; Phase 7 adds the kind/mapping/guard logic to the
  same header). Phase 7 does not modify `kernel::CpuLocal`'s field list, the
  `arch::setCurrentCpuLocalBase` interface, the AMD64 GSBase backend, or the SMP
  bringup paths — all of which are Phase 4.5 territory.
- **No `vmsmalloc(0)` / `vmsfree(nullptr)` policy change.** DEC-023 specifies "assert,
  don't paper over with malloc-style leniency". Phase 7 implements the (debug-only)
  assert; it does not introduce a "return unique pointer for size=0" alternative.
- **No new test infrastructure.** Phase 7 reuses the existing harness — assertions are
  caught via the harness's exception-based trap (P1-ITEM-002 resolved 2026-05-27).
- **No `vmsfree` post-validation assertions** beyond what Phase 6 already shipped. DEC-026
  steps 6/6a/6b are Phase 6's responsibility. Phase 7 only adds the *entry-point* asserts
  (nullptr, IRQ context, preempt/pin).
- **No additional `vmsfree` arithmetic checks.** The slot-range and modulo checks are
  already in Phase 6.
- **No retrofit of existing kernel interrupt entry assembly.** The depth-counter
  increment happens inside `dispatchInterrupt`'s C++ body, not in the IDT entry assembly
  stubs. Trade-off accepted (P7-DEC-008): a ~few-dozen-instruction window where the depth
  counter is out-of-date relative to the actual interrupt state, in exchange for keeping
  the increment logic out of architecture-dependent assembly. The increment fires before
  any handler runs, so DEC-014's enforcement is effective for every consumer that runs
  via the registered-handler path.

## Consumer Contract

### Changes to `kernel/mm/vmsmalloc.cpp`

The `vmsmalloc` and `vmsfree` bodies are extended with entry-point assertions. Source
order at the head of each function:

**`vmsmalloc(size_t size)`:**

```cpp
void* vmsmalloc(size_t size) {
    // All asserts below are debug-only (P7-DEC-001 amended 2026-05-27). Release builds
    // trust the caller-side contract; bad inputs produce silent misbehavior.

    assert(size > 0, "vmsmalloc: size must be positive");                      // DEC-023
    assert(size <= arch::smallPageSize, "vmsmalloc: size exceeds page");        // DEC-004
    assert(!inForbiddenContextForVmsmalloc(),                                   // DEC-014
           "vmsmalloc: illegal in IRQ/NMI/#GP/#MC/#UD/#DF context");
    assert(preemptionDisabled(),                                                // DEC-030
           "vmsmalloc: caller must hold preempt-disable");
    assert(cpuPinned(),                                                         // DEC-030
           "vmsmalloc: caller must be pinned to current CPU");

    // Phase 5 / Phase 6 body follows unchanged.
    // ... (sizeClassIndex dispatch, DEC-029 bypass, fast path, etc.)
}
```

**`vmsfree(void* p)`:**

```cpp
void vmsfree(void* p) {
    // All asserts below are debug-only (P7-DEC-001 amended 2026-05-27).

    assert(p != nullptr, "vmsfree: pointer must be non-null");                  // DEC-023
    assert(!inForbiddenContextForVmsmalloc(),                                   // DEC-014
           "vmsfree: illegal in IRQ/NMI/#GP/#MC/#UD/#DF context");
    assert(preemptionDisabled(),                                                // DEC-030
           "vmsfree: caller must hold preempt-disable");
    assert(cpuPinned(),                                                         // DEC-030
           "vmsfree: caller must be pinned to current CPU");

    // Phase 6 body follows unchanged (DEC-026 validation chain).
    // ... (range check, DEC-029 dispatch, ensureTLBEntryFresh, magic, slot-range, ...)
}
```

The helper predicates are defined at file scope in `vmsmalloc.cpp`:

```cpp
namespace {
    // Returns true if the current execution context is one Phase-7 forbids.
    // #PF is conditionally legal (DEC-014 amended / ITEM-052) and is NOT included.
    // Reads the per-CPU depth counters introduced by Phase 7's interrupt-context
    // tracking subsystem.
    inline bool inForbiddenContextForVmsmalloc() {
        const auto& d = kernel::interrupts::currentCpuInterruptDepths();
        return d.irq > 0 || d.nmi > 0 || d.ud > 0
            || d.df  > 0 || d.gp > 0 || d.mc > 0;
    }

    // DEC-030 placeholder predicates (P7-DEC-003 amended). Both return true today;
    // future scheduler updates the bodies. Two predicates rather than one names the
    // distinct contract obligations explicitly.
    inline bool preemptionDisabled() {
        // TODO(DEC-030, future scheduler): replace with per-CPU preempt-count check.
        return true;
    }
    inline bool cpuPinned() {
        // TODO(DEC-030, future scheduler): replace with per-thread migrate-disable
        // check (or equivalent — see DEC-030 for the decoupled vs combined design
        // discussion).
        return true;
    }
}
```

### CpuLocal infrastructure — provided by Phase 4.5 (no Phase 7 changes)

Phase 4.5 establishes the per-CPU `kernel::CpuLocal` struct, the
`arch::setCurrentCpuLocalBase` / `getCurrentCpuLocalBase` portable interface, the
AMD64 GSBase backend, the `cpuLocalReady` flag and `cpuLocal()` accessor, the
`getLogicalProcessorID` rewrite, and the VMSubstrate-init + AP-bootstrap integration.
Phase 7 consumes `kernel::cpuLocal()` (for the forbidden-context predicate) and adds
fields to `InterruptContextDepths` (kind enum / mapping / guard) — but does NOT modify
`kernel::CpuLocal`'s struct layout or the GSBase backend. See `specs/vmsmalloc-phase-4.5.md`
for the infrastructure design.

All of the above is delivered by Phase 4.5 (`vmsmalloc-phase-4.5.md`). Phase 7
consumes it.

### Interrupt context tracking — consumer logic (struct comes from Phase 4.5)

Phase 4.5 ships the `InterruptContextDepths` struct (declared in
`kernel/include/interrupts/InterruptContextDepths.h`) as a field of `kernel::CpuLocal`.
Phase 7 extends the same header with the consumer logic — `InterruptKind` enum,
`kindForVector` constexpr mapping, and `InterruptContextGuard` RAII — and integrates the
guard into `dispatchInterrupt`.

**Per-CPU storage from Phase 4.5** (referenced here for context; the struct definition
itself lives in Phase 4.5's spec):

```cpp
namespace kernel::interrupts {

    // (Defined in Phase 4.5; reproduced here for context.)
    struct alignas(arch::cacheLineSize) InterruptContextDepths {
        uint32_t irq, nmi, ud, df, gp, mc, pf;
    };

    // Read-only accessor on the calling CPU; cheap (single per-CPU-base load + offset).
    // Implemented as `return kernel::cpuLocal().interruptDepths;` — reads through the
    // CpuLocal struct established by the GSBase-pointer refactor.
    inline const InterruptContextDepths& currentCpuInterruptDepths() noexcept {
        return kernel::cpuLocal().interruptDepths;
    }

    // Vector → kind mapping. Returns a sentinel "Other" for benign exceptions (#DE / #DB
    // / #BP / #OF / #BR / #NM / #TS / #NP / #SS / #MF / #AC / #XM / #VE / #CP and other
    // unhandled vectors < 32) that we don't currently track in their own counter.
    enum class InterruptKind : uint8_t { Other, IRQ, NMI, UD, DF, GP, MC, PF };
    constexpr InterruptKind kindForVector(uint8_t vec);

    // RAII guard. Constructed at the top of dispatchInterrupt; destructor decrements.
    // No-op for InterruptKind::Other (we don't store a counter for it).
    class InterruptContextGuard {
    public:
        explicit InterruptContextGuard(InterruptKind k) noexcept;
        ~InterruptContextGuard() noexcept;
        InterruptContextGuard(const InterruptContextGuard&) = delete;
    private:
        InterruptKind kind_;
    };

}  // namespace kernel::interrupts
```

**Integration with `dispatchInterrupt`** (modify
`kernel/interrupts/InterruptRoutingAndDispatch.cpp:317`):

```cpp
void dispatchInterrupt(arch::InterruptFrame& frame) {
    // Phase 7: increment the per-CPU depth counter for this interrupt kind BEFORE
    // any handler runs. Interrupts are still disabled at this point (the IDT
    // entry path keeps IF clear until the body explicitly re-enables it or iret
    // restores it). The guard's destructor decrements at function exit.
    kernel::interrupts::InterruptContextGuard ctx(
        kernel::interrupts::kindForVector(frame.vector_index));

    // Existing body unchanged:
    auto& eoiBehavior = eoiBehaviorTable[frame.vector_index];
    if (eoiBehavior.triggerType == ...) { /* issue EOI */ }
    if (handlersByVector[frame.vector_index].get() != nullptr) {
        for (auto& handler : *handlersByVector[frame.vector_index]) {
            if (handler) (*handler)(frame);
        }
    }
}
```

**Concurrency / ordering.** The depth counters are CPU-local — only the calling CPU
reads or writes them. No atomics, no memory barriers. The increment/decrement happens
with interrupts disabled (CPU enters dispatchInterrupt with IF clear), so the read in
the predicate sees a consistent value from any non-interrupt code path. A nested
interrupt (e.g., #PF firing during an IRQ handler that has re-enabled interrupts) sees
the outer counter still incremented and increments its own counter — both layers visible
to predicates inside the nested handler.

**Out-of-date window** (P7-DEC-008 hazard). Between the CPU's IDT entry (which switches
to interrupt context) and the construction of `InterruptContextGuard` in
`dispatchInterrupt`'s body, the per-CPU depth counter is *not yet* incremented — the
register-save assembly and the call into `dispatchInterrupt` run in interrupt context
without that being reflected in the counter. Any code that runs during this window and
queries the predicate sees stale "false" — would conclude "not in interrupt context"
when in fact we are. Accepted trade-off for keeping the increment logic out of
arch-dependent assembly. No `vmsmalloc` / `vmsfree` call occurs during this window
(register save + dispatch jump is bounded to a few dozen instructions of generated
code, all known-safe).

### Changes to `kernel/include/mem/VMSubstrate.h`

Two header-level edits.

**(1) DEC-025 — compile-time alignment check on `make<T>`:**

```cpp
// At the top of VMSubstrate.h:
#include <mm/VMSubstrateSlab.h>   // for sizeClassFor, slotAlignment

// Replace lines 46-50:
template <typename T, typename... Ts>
SafePtr<T> make(Ts&&... args) {
    constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
    static_assert(c < vmsmalloc::kNumSizeClasses ||
                  sizeof(T) <= arch::smallPageSize,
                  "VMSubstrate::make<T>: sizeof(T) exceeds page");
    // For slab-backed allocations (c is a valid class), enforce the alignment cap.
    static_assert(c >= vmsmalloc::kNumSizeClasses ||
                  alignof(T) <= vmsmalloc::slotAlignment(c),
                  "VMSubstrate::make<T>: alignof(T) exceeds slot alignment of its size class. "
                  "Either pad T into a power-of-two size class, split the over-aligned "
                  "subobject, or use a different allocator.");
    auto* mem = vmsmalloc(sizeof(T));
    return new (mem) T(forward<Ts>(args)...);
}
```

The `static_assert` is two-pronged because `c == kNumSizeClasses` is the DEC-029
whole-page bypass sentinel; the alignment cap only applies to slab-backed allocations.
For whole-page allocations the alignment is `pageSize` (DEC-029), strictly stronger than
any reasonable `alignof(T)`, so no check is needed there.

**(2) DEC-028 amendment — convention-internal comment:**

```cpp
// Replace lines 21-22 in VMSubstrate.h:

// Convention-internal: external callers should prefer make<T> / destroy<T>.
// These are unavoidably declared here because make<T> / destroy<T> are templates
// whose bodies must live in this public header; structural hiding is not possible
// under the template-visibility constraint. See parent-spec DEC-028 (amended).
void* vmsmalloc(size_t size);
void vmsfree(void*);
```

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 5 — `vmsmalloc` body in `kernel/mm/vmsmalloc.cpp` | Phase 7 extends the function's entry-point asserts in-place; also demotes Phase-5's existing `size <= pageSize` check on the DEC-029 bypass branch from release-active to debug-only. | Yes — Phase 5 must land. |
| Phase 6 — `vmsfree` body in the same TU | Phase 7 extends `vmsfree`'s entry-point asserts in-place. | Yes — Phase 6 must land. |
| `kernel/interrupts/InterruptRoutingAndDispatch.cpp:317` — `dispatchInterrupt(frame)` | Phase 7 modifies the body to construct an `InterruptContextGuard` at the top. The existing function body (EOI, registered-handler invocation) is unchanged. | Yes — live. |
| Per-CPU data infrastructure | Phase 7 **replaces** the existing GSBase-low-8-bits scheme with a full GSBase-pointer-to-`CpuLocal` (P7-DEC-010). `InterruptContextDepths` sits inside `CpuLocal`. | Refactored in Phase 7 itself. |
| `kernel/arch/amd64/smp/smp.cpp:18–31` — `setLogicalProcessorID` / `getLogicalProcessorID` | Phase 7 removes `setLogicalProcessorID` and rewrites `getLogicalProcessorID` to read through `kernel::cpuLocal()`. Sole consumers of the GSBase scheme; the refactor is contained. | Refactored in Phase 7. |
| `kernel/arch/amd64/smp.h` declarations | Phase 7 removes the `setLogicalProcessorID` declaration. | Refactored in Phase 7. |
| `PageAllocator::allocateSmallPage(cpu)` — NUMA-aware page allocation | Used at SMP-init time to allocate each CPU's `CpuLocal` page on `NUMAPolicy::domainFor(cpu)`. Per Phase 1 ITEM-051, callable from the BSP for AP CPU IDs before AP bringup. | Yes — live. |
| `NUMAPolicy::domainFor(cpu)` | Used to compute placement domain for `allocateSmallPage`. | Yes — live. |
| SMP bringup path (BSP init + AP startup) | Phase 7 adds the `arch::setCurrentCpuLocalBase(page)` call at each CPU's earliest C++-reachable init point. Confirm the existing bootstrap-routine hook at step 1. | Refactored in Phase 7. |
| `kernel/include/arch/amd64/amd64.h` — vector-number constants (NMI, #UD, #DF, #GP, #MC, #PF) | The `kindForVector` mapping references these constants. **Action item:** confirm the names at step 1 (e.g., `arch::VECTOR_NMI = 2`); if absent, the mapping uses the AMD64-architectural-spec values directly with a comment. | Yes — confirm at step 1. |
| `kernel::mm::vmsmalloc::kNumSizeClasses` / `sizeClassFor` / `slotAlignment` (Phase 2 P2-DEC-002) | Consumed by the `make<T>` static_assert. | Yes — Phase 2 must be in. |
| Test harness assertion-trap support (P1-ITEM-002 resolved) | The harness's `assert` throws an exception, catchable from test code. Used for negative tests. | Yes — live per user 2026-05-27. |
| Kernel `assert(...)` macro | Phase 7 uses this for every runtime check (all debug-only per P7-DEC-001 amended). The macro is presumed to compile to no-op in release builds per the kernel's existing build flags. | Yes — live. |

## Invariants

<!-- Conditions Phase 7's code must preserve at all times. -->

- **All runtime asserts are debug-only.** Release builds elide every `assert(...)` at the
  entry of `vmsmalloc` / `vmsfree` (per the kernel's existing assert macro semantics).
  Bad inputs in release produce silent misbehavior — caller-side contract is trusted.
- **Source-order discipline at function entry.** The asserts at the top of both
  `vmsmalloc` and `vmsfree` run in this order:
  1. Argument-validity (`size > 0` for vmsmalloc; `p != nullptr` for vmsfree).
  2. Size-bound (`size <= pageSize` — vmsmalloc only).
  3. Forbidden-context predicate (`!inForbiddenContextForVmsmalloc()`).
  4. `preemptionDisabled()`.
  5. `cpuPinned()`.
  
  All five are debug-only. Order is documentation-driven (argument-validity first, then
  context contract, then scheduler contract).
- **#PF context is NOT in `inForbiddenContextForVmsmalloc()`** per DEC-014's amendment
  (ITEM-052). A page-fault handler may call vmsmalloc / vmsfree provided the caller-side
  obligation (no transitive page faults in vmsmalloc/VMSubstrate/RadixVM) holds. Phase 7's
  predicate explicitly excludes the `pf` depth counter from the check.
- **#UD and #DF added to the forbidden-context list** alongside the original IRQ/NMI/#GP/#MC.
  Per user direction 2026-05-27. Same rationale: same-CPU reentry hazard is identical to
  the others. Parent-spec DEC-014's wording is implicitly extended.
- **The preempt-disable and CPU-pinned predicates are debug-only AND vacuous today.**
  Both return `true` unconditionally pending a preemptive scheduler. When the scheduler
  lands, the predicate bodies are updated independently; the assert sites do not change.
- **Per-CPU depth counters are CPU-local — no atomics needed.** Only the calling CPU
  reads or writes its own counters; the interrupt-disabled entry-and-exit window of
  `dispatchInterrupt` guarantees no concurrent access. The accessor `currentCpuInterruptDepths()`
  returns a const reference; no mutation by predicate callers.
- **`InterruptContextGuard` increments BEFORE handlers run and decrements AFTER.** RAII
  ensures correct ordering even if a handler throws (kernel has no exceptions, but the
  pattern is still load-bearing for early returns). The guard's destructor is `noexcept`.
- **`InterruptKind::Other` is a no-op for the guard.** Benign exceptions (#DE / #DB /
  #BP / etc.) that we don't track in their own counter don't increment anything; the
  guard's constructor/destructor return immediately.
- **DEC-025's `static_assert` fires at compile time on the caller, not at runtime.** The
  `make<T>` template instantiation for an over-aligned `T` produces a clean compile error
  with a directive ("pad, split, or use a different allocator"). No runtime check is needed.
- **DEC-028 convention comment changes no code behavior.** The comment at the
  `vmsmalloc` / `vmsfree` declarations is purely advisory.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `vmsmalloc(0)` | Asserts at the `size > 0` check (release + debug). | No (caller bug) |
| `vmsmalloc(arch::smallPageSize + 1)` | Asserts at the `size <= pageSize` check (release + debug). | No (caller bug) |
| `vmsmalloc(...)` called from IRQ context | Asserts at the forbidden-context check (debug only); release builds silently corrupt magazine state per parent-spec hazard. | No (caller bug) |
| `vmsmalloc(...)` called from NMI / #GP / #MC context | Same as IRQ — asserts debug; release-build corruption per parent hazard. | No (caller bug) |
| `vmsmalloc(...)` called from #PF context with no transitive re-entrancy | Permitted per DEC-014 amended. The forbidden-context predicate excludes #PF. | Normal path |
| `vmsmalloc(...)` called from #PF context that transitively re-enters | Caller-side bug (DEC-014 amended ITEM-052). Phase 7 does not detect it; release builds corrupt magazine state. Caller-side discipline. | No (caller bug; undetected) |
| `vmsfree(nullptr)` | Asserts at the nullptr check (release + debug). | No (caller bug) |
| `vmsfree(...)` from IRQ/NMI/#GP/#MC | Asserts debug-only; same as vmsmalloc. | No (caller bug) |
| `vmsfree(...)` from #PF with no transitive re-entrancy | Permitted. | Normal path |
| Future preemptive scheduler lands; predicate updated to check actual preempt state | If a caller violates DEC-030, the debug-only `preemptionDisabledAndCpuPinned()` assert fires; release builds rely on caller discipline. | No (caller bug) |
| `make<T>` instantiated with `T` whose `sizeof(T) > pageSize` | Compile error via the size static_assert. | No (caller bug) |
| `make<T>` instantiated with `T` whose `alignof(T) > slotAlignment(sizeClassFor(sizeof(T)))` | Compile error via the alignment static_assert with the "pad, split, or use a different allocator" message. | No (caller bug) |
| External code calls `vmsmalloc` directly (bypassing `make<T>`) | Compiles cleanly (the function is declared in the public header per DEC-028 amended). The convention-internal comment is purely advisory; no enforcement. Caller takes on the freshness obligation (would need to call `ensureTLBEntryFresh` manually on cross-CPU access — undocumented elsewhere because the contract assumes consumers use `make<T>`). | Caller bug, but compiles |

## Questions

<!-- Open questions for resolution during implementation. -->

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P7-ITEM-001 | Resolved 2026-05-27 → new subsystem (P7-DEC-008) | | | Does the kernel already expose interrupt-context predicates? | Resolved: no, it doesn't. Phase 7 designs and implements a per-CPU interrupt-context-tracking subsystem as part of its scope (see P7-DEC-008). Per-type depth counters in per-CPU data; `InterruptContextGuard` RAII at the top of `dispatchInterrupt`; `inForbiddenContextForVmsmalloc()` reads the counters. Includes #UD and #DF in addition to the original IRQ/NMI/#GP/#MC/#PF set. |
| P7-ITEM-002 | Resolved 2026-05-27 (moot under debug-only) | | | Does the kernel have separate `kassert` (release-active) and `kassert_debug` macros? | Resolved (moot): per user direction 2026-05-27, all Phase 7 runtime asserts are debug-only, so only one macro is needed — whatever the kernel's existing `assert(...)` provides. No special-case logic needed for release-active checks. |
| P7-ITEM-003 | Resolved 2026-05-27 (leave as-is) | | | Should the `make<T>` static_assert also catch whole-page allocations with `alignof(T) > pageSize`? | Resolved: leave as-is with the existing short-circuit. The only consumer of vmsmalloc is RadixVM (user direction 2026-05-27), which does not request alignments beyond cache-line / pow2-class. If the static_assert ever fires on a future consumer with `alignof(T) > pageSize`, the fix is a one-line workaround at the caller (split the over-aligned subobject or use a different allocator) — same workaround the original alignment cap recommends. Not worth a second static_assert that protects against a contingency we can equally easily catch via the missing-defense's loud symptom. |
| P7-ITEM-004 | Resolved 2026-05-27 → P7-DEC-003 amended | | | DEC-030 placeholder: one combined predicate or two separate ones? | Resolved: two predicates (`preemptionDisabled()` and `cpuPinned()`), both stubbed to `return true` today. The assertion calls them as `assert(preemptionDisabled() && cpuPinned())`. Future scheduler integration updates the two function bodies independently; if CroCOS eventually adopts the combined model (where `preemptDisable` implies migrate-disable), one predicate becomes vestigial but the call site is unchanged. |

## Decisions

<!-- Settled decisions specific to Phase 7. -->

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P7-DEC-001 | Settled (amended 2026-05-27 — all debug-only) | **All Phase-7 runtime asserts at the entry of `vmsmalloc` / `vmsfree` are debug-only.** Includes the argument-validity checks (`size > 0`, `size <= pageSize`, `p != nullptr`), the forbidden-context check, and the preempt-disable / CPU-pinned checks. Phase-5's existing DEC-029-bypass-branch `kassert(size <= pageSize)` is also demoted to debug-only for consistency. | Per user direction 2026-05-27. Release builds trust the caller-side contract — vmsmalloc's only consumer is RadixVM, whose call sites are tightly controlled. The cost of release-active checks (one comparison per call on the hot path) isn't justified for caller-side discipline violations that should never reach production. Diagnostic-resistance in release is a documented hazard; in practice the kernel's other crash modes (page faults on out-of-range pointers, etc.) make bad-input bugs loud enough. |
| P7-DEC-002 | Settled (amended 2026-05-27 — added #UD, #DF) | **`inForbiddenContextForVmsmalloc()` returns true iff context ∈ {IRQ, NMI, #UD, #DF, #GP, #MC}.** #PF is excluded per DEC-014 amended (ITEM-052). The predicate reads per-CPU `InterruptContextDepths` from the new interrupt-context-tracking subsystem (P7-DEC-008); a non-zero counter in any of the six tracked-as-forbidden kinds returns true. | DEC-014's amendment was explicit about #PF's conditional legality. The user added #UD and #DF to the forbidden set 2026-05-27 — same-CPU reentry hazard is identical to IRQ/NMI/#GP/#MC. The predicate's name makes the inclusion list explicit at the assertion site. |
| P7-DEC-003 | Settled (amended 2026-05-27 — two predicates) | **DEC-030's caller-side contract is asserted via two separate predicates** `preemptionDisabled()` and `cpuPinned()`, both stubbed to `return true` unconditionally with a shared TODO comment. The assertion site reads `assert(preemptionDisabled() && cpuPinned())`. Future scheduler integration updates the two predicate bodies independently. | Resolves P7-ITEM-004 with the split form. Names DEC-030's two distinct caller obligations at the call site. If CroCOS eventually adopts the combined preempt-implies-no-migration model, one of the two predicates becomes vestigial (always returns true), but the call site is unchanged — better forward-compatibility than the combined form. |
| P7-DEC-004 | Settled | **The `make<T>` `static_assert`s are two-pronged**: one for `sizeof(T) <= pageSize` (the DEC-004/DEC-029 bound) and a separate one for `alignof(T) <= slotAlignment(c)` (the DEC-025 alignment cap). The alignment check short-circuits if `c == kNumSizeClasses` (whole-page allocation per DEC-029, which provides page-alignment automatically — stronger than any practical `alignof(T)` for vmsmalloc's only consumer, RadixVM). Per P7-ITEM-003's 2026-05-27 resolution: no third static_assert for `alignof(T) > pageSize` on the whole-page branch — left as a workaroundable contingency. | Two static_asserts give a clearer diagnostic message than one combined check. The whole-page short-circuit accepts the (extremely rare) corner where a future consumer requests over-page alignment; the fix would be a caller-side change. Not worth pre-empting. |
| P7-DEC-005 | Settled | **The DEC-028 convention-internal comment is added at the `vmsmalloc` / `vmsfree` declarations in `VMSubstrate.h`, citing the parent-spec DEC-028 amendment.** No `[[deprecated]]` annotation — the functions are not deprecated, just convention-internal. | A comment is the right shape per DEC-028's amended rationale: structural hiding is impossible under template visibility; the comment is the only documentation surface that future code reviewers will see. Calling the functions directly compiles cleanly (no `[[deprecated]]` warning), which matches the spec's "convention only" framing. |
| P7-DEC-006 | Settled | **Negative tests live in `tests/kernel/vmsmalloc/AssertionsTest.cpp`** (extends the directory introduced by Phase 2's `SlabLayoutTest.cpp`). Each assertion gets one positive test (the trigger input causes the harness to catch the assertion-failure exception) and the existing positive smoke tests confirm the function returns correctly for valid inputs. | Co-located with the other vmsmalloc tests, matches the project's existing tests/ structure (per CLAUDE.md). Uses the harness's existing exception-based assertion-trap mechanism (P1-ITEM-002 resolved). |
| P7-DEC-007 | Settled | **Phase 7 also adds `#include <mm/VMSubstrateSlab.h>` to `kernel/include/mem/VMSubstrate.h`** (needed for `sizeClassFor` / `slotAlignment` / `kNumSizeClasses` at the `make<T>` static_assert). This is the consequence of the DEC-028 amendment leaving `vmsmalloc` / `vmsfree` in the public header: the static_assert needs the size-class accessors, and those come from `VMSubstrateSlab.h`. | The include pulls a handful of constexpr accessors and the `SlabDescriptorBase` declaration into translation units that include `VMSubstrate.h`. Acceptable — the original DEC-028 rationale ("narrow public API") was already revoked by the amendment; the size-class constants are not load-bearing for any non-vmsmalloc consumer. Watch for compile-time impact on TUs that don't use slab allocation — adding a constexpr-only header should be near-free. |
| P7-DEC-008 | Settled (added 2026-05-27 — per-CPU interrupt-context tracking) | **Phase 7 introduces a per-CPU interrupt-context-depth tracker.** Layout: `InterruptContextDepths { uint32_t irq, nmi, ud, df, gp, mc, pf; }` (28 B) lives in per-CPU storage. Integration: an `InterruptContextGuard` RAII object is constructed at the top of `kernel::interrupts::dispatchInterrupt` (currently at `InterruptRoutingAndDispatch.cpp:317`) with the appropriate `InterruptKind` derived from `frame.vector_index` via a `kindForVector(vec)` constexpr mapping. The guard's constructor increments the matching counter; the destructor decrements. Predicates (`inForbiddenContextForVmsmalloc()` and the per-kind `inIRQContext()` etc.) read the counters via `currentCpuInterruptDepths()`. CPU-local, no atomics. | Resolves P7-ITEM-001. The depth-counter approach (vs. a bitmap-flag or hierarchical packed counter) is forward-flexible: nested interrupts (e.g., #PF during an IRQ handler that re-enabled interrupts) increment their own counter cleanly; a future preempt-disable counter slots in alongside without rework. The RAII guard in `dispatchInterrupt`'s body (vs. assembly stubs) keeps the increment logic out of architecture-dependent code at the cost of a small "out-of-date window" between the CPU's IDT entry and the guard's construction (P7-DEC-009 hazard); the trade is worth it because the alternative requires touching the IDT macro generator and ties the subsystem to AMD64. |
| P7-DEC-009 | Settled (added 2026-05-27 — out-of-date window accepted) | **The `dispatchInterrupt`-based increment leaves a "context depth out-of-date" window** between the CPU's IDT entry (when the CPU is physically in an interrupt) and the construction of `InterruptContextGuard` (when the per-CPU counter reflects it). The window covers the register-save assembly stub and the function-call jump into `dispatchInterrupt`. No vmsmalloc / vmsfree call occurs during this window (the register save + jump is bounded and known-safe); the window is "academic" per user 2026-05-27. | Trade-off: keep the increment in architecture-portable C++ at the cost of a small out-of-date window. The alternative (incrementing in the assembly stubs) buys correctness from the very first instruction but ties the subsystem to the architecture-specific IDT macro generator. Phase 7's user-confirmed preference is portability — easier to retarget the kernel to a future architecture, and the window's hazard surface is empty because nothing in it calls vmsmalloc. |
| P7-DEC-010 | Moved to Phase 4.5 (2026-05-27) — see P45-DEC-001 / P45-DEC-003 / P45-DEC-004 | **Per-CPU storage migrates from "GSBase low 8 bits = ProcessorID" to "GSBase = pointer to a `kernel::CpuLocal` struct" — placed in each VMSubstrate arena's metadata region** (extending Phase 3's existing per-CPU-arena pattern that already reserves a page for the vmsmalloc local cache). `CpuLocal` is `alignas(cacheLineSize)` and holds `{ logicalID; InterruptContextDepths interruptDepths; Magazine magazines[kNumSizeClasses]; }` — the vmsmalloc magazines are folded into `CpuLocal` (Phase 3 reframed; `magazineFor(i)` becomes `cpuLocal().magazines`). Phase-3 arena layout's previous `kVmsmallocLocalCachePages` reservation is renamed `kCpuLocalPages` and now hosts the whole `CpuLocal` struct (one page is sufficient for the current fields plus future growth). `setLogicalProcessorID` is removed; the logicalID is written into `CpuLocal` at arena creation time (during VMSubstrate init). AMD64 backend uses `wrgsbase` / `rdgsbase`; ARMv8 (`TPIDR_EL1`) and RISC-V (`tp`) backends are designed to slot in without changing call sites. | The existing GSBase scheme has exactly one consumer (`getLogicalProcessorID` at `smp.cpp:27`), so the refactor is contained (user direction 2026-05-27). The arena-metadata placement reuses Phase 3's existing per-CPU NUMA-placed storage pattern — no separate `PageAllocator::allocateSmallPage` call needed, no separate "where does CpuLocal live?" answer. Folding the magazines unifies "per-CPU kernel state" under one struct: `cpuLocal()` is the single source of truth for both the kernel-wide fields (logicalID, interruptDepths) and vmsmalloc's per-CPU magazine state. The arena-metadata framing means CpuLocal becomes available *during* VMSubstrate init — which is exactly when we want it (see P7-DEC-011 for the readiness gate). |
| P7-DEC-011 | Moved to Phase 4.5 (2026-05-27) — see P45-DEC-002 | **`cpuLocal()` and `getLogicalProcessorID()` are only callable after VMSubstrate's init has run.** A file-scope `kernel::cpuLocalReady` bool flag is initialized `false` at boot and set `true` as the last step of VMSubstrate's init routine (after every arena's CpuLocal page is mapped, the BSP's GSBase points at its arena-resident CpuLocal, and the BSP's logicalID is written). Both accessors assert `cpuLocalReady == true` at every call. Pre-`memory_management` call sites are caller bugs by construction — at that point only the BSP is running, so the answer (when the call is asking for "current logical CPU ID") is trivially 0; such sites must use a hard-coded `BSP_LOGICAL_ID = 0` constant instead of calling the accessor. The assertion is retained in early development to catch any unaudited early callers; it can be gated behind `#ifdef CROCOS_DEBUG_CPULOCAL_INIT` (or removed) once the migration is settled. | Per user direction 2026-05-27. The flag forces the migration discipline rather than papering over it with a BSS-bootstrap struct. Any caller that fires the assertion is identifying itself as needing audit: the fix is to replace `getLogicalProcessorID()` with a literal `0` (since the call is pre-SMP-bringup, only the BSP exists). Over time the assertion becomes vestigial — at which point it's cheap to remove or gate behind a debug flag. Simpler than the BSS-bootstrap approach considered earlier, and provides a louder failure mode for misuse during the transition. |

## Hazards

- **Release builds have zero runtime assertion coverage.** All Phase 7 asserts are
  debug-only. A `vmsmalloc(0)` or `vmsfree(nullptr)` in a release kernel produces silent
  misbehavior — the first either allocates an 8-byte slot from class 0 (caller then
  writes to that slot, leaking the slot if it's never freed); the second crashes
  somewhere downstream (range check, or inside `ensureTLBEntryFresh`). Acceptable per
  user direction 2026-05-27; called out so future code reviewers don't expect release
  diagnostics on bad inputs.
- **The depth-counter out-of-date window** (P7-DEC-009): between the CPU's IDT entry and
  the `InterruptContextGuard` construction, the per-CPU counter is stale. The window
  covers the register-save assembly and the call into `dispatchInterrupt`. No vmsmalloc
  / vmsfree code can run in this window (it's pure dispatch glue), so the hazard surface
  is empty today. **Watch for future code paths that run before `dispatchInterrupt`'s
  body and might call vmsmalloc** — e.g., a future "early diagnostic" path inside the
  IDT stub. Such code must NOT call into vmsmalloc, regardless of debug-only assert
  status.
- **Vector → kind mapping must stay in sync with the IDT's vector assignment.** If a
  future change reassigns vector 14 from #PF to something else (extremely unlikely under
  AMD64; the architectural-spec mapping is fixed), `kindForVector(14)` returns the wrong
  value and #PF callers are incorrectly marked as in #PF context. Defense: `kindForVector`
  is a `constexpr` switch over the AMD64-spec vector numbers, with a static_assert that
  pins the assumed vector values against `arch::VECTOR_NMI` etc. (if those constants
  exist) or against the AMD64 spec values directly.
- **`InterruptContextGuard` constructor cost on every interrupt.** Two per-CPU memory
  accesses (read pointer, increment counter). Negligible compared to the IDT's existing
  register-save and EOI work, but worth noting — Phase 7 is the first piece of code that
  adds bookkeeping to every interrupt dispatch. If a future kernel-profiling exercise
  reveals interrupt latency regressions, this is a candidate to inspect.
- **CpuLocal refactor changes the meaning of GSBase across the whole kernel.** Before
  Phase 7, GSBase's low 8 bits held the `ProcessorID`. After Phase 7, GSBase is a full
  pointer to the per-CPU `CpuLocal` struct. The only existing consumer is
  `getLogicalProcessorID()` (per user audit 2026-05-27), so the migration is contained;
  but **any future code path that does direct `rdgsbase` / `wrgsbase` reads bypassing
  the `arch::getCurrentCpuLocalBase()` abstraction must be updated**. Grep for `gsbase`
  during step 1 to catch any I'm missing.
- **`wrgsbase` requires the CR4.FSGSBASE bit enabled.** CroCOS already sets this (CLAUDE.md
  notes "CPU features: qemu64 with fsgsbase support" — the QEMU configuration). The
  existing `setLogicalProcessorID` uses `wrgsbase` already, so the prerequisite holds.
  If a future build configuration disables FSGSBASE, Phase 7's GSBase write fails with
  #UD; the alternative is `wrmsr(IA32_GS_BASE, ptr)` which is slower but works on every
  AMD64 CPU.
- **Readiness flag forces pre-VMSubstrate call-site audit (P7-DEC-011).** Any caller
  that fires `assert(cpuLocalReady)` is identifying itself as pre-`memory_management`.
  The fix is *always* to replace `getLogicalProcessorID()` with literal `0` at that
  site (the BSP is the only running CPU at that point). Step-1 audit grep: find every
  call to `getLogicalProcessorID()` in code paths reachable before VMSubstrate init.
  The assertion is intentionally loud to make the migration mechanical.
- **AP bootstrap must call `setCurrentCpuLocalBase` BEFORE any other instruction that
  reads cpuLocal or fires an interrupt.** The AP's GSBase is whatever the bootloader /
  AP-startup-stub left it as — junk for our purposes — until the bootstrap routine
  re-points it. Any interrupt that fires before that re-point would crash
  `dispatchInterrupt`'s `cpuLocal().interruptDepths` access. Defense: keep the
  `setCurrentCpuLocalBase` call as the FIRST instruction of the AP's C++ entry, before
  any IDT unmasking or interrupt enablement.
- **The readiness assertion is a development-only safety net (P7-DEC-011).** Once the
  pre-VMSubstrate call-site audit is complete, the assertion is largely vestigial — its
  cost (one bool load per `cpuLocal()` call) is paid on every alloc and free. Gate
  behind `#ifdef CROCOS_DEBUG_CPULOCAL_INIT` (or remove) once we're confident no early
  caller remains. Migration trigger: enough successful boots without the assertion
  firing.
- **ARMv8 / RISC-V backends are unimplemented in Phase 7.** The portable interface is
  shaped to accept them; the AMD64 backend is the only one shipped. If a future port
  starts, the new backend goes in `kernel/arch/<arch>/cpu_local_base.cpp` and the call
  sites are unchanged.
- **`make<T>` `static_assert` failure messages must point callers at the fix.** The
  "pad, split, or use a different allocator" wording is borrowed from DEC-025's text. A
  reviewer encountering the static_assert should immediately understand the workaround.
- **Including `VMSubstrateSlab.h` from `VMSubstrate.h` may pull additional symbols into
  unrelated TUs.** `VMSubstrateSlab.h` contains `SlabDescriptorBase`, `kSlabSizeClasses`,
  `kSlabDescriptorMagic`, and the accessors — none of which are load-bearing for
  non-vmsmalloc consumers, but they consume compile-time. Confirm at step 1 that no
  existing TU's compile time spikes. If it does, factor the accessors into a smaller
  `VMSubstrateSlabClasses.h` header that contains only `kNumSizeClasses` / `sizeClassFor` /
  `slotAlignment` / `kSlabSizeClasses`.
- **The DEC-030 placeholder predicates (`preemptionDisabled()` / `cpuPinned()` both
  return true) are invisible in release builds because the entire assert is debug-only.**
  A reviewer expecting compile-time enforcement of DEC-030 will not find it. Document
  the placeholder's vacuity in the predicates' TODO comments so future maintainers don't
  mistake them for real checks.
- **The DEC-028 convention-internal comment is advisory only.** A future `kmalloc`-style
  consumer that calls `vmsmalloc` / `vmsfree` directly compiles cleanly. Reader-side TLB
  freshness becomes the caller's obligation; `make<T>` / `destroy<T>` are the type-safe
  paths. Hazard for code review: a refactor that exposes a bare-pointer wrapper around
  `vmsmalloc` (e.g., for kernel debugging tools) silently drops the SafePtr contract.

## Verification Targets

| Property | Method |
|---|---|
| `vmsmalloc(0)` triggers the harness's assertion-trap exception (debug build) | Negative test in `AssertionsTest.cpp` |
| `vmsmalloc(arch::smallPageSize + 1)` triggers the assertion-trap exception (debug build) | Negative test |
| `vmsfree(nullptr)` triggers the assertion-trap exception (debug build) | Negative test |
| `make<T>` for `T` with `sizeof(T) > pageSize` fails to compile | Static-assert test in `tests/kernel/vmsmalloc/MakeStaticAssertsTest.cpp` |
| `make<T>` for `T` with `alignof(T) == 32, sizeof(T) == 96` (96 B class with alignof > 16) fails to compile | Static-assert test |
| `make<T>` for `T` with `alignof(T) == 16, sizeof(T) == 96` compiles cleanly | Compile-only positive test |
| `make<T>` for `T` with `alignof(T) == 256, sizeof(T) == 1024` (whole-page bypass) compiles cleanly (DEC-029 short-circuit) | Compile-only positive test |
| `InterruptContextGuard{InterruptKind::IRQ}` increments `irq` then decrements on destruction | Unit test in `tests/kernel/interrupts/InterruptContextDepthsTest.cpp` (new file) — construct/destruct under a mocked per-CPU storage |
| `kindForVector(2) == NMI`, `kindForVector(14) == PF`, `kindForVector(32) == IRQ`, `kindForVector(0) == Other` | `static_assert` block in the new test file |
| `inForbiddenContextForVmsmalloc()` returns true when any of {IRQ, NMI, UD, DF, GP, MC} depth > 0; false when only PF depth > 0 | Unit test exercising each kind in turn |
| Existing Phase-5 / Phase-6 boot smoke passes unchanged after Phase 7 lands | `make run` boots; klog shows the same alloc/free cycle output |
| `naiveTest` regression: unchanged | `cmake --build cmake-build-debug --target run` |
| `#include <mm/VMSubstrateSlab.h>` from `VMSubstrate.h` does not break existing consumers | Full kernel build succeeds (`cmake --build cmake-build-debug --target Kernel`) |
| `dispatchInterrupt` change does not regress interrupt latency under `naiveTest` workloads | QEMU `naiveTest` runtime within ±5% of pre-Phase-7 baseline |
| The `Convention-internal` comment is present at lines 21-22 of `VMSubstrate.h` | Code review during merge |

## Testing Approach

- **Negative tests** in `tests/kernel/vmsmalloc/AssertionsTest.cpp` (new file). Each
  test triggers one assertion (debug build) and catches the harness's exception. Per
  P7-DEC-006, this reuses the existing harness machinery — no new infrastructure.
- **Interrupt-context-depths unit tests** in `tests/kernel/interrupts/InterruptContextDepthsTest.cpp`
  (new file). `static_assert` block validating `kindForVector` for each known vector,
  plus runtime tests of `InterruptContextGuard`'s increment/decrement behavior under
  mocked per-CPU storage. Nested-guard test confirms independent counters.
- **Compile-only positive tests** in `tests/kernel/vmsmalloc/MakeStaticAssertsTest.cpp`
  (new file). Confirms `make<T>` accepts well-aligned types and rejects over-aligned
  ones at compile time. Run via the existing `KernelTests` build target.
- **Boot smoke regression** — Phase 5 + Phase 6 smoke continues to work. Phase 7 adds
  the interrupt-context-tracking subsystem to `dispatchInterrupt`, which runs on every
  interrupt; latency regression is a release gate (±5% naiveTest runtime).
- **No TSan variant.** Phase 7 is per-CPU-local state with interrupts disabled at the
  increment site; no concurrency to validate.
- **`naiveTest`** continues to pass and within the ±5% latency envelope.

## Implementation Phases

<!-- Concrete ordered steps for Phase 7 itself. -->

1. **Confirm starting state.**
   - Phases 1–6 + Phase 4.5 are merged. `kernel/mm/vmsmalloc.cpp` has both `vmsmalloc`
     and `vmsfree` bodies. `kernel/include/cpu_local.h` exposes `kernel::CpuLocal` and
     `cpuLocal()`. `kernel/include/interrupts/InterruptContextDepths.h` defines the
     struct (no kind/guard yet — Phase 7 adds those to the same header).
   - Confirm AMD64 architectural vector constants in `kernel/include/arch/amd64/amd64.h`
     (`VECTOR_NMI`, `VECTOR_UD`, `VECTOR_DF`, `VECTOR_GP`, `VECTOR_MC`, `VECTOR_PF`). If
     absent, the `kindForVector` mapping inlines the AMD64-spec values with a static_assert
     pinning each.
   - Confirm `dispatchInterrupt`'s current body at
     `kernel/interrupts/InterruptRoutingAndDispatch.cpp:317` matches the spec's
     integration-site description.

2. **Extend `kernel/include/interrupts/InterruptContextDepths.h` with the consumer logic.**
   - Add the `InterruptKind` enum, `kindForVector(uint8_t vec)` constexpr mapping, and
     `InterruptContextGuard` RAII declarations to the header.
   - Create `kernel/interrupts/InterruptContextDepths.cpp` with the
     `InterruptContextGuard` constructor/destructor — bodies read/write the per-CPU
     `InterruptContextDepths` field via `kernel::cpuLocal().interruptDepths`.

3. **Integrate the guard into `dispatchInterrupt`.**
   - Edit `kernel/interrupts/InterruptRoutingAndDispatch.cpp:317`. Add
     `kernel::interrupts::InterruptContextGuard ctx(kindForVector(frame.vector_index));`
     as the **first statement** of the function body, before the EOI logic.
   - Confirm interrupts remain disabled at this point (the IDT entry path keeps IF clear
     until the body explicitly re-enables it or `iret` restores it). The guard's
     increment runs with interrupts disabled — atomicity is automatic.

4. **Add the helper predicates to `kernel/mm/vmsmalloc.cpp`'s anonymous namespace.**
   - `inForbiddenContextForVmsmalloc()` — reads `currentCpuInterruptDepths()` and returns
     true if any of `{irq, nmi, ud, df, gp, mc}` is non-zero.
   - `preemptionDisabled()` — returns `true` with a `// TODO(DEC-030)` comment.
   - `cpuPinned()` — returns `true` with a `// TODO(DEC-030)` comment.

5. **Extend `vmsmalloc(size_t)` with the entry-point asserts (all debug-only).**
   - Add the five asserts at the head of the function body, in the order specified by
     the Consumer Contract source-sketch (size > 0, size <= pageSize, !forbidden context,
     preempt disabled, cpu pinned).
   - **Demote** the existing Phase-5 `kassert(size <= arch::smallPageSize, ...)` on the
     DEC-029 bypass branch to debug-only (or remove if it's now redundant with the
     entry-point assert).

6. **Extend `vmsfree(void*)` with the entry-point asserts (all debug-only).**
   - Add the four asserts (no size check — vmsfree doesn't take a size).

7. **Modify `kernel/include/mem/VMSubstrate.h`:**
   - Add `#include <mm/VMSubstrateSlab.h>` near the top (per P7-DEC-007).
   - Replace `make<T>`'s body with the two-pronged static_assert + the existing alloc
     code.
   - Add the convention-internal comment block at lines 21-22 (DEC-028 amendment).

8. **Add `tests/kernel/interrupts/InterruptContextDepthsTest.cpp`** (new directory if it
    doesn't exist). Tests:
    - `static_assert(kindForVector(2) == InterruptKind::NMI)` etc. for each tracked vector.
    - `InterruptContextGuard{InterruptKind::IRQ}` increments `irq` by 1 on construction
      and decrements on destruction (use a mock per-CPU storage / mock `cpuLocal()`; the
      test runs in userspace).
    - Nested guards (`Guard<IRQ>{}; Guard<PF>{};`) maintain independent counters.

9. **Add `tests/kernel/vmsmalloc/AssertionsTest.cpp`** with negative tests for each
    runtime assertion (debug build). Uses the harness's exception-based assertion-trap.

10. **Add `tests/kernel/vmsmalloc/MakeStaticAssertsTest.cpp`** with compile-only
    positive and negative tests for `make<T>` static_asserts.

11. **Build, regression-test, smoke.**
    - `cmake --build cmake-build-debug --target Kernel` succeeds.
    - `cmake --build cmake-build-debug --target run` boots; Phase-5/6 smoke output unchanged.
    - `make run_numa` / `make run_numa_hmat` boot; each CPU's CpuLocal page lives on its
      home NUMA domain (verify via klog of `cpuLocal()` VA per CPU, debug build only).
    - `cmake --build build --target KernelTests` succeeds, new tests pass.
    - `naiveTest` continues to pass; runtime within ±5% of baseline (P7-DEC-009 hazard).

12. **Audit and document.**
    - Confirm every assertion site cites the relevant parent-spec decision in a source
      comment.
    - Confirm `InterruptContextGuard` is the **first statement** of `dispatchInterrupt`'s
      body — no logic between the function entry and the guard construction.
    - Confirm no stray `rdgsbase` / `wrgsbase` / `IA32_GS_BASE` direct reads remain outside
      `arch/amd64/cpu_local_base.cpp` (grep audit).
    - Update `[[project_slab_abstraction_plan]]` memory: Phase 7 status → drafted /
      implemented.

13. **Optional follow-ups (under user latitude).**
    - Add a `preemptCount` field to `CpuLocal` when the future scheduler design firms up.
      The per-CPU struct is already the natural home.
    - Implement ARMv8 `cpu_local_base.cpp` backend via `TPIDR_EL1` when a port begins.
    - Implement RISC-V `cpu_local_base.cpp` backend via `tp` when a port begins.
    - Add positive integration test: `make<T>` for a typical RadixVM metadata struct.
    - Surface a `dumpInterruptContextDepths()` debug helper for kernel debugging tools.

## References

- `kernel/include/mem/VMSubstrate.h:21–22` — `vmsmalloc` / `vmsfree` declarations
  (Phase 7 adds the convention-internal comment).
- `kernel/include/mem/VMSubstrate.h:46–50` — `make<T>` template (Phase 7 adds the
  static_asserts).
- `kernel/include/mem/VMSubstrate.h:52–58` — `destroy<T>` template (unchanged).
- `kernel/mm/vmsmalloc.cpp` — Phase 5+6 TU, extended with the entry-point asserts and
  the helper predicates.
- `kernel/interrupts/InterruptRoutingAndDispatch.cpp:317` — `dispatchInterrupt`'s body;
  Phase 7 inserts the `InterruptContextGuard` construction at the top.
- `kernel/include/interrupts/InterruptContextDepths.h` — **new (Phase 7)**: the per-CPU
  context-depth tracking subsystem (struct, enum, constexpr mapping, RAII guard).
- `kernel/interrupts/InterruptContextDepths.cpp` — **new (Phase 7)**: guard
  constructor/destructor implementation (reads through `kernel::cpuLocal()`).
- `kernel/include/arch/cpu_local_base.h` — **new (Phase 7)**: architecture-portable
  `arch::setCurrentCpuLocalBase` / `getCurrentCpuLocalBase` declarations.
- `kernel/arch/amd64/cpu_local_base.cpp` — **new (Phase 7)**: AMD64 backend
  (`wrgsbase` / `rdgsbase`).
- `kernel/include/cpu_local.h` — **new (Phase 7)**: portable `kernel::CpuLocal` struct
  (cache-line-aligned; hosts `logicalID` + `InterruptContextDepths`) and the
  `cpuLocal()` accessor.
- `kernel/arch/amd64/smp/smp.cpp:18–31` — `setLogicalProcessorID` (removed) and
  `getLogicalProcessorID` (rewritten to read through `cpuLocal()`).
- `kernel/include/arch/amd64/smp.h` — `setLogicalProcessorID` declaration removed.
- SMP bringup paths (BSP early init + AP bootstrap) — per-CPU page allocation +
  `setCurrentCpuLocalBase` call. Exact files identified at step 1.
- `kernel/include/arch/amd64/amd64.h` — AMD64 architectural vector constants
  (confirmed at step 1; the `kindForVector` mapping references these).
- `kernel/mm/VMSubstrateSlab.h` — Phase 2 output; consumed by `make<T>`'s static_assert
  (via P7-DEC-007's include).
- `tests/kernel/interrupts/InterruptContextDepthsTest.cpp` — **new (Phase 7)**: unit
  tests for the interrupt-context subsystem.
- `tests/kernel/vmsmalloc/AssertionsTest.cpp` — **new (Phase 7)**: negative tests for
  the runtime entry-point asserts.
- `tests/kernel/vmsmalloc/MakeStaticAssertsTest.cpp` — **new (Phase 7)**: static_assert
  coverage for `make<T>`.
- **Folded into Phase 7 — per-CPU info infrastructure refactor (user direction 2026-05-27):**
  the GSBase→CpuLocal-pointer migration is now part of this phase's scope (see
  P7-DEC-010). ARMv8 (`TPIDR_EL1`) and RISC-V (`tp`) backends are designed-for but not
  shipped; the portable interface (`arch::setCurrentCpuLocalBase` /
  `getCurrentCpuLocalBase`) is in place so future ports add their backend without
  touching call sites.
- Parent spec `specs/vmsmalloc.md`:
  - DEC-004 — max object size = `pageSize`. Phase 7 hoists the guard from Phase 5's
    bypass branch to the function entry.
  - DEC-014 (amended ITEM-052) — IRQ/NMI/#GP/#MC illegal; #PF conditionally legal.
  - DEC-023 — `vmsmalloc(0)` and `vmsfree(nullptr)` both assert.
  - DEC-025 — `make<T>` compile-time alignment cap.
  - DEC-028 (amended 2026-05-27) — `vmsmalloc` / `vmsfree` stay in public header;
    convention-internal comment.
  - DEC-029 — large-request bypass (Phase 7's hoist preserves the bypass dispatch logic
    in Phase 5; Phase 7 only relocates the `size <= pageSize` guard).
  - DEC-030 — caller-side preempt-disable / migrate-disable obligation. Phase 7's
    placeholder predicate.
- Phase 1 spec `specs/vmsmalloc-phase-1.md` — P1-ITEM-002 resolved 2026-05-27 (test
  harness assertion-trap is exception-based; usable from Phase 7's tests).
- Phase 5 spec `specs/vmsmalloc-phase-5.md` — `vmsmalloc.cpp` TU layout; the entry-point
  guard previously at the DEC-029 bypass branch.
- Phase 6 spec `specs/vmsmalloc-phase-6.md` — `vmsfree` body that Phase 7 extends.
- Memory: `[[project_slab_abstraction_plan]]` — phase plan; updated on Phase 7 completion.
