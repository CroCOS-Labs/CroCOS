# vmsmalloc — Subsystem Overview

> **Reading order.** Start here for the map. Authoritative design decisions live in
> `vmsmalloc.md` (the parent spec). Implementation-level detail per phase lives in
> `vmsmalloc-phase-{1..9}.md`. This document indexes the others — it does NOT
> duplicate their decisions.

## Subsystem in one paragraph

`vmsmalloc` is a per-size-class slab allocator that sits on top of
`kernel::mm::VMSubstrate` and serves VMM-metadata-sized allocations
(8–512 B fast path, 513–4096 B via whole-page bypass). Its consumer is RadixVM.
The design uses a Bonwick-Adams-shaped magazine + depot pattern adapted for
slab-granularity caching: each CPU's `kernel::CpuLocal` struct holds a magazine
(a `chainNext`-linked chain of slabs) per size class; each NUMA domain has a
shared lock-free Treiber stack (`partial[d][c]`) of magazines published by
freers and consumed by allocators. The Treiber stack uses a 64-bit
packed-tagged-head encoding (27-bit page-offset + 37-bit ABA counter) keyed off
the VMSubstrate VA base, making the ABA-safe CAS lock-free on AMD64 / ARMv8 /
RISC-V without `cmpxchg16b`. Public API is `VMSubstrate::make<T>` /
`destroy<T>` (returning `SafePtr<T>` for type-enforced TLB freshness on
cross-CPU access); the underlying `vmsmalloc` / `vmsfree` are
convention-internal.

## Phase dependency graph

```
       ┌─────────────────┐
       │ Phase 1         │  LibAlloc::SlabBookkeeper
       │ (LibAlloc prep) │  generalization + kTailBits + isEmpty
       └────────┬────────┘
                │
                ▼
       ┌─────────────────┐         ┌─────────────────┐
       │ Phase 2         │         │ Phase 4         │
       │ Descriptor      │         │ ChainedTreiber- │
       │ layout, schema, │         │ Stack in        │
       │ accessors       │         │ Core            │
       └────────┬────────┘         └────────┬────────┘
                │                           │
                ▼                           │
       ┌─────────────────┐                  │
       │ Phase 3         │                  │
       │ VMSubstrate     │                  │
       │ per-CPU metadata│                  │
       │ + per-domain    │                  │
       │ buffers + init  │                  │
       └────────┬────────┘                  │
                │                           │
                ▼                           │
       ┌─────────────────┐                  │
       │ Phase 4.5       │                  │
       │ CpuLocal infra  │                  │
       │ (cpuLocal(),    │                  │
       │ GSBase→struct,  │                  │
       │ readiness flag) │                  │
       └────────┬────────┘                  │
                │                           │
                ▼                           │
       ┌─────────────────┐                  │
       │ Phase 5         │◄─────────────────┘
       │ vmsmalloc fast  │
       │ path + slow path│
       └────────┬────────┘
                │
                ▼
       ┌─────────────────┐
       │ Phase 6         │
       │ vmsfree         │
       │ validation +    │
       │ publish         │
       └────────┬────────┘
                │
                ▼
       ┌─────────────────┐
       │ Phase 7         │
       │ Assertions +    │
       │ interrupt-      │
       │ context guard   │
       └────────┬────────┘
                │
        ┌───────┴───────┐
        ▼               ▼
┌─────────────────┐  ┌─────────────────┐
│ Phase 8         │  │ Phase 9         │
│ Userspace test  │  │ In-kernel       │
│ harness + TSan  │  │ stress (naive-  │
│                 │  │ test-style)     │
└─────────────────┘  └────────┬────────┘
                              │
                ┌─────────────┴─────────────┐
                ▼                           ▼
       ┌─────────────────┐         ┌─────────────────┐
       │ Phase 10        │         │ Phase 11        │
       │ Magazine tuning │         │ scavenge() —    │
       │ policy          │         │ Empty-slab      │
       │ (deferred)      │         │ reclamation     │
       └─────────────────┘         │ (deferred)      │
                                   └─────────────────┘
```

**Implementation order (linearized):** 1 → 2 → 3 → 4 → 4.5 → 5 → 6 → 7 → 8 → 9 → 10 → 11.

Phases 4 and 1–3 are independent: Phase 4 (`Core::ChainedTreiberStack`) lives in
`libraries/Core/` and depends only on existing `Atomic<T>` plumbing; it can be
implemented in parallel with Phases 1–3 if convenient. Phase 4.5 sits between
Phase 3 and Phase 5 because Phase 5 / 6 reference `kernel::cpuLocal().magazines[c]`
in their fast paths.

## What each phase delivers

**Phase 1 — LibAlloc prerequisites** (`vmsmalloc-phase-1.md`).
Generalizes `LibAlloc::SlabBookkeeper<N>` to accept any positive `N` (was
restricted to multiples of 64). Adds `seedAllAvailable(usableCount)` overload
that masks the bitmap's tail bits via `reserveSlot`. Introduces a
`kTailBits` constexpr and amends `isEmpty()` to read
`reservedCount == kTailBits` (so both PageAllocator and vmsmalloc see correct
"no live allocations" semantics). Ships a TSan-instrumented userspace stress
runner (`LibAllocTestRunnerTSan`).

**Phase 2 — Slab descriptor layout** (`vmsmalloc-phase-2.md`).
Creates `kernel/mm/VMSubstrateSlab.h` with `SlabDescriptorBase` (uniform
32-byte prefix), `SlabDescriptor<N>` (= prefix + `SlabBookkeeper<N>` suffix),
`kSlabDescriptorMagic = 0x5DAB5DABDE5CC9C0`, `kSlabSizeClasses`, and the
constexpr-table accessors `slotSize(c)` / `slotCount(c)` / `slot0Offset(c)`
derived via a 2-step fixpoint. Compile-time-validated; no runtime code.

**Phase 3 — VMSubstrate metadata storage** (`vmsmalloc-phase-3.md`).
Two new VMSubstrate primitives: `reservePerDomainStaticBuffer(byteSize, d)` for
the shared per-domain `partial[d][c]` and `tuning[d][c]` storage (NUMA-distributed
in the topmost arena-equivalent VA slot), and `cpuLocalPageFor(i)` for the
per-CPU CpuLocal page (reserved between the occupancy buffer and the allocatable
region of each CPU's first arena). Implements `vmsmallocInit` to populate
per-domain buffers. The arena-layout shrinkage preserves the existing
`kArenaPageCount % kBranchFactor == 0` invariant.

**Phase 4 — Concurrent Treiber stack** (`vmsmalloc-phase-4.md`).
Adds `Core::TreiberStack<T, ...>` and `Core::ChainedTreiberStack<T, ...>` to
`libraries/Core/include/core/atomic/TreiberStack.h`. `ChainedTreiberStack`
exposes `push(element)` (extend-or-singleton), `pushChain(head, depth)`
(always-new-chain single CAS, for magazine flush), and `pop()` (whole chain in
one CAS). Template-parameterized hooks (`onPreTouch`, `onCasFailure`,
`onEmptyStack`) let consumers (vmsmalloc) inject per-event behavior without
touching the primitive. Memory ordering pinned via named constants per DEC-042.
Parallel `CoreTestRunnerTSan`.

**Phase 4.5 — CpuLocal infrastructure** (`vmsmalloc-phase-4.5.md`).
Replaces the existing "GSBase low 8 bits = ProcessorID" scheme with "GSBase =
pointer to a `kernel::CpuLocal` struct" hosted in each VMSubstrate arena's
metadata page. `CpuLocal` carries `logicalID`, `InterruptContextDepths`, and
the vmsmalloc `Magazine[kNumSizeClasses]` array. Architecture-portable
`arch::setCurrentCpuLocalBase` / `getCurrentCpuLocalBase` (AMD64 backend via
GSBase; ARMv8 / RISC-V designed-for but unshipped). A `cpuLocalReady` flag
gates `cpuLocal()` / `getLogicalProcessorID()` to after VMSubstrate init has
run — pre-init callers must use the literal `BSP_LOGICAL_ID = 0`.

**Phase 5 — vmsmalloc happy path** (`vmsmalloc-phase-5.md`).
Implements the `vmsmalloc(size)` body in `kernel/mm/vmsmalloc.cpp`. Fast path
with DEC-039 pre-read of `chainNext` before `bookkeeper.allocSlot`; refill via
`partialFor(d)[c].pop()` (with Phase 4's `onPreTouch` hook calling
`ensureTLBEntryFresh` inside the CAS loop); lazy first-touch freshness on
`m.head` transitions (DEC-040 amended); DEC-036 eager-free walk; DEC-029
large-request bypass via `VMSubstrate::allocPage()`; DEC-018 fresh-slab slow
path. The 64-bit DEC-015 packed-tagged-head encoding lives as the
`HeadEncoding` policy for `ChainedTreiberStack`.

**Phase 6 — vmsfree happy path** (`vmsmalloc-phase-6.md`).
Implements the `vmsfree(p)` body. DEC-026 validation chain: range check →
DEC-029 page-aligned dispatch → `ensureTLBEntryFresh` → magic check →
slot-range bounds (ITEM-048) → modulo → debug-only DEC-024 `0xCC` poison →
`bookkeeper.freeSlot` → conditional publish. Same-domain `becameAvailable`
extends the local magazine (and flushes via `pushChain` when `m.depth`
reaches `maxChainLength`); cross-domain calls `partialFor(home)[c].push(*desc)`
(P6-DEC-002: may extend the home-domain top chain — better shared-stack
amortization than always-singleton).

**Phase 7 — Assertion paths + interrupt-context guard** (`vmsmalloc-phase-7.md`).
Debug-only entry-point asserts on both `vmsmalloc` and `vmsfree`:
`size > 0` / `size <= pageSize` / `p != nullptr` (DEC-023, DEC-004),
forbidden-context predicate (DEC-014 amended for #PF — includes
IRQ/NMI/#GP/#MC/#UD/#DF), `preemptionDisabled()` and `cpuPinned()`
placeholders (DEC-030). Extends `InterruptContextDepths.h` with
`InterruptKind`, `kindForVector`, and `InterruptContextGuard` RAII;
integrates the guard at the top of `kernel::interrupts::dispatchInterrupt`.
DEC-025 `static_assert` on `make<T>`. DEC-028-amendment convention-internal
comment on `vmsmalloc` / `vmsfree` declarations.

**Phase 8 — Userspace integration test harness** (`vmsmalloc-phase-8.md`).
`tests/kernel/vmsmalloc/` extended with `mocks/` (MockVMSubstrate over a
~64 MiB `mmap` region, thread_local MockCpuLocal, configurable
MockNUMAPolicy), `IntegrationTest.cpp` (10 single-threaded scenarios),
`ConcurrentTest.cpp` (10 multi-thread scenarios validated under TSan), and
`DebugIntrospection.h` (test-only accessors into vmsmalloc internals via
`#ifdef CROCOS_VMSMALLOC_TEST_HARNESS`). ARMv8 M1 TSan is the primary
release gate; AMD64 TSan (via Rosetta cross-compile or SSH Linux box) is
the on-demand cross-check for adjudicating false positives.

**Phase 9 — In-kernel stress test** (`vmsmalloc-phase-9.md`).
`naiveTest`-style per-CPU stress at `smp_bringup`. Each CPU allocates 256
slots per class plus 256 whole-page allocations per iteration, writes a
`(cpu, class, index, iteration)` content pattern, verifies, then frees —
with ~10% deterministic cross-domain hand-off via per-recipient
`AtomicLinkedList<HandoffEntry>` inboxes. Mutually exclusive with
`naiveTest` via `CROCOS_VMSMALLOC_STRESS` build flag. The canonical
validator for DEC-015's address-math invariant against real kernel VAs.

## Thematic index — where the design lives

For readers diving into a specific topic, the authoritative parent-spec
references are below. Phase specs implement; the parent spec decides.

**Concurrency model.** DEC-002 (lazy reclamation), DEC-034 (chained magazines),
DEC-037 (unified magazine / no `current[]`), DEC-039 (pre-read `chainNext`
before `allocSlot`), DEC-040 (lazy first-touch freshness via Phase-4
`onPreTouch` hook), DEC-041 (head-linkage; `m.tail` removed),
DEC-042 (memory-ordering policy across Treiber CAS / chain links / bookkeeper).

**Per-CPU storage.** P3-DEC-004 (CpuLocal page in arena metadata) +
P45-DEC-001 (placement) + P45-DEC-002 (cpuLocalReady flag + assertion gate) +
P45-DEC-003 (portable interface). Magazines fold into `kernel::CpuLocal` per
P7-DEC-010 (now P45) — there is no separate `magazineFor(i)` accessor.

**NUMA model.** DEC-018 (slab home-domain recording at creation), DEC-019
(no cross-NUMA steal — local-stack-miss goes to fresh `allocPage`),
DEC-033 (per-domain buffer + per-CPU arena cache split — partially superseded
by P7-DEC-010 / P45-DEC-001 for the per-CPU half), DEC-038 (raw DomainID
indexing for CPU-bearing domains only).

**Failure modes / assertions.** DEC-014 amended (forbidden IRQ/NMI/#GP/#MC/#UD/#DF
context; #PF conditionally legal), DEC-023 (`vmsmalloc(0)` / `vmsfree(nullptr)`
assert), DEC-026 (`vmsfree` validation chain), DEC-029 (whole-page bypass),
DEC-030 (preempt-disable + CPU-pinned caller obligation), P7-DEC-001
(all runtime asserts debug-only).

**ABA-safe head encoding.** DEC-015 (27-bit page-offset + 37-bit counter
packed into 64 bits; single-PDPT assumption; push-only counter advance with
explicit `& kCounterMask`).

**Magazine model (Bonwick-Adams adaptation).** DEC-002 (per-domain Treiber
stack), DEC-034 (chained-transfer magazines, `chainNext` linkage),
DEC-035 (runtime-tunable K via overflow/starvation counters — Phase 10
implements the policy), DEC-036 (allocator-path bounded eager-free of Empty
inbox slabs), DEC-041 (head-linkage). Bonwick-Adams terminology mapping is
in the parent spec's "Terminology" section.

**Public API surface.** DEC-028 amended (`make<T>` / `destroy<T>` is the
public API; `vmsmalloc` / `vmsfree` are convention-internal because they
must stay declared in `VMSubstrate.h` for the templates to instantiate),
DEC-025 (`make<T>` static_assert on alignment).

## Future work — Phases 10 and 11

Drafted only at the parent-spec decision level; full sub-specs deferred
until Phase 9 implementation is in-hand and real workload data is available
from RadixVM consuming vmsmalloc.

**Phase 10 — Magazine tuning policy** (DEC-035 / DEC-043).
Implements the read-and-adjust cycle on `MagazineTuning::overflowCount` /
`starvationCount` to mutate `partialFor(d)[c].setMaxChainLength(K)`
dynamically. Single-runner-via-try-lock per `(domain, class)`: the policy
gates on event count, attempts `policyLock.exchange(1)`, runs if it wins,
skips if it loses (another CPU is already running). High overflow → grow K;
high starvation → shrink K. Bounds `[kMinK, kMaxK]` from P3-DEC-002.
**Trigger to start drafting:** Phase 9 stress runs are stable AND we have
RadixVM-shaped workload data to calibrate the policy's response curve.

**Phase 11 — `scavenge()` for Empty-slab reclamation** (ITEM-009).
Explicit reclamation pass for the residual cases DEC-036's eager-free walk
can't handle: (a) tail-Empty slabs in a magazine on a quiet system (the
floor of 1 prevents eager-free from reclaiming the last cached slab), and
(b) Empty slabs on the shared Treiber stack (which the eager-free walk
doesn't touch). Likely shape: a triggered (not periodic) pass that drains
magazines and walks the shared stacks, `freePage`'ing all-Empty chains
above some idle threshold. **Trigger to start drafting:** Phase 9 confirms
the residual Empty-slab footprint is large enough to matter on long
quiescent runs.

## Documents inventory

- **`vmsmalloc.md`** — parent spec. ~1115 lines. Authoritative decision
  source. Heavy; load when investigating a specific decision.
- **`vmsmalloc-overview.md`** — this document. Lightweight map. Load for
  navigation / quick orientation.
- **`vmsmalloc-phase-1.md`** through **`vmsmalloc-phase-9.md`** (incl.
  4.5) — implementation-level specs. Load when working on a specific phase.
- **`README.md`** — meta-doc for the `specs/` directory's workflow
  conventions.

## Memory references

- `[[project_slab_abstraction_plan]]` — phase plan + status; updated
  per-phase as work lands.
- `[[project_armv8_dev_tsan]]` — ARMv8 + TSan as the default release gate
  (consumed by Phases 1, 4, 8).
- `[[project_crocos_target_hardware]]` — consumer desktop hardware as
  primary target; multi-NUMA correctness preserved architecturally.
- `[[project_vmsubstrate_status]]` — VMSubstrate implementation status
  (underlying substrate for vmsmalloc).
