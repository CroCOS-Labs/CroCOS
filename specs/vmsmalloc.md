---
kind: leaf
status: drafting
parent: ~
components: []
---

# vmsmalloc — VMSubstrate slab allocator layer

> Per-size-class slab allocator that sits on top of `kernel::mm::VMSubstrate`, providing
> `vmsmalloc(size)` / `vmsfree(ptr)` for VMM metadata objects. Backs slabs with
> `VMSubstrate::allocPage()` and manages per-CPU / per-NUMA-domain partial-slab pools using
> the generic `LibAlloc::Slab` primitive.

## Non-Goals

<!-- What this component explicitly does not handle. Be specific — name things a reader might reasonably
     expect to be in scope. -->

- Not a general-purpose `kmalloc` replacement — scope is VMM metadata objects allocated through
  `VMSubstrate::make<T>` / `destroy<T>`.
- Not the `LibAlloc::SlabAllocator` rewrite — that is a separate consumer of the generic
  `LibAlloc::Slab`.
- Not the backing PageAllocator itself — slab pages come from `VMSubstrate::allocPage()` which
  is already live.
- Not responsible for the `Core::SplitBitmap` / `LibAlloc::Slab` primitives themselves — those
  are upstream dependencies.
- **Not responsible for CPU / memory hotplug.** The DEC-033 metadata region is sized at boot from
  `NUMAPolicy::domainCount()` and the active CPU count and is never resized afterward. If
  CroCOS ever grows hotplug support, the metadata region (and the matching `partial[]` /
  `magazines[]` / `tuning[]` arrays) will need a strategy for online growth, but that's a
  kernel-wide concern far beyond vmsmalloc and is deliberately out of scope here.

## Terminology

<!-- Mapping between this spec's internal terms and the Bonwick-Adams ("Magazines and Vmem",
     USENIX 2001) terminology that originated the per-CPU-magazine + global-depot pattern.
     Useful for cross-reading the paper against the spec. -->

| This spec | Bonwick-Adams (UMA) | Notes |
|---|---|---|
| **chain** — an M-element stack of slab descriptors linked via `chainNext`; the head also carries `chainDepth` (= M) and `next` (Treiber linkage to the next chain on the shared stack) | **magazine** | Same structural role (M-element amortization unit) but different granularity — see below. |
| **`Magazine` struct** (per-CPU holder of one chain; accessed as `kernel::cpuLocal().magazines[c]` per Phase 7 P7-DEC-010 — magazines are a field of the unified `kernel::CpuLocal` struct in arena metadata) | the **per-CPU cache layer** (in B-A this layer holds *two* magazines: `M_loaded` and `M_previous`, swapped on empty/full to amortize depot trips) | Naming collision: our `Magazine` struct corresponds to B-A's *cache layer*, not B-A's *magazine*. We hold one chain per `(CPU, class)`; B-A holds two magazines per `(CPU, cache)`. The two-magazine optimization is a deferred refinement (DEC-002 / DEC-034 design). |
| **shared `partial[d][c]` Treiber stack** (lock-free stack of chain heads, per-domain-per-class) | **depot** (the globally-shared pool of full and empty magazines) | Same role. We use a lock-free Treiber stack with `pushChain` / `push(element)` / `pop()` (Phase 4); B-A uses a locked list of magazines. |
| **slab** | **slab** | Same. |
| **slot** (a fixed-size region within a slab, returned to the caller) | **object** | Same. |
| **`maxChainLength`** (runtime-tunable per `(domain, class)`; lives on the `ChainedTreiberStack` instance) | **magazine size M** (Bonwick's notation; runtime-tunable in modern UMA implementations) | Same role. P3-DEC-002 starts at `kInitialK = 8`. |
| **`tuning[d][c].overflowCount` / `.starvationCount`** | the per-cache overflow / starvation counters used to grow / shrink M | Same role. Bumped via Phase-4 P4-DEC-010 hooks on CAS failure / empty pop. |

**Structural differences worth flagging when reading B-A against this spec:**

1. **Cache granularity.** B-A magazines hold *object pointers* (caller-level allocations);
   our chains hold *slab pointers* (each slab itself holds many slot-sized objects). So
   we amortize slab handoffs to the depot, not object handoffs to the slab — one level
   higher than canonical B-A. The motivation: the slab is the unit RadixVM consumes
   most-frequently and our per-CPU cache wants to amortize the slab handoff itself.
2. **One chain per (CPU, class), not two.** B-A's per-CPU cache holds two magazines
   (`M_loaded` + `M_previous`) to handle alloc-free-alloc-free symmetric workloads
   without round-tripping the depot. Our `Magazine` struct holds one chain; we go
   straight to the depot when `m.depth == 0`. Adding the second chain is a possible
   future refinement; not load-bearing for correctness.
3. **Lock-free depot via Treiber stack.** B-A's depot is a locked list of magazines.
   Ours is a lock-free Treiber stack with the DEC-015 packed-tagged-head ABA defense.
   `pushChain` (entire chain in one CAS) is the rough equivalent of B-A's "return a
   full magazine to the depot"; `push(element)` is novel (it may extend the depot's
   top chain rather than enqueue a new magazine) and is used for the cross-domain
   singleton-free path (DEC-019 / DEC-034 gate, Phase 6 P6-DEC-002).

Within this spec we use **chain** / **magazine** / **partial stack** consistently; the
above mapping is for readers cross-referencing Bonwick.

## Consumer Contract

<!-- What callers can rely on: usage rules, ownership semantics, memory model guarantees. If concurrency
     semantics are non-trivial, add a Concurrency Model supplementary section and cross-reference it here. -->

- **Public surface (DEC-028):** the only public entry points are `VMSubstrate::make<T>(args...)`
  (returns `VMSubstrate::SafePtr<T>`) and `VMSubstrate::destroy<T>(SafePtr<T>)`. The underlying
  `vmsmalloc(size)` / `vmsfree(ptr)` are implementation-detail primitives with internal linkage
  (or namespace-private), declared in a non-public header (e.g. `kernel/mm/VMSubstrateSlab.h`).
  Consumers cannot obtain a bare `void*` from this layer; reader-side TLB freshness is enforced
  by `SafePtr`'s `operator*` / `operator->` (which call `ensureTLBEntryFresh` on first cross-CPU
  access). The rest of this contract describes the primitives' behavior — bindings between
  `make<T>` and `vmsmalloc` are immediate (`make<T>` is a thin wrapper that calls `vmsmalloc`,
  placement-news a `T`, and constructs a `SafePtr<T>` over the result).
- `vmsmalloc(size)` returns a pointer to at least `size` bytes within a VMSubstrate arena.
  `size` must satisfy `0 < size ≤ pageSize`; `size == 0` and `size > pageSize` both assert
  (DEC-004, DEC-023). For `size > largestSizeClass`, the request is served by the DEC-029
  whole-page bypass (`VMSubstrate::allocPage`); the returned pointer is page-aligned and the
  page carries no `SlabDescriptor`.
- Returned pointer alignment matches DEC-001: for power-of-two size classes, alignment equals the
  size-class value; for non-pow2 classes, alignment is `alignof(max_align_t)` (= 16 B per DEC-022).
  Whole-page (DEC-029) allocations are page-aligned. Per DEC-025, `VMSubstrate::make<T>` rejects at
  compile time any `T` whose `alignof(T)` exceeds the slot alignment of its chosen size class.
  `vmsmalloc` itself takes only a `size` argument and does not silently promote requests across
  classes for alignment.
- `vmsmalloc` **never returns null**; arena exhaustion panics (DEC-012).
- `vmsfree(ptr)` is safe from any CPU on any arena. After the range and alignment checks
  (DEC-026, DEC-029): a page-aligned `ptr` is a whole-page allocation (DEC-029) and is freed via
  `VMSubstrate::freePage(ptr)` directly. A non-page-aligned `ptr` is slab-backed —
  pointer-to-slab lookup is `(ptr & ~(pageSize - 1))` (DEC-006), and `vmsfree` calls
  `VMSubstrate::ensureTLBEntryFresh(ptr)` before reading any descriptor field to cover cross-CPU
  first-touch staleness (DEC-016).
- `vmsfree` asserts on `nullptr` (DEC-023), on double-free, and on non-slab / misaligned pointers
  (DEC-013).
- Concurrency: each slab has at most one owning allocator CPU at a time, tracked implicitly via
  per-CPU "current slab" pointers (DEC-010). Multi-CPU free is safe by construction via
  `LibAlloc::SlabBookkeeper`'s SplitBitmap. See **Concurrency Model** below.
- `vmsmalloc` and `vmsfree` are **illegal from IRQ, NMI, #GP, and #MC context, and conditionally
  legal from #PF context** (DEC-014 amended). A page-fault handler may call into vmsmalloc
  (e.g. for CoW allocation) provided the caller can guarantee no transitive re-entrancy —
  concretely, `VMSubstrate` itself and RadixVM must never page-fault during their operation.
  Kernel paths in any forbidden context that need VMM metadata must use a pre-allocated pool.
  All other callers run in process / kernel-thread context.
- **Preemption / migration (DEC-030):** callers must hold the thread non-preemptible and pinned
  to its current CPU for the duration of every `vmsmalloc` / `vmsfree` call. CroCOS has no
  scheduler today so both conditions hold trivially; when a preemptive scheduler lands, callers
  must use the scheduler's preempt-disable / migrate-disable primitives explicitly. Debug-build
  asserts at function entry will be extended to check both at that point.
- `VMSubstrate::allocPage` / `freePage` are guaranteed never to call back into `vmsmalloc`
  (DEC-017), so the slab-creation slow path is straight-line.
- Init-edge rule: `make<T>` / `destroy<T>` (and therefore `vmsmalloc`/`vmsfree`) are legal callers
  only after the `[VMSubstrateSlab]` `.icd` routine has run (DEC-021).
- Returned memory is **not** zero-initialized in any build; debug builds poison freed slots with
  `0xCC` (DEC-024). Initialization of returned storage is `make<T>`'s placement-new.

## Dependencies

<!-- Upstream components and interfaces this relies on. Flag anything that must be stable before
     implementation can begin. -->

| Dependency | Role | Must be stable first? |
|---|---|---|
| `kernel::mm::VMSubstrate::allocPage` / `freePage` | Page-granularity backing store for slab data pages | Yes — live; best-effort NUMA-local per DEC-018. **Per DEC-046: `allocPage` invalidates the calling CPU's own TLB entry (via `invlpg`) and clears the calling CPU's own dirty-bitmap bit for the returned VA before returning** — the fresh-allocation branch of vmsmalloc relies on this so it can skip `ensureTLBEntryFresh` on freshly-`allocPage`'d pages. |
| `kernel::mm::VMSubstrate::reservePerDomainStaticBuffer(size_t byteSize, DomainID d) -> void*` and `localCachePageFor(arch::ProcessorID i) -> void*` | Storage primitives for the vmsmalloc metadata region (DEC-033, rewritten 2026-05-26). | **New primitives — must be added.** (a) `reservePerDomainStaticBuffer(byteSize, d)`: called once per CPU-bearing domain during `vmsmallocInit`; VMSubstrate picks a VA in the static-buffer region (the topmost arena-equivalent VA slot, see DEC-033), allocates `ceil(byteSize / pageSize)` physical pages on domain `d` via `PageAllocator::allocateSmallPage(cpuOnDomain(d))`, walks the existing arena-style page-table math (architecture-portable via `arch::pageTableDescriptor`) to install the leaf PTEs, and returns the buffer's VA. Pages are pinned for the lifetime of the kernel; no `freePage` symmetry. (b) `localCachePageFor(i)`: returns the VA of CPU `i`'s vmsmalloc-local-cache page, a structurally-reserved page in CPU `i`'s first arena placed between the occupancy buffer and the allocatable region (see Arena Layout Modification below). The page is allocated and mapped by VMSubstrate when the arena is created (already on CPU `i`'s NUMA domain); the accessor is pure address arithmetic. Both primitives are single-threaded init-only (assert pre-SMP context in debug builds). |
| `kernel::mm::PageAllocator::allocateSmallPage(cpu)` | NUMA-aware physical page allocation for metadata pages — explicit-domain (DEC-033) | Yes — live. **Per ITEM-051: per-CPU `LocalPool` array is sized to all detected logical CPUs at PageAllocator init time, and `allocateSmallPage(apID)` is callable from the BSP for any detected logical CPU ID before that AP comes online.** The call uses the AP's `nearestPool` to honor NUMA placement intent regardless of whether the target CPU is currently running. This is what makes DEC-021's vmsmallocInit work as written from `memory_management` phase (before `smp_bringup`). Already implemented; pinned as a dependency contract. |
| `LibAlloc::SlabBookkeeper` (`libraries/LibAlloc/include/liballoc/Slab.h`) | Per-slab slot bookkeeping | **Needs three edits first** — (i) remove the artificial `SlotCount % 64 == 0` restriction (Slab.h:43–44, marked with a TODO referencing DEC-011) and switch `kWordCount` to ceiling division (Slab.h:46), with tail-bit masking via `seedAllAvailable(usableCount)` or post-init `reserveSlot` loop; (ii) add double-free detection in `SplitBitmap::releaseBit` per DEC-013; (iii) document the existing acq-rel ordering on `allocatedCount.fetch_add/sub` (Slab.h:93 / 129) in the header per DEC-042 #4 (behavior already correct, contract just needs to be pinned externally) |
| `Core::SplitBitmap` | Bitmap primitive under Slab | Yes — landed |
| NUMA topology query (current CPU → DomainID) | Per-domain slab pool routing + metadata NUMA placement | Yes — live via `NUMAPolicy` |
| `Core::TreiberStack<T, LinkageTraits, HeadEncoding>` / `Core::ChainedTreiberStack<…>` (`libraries/Core/include/core/atomic/TreiberStack.h`) | Cross-CPU partial-slab pool (stores chain heads per DEC-034). Generic intrusive Treiber stacks built as Phase-4 Core primitives; vmsmalloc supplies the DEC-015 packed-tagged-head encoding as the `HeadEncoding` policy, `&SlabDescriptorBase::next` as the Treiber linkage, and `&SlabDescriptorBase::chainNext` as the intra-chain linkage. | Yes — built in Phase 4 (`vmsmalloc-phase-4.md`, prerequisite to vmsmalloc's fast-path phase). Encoding fixed; depends only on the VMSubstrate VA window staying within one PDPT (asserted at boot). |
| Cross-CPU TLB freshness for slab pages | Remote freers reading `desc->magic` | Yes — handled freer-side via `ensureTLBEntryFresh` (DEC-016) |
| Per-CPU magazine outbox/inbox (DEC-034) | Amortizes shared-stack CAS over K transfers | Self-contained — no external dependency beyond the metadata region (DEC-033) |
| Magazine tuning policy (DEC-035) | Runtime adjustment of K via overflow/starvation counters | Implementation knob — policy details deferred; structure must be in place |

## Invariants

<!-- Conditions that must hold at all times within this component. State them as falsifiable assertions,
     not aspirations. -->

- Every live `vmsmalloc`-returned pointer `p` satisfies: `desc = (SlabDescriptor*)(p & ~(pageSize-1))`
  is a valid initialized slab descriptor, and the slot at offset `(p - desc) / desc->slotSize` is
  marked allocated in `desc->bookkeeper`.
- A slab is in exactly one of {in-some-CPU's-magazine-chain (anywhere along the chainNext walk),
  on-its-NUMA-domain-partial-stack (anywhere along a chain on the shared stack), Full-and-orphaned}
  at any moment. Per DEC-037 the magazine's head is implicitly the "active allocation target" —
  there is no separate "current" state. Orphaned-Full slabs are re-published by the first freer
  that transitions them to Partial (via the cross-domain gate or the local magazine push per
  DEC-034). A chain element is "owned" by whichever CPU currently holds the chain head — it
  cannot be independently popped or pushed.
- A slab's home NUMA domain is fixed at creation (determined by the arena it was allocated from)
  and never changes.
- The descriptor at slab page offset 0 includes a magic signature value used by `vmsfree` to
  reject non-slab pointers (DEC-013).
- All bookkeeper mutations report an accurate `OccupancyTransition`; the partial-stack
  push-on-Full→Partial rule depends on this.
- **Chain encoding (DEC-034, refined by DEC-041; scoped per ITEM-046):** for a chain of length
  `n >= 1` **on the shared Treiber stack** (or at flush time after the flushing CPU writes
  `m.head->chainDepth = m.depth` and `m.head->next = oldHead.descPtr`), the chain head has
  `chainDepth == n` and `next` points to the next chain head on the shared Treiber stack.
  `chainNext` walks the chain in LIFO order; the bottommost slab's `chainNext` is `nullptr`.
  For a chain of length 1, `chainNext == nullptr` and `chainDepth == 1` (head and bottom
  coincide, so `next` is also the chain's own shared-stack linkage). **Non-head chain slabs'
  `next` fields are undefined** while the slab is in a chain — they may hold stale values
  from any prior flush cycle. They are only written when the slab next becomes a chain head
  (which happens at the next flush in which it is the magazine head).
- **Magazine-head field-scope (ITEM-046):** while a chain is in a magazine (between pop from
  the shared stack and either eager-free, freer-side push, fast-path consumption, or the next
  flush), the head's `chainDepth` and `next` fields are **undefined** — they carry whatever
  the prior owner wrote at the prior flush, which need not match the magazine's current
  `m.depth`. The authoritative depth source while in the magazine is `m.depth`; `chainDepth`
  is only re-established at flush time. No code path consumes `m.head->chainDepth` or
  `m.head->next` while the chain is owned by the magazine.
- **Magazine state (DEC-034/037/041):** at all times for each `(CPU, class)` magazine,
  `m.depth == 0 ⇔ m.head == nullptr`. While `m.depth > 0`, walking `chainNext` from `m.head`
  exactly `m.depth - 1` steps reaches a slab whose `chainNext == nullptr` (the chain bottom).
  Freer-side pushes and allocator-side pops preserve the invariant by construction (push:
  extend at `m.head` end; pop: advance `m.head` along `chainNext`). DEC-041 removed the
  separate `m.tail` field — no operation needs the bottom directly.
- **Eager-free floor (DEC-036):** after the allocator-side eager-free walk completes,
  either `m.depth == 1` (single cached slab remains, regardless of state) or `m.head`'s bookkeeper
  reports non-Empty. Stated differently: an Empty slab is never the head of an inbox chain of
  length ≥ 2.
- **Magazine occupancy states (DEC-034/037):** between vmsmalloc / vmsfree calls (i.e. at all
  externally observable moments), slabs in any CPU's magazine chain are either `Partial` or
  `Empty`. They cannot be `Full`: a slab transitions to Full only via `bookkeeper.allocSlot`
  inside vmsmalloc's fast path, and DEC-037 requires the slab be popped from the magazine in
  the same call before vmsmalloc returns. Transiently — between the `allocSlot` atomic that
  drove `becameFull` and the magazine pop — `m.head` is briefly Full, but this window is on
  the same CPU executing the allocator and is not externally observable. (DEC-014/030 prevent
  intra-CPU reentry that could observe it.)
- **Magazine-domain consistency (DEC-019/DEC-034):** every slab in `magazines[i][c]`'s chain
  satisfies `desc->numaDomain == NUMAPolicy::domainFor(i)`. Proof by induction on the paths
  that introduce a slab to the magazine: (a) freer-side same-domain push only runs when
  `desc->numaDomain == numaOf(currentCPU)` (the cross-domain gate); (b) refill from
  `partial[localDomain][c]` only sees slabs whose `desc->numaDomain == localDomain` (same gate
  on the producing side); (c) fresh slab creation sets `desc->numaDomain = localDomain`
  unconditionally (DEC-018). No path introduces a foreign-domain slab. Therefore the partial
  stack `partial[d][c]` likewise contains only slabs with `desc->numaDomain == d`.

## Failure Modes

<!-- What can go wrong and the defined behavior for each. Is it recoverable? Does it propagate to callers?
     Does it panic? -->

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| Arena exhausted (`allocPage` fails on slab-creation path) | Panic (DEC-012) | No |
| Request size > one page | Assert (DEC-004) | No |
| Double-free of a slot | Assert via extended `SlabBookkeeper` (DEC-013) | No |
| Free of non-slab pointer / misaligned pointer | Assert via mask + range + descriptor signature check (DEC-013) | No |
| Partial-slab stack empty on alloc miss | Allocate fresh page via `VMSubstrate::allocPage()`, build descriptor, use as current | Normal path |
| Slab becomes Empty under lazy reclamation | Slab stays where it is (shared stack, inbox, or outbox). If it surfaces as the head of an inbox chain of length ≥ 2 on the allocator slow path, DEC-036 freePages it. Otherwise reused on next pop. Residual quiescent pinning handled by future `scavenge()` (ITEM-009) | Normal — no error |
| `vmsmalloc(0)` | Assert (DEC-023) | No |
| `vmsfree(nullptr)` | Assert (DEC-023) | No |
| Call from IRQ, NMI, #GP, or #MC context | Illegal (DEC-014 amended). Asserts at function entry in debug builds; undefined in release. Use a pre-allocated pool from these contexts. #PF context is *not* in this category — it is conditionally legal per the next row. | No |
| Call from #PF context with transitive re-entrancy (i.e., vmsmalloc work that itself page-faults) | Caller-side bug (DEC-014 amended). The caller-side obligation is that VMSubstrate and RadixVM must not page-fault during vmsmalloc operation; violating this risks same-CPU magazine reentry. No runtime detection — discipline obligation. | No (caller bug) |
| `vmsfree` called on a non-VMSubstrate pointer (stack, kernel-image, unmapped VA) | Range-check `vmsBase ≤ p < vmsBase + vmsSize` runs before `ensureTLBEntryFresh` (DEC-026); out-of-range asserts cleanly. | No |
| `vmsfree` called on a non-page-aligned pointer in the VMSubstrate range but in an unpopulated arena | Page fault inside `ensureTLBEntryFresh` (DEC-031). Loud but less self-explanatory than the descriptor-magic assert. Accepted as a documented gap because RadixVM's free sites are tightly controlled and the residual band is narrow. Page-aligned bad pointers in the same arena go via `freePage` (DEC-029), not this path. | No (caller bug) |
| `vmsmalloc(size)` with `largestSizeClass < size ≤ pageSize` | Served by the DEC-029 whole-page bypass: `VMSubstrate::allocPage()` returns a page-aligned pointer. No slab descriptor. Freed via `VMSubstrate::freePage` on the page-aligned branch of `vmsfree`. | Normal path |
| Mid-call preemption or thread migration on a future preemptive scheduler | Forbidden by DEC-030 (caller-side contract). Callers must disable preemption and migration around every call. Today this holds trivially because CroCOS has no scheduler. | No (caller bug) |
| Bare-pointer access from a CPU other than the allocating CPU | Impossible by construction (DEC-028): the public API returns `SafePtr<T>` and `vmsmalloc`'s raw pointer never escapes the implementation TU. Cross-CPU access through `SafePtr` automatically calls `ensureTLBEntryFresh` on first touch. | N/A |
| Request for a type with `alignof(T)` exceeding its size class's slot alignment | Compile-error at `VMSubstrate::make<T>` via `static_assert` (DEC-025). Cannot reach `vmsmalloc` at runtime. | No |
| `VMSubstrate::reservePerDomainStaticBuffer` or arena creation fails to obtain a physical page during init (DEC-021/DEC-033) | Panic — consistent with DEC-012's exhaustion contract. The kernel cannot continue without vmsmalloc; metadata allocation is one-shot at init and cannot be deferred. The failure path asserts on the first failed page allocation. | No |
| Caller indexes `perDomainBufs[d]` with `d ∉ D` (a DomainID that has no CPU and therefore no allocated buffer) | The `nullptr` slot causes a null-dereference fault at the accessor site — loud and immediate. By DEC-018 / DEC-038 the indexing sites only ever produce DomainIDs in `D`, so this fault indicates a real bug. | No |

## Questions

<!-- Open questions. Status: Open | Deferred (not urgent, not currently blocking).
     Blocking: whether this question blocks decomposition (subsystem) or implementation (leaf).
     Blocked-by: ITEM-nnn dependencies within this table. -->

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| ITEM-001 | Resolved → DEC-001 | | | What alignment guarantees does `vmsmalloc` provide? | Resolved: natural alignment of the size class for power-of-two sizes, `alignof(max_align_t)` for non-pow2 sizes. |
| ITEM-002 | Resolved → DEC-002 | | | What is the partial-slab steal data structure? | Resolved: per-NUMA Treiber stack. Lazy reclamation only — no tombstones, no immediate `freePage`. (Earlier draft mentioning tombstones is superseded by DEC-002.) |
| ITEM-003 | Resolved → DEC-003 | | | What is the size class schema? | Resolved: `constexpr` array of size classes, mirror of `InternalAllocator::slabSizeClasses`. Tuned to RadixVM's actual usage. |
| ITEM-004 | Resolved → DEC-004 | | | What is the max object size handled by `vmsmalloc`? | Resolved: max == 1 page (4 KiB on AMD64). RadixVM is the sole consumer and accepts the constraint. |
| ITEM-005 | Resolved → DEC-002 | | | Is `Empty`-slab eviction immediate or hysteretic? | Resolved by DEC-002: no automatic eviction at all — Empty slabs are kept on the partial-slab stack and reused on next pop. Explicit `scavenge()` deferred as ITEM-009. |
| ITEM-006 | Resolved → DEC-006 | | | How does the allocator find a pointer's size class on free? | Resolved: descriptor at page offset 0; `(SlabDescriptor*)(ptr & ~(pageSize-1))`. |
| ITEM-007 | Resolved → DEC-007 | | | Multi-page slabs for large size classes? | Resolved by DEC-004: no multi-page slabs. Max class fits in a single page. |
| ITEM-008 | Resolved → DEC-011 | | | How are size classes with fewer than 64 slots per page handled? | Resolved: extend `LibAlloc::SlabBookkeeper` to accept arbitrary `SlotCount`. Prerequisite work in LibAlloc. |
| ITEM-009 | Deferred | No | DEC-002, DEC-036 | Should there be an explicit `scavenge()` to physically reclaim Empty slabs from idle Treiber stacks? | DEC-036 already reclaims Empty inbox slabs on the allocator path in active workloads. Residual scope for scavenge: (a) tail-Empty in inbox (one Empty per (CPU, class) on truly quiet systems), (b) Empty slabs on the shared Treiber stack (the eager-free walk does not pop chains it would otherwise leave alone). Less urgent than under DEC-002 alone; still useful for fully-quiescent systems. |
| ITEM-010 | Resolved → DEC-010 | | | Should slabs be allocator-CPU-owned (single owner at a time) or freely shared? | Resolved: implicit ownership via per-CPU current pointer; no atomic owner stamp. |
| ITEM-011 | Resolved → DEC-019 | | | What is the cross-NUMA-domain steal policy on a local partial-stack miss? | Resolved: never steal. NUMA locality of VMM metadata is a hard priority; on local-stack miss the slow path always allocates a fresh page via `VMSubstrate::allocPage`, which is itself best-effort NUMA-local per DEC-018. |
| ITEM-012 | Resolved → DEC-014 | | | Are `vmsmalloc` / `vmsfree` legal from IRQ / NMI context, and what protects per-CPU state from intra-CPU reentry? | Resolved: IRQ and NMI context are both **illegal**. No intra-CPU reentry protection needed; allocator paths run only in process / kernel-thread context. |
| ITEM-013 | Resolved → DEC-023 | | | What are the edge-input contracts: `vmsmalloc(0)` and `vmsfree(nullptr)`? | Resolved: both assert. Strict contract — no malloc-style leniency. RadixVM templates that produce zero-sized allocations must be fixed upstream rather than papered over here. |
| ITEM-014 | Resolved → DEC-024 | | | Does `vmsmalloc` zero-initialize returned memory? Are freed slots poisoned in debug builds? | Resolved: no zero-init (placement-new via `make<T>` remains the initialization contract); debug builds fill freed slots with a poison pattern in `vmsfree` before the bookkeeper free. |
| ITEM-015 | Resolved → DEC-020 | | | How is the `partial[domain][class]` array sized? Compile-time `MAX_NUMA_DOMAINS` (no such constant currently exists) or dynamic sizing at NUMA discovery? | Resolved: runtime-sized after NUMA discovery, backed by `PageAllocator::allocateSmallPage`. Row-major `partial[d * numClasses + c]`. Feeds the init-phase requirement tracked by ITEM-018. |
| ITEM-016 | Resolved → DEC-018 | | | Does `VMSubstrate::allocPage()` guarantee a page whose backing physical memory is in the caller's NUMA domain? | Resolved: `allocPage` is best-effort NUMA-local with fallback under exhaustion. `desc->numaDomain` records the caller's home domain (not the physical placement); drift accepted as a graceful-degradation cost. |
| ITEM-017 | Resolved → DEC-017 | | | Can `VMSubstrate::allocPage()` recursively re-enter `vmsmalloc` (e.g., to allocate internal metadata)? | Resolved: no. `vmsmalloc`/`vmsfree` sit strictly above `allocPage`/`freePage`; the lower layer is guaranteed never to call back. |
| ITEM-018 | Resolved → DEC-021 | | | Which init phase brings vmsmalloc online, and what is the earliest legal caller? | Resolved: `.icd` entry `[VMSubstrateSlab]` in phase `memory_management`, `depends_on = ["VMSubstrate"]`, `routine = "kernel::mm::vmsmallocInit"`. No capability is exposed; consumers (RadixVM, etc.) declare `depends_on = ["VMSubstrateSlab"]` directly by name. |
| ITEM-019 | Resolved → DEC-015 | | | What scheme makes the per-NUMA partial-slab Treiber stack ABA-safe? | Resolved: 64-bit tagged head packing a (descriptor-page-offset, counter) pair against a fixed VMSubstrate base. The entire VMSubstrate virtual range is contained in a single PDPT (512 GiB / 39 bits), so a page-aligned descriptor needs only 27 bits of offset, leaving 37 bits for the counter. |
| ITEM-020 | Resolved → DEC-022 | | | What value does `alignof(max_align_t)` actually take under the kernel's freestanding toolchain (`-mno-sse`, `-mno-sse2`, `-mno-avx`)? | Resolved: verified empirically to be **16 B**. Compiled `static_assert(alignof(max_align_t) == 16)` cleanly with `x86_64-elf-g++ -ffreestanding -nostdlib -mcmodel=kernel -mno-sse -mno-sse2 -mno-avx -mno-red-zone -fno-exceptions -fno-rtti -std=gnu++2b`; the `== 8` variant fails. The SysV AMD64 ABI mandates 16 regardless of SSE availability. |
| ITEM-021 | Resolved → DEC-016 | | | How does a remote freer's read of `desc->magic` / `desc->sizeClass` interact with VMSubstrate's per-CPU TLB-freshness model? | Resolved: `vmsfree` calls `ensureTLBEntryFresh(ptr)` before reading the descriptor; cost paid only on first-touch from this CPU. |
| ITEM-022 | Resolved → DEC-014 | | | Is NMI context a legal caller of `vmsmalloc`/`vmsfree`? | Resolved: NMI is illegal — same status as IRQ. NMI handlers that need VMM metadata must use a pre-allocated pool. |
| ITEM-023 | Resolved → DEC-014 | | | What is the scope of the CLI/STI bracket on the allocator slow path? | Resolved: no CLI/STI bracket is needed — IRQ-context callers are forbidden (DEC-014), so the slow path cannot be re-entered on the same CPU. The "lost slab" hazard is eliminated by construction. |
| ITEM-024 | Resolved → DEC-025 | | | How does `vmsmalloc` satisfy `alignof(T) > alignof(max_align_t)` for types whose `sizeof(T)` lands in a non-pow2 size class? | Resolved: alignment is capped at 16 for non-pow2 size classes; `VMSubstrate::make<T>` carries a compile-time `static_assert` rejecting any `T` whose alignment exceeds the slot alignment of its chosen size class. Callers that need higher alignment must redesign their type. |
| ITEM-025 | Resolved → DEC-026 | | | What is the validation order in `vmsfree` for arbitrary (possibly non-slab, possibly non-VMSubstrate) pointers? | Resolved: a VMSubstrate-range check (`vmsBase ≤ p < vmsBase + vmsSize`) runs *before* `ensureTLBEntryFresh`, so non-VMSubstrate pointers assert cleanly without touching the freshness machinery. |
| ITEM-026 | Resolved → DEC-016 (clarified) | | | Does `ensureTLBEntryFresh` correctly handle a slab page whose physical backing has changed since this CPU last touched it? | Resolved: yes, by construction of VMSubstrate's dirty-bitmap. `setDirtyForOtherCPUs(va)` fires on both `allocPage` (`VMSubstrate.cpp:598`) and `freePage` (`VMSubstrate.cpp:637`), so every PTE mutation sets every remote CPU's dirty bit for that VA. `ensureTLBEntryFresh` invlpgs whenever the bit is set, after which the next access reloads the TLB from the current PTE (current PA). No stale-PA window. |
| ITEM-027 | Resolved → DEC-027 | | | How is the `current[i][c]` per-CPU array sized and initialized? | Resolved: compile-time `static SlabDescriptor* current[MAX_PROCESSORS][numSizeClasses]` in BSS. Zero-initialized by BSS clear; no runtime init needed. Each CPU row is exactly 64 B (one cache line) under the DEC-003 8-class schema. |
| ITEM-028 | Superseded by ITEM-032 | | | Should DEC-004's contract be tightened to "max usable slot ≤ pageSize − sizeof(SlabDescriptor) − alignment slack" rather than "≤ pageSize"? | Folded into ITEM-032 since the broader contradiction with DEC-003 (no class above 512 B) is the more serious issue. |
| ITEM-029 | Resolved → DEC-030 | | | What is the preemption / CPU-migration contract for `vmsmalloc` / `vmsfree`? | Resolved: caller-side obligation. Callers must hold the thread non-preemptible and CPU-pinned for the duration of the call. Today (no scheduler) both conditions hold trivially. When a preemptive scheduler lands, callers must use the scheduler's preempt-disable / migrate-disable primitives before calling, and the vmsmalloc/vmsfree entry assertions extend to check them. Mirrors DEC-014's caller-side framing. |
| ITEM-030 | Resolved → DEC-015 (clarified) | | | When does the ABA counter in the DEC-015 tagged head advance — push only, pop only, or every successful CAS? | Resolved 2026-05-25: push-only. DEC-015 amended to spell out the increment rule and the explicit modular-arithmetic discipline for the 37-bit counter field. |
| ITEM-031 | Resolved → DEC-031 | | | The DEC-026 range check accepts VAs in the 512 GiB VMSubstrate window that fall in *unpopulated* arenas; `ensureTLBEntryFresh` then reads from a per-arena dirty bitmap page that does not exist. Is this addressed in VMSubstrate, or does `vmsfree` need a finer-grained "in an active arena" gate? | Resolved: accept the fault as best-effort detection. RadixVM (sole caller) has tightly-controlled free sites; the residual band is non-page-aligned bad pointers landing in unpopulated arenas (DEC-029's dispatch sends page-aligned cases to `freePage`). Documented as a known gap; revisit only if observed in practice. |
| ITEM-032 | Resolved → DEC-029 | | | DEC-004 (max size = `pageSize`) and DEC-003 (largest class = 512 B) contradict each other for sizes 513..4095. What is the actual contract upper bound? | Resolved: requests larger than `largestSizeClass` bypass the slab machinery and return a whole page from `VMSubstrate::allocPage()`. `vmsfree` disambiguates by alignment — page-aligned pointers go to `freePage`, non-page-aligned pointers use the descriptor path. Supersedes ITEM-028. |
| ITEM-033 | Resolved → DEC-032 + DEC-033 | | | `partial[d][c]` Treiber heads are 8 B each, 8 classes per domain → 64 B = one cache line. All CPUs in a NUMA domain CASing on *different* size classes contend on the same cache line. How is false sharing mitigated? | Resolved by two interlocking decisions: DEC-032 pads each `TreiberHead` to 64 B (one cache line); DEC-033 moves both `partial[]` and `current[]` storage out of static BSS into a dedicated vmsmalloc-metadata arena (top of the VMSubstrate PDPT), sized dynamically from the runtime NUMA domain and CPU counts. Together they eliminate cross-class CAS contention without exploding the static storage footprint. |
| ITEM-034 | Resolved → DEC-028 | | | `vmsmalloc` returns bare `void*`. Reads/writes from a CPU that is not the allocating CPU must go through `VMSubstrate::SafePtr` (or a manual `ensureTLBEntryFresh` call) to avoid the stale-TLB scenario: that CPU's TLB may have a valid entry for the slab page's VA from a prior occupant (before this slab was created) pointing to a *different* PA. The slab's own PA is stable under lazy reclamation (DEC-002), but the *first time* a non-allocating CPU touches the VA the dirty bit (set by `setDirtyForOtherCPUs` on `allocPage`) must drive an invlpg. Is this caller-side obligation explicit enough in the contract, or should `vmsmalloc` return a `SafePtr` directly? | Resolved: `vmsmalloc` / `vmsfree` are demoted to implementation-internal primitives; the only public surface is `VMSubstrate::make<T>` (returns `SafePtr<T>`) and `destroy<T>`. Bare-pointer cross-CPU access is impossible at the source level, so reader-side freshness is type-enforced. |
| ITEM-035 | Resolved → DEC-001 / DEC-008 | | | DEC-001 and DEC-008 disagreed on the slot-0 placement formula. | Resolved 2026-05-25: keep DEC-001's `alignUp(sizeof(SlabDescriptor), max(slotSize, alignof(max_align_t)))`; DEC-008 now references it rather than restating a divergent formula. The max-with-floor variant over-aligns the 8 B class (one slot lost) but is robust against future schema changes that might introduce a sub-16 non-pow2 class. |
| ITEM-036 | Resolved → DEC-037 | | DEC-010, DEC-034 | Dual-ownership race: a slab can simultaneously be CPU A's `current[i][c]` and on the shared partial stack (and later CPU C's `current[j][c]`) when A's `allocSlot` fills the slab to Full but A doesn't drop `current[i][c]` until its next call. A remote freer driving Full→Partial sees `becameAvailable()` and publishes the slab while it's still A's current. The bookkeeper's owner-only `allocSlot` assumption is then violated when A's next call (and C's first call) concurrently allocate on the same slab. | Resolved 2026-05-25 by DEC-037, which dissolves the dual-ownership concept entirely: the separate `current[i][c]` array is eliminated and `magazines[i][c].head` plays the role of "active allocation target". Pop-on-`becameFull` is the natural discipline: the just-filled slab is removed from the magazine before any freer can observe Full→Partial. This closes the race structurally rather than via a discipline on top of two parallel concepts. |
| ITEM-037 | Resolved (Invariants amended) | | DEC-034 | Chain encoding invariant didn't explicitly state that non-bottom chain slabs have **undefined** `desc->next` field values. | Resolved 2026-05-25: Invariants section now reads "Non-bottom chain slabs' `next` fields are undefined while the slab is in a chain — they may hold stale values from any prior flush cycle. They are only written when the slab next becomes a chain bottom (which happens at the next flush in which it appears at the tail position)." |
| ITEM-038 | Resolved → DEC-037, superseded by DEC-041 | | DEC-034 | The **Invariants** section states `m.depth == 0 ⇔ m.head == nullptr ⇔ m.tail == nullptr` for every magazine, but **Concurrency Model** says "`m.tail` is not maintained on the inbox; only the outbox uses tail." After an inbox refill, `m.depth > 0` and `m.head != nullptr` but `m.tail == nullptr` (left over from the prior outbox-flush clear). The triple-equivalence invariant is therefore false in inbox mode. | Resolved 2026-05-25 by DEC-037 (unified magazine eliminates inbox/outbox distinction; walk-on-refill maintains `m.tail` so the triple-equivalence holds unconditionally). Superseded 2026-05-26 by DEC-041: `m.tail` itself is removed from the Magazine struct (the original DEC-037 rationale that flush needed `m.tail` to write the previous-shared-head link was wrong — flush writes `m.head->next` under head-linkage). The invariant degenerates to `m.depth == 0 ⇔ m.head == nullptr`. |
| ITEM-039 | Resolved → DEC-038 (clarified) | | DEC-021, DEC-033 | The init routine calls `PageAllocator::allocateSmallPage(someCPUInDomain(d))` for each per-domain metadata page. NUMA systems can have domains with memory but no CPUs (e.g., persistent-memory-only domains, GPU-attached HBM domains). `someCPUInDomain(d)` is undefined when no CPU is in `d`. | Resolved 2026-05-25: vmsmalloc treats CPU-less domains as non-existent. The init routine only allocates per-domain metadata for the subset `{ d : ∃ CPU c with NUMAPolicy::domainFor(c) == d }`. The page allocator may still serve a *data* page from a CPU-less domain under memory pressure (best-effort NUMA per DEC-018), but `desc->numaDomain` records the caller's home domain — which always has at least the calling CPU — so the slab routes back to a partial-stack row that exists. DEC-038 updated to spell out the CPU-bearing-domain restriction. |
| ITEM-040 | Resolved → DEC-038 | | DEC-021, DEC-033 | DomainID values may be sparse — `PageAllocator.h`'s contract is "perDomainAllocs must be sized to (maxDomainID + 1), with nullptr slots for any domain IDs that have no physical memory." vmsmalloc's `partial[d][c]` and `tuning[d][c]` indexing must decide between (a) packing domains into a contiguous vmsmalloc-internal index, or (b) using raw DomainID with empty rows for sparse IDs. | Resolved 2026-05-25 → DEC-038: use raw DomainID indexing (option b). |
| ITEM-041 | Resolved → DEC-039 | | DEC-037 | **Chain-link corruption race on same-domain `becameFull` pop.** The Cross-CPU race surfaces analysis claimed the pop is safe because "the slab is on the shared stack (from the freer's publish)" — but this only holds for *cross-domain* freers (DEC-019/034 gate sends those to the Treiber). For **same-domain** freers — which extend their own magazine via `desc->chainNext = m.head; m.head = desc` — the freer overwrites the just-Full slab's `chainNext` after the owner's `allocSlot` returned `becameFull` but before the owner's pop reads `chainNext`. Sequence: (1) owner A's `allocSlot(X)` returns `becameFull`; (2) same-domain freer B's `freeSlot(X)` returns `becameAvailable`; (3) B's same-domain push sets `X->chainNext = m_B.head (= W); m_B.head = X` — overwriting `X->chainNext` from A's successor Y to B's previous head W; (4) A's pop reads the now-corrupted `X->chainNext = W` and splices W into A's chain, orphaning Y. | Resolved 2026-05-26 → DEC-039: pre-read `m.head->chainNext` into a local before calling `allocSlot`. `desc->chainNext` can only be mutated by a freer driving the slab Full→Partial, which requires the slab to be Full first — which only happens via the owner's own `allocSlot` atomic. Therefore the pre-read precedes any possible mutation. One extra load per fast-path allocation; cost is negligible. |
| ITEM-042 | Resolved → DEC-040 | | DEC-016, DEC-034 | **Allocator-side TLB freshness gap.** `vmsfree` calls `ensureTLBEntryFresh(p)` before any descriptor read (DEC-016). The allocator side has no such call. When CPU i pops a chain head from the Treiber stack and walks `chainNext`, it reads slab descriptors on pages whose PA may have changed since CPU i last touched the VA. Scenario: CPU i previously popped slab S, used it, flushed S back to the Treiber (CPU i's TLB still caches S's PTE). Some other CPU later pops S, eager-frees S via `freePage` (DEC-036) — this sets CPU i's dirty bit but doesn't invalidate CPU i's TLB. A new slab S' allocates at the same VA. CPU i pops S' from the Treiber and reads S' descriptor without consuming its dirty bit → reads S's stale content. | Resolved 2026-05-26 → DEC-040: call `VMSubstrate::ensureTLBEntryFresh(slab)` on each popped slab page before reading its descriptor during the refill walk. The cost is one freshness check per chain element on the slow path, amortized over K alloc operations by DEC-034. The check is structurally cheap (single dirty-bit test in the common case where the TLB is already fresh). |
| ITEM-043 | Resolved → DEC-041 | | DEC-034, DEC-040 | **Single-CAS pop is not actually realizable under the bottom-linkage chain encoding.** The Invariants section originally said "only the *bottommost* slab's `next` field participates in the shared-Treiber linkage (set to the previous shared-stack head at flush time)" and the flush code was consistent (`m.tail->next = oldHead.descPtr`). But the Concurrency Model claimed refill is "One Treiber CAS pops a chain head from `partial[d][c]`." A Treiber pop CAS requires the new head value (what was below the popped chain on the shared stack) — under bottom-linkage that value lives in `bottom->next`, unreachable without first walking `chainNext` for `chainDepth - 1` hops. So the pop would be *walk-then-CAS*, not a single CAS, and the walk reads descriptor fields before any CAS provides an acquire fence. | Resolved 2026-05-26 → DEC-041: chain-head, not chain-bottom, carries the shared-stack `next` link (the encoding the rest of the spec was implicitly assuming). With head-linkage, pop is genuinely single-CAS, and `m.tail` becomes vestigial and is dropped. The post-pop chain walk in DEC-040 becomes a pure TLB-freshness sweep. |
| ITEM-044 | Resolved → DEC-042 | | DEC-015, DEC-034, DEC-037, DEC-039, DEC-040, DEC-041 | **Memory ordering of Treiber CASes and surrounding non-atomic writes is unspecified.** The flush sequence does several non-atomic writes — `m.head->chainDepth = m.depth`, `m.head->next = oldHead.descPtr` (per DEC-041), and prior to that the descriptor fields written at slab creation (`magic`, `sizeClass`, `numaDomain`, bookkeeper state) — then publishes via push-CAS on `partial[d][c]`. A remote popper does the pop-CAS, then reads chainDepth / chainNext / bookkeeper / descriptor fields. For the non-atomic writes to be visible to the popper, the push CAS must be release and the pop CAS must be acquire (or both SeqCst). RELAXED CASes would compile but produce silent reads of partially-published descriptor state on weak architectures (and even on x86 the compiler can reorder around RELAXED). DEC-039's pre-read correctness analysis similarly assumes acq-rel on the bookkeeper's `allocSlot`/`freeSlot` atomics. | Resolved 2026-05-26 → DEC-042: pin push CAS = release, pop CAS = acquire, `chainNext` atomic ops = RELAXED (synchronization piggybacks on surrounding acq-rel), tuning counters = RELAXED, and add a hard dependency that `LibAlloc::SlabBookkeeper`'s atomics are at least acq-rel. |
| ITEM-045 | Resolved → DEC-039 (justification extended) | | DEC-019, DEC-034, DEC-039 | **DEC-039's race-freedom proof omits cross-domain push as a `chainNext` mutator.** The proof enumerates only (a) CPU A's own operations and (b) same-domain freer pushes, but the cross-domain singleton push (DEC-019/034) *also* writes `desc->chainNext = nullptr` as part of its publish sequence. Is the pre-read fix still sound when the mutator is a cross-domain freer? | Resolved 2026-05-26: yes. The pre-read fix works for both pusher categories. Cross-domain push also requires `transition.becameAvailable()` (Full→Partial), which requires the slab to first be Full, which can only happen via CPU A's own `allocSlot`. The happens-before chain `A.pre_read [seq-cb] A.allocSlot release [sync-with] freer.freeSlot acquire [seq-cb] freer.chainNext_write` holds identically for cross-domain freers. DEC-039's enumeration is amended to include case (c) — cross-domain singleton push. Justification gap, not a correctness gap. |
| ITEM-046 | Resolved → Invariants tightened | | DEC-034, DEC-041 | **Chain encoding invariant is overstated.** "For a chain of length `n >= 1`, the chain head has `chainDepth == n` and `next` points to the next chain head" was written as if it held unconditionally, but it only holds for chains *on the shared Treiber stack* (or at flush time, after the flushing CPU writes `m.head->chainDepth = m.depth` and `m.head->next = oldHead.descPtr`). While a chain is in a magazine, the head's `chainDepth` and `next` are left in whatever state the prior owner wrote at flush — possibly stale relative to `m.depth`. Does any code path consume those fields while the chain is in a magazine? | Resolved 2026-05-26: no consumer reads `m.head->chainDepth` or `m.head->next` while the chain is owned by the magazine. The magazine's authoritative depth is `m.depth`; flush re-establishes both fields before the publishing CAS. Invariants section is tightened to scope the `chainDepth == n` clause to shared-stack chains and to add an explicit "magazine-head field-scope" invariant stating that `m.head->chainDepth` / `m.head->next` are undefined while in a magazine. Clarity fix, no behavioral change. |
| ITEM-047 | Resolved → Hazard added | | DEC-038 | **DEC-038's "page-fault on misindex" diagnostic claim is page-granular, not row-granular.** On a multi-socket system where a metadata page straddles both CPU-bearing and CPU-less DomainID rows, the page is mapped (because at least one of its covered rows is for a CPU-bearing domain), so erroneous indexing into the CPU-less rows on that page silently reads/writes zero-initialized memory rather than faulting. Is this acceptable, or does the design need a row-level guard? | Resolved 2026-05-26: accepted as a documented limitation. The diagnostic property is "free side-benefit on pure CPU-less pages" — it applies whenever a page covers *only* CPU-less rows. Mixed pages silently expose those rows. Mitigation if it ever matters: add a `domainHasCPU(d)` predicate check on the freer's hot path before indexing (one branch). Out of scope today — RadixVM's freer always reads `desc->numaDomain` from a slab descriptor created on a CPU-bearing domain (DEC-018), so the corner case requires *both* a buggy `desc->numaDomain` *and* the buggy domain falling in a mixed metadata page. Hazard added to flag any future regression that broadens the trust surface. |
| ITEM-048 | Resolved → DEC-026 amended | | DEC-013, DEC-026 | **vmsfree validation needs an explicit lower-bound check on `p`.** DEC-026 step (6) ("`desc->magic` + alignment / slot-arithmetic check") relies on `(p - slotZeroAddr(desc)) % desc->slotSize == 0` to validate slot alignment, but does not pin a check on `p >= slotZeroAddr(desc)`. A buggy `p` pointing inside the descriptor region (offsets 0..slot0Offset−1) could satisfy the modulo check via unsigned underflow (e.g. for `slotSize == 8` and `p == 8`, `slot0Offset == 64`: `(8 - 64) mod 8 == 0` in two's complement). Without a lower-bound check, the buggy free could splatter into the descriptor's storage. | Resolved 2026-05-26: amend DEC-026 step (6) to require `slotZeroAddr(desc) ≤ p < slotZeroAddr(desc) + desc->slotSize * desc->slotCount` *before* the modulo arithmetic. Two extra compares per vmsfree (negligible on the hot path). Closes the underflow corner. Implementation detail that was implicit in "p within slab data range" but worth pinning. |
| ITEM-049 | Resolved → DEC-045 amended | | | **`SlabDescriptorBase` field list omits `slotSize` despite DEC-045 calling it a denormalized descriptor field.** DEC-045 lists the uniform fixed prefix as holding `magic, sizeClass, numaDomain, next, chainNext, chainDepth, padding` (≈ 40–48 B). Later in the same decision: "`desc->slotSize` *is* retained as a denormalized field (vs. a third table) only because it's read on the fast-path side of `vmsfree` (between the magic check and the modulo check) where a table lookup adds a measurable indirection". The two statements disagree on whether `slotSize` is in `SlabDescriptorBase` (and the field list is incomplete) or derived from `sizeClass` via the `constexpr` table (and the "denormalized field" claim is misleading). | Resolved 2026-05-26: defer the precise field layout to the LibAlloc implementation. DEC-045 amended to pin only the abstract read interface vmsmalloc consumes (`magic`, `sizeClass`, `numaDomain`, `next`, `chainNext`, `chainDepth`, plus a `slotSize(desc)` / `slotCount(desc)` accessor pair); whether each accessor is a direct prefix field, a denormalized cache, or a constexpr-table lookup keyed by `sizeClass` is an implementation choice on LibAlloc's side. DEC-026 step 6a/6b reframed to use the accessor form. |
| ITEM-050 | Resolved → DEC-046 | | | **VMSubstrate::allocPage's contract for the calling CPU's local TLB / dirty-bit state is load-bearing but not pinned.** The Concurrency Model's fresh-allocation branch states: "No `ensureTLBEntryFresh` needed on a freshly `allocPage`'d page for the calling CPU — `allocPage` clears the calling CPU's own dirty bit and installs the PTE; any prior stale entry was cleared at the previous `freePage`." DEC-016's clarification confirms `setDirtyForOtherCPUs` fires on both `allocPage` and `freePage` — but that primitive by name affects only *other* CPUs' dirty bits. The fresh-page correctness relies on a separate, currently-unstated guarantee: the calling CPU's own TLB is invalidated (or guaranteed cold) for the returned VA when `allocPage` returns, AND the calling CPU's own dirty-bitmap bit is cleared. | Resolved 2026-05-26 → DEC-046: pin the contract that `VMSubstrate::allocPage` invalidates the calling CPU's own TLB entry for the returned VA (via `invlpg`) and clears the calling CPU's own dirty-bitmap bit before returning. Already implemented in `VMSubstrate.cpp`'s allocPage path; DEC-046 records the contract so future changes preserve the property. The Concurrency Model's fresh-allocation branch is unchanged in behavior, now backed by a pinned dependency. |
| ITEM-051 | Resolved → Dependencies amended | | | **PageAllocator per-CPU pool availability for not-yet-running APs at memory_management init phase.** DEC-021's init routine runs in `memory_management`, which precedes `smp_bringup` — only the BSP is executing. The routine calls `PageAllocator::allocateSmallPage(cpuInDomain(d))` and `PageAllocator::allocateSmallPage(NUMAPolicy::domainFor(i))` (per-CPU magazine pages) for AP logical IDs whose physical processors are still parked. | Resolved 2026-05-26: the PageAllocator dependency row is amended to pin the contract that per-CPU pools are sized for all detected logical CPUs at PageAllocator init time and that `allocateSmallPage(apID)` is callable from the BSP for any detected logical CPU ID before that CPU comes online (the pool index is structurally addressable; the call uses the AP's `nearestPool` to honor NUMA placement intent). Already implemented; DEC-021's init routine works as written. |
| ITEM-052 | Resolved → DEC-014 amended | | | **CPU faults, MCE, and async-exception reentry into vmsmalloc are not addressed by the context contract.** DEC-014 forbids IRQ/NMI; DEC-030 forbids preemption/migration mid-call. Neither covers same-CPU reentry via a hardware fault (#PF, #GP, #UD), Machine Check (#MC), or future async-exception delivery. Today no fault handler in CroCOS calls into vmsmalloc, so the gap is vacuous — but if a future change adds a fault handler that needs to allocate VMM metadata (e.g., to log a fault into a heap-allocated structure), it would silently corrupt magazine state on the interrupted thread's CPU (same-CPU reentry into the slow path between `m.head` writes). | Resolved 2026-05-26: DEC-014 amended. **Page-fault context (#PF) is conditionally legal** — a page-fault handler may call into `vmsmalloc` / `vmsfree` (e.g., for CoW allocation), with the caller-side obligation that no transitive re-entrancy occur. Concretely: `VMSubstrate` itself and RadixVM must never page-fault during their operation, so a PF handler that uses vmsmalloc only for non-PF-causing work is safe. **#GP and #MC remain illegal callers** — no use case identified, and same-CPU reentry hazard is identical to IRQ. Debug-build entry assertion is "not in IRQ/NMI/#GP/#MC context"; #PF is permitted. |

## Decisions

<!-- Design decisions, both those resolved from Questions and those recorded directly.
     Questions resolved here retain their ITEM-nnn. New decisions take the next available ITEM-nnn.
     Certainty — settled: changing requires significant rework; provisional: best current guess. -->

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| DEC-001 | Settled | Returned pointer alignment: for power-of-two size classes, alignment == `slotSize` (so 16 B → 16, 256 B → 256, etc.); for non-pow2 classes (e.g. 96 B), alignment is `alignof(max_align_t)` (= 16 B on this toolchain per DEC-022). Slot 0 within each slab is placed at `alignUp(sizeof(SlabDescriptor), max(slotSize, alignof(max_align_t)))` to satisfy this for every class. | Matches malloc convention. Large pow2 classes naturally get cache-line alignment for free; small classes still get the minimum the C++ memory model expects. |
| DEC-002 | Settled (refined by DEC-034) | Partial-slab pool is a per-NUMA-domain Treiber stack. **Lazy reclamation only**: a slab that transitions to Empty stays on the stack with its data page intact. The next popper that encounters an Empty slab reuses it as-is for the same size class. No tombstones, no immediate `freePage`. ABA-safety is provided by DEC-015's tagged-head encoding. **Refined by DEC-034:** the stack now stores chains of slabs rather than singleton descriptors — `desc->next` continues to link chain *heads*, and each head bears a `chainNext`-linked chain of K−1 sub-elements (chain depth recorded in `head->chainDepth`). Reclamation policy is otherwise unchanged: Empty slabs encountered anywhere in a chain stay live and are reused on the next pop. | Treiber gives O(1) push/pop and unbounded capacity, no per-domain bitpool memory tax. Eager reclamation was originally desired but requires off-page descriptors, which conflict with DEC-008 (and DEC-008 was forced by DEC-006's "no dynamic allocation at this layer"). Quiescent-system page lingering is mitigable by a future explicit scavenge pass (ITEM-009). The chain-head refinement under DEC-034 amortizes shared-stack CAS cost across K transfers without changing the Treiber's ABA encoding or the lazy-reclaim policy. Decided 2026-05-20, refined 2026-05-25. |
| DEC-006 | Settled | Pointer→slab lookup is `(SlabDescriptor*)(ptr & ~(pageSize - 1))` — the slab descriptor lives at offset 0 of its own data page. No side table, no metaslab pool. | Each slab is exactly one page (DEC-007), so masking yields the page-aligned base address directly. There is no general-purpose allocator available at this layer for an off-page descriptor table, so the descriptor must live in memory the slab itself owns. Decided 2026-05-20. |
| DEC-008 | Settled (revised by DEC-006, formula unified with DEC-001 via ITEM-035) | Slab descriptors live at offset 0 of the slab's data page; slot 0 starts at the offset specified by DEC-001 (`alignUp(sizeof(SlabDescriptor), max(slotSize, alignof(max_align_t)))`). The data page is the descriptor's only storage. | Forced by DEC-006: no separate allocation pool exists. Slot-density cost is paid once per slab in the form of one or more reserved slots near offset 0. The previous DEC-008 wording (`alignUp(sizeof(SlabDescriptor), slotSize)`) under-aligned the 8 B class relative to DEC-001's contract for hypothetical sub-16 non-pow2 classes; the unified formula is robust against future schema changes. |
| DEC-003 | Settled | Size classes are a `constexpr` array, mirroring `LibAlloc::InternalAllocator::slabSizeClasses` (currently `{8, 16, 32, 64, 96, 128, 256, 512}`). Final values are tunable based on RadixVM measurements. | Established pattern in the codebase; trivial to retune. Avoids the cost of a runtime size-class machinery for a workload with a fixed consumer. |
| DEC-004 | Settled (refined by DEC-029) | Maximum object size is one page (4 KiB on AMD64). Requests above this assert. Requests in the band `largestSizeClass < size ≤ pageSize` are served via the DEC-029 whole-page bypass, not the slab path. | RadixVM is the sole consumer and is being written with this restriction in mind. Eliminates multi-page-slab complexity entirely. |
| DEC-007 | Settled (implied by DEC-004) | No multi-page slabs. Each slab occupies exactly one VMSubstrate page. | Direct consequence of DEC-004 — the largest size class fits comfortably in a single page. |
| DEC-010 | Settled (rewritten 2026-05-25 by DEC-037) | Slab ownership is tracked implicitly via the magazine's head pointer: while `magazines[i][c].head == desc` and `magazines[i][c].depth > 0`, CPU `i` is the sole allocator that may call `bookkeeper.allocSlot(desc)`. There is no separate `current[]` array — the magazine head plays both the "active allocation target" role and (when extended by freers) the "freshly pushed Partial slab" role; they are temporally distinct on a single CPU per DEC-014/030, so the same struct serves both. On allocator success that drives `transition.becameFull()`, the owner pops `m.head` synchronously and the slab becomes orphaned-Full; the owner does **not** keep a stale reference to a Full slab. Freers never touch magazine ownership state on remote CPUs; they call `bookkeeper.freeSlot` and, on a `Full→Partial` transition, push the descriptor either into the local magazine (same-domain) or as a singleton onto the home-domain Treiber (cross-domain). A `Full` slab dropped by the becameFull-pop is "lost" until a freer transitions it back to Partial; this is safe because no allocator could use a Full slab anyway. | Composes naturally with the Treiber-stack pool (DEC-002) and the chain-magazine layer (DEC-034). Avoids the atomic-stamp contention `BigPageMetadata::allocHolder` deals with — the magazine head pointer is per-CPU and uncontended (DEC-014/030 prevent intra-CPU reentry). The synchronous pop-on-`becameFull` is what closes the dual-ownership race ITEM-036 surfaced; the original DEC-010 framing (drop at next miss) left a window in which a remote freer could republish a still-owned slab. The Full-slab "lost until freer" pattern is bounded — only one such slab per (CPU × size class) can exist at a time. Decided 2026-05-20, rewritten 2026-05-25 (DEC-037). |
| DEC-011 | Settled | `LibAlloc::SlabBookkeeper` accepts any positive `SlotCount`. The bookkeeper is template-parameterized; the bitmap word count is `kWordCount = (SlotCount + 63) / 64` (ceiling division); the tail word's high bits in `[SlotCount, kWordCount * 64)` are permanently masked as occupied at init (either via a `seedAllAvailable(usableCount)` variant or by calling `reserveSlot` for each tail bit). Allocator paths see only valid slots because the masked-off bits never appear in either the alloc-bitmap (always 0 → never claimable) or the free-bitmap (never released). The current `Slab.h` carries an artificial `SlotCount % 64 == 0` restriction (Slab.h:43–44) that pre-dates this spec — a TODO comment marks the lines for removal, tracked as a Phase-1 prerequisite. **Independent of vmsmalloc's needs, the restriction is unjustified**: nothing in `Core::SplitBitmap`'s design requires whole-word slot counts; the constraint exists only because the bookkeeper hasn't yet needed sub-word sizing. **vmsmalloc relies on this generalization for DEC-003 classes 64+**: under the current restriction, class 64 would need ≥ 64 slots × 64 B = 4096 B of slot data, which doesn't fit in a single page after the descriptor (DEC-007/008); classes 96, 128, 256, 512 have the same problem. With the generalization, the per-class `slotCount[c]` table (DEC-045) takes the actual maximum (e.g. 7 slots for class 512, 15 for class 256). **DEC-042 #4 is already satisfied at the implementation level**: `allocatedCount.fetch_add(1, ACQ_REL)` (Slab.h:93) and `.fetch_sub(1, ACQ_REL)` (Slab.h:129) provide the required acq-rel ordering — the Phase-1 LibAlloc obligation is to *document* the contract in the header, not change behavior. | Cleanest single-code-path bookkeeper; the alternative of a parallel "small-slab" implementation duplicates state machine logic. Cost is borne by LibAlloc, not vmsmalloc. Decided 2026-05-20, reframed 2026-05-26 (restriction treated as a Slab.h defect, not a constraint to work around). |
| DEC-012 | Settled | Arena exhaustion (a failed `VMSubstrate::allocPage()` on the slab-creation path) panics. `vmsmalloc` never returns null. | Consistent with the kernel's prevailing exhaustion handling (the page allocator already asserts on resource exhaustion). Simpler contract; callers don't need null-check boilerplate. RadixVM accepts this. Decided 2026-05-20. |
| DEC-013 | Settled | `vmsfree` asserts on double-free and on non-slab / misaligned pointers. Double-free requires extending `LibAlloc::SlabBookkeeper`/`SplitBitmap` so `releaseBit` (or a sibling) detects releasing an already-free bit. Pointer sanity is checked by mask + range + descriptor signature (e.g. a magic field at offset 0 of the descriptor). | Catches the worst class of RadixVM bug at the point of corruption rather than later. Hot-path cost is small (one branch, one tagged load); the bug-finding value is high during development. Adds a second LibAlloc-side prerequisite. Decided 2026-05-20. |
| DEC-014 | Settled (paired with DEC-030; amended 2026-05-26 by ITEM-052: #PF conditionally legal) | **IRQ-, NMI-, #GP-, and #MC-context callers are illegal. Page-fault (#PF) context is conditionally legal.** `vmsmalloc` / `vmsfree` may be called from process / kernel-thread context unconditionally, and from a #PF handler *only if the caller can guarantee no transitive re-entrancy* — concretely, `VMSubstrate` itself and RadixVM (the sole current vmsmalloc consumer) must never page-fault during their operation, so a #PF handler that uses vmsmalloc for non-PF-causing work (e.g. CoW page allocation) is safe. The other synchronous fault classes (#GP, #MC, #UD, etc.) have no identified use case and remain illegal — same-CPU reentry hazard is identical to IRQ. No CLI/STI bracket is needed on the per-CPU magazine path (DEC-037) because no IRQ handler can re-enter on the same CPU and observe partial magazine state; #PF reentry is bounded by the no-transitive-PF caller obligation. Debug builds should assert "not in IRQ/NMI/#GP/#MC context" at function entry (#PF context is permitted). Kernel paths that must allocate VMM metadata from a forbidden context must use a pre-allocated pool. Paired with DEC-030's preemption/migration contract: together they guarantee that the magazine-update sequences (allocator's pop-on-`becameFull`, freer's chain extension, flush) cannot be re-entered on the same CPU and cannot observe inconsistent `getCurrentCPU()` reads. | Eliminates the slow-path "lost slab" hazard (ITEM-023) by construction. The magazine-mutation sequences are uninterruptible by IRQ-driven allocator reentry, so no atomic-current-publication scheme is needed. Removes the asymmetry between fast path (CLI-protected) and slow path (uncovered), which was the central correctness gap in the previous formulation. The ITEM-052 amendment opens the #PF path because CoW and similar VMM operations naturally call into vmsmalloc from PF handlers; the corresponding caller-side obligation (VMSubstrate and RadixVM must not themselves page-fault) is the cost. Decided 2026-05-21, amended 2026-05-26 (ITEM-052). |
| DEC-015 | Settled (clarified by DEC-034, ABA rule pinned by ITEM-030) | Per-NUMA partial-slab Treiber stack uses a 64-bit tagged head: `head = (counter << 27) \| (descPageOffset >> 0)` where `descPageOffset = (descPagePA - vmsBase)` is bits [38:12] of the descriptor's VA within the VMSubstrate range. The VMSubstrate range is a single PDPT (512 GiB), so 27 bits suffice for any page-aligned descriptor offset and 37 bits remain for the ABA counter. Push/pop use a single 64-bit CAS on the head. The encoding lives next to the stack definition and is asserted at boot to fit the actual VMSubstrate VA window. **ABA counter advance rule (push-only, ITEM-030):** every successful push CAS computes the new head as `newHead = ((oldCounter + 1) & kCounterMask) << 27 \| newDescPageOffset` where `kCounterMask = (1ull << 37) - 1`. Pop CAS does **not** bump the counter — it computes `newHead = (oldCounter << 27) \| nextDescPageOffset` (the popped slab's `next` slab's offset, with the head's existing counter carried through). Push-only is sufficient because any "head pointer returned to A" sequence requires at least one push of A in between. **Wraparound discipline:** the explicit `& kCounterMask` is load-bearing — without it, an unchecked `oldCounter + 1` at the 37-bit boundary would carry into bit 27 (the descriptor-offset MSB) and corrupt the next push's encoded offset. Implementation must mask before shifting, every push. Residual ABA hazard at wraparound is bounded by the time to issue 2^37 successful pushes against one `partial[d][c]` head; at 10M pushes/sec (singleton transfers) this is ~14 hours, multiplied by K under DEC-034 chain-transfer (~14 × K hours at the same effective alloc/free rate). For a stalled CAS to falsely succeed at wraparound, the stalled thread would need to be off-CPU for that entire window while exactly the same descriptor cycled to the head — practically impossible but worth a one-line comment in the implementation. **Under DEC-034:** the offset references the chain *head*, not an arbitrary chain element. Chain elements are reachable only through the head's `chainNext` and never participate in the tagged-head encoding. The encoding is therefore unchanged by the introduction of magazines. | Cheapest viable scheme that fits naturally given the VMSubstrate's fixed, narrow VA window. Avoids `CMPXCHG16B`'s 16 B alignment requirement and the complexity of per-CPU hand-off / hazard pointers. Push-only counter advance keeps the rule simple (one increment site, in push's CAS-success path) and the hazard analysis matches the existing math. The explicit mask defends against a class of subtle bugs that would otherwise only surface ~14 hours into a stress run. If the VMSubstrate range ever expands beyond a single PDPT, the boot-time assert fires and the encoding can be revisited. Decided 2026-05-21, clarified 2026-05-25 (DEC-034, ITEM-030). |
| DEC-016 | Settled | `vmsfree(p)` calls `VMSubstrate::ensureTLBEntryFresh(p)` after the nullptr / interrupt-context / range-check guards (DEC-026), before reading any descriptor field (magic, sizeClass, numaDomain, bookkeeper). The check is unconditional but cheap: `ensureTLBEntryFresh` is structured so the steady-state path (TLB entry already valid for this CPU) is a single dirty-bit test, with `invlpg` only fired on actual first-touch. **Stale-PA on recycled pages is handled by construction:** VMSubstrate's `setDirtyForOtherCPUs(va)` fires on both `allocPage` (`VMSubstrate.cpp:598`) and `freePage` (`VMSubstrate.cpp:637`), so any `allocPage → freePage → allocPage` cycle on the same VA leaves every remote CPU's dirty bit sticky-set. The next remote `ensureTLBEntryFresh` invlpgs and the hardware reloads the current PTE / current PA — there is no window in which a remote freer can read `desc->magic` from a previous occupant of the VA. | Localizes the cost to the freer hot path; no changes to VMSubstrate or to the in-page descriptor placement (DEC-006/DEC-008). The alternative of mapping descriptors in a globally-fresh region would force off-page descriptors (conflicts with DEC-006/DEC-008); tightening the substrate to eager-shootdown on every `allocPage` would push IPI cost onto every slab creation and into every other `allocPage` caller. The dirty-bitmap-on-every-PTE-mutation guarantee is what makes the lazy-reclamation policy (DEC-002) safe — confirmed against the VMSubstrate implementation 2026-05-21. Decided 2026-05-21, clarified 2026-05-21 (ITEM-026). |
| DEC-017 | Settled | `vmsmalloc` / `vmsfree` are strictly layered above `VMSubstrate::allocPage` / `freePage`. The substrate guarantees these page-level entry points never call back into `vmsmalloc` (directly or transitively); the slow path may therefore interleave `allocPage` with magazine-state updates without reentry-defense. Any future VMSubstrate change that wants slab-allocated metadata must use a different mechanism (bootstrap pool, PageAllocator directly, etc.), not `vmsmalloc`. | Clean layering — same shape as PageAllocator vs. VMSubstrate. Avoids the extra ordering rules and proof obligation that the reentrant alternative would impose on every line of the slow path. The constraint on VMSubstrate's internal metadata is mild because metadata budget is already bounded by the radix tree pre-allocation. Decided 2026-05-21. |
| DEC-018 | Settled | `VMSubstrate::allocPage` is best-effort NUMA-local (it forwards the caller's CPU into `PageAllocator::allocateSmallPage(cpu)` → `nearestPool(cpu)`, which falls back to a remote pool when the local one is exhausted). vmsmalloc records the *caller's home domain* in `desc->numaDomain` at slab creation: `desc->numaDomain = NUMAPolicy::domainFor(currentCPU())`. The recorded domain does not necessarily equal the physical placement domain; under local-exhaustion fallback the values diverge until the slab is destroyed. | Cheap (no VA→PA→domain lookup on the slow path) and keeps the slab descriptor's home domain stable for the partial-stack routing rule (Full→Partial push uses `desc->numaDomain`). On a healthy NUMA system the recorded domain matches physical placement; under exhaustion the worst case is a partial-stack reuse pattern that's no worse than the b-option for any one slab, but consistently routes future allocators of the caller's domain to a slab they wrote to first. Accepts the graceful-degradation trade-off rather than paying the lookup cost on every slab creation. Decided 2026-05-21. |
| DEC-019 | Settled (extended by DEC-034, DEC-037) | **No cross-NUMA-domain steal.** When the local magazine is empty and `partial[localDomain][c]` is empty on the allocator slow path, `vmsmalloc` goes directly to `VMSubstrate::allocPage()` to build a fresh slab; remote partial stacks are never consulted, even if they have stock. Concretely the slow path is: (1) refill magazine via Treiber pop from `partial[localDomain][c]`; (2) on shared-stack-empty, bump `starvationCount` and `allocPage()` + init descriptor with `desc->numaDomain = localDomain`. **Magazine cross-domain gate (DEC-034):** the freer side enforces the symmetric rule — a freer whose own domain differs from `desc->numaDomain` skips its own magazine entirely and pushes `desc` as a singleton chain directly onto `partial[desc->numaDomain][desc->sizeClass]`. Magazines therefore hold same-domain slabs only, preserving the "VMM metadata stays on its home domain" intent on both the alloc and free hot paths. | NUMA-local placement of VMM metadata is a hard priority for the RadixVM workload — stealing a remote slab would pin VMM structures off-node for the slab's lifetime, which is much worse than the (bounded) cost of an extra `allocPage`. `allocPage` is itself best-effort NUMA-local per DEC-018, so the fresh page is almost always local; under local exhaustion both "steal" and "allocPage" would yield remote memory anyway, and the spec prefers the simpler/more predictable rule. Eliminates the ping-pong hazard entirely and removes the need for the K-counter or fallback-order machinery the alternatives required. The magazine gate (DEC-034) keeps cross-domain free latency identical to plain Treiber while leaving the magazine layer focused on its design target (same-domain fan-out). Decided 2026-05-21, extended 2026-05-25 (DEC-034/037). |
| DEC-020 | Superseded by DEC-033 | The per-NUMA-domain `partial` array is sized at vmsmalloc init time, after NUMA discovery. Storage is one small page (4 KiB) obtained from `PageAllocator::allocateSmallPage()`, indexed row-major as `partial[d * numSizeClasses + c]` where `d ∈ [0, NUMAPolicy::domainCount())` and `c ∈ [0, numSizeClasses)`. A 4 KiB page holds 512 × 8 B `TreiberHead` entries, so the layout supports up to `512 / numSizeClasses` domains (64 with the 8-class schema from DEC-003), which is far beyond any realistic NUMA system. Init asserts `domainCount * numSizeClasses * sizeof(TreiberHead) ≤ pageSize`. | Superseded by DEC-033: cache-line padding (DEC-032) inflates `partial` storage past one page in any non-trivial NUMA system, so the original "fits in one page" assumption no longer holds. Storage moves to the dedicated vmsmalloc-metadata arena and sizes dynamically. Original decision retained for historical context — the runtime-sizing and row-major indexing carry forward; only the storage source and ceiling change. Decided 2026-05-21, superseded 2026-05-21. |
| DEC-021 | Settled (rewritten 2026-05-26 for the per-domain-buffer + per-CPU-arena-cache split) | vmsmalloc registers a `[VMSubstrateSlab]` entry in `kernel/general.icd`: `phase = "memory_management"`, `required = true`, `per_cpu = false`, `depends_on = ["VMSubstrate"]`, `routine = "kernel::mm::vmsmalloc::vmsmallocInit"`. No capability is exposed; downstream consumers (RadixVM, future kernel users) declare `depends_on = ["VMSubstrateSlab"]` by name. **Init routine algorithm:** (1) Derive the CPU-bearing-domain set `D = { NUMAPolicy::domainFor(i) : i ∈ [0, cpuCount) }` (DEC-038 / ITEM-039). (2) Compute the per-domain buffer size as `constexpr size_t kPerDomainBufBytes = roundUpToPage(kPartialOffset + kNumSizeClasses * sizeof(TreiberHead) + kTuningOffset + kNumSizeClasses * sizeof(MagazineTuning))` — declared as a `constexpr` in `VMSubstrateSlab.h` so VMSubstrate's arena-layout math and vmsmalloc's accessors derive from the same value. (3) For each `d ∈ D`: call `void* buf = VMSubstrate::reservePerDomainStaticBuffer(kPerDomainBufBytes, d)`, store `buf` in module-local `perDomainBufs[d]` (a `static` array of `void*` indexed by raw DomainID, sized to `maxDomainID + 1`; non-`D` slots stay null). Zero the buffer (or rely on `reservePerDomainStaticBuffer`'s zero-fill guarantee, see DEC-033). Walk each `MagazineTuning` entry and store `currentK = kInitialK`. (4) For each `i ∈ [0, cpuCount)`: get `void* cache = VMSubstrate::localCachePageFor(i)`, zero the `kNumSizeClasses` `Magazine` entries (zeroing produces `head = nullptr`, `depth = 0`; per DEC-041 there is no `tail` field). `localCachePageFor` does not allocate — the page was allocated and mapped by VMSubstrate when arena `i` was created, on `NUMAPolicy::domainFor(i)`. (5) Capture `vmsBase` and `vmsSize` from VMSubstrate's published VA window into module-local constants for `vmsfree`'s range check (DEC-026). (6) Log a `klog::info` summarizing per-domain buffer count and per-CPU cache pages initialized. After return, `vmsmalloc` / `vmsfree` are legal callers. Per-domain buffers and per-CPU cache pages are pinned for the lifetime of the kernel; metadata never grows or shrinks. | Transitive dependency on VMSubstrate gives the right ordering: VMSubstrate already depends on PageAllocator which depends on `numa_topology`, and `createArena` (which now allocates the per-CPU cache page) runs inside VMSubstrate's `init`. The routine has no concurrency concerns — runs on the BSP before `smp_bringup`. The two-API split (`reservePerDomainStaticBuffer` for global per-domain storage, `localCachePageFor` for arena-resident per-CPU storage) reflects the physical-locality split: per-domain buffers are accessed cross-CPU (any CPU can CAS on `partial[d][c]` of any domain `d`) and live in a dedicated NUMA-distributed region; per-CPU caches are accessed only by their owning CPU (DEC-014/030) and live in the CPU's existing arena where they're automatically on the right NUMA domain. No `installKernelPTE` is needed — both primitives encapsulate the page-table work behind a higher-level interface, and address arithmetic flows through VMSubstrate's existing arena-style math (architecture-portable via `arch::pageTableDescriptor`). Decided 2026-05-21, revised 2026-05-21 (DEC-033), rewritten 2026-05-25 (DEC-033 lightweight + DEC-037 unified), restricted 2026-05-25 (DEC-038/ITEM-039), rewritten 2026-05-26 (per-domain-buffer + per-CPU-arena-cache split). |
| DEC-022 | Settled | `alignof(max_align_t) == 16` under the kernel's `x86_64-elf-g++` freestanding toolchain (flags: `-ffreestanding -nostdlib -mcmodel=kernel -mno-sse -mno-sse2 -mno-avx -mno-red-zone -fno-exceptions -fno-rtti -std=gnu++2b`). DEC-001 inherits this: non-pow2 size classes (96 B is the only such class in DEC-003) are aligned to 16 B, and slot 0 offset arithmetic uses 16 as the lower bound. The vmsmalloc translation unit carries a `static_assert(alignof(max_align_t) == 16)` so a toolchain change is loud. | The AMD64 SysV ABI specifies `__BIGGEST_ALIGNMENT__ = 16` regardless of SSE availability; `max_align_t` follows. Verified empirically on 2026-05-21 by compiling both `== 16` (passes) and `== 8` (fails: "16 == 8") variants against the kernel toolchain. Pins down DEC-001's previously-symbolic value. Decided 2026-05-21. |
| DEC-023 | Settled | Edge-input contracts: `vmsmalloc(0)` asserts (the assertion is the *first* line of the function, before the oversize check); `vmsfree(nullptr)` asserts (the assertion is the *first* line, before `ensureTLBEntryFresh`). Neither malloc-tradition behavior (return-unique-ptr / silent no-op) is offered. | Strict contract surfaces zero-sized requests and stale-pointer frees at the point of corruption rather than at a downstream bookkeeper assert. RadixVM templating that incidentally produces zero-sized allocations must be fixed at the template, not papered over here — those sites are bugs (no useful semantics for a zero-byte VMM metadata object). Same logic for `vmsfree(nullptr)`: a kernel cleanup path that frees an uninitialized field is a bug worth catching. Cost is one extra compare-and-branch at the head of each function — negligible. Decided 2026-05-21. |
| DEC-024 | Settled | `vmsmalloc` does **not** zero-initialize returned memory in any build configuration — initialization is the caller's responsibility, established by the `VMSubstrate::make<T>` placement-new pattern. In debug builds (`#ifdef DEBUG` or equivalent), `vmsfree` fills the slot's `desc->slotSize` bytes with the poison pattern `0xCC` immediately *before* invoking `bookkeeper.freeSlot`; release builds skip the fill entirely. The poison fill writes to `p`, not to the descriptor area at offset 0, so the magic/sizeClass/numaDomain fields are never touched. | Release-build hot path stays one fewer memory write than zero-init. Debug poison gives meaningful bug-finding leverage on use-after-free (the freed bytes look obviously wrong as a pointer, integer, or vtable) without paying the cost in shipping kernels. `0xCC` chosen because it dereferences as a non-canonical user address on AMD64 (sign-extended `0xCCCC...`) and is also the `int3` opcode, so a UAF-jumped instruction trip-wires cleanly. Decided 2026-05-21. |
| DEC-025 | Settled | Alignment contract is capped at the slot alignment of the size class returned by `vmsmalloc(size)`. Define `slotAlignment(class)` as: `class` itself for power-of-two classes, `alignof(max_align_t) == 16` for non-pow2 classes (DEC-022). `VMSubstrate::make<T>` carries `static_assert(alignof(T) <= slotAlignment(sizeClassFor(sizeof(T))))` and rejects under-aligned types at compile time. Concretely under the DEC-003 schema `{8, 16, 32, 64, 96, 128, 256, 512}`: the 96 B class accepts `alignof(T) ≤ 16` only; all other classes accept up to the class size. Callers whose type needs higher alignment than its class provides must (a) pad `T` so `sizeof(T)` lands in a pow2 class with sufficient alignment, (b) split the over-aligned subobject into a separate allocation, or (c) use a different allocator. `vmsmalloc` does **not** silently promote requests across classes based on alignment. | Keeps the runtime contract and slot arithmetic simple — `vmsmalloc` is a single-argument function, class selection depends only on `size`. Pushes the over-alignment case to compile time at the `make<T>` call site, where the caller already has full type information and can react. Silent promotion was rejected because it hides the size-class jump from callers and complicates DEC-003 tuning against measured RadixVM size distributions. The 96 B class is the only "trap" under the current schema, and `static_assert` makes any future trap loud. Decided 2026-05-21. |
| DEC-026 | Settled (amended by ITEM-048: explicit slot-range lower bound; ITEM-049: accessor reframing) | `vmsfree(p)` validates the pointer in this order: (1) `p != nullptr` (DEC-023); (2) debug-only "legal calling context" per amended DEC-014 (not in IRQ/NMI/#GP/#MC); (3) `vmsBase ≤ p < vmsBase + vmsSize` — range-check against the VMSubstrate VA window, assert on miss; (4) `VMSubstrate::ensureTLBEntryFresh(p)` (DEC-016); (5) `desc = (SlabDescriptorBase*)(p & ~(pageSize - 1))`; (6) `desc->magic == kSlabDescriptorMagic` (DEC-013/044); **(6a) slot-range check: `slotZeroAddr(desc) ≤ p < slotZeroAddr(desc) + slotSize(desc) * slotCount(desc)` (ITEM-048 — explicit lower bound rejects pointers in the descriptor region that would coincidentally pass the modulo via unsigned underflow). `slotSize(desc)` and `slotCount(desc)` are LibAlloc-owned accessors (ITEM-049 — may resolve to direct prefix fields, denormalized caches, or constexpr-table lookups keyed by `desc->sizeClass`);** (6b) alignment / slot-arithmetic check `(p - slotZeroAddr(desc)) % slotSize(desc) == 0` (DEC-013); (7) debug-only poison (DEC-024); (8) `bookkeeper.freeSlot`; (9) Full→Partial push if applicable. The range bounds `vmsBase` / `vmsSize` are constants captured at vmsmalloc init (DEC-021) from the VMSubstrate's published VA window. | Putting the range check before `ensureTLBEntryFresh` is the smallest fix that keeps the freer path safe on arbitrary garbage pointers — `ensureTLBEntryFresh` is only contractually defined over VMSubstrate VAs, so calling it on a stack or kernel-image VA could fault or read garbage page-table entries. Two compares are cheaper than tightening `ensureTLBEntryFresh` system-wide (which would impose the range check on every other freshness caller too). The explicit slot-range lower bound added by ITEM-048 closes the underflow corner — without it a pointer like `(p == 8)` on a `slotSize == 8` class with `slot0Offset == 64` would pass the modulo check via two's-complement underflow (`(8 - 64) mod 8 == 0`) and the buggy free could splatter into the descriptor's own storage. Two extra compares per `vmsfree`. Decided 2026-05-21, amended 2026-05-26 (ITEM-048). |
| DEC-027 | Superseded by DEC-033 | The per-CPU "current slab" array is a compile-time `static SlabDescriptor* current[MAX_PROCESSORS][numSizeClasses]` in the vmsmalloc translation unit, zero-initialized via the standard BSS clear (no per-entry init in `vmsmallocInit`). Layout is row-major with `cpu` as the major index, so each CPU's slot row is contiguous: under DEC-003's 8-class schema, one row is exactly `8 * 8 B = 64 B` (one cache line). Fast-path load is `current[arch::getCurrentProcessorID()][classIndex]`. False sharing across CPUs is impossible because each CPU writes only its own row. | Superseded by DEC-033: `current[]` moves to the dedicated vmsmalloc-metadata arena, sized to the actual `cpuCount` rather than `MAX_PROCESSORS`. Saves up to 16 KiB of BSS on small systems at the cost of one base-pointer indirection on the fast path. Row-major layout and per-CPU-row cache-line alignment carry forward — the fast path is now `currentBase[cpu * numSizeClasses + classIndex]`. Decided 2026-05-21, superseded 2026-05-21. |
| DEC-032 | Settled | **Cache-line pad each Treiber head.** `partial[][]` entries are wrapped in `struct alignas(kCacheLineSize) TreiberHead { uint64_t head; };` (64 B on AMD64 per `arch::cacheLineSize`). Concurrent CAS on different size classes within a NUMA domain no longer ping-pong a single cache line; each head lives on its own line. Cross-domain interaction is already eliminated by DEC-019 (no cross-domain steal). | RadixVM's expected workload includes heavy multi-class allocation across all CPUs in a domain; under the previously-packed layout, all 8 heads for a domain shared one cache line and every push/pop on any class invalidated every other CPU's copy. Padding from day one is the recommended hazard mitigation in the spec's Hazards section ("do it from day one rather than chasing a contention regression later"). Storage cost (8× on the `partial[]` footprint) is absorbed by DEC-033's metadata-arena allocation, which is no longer constrained to a single page. Decided 2026-05-21. |
| DEC-033 | Settled (rewritten 2026-05-26; partially superseded 2026-05-27 by Phase 7 P7-DEC-010 — see below) | **vmsmalloc metadata storage is split across two locations.** (a) **Per-domain global buffers** for `partial[d][c]` (Treiber heads, DEC-002/015/032) and `tuning[d][c]` (magazine-sizing counters, DEC-035/043) live in a static-buffer region of VMSubstrate's VA window. The region occupies the same VA range that the topmost arena would otherwise use — VMSubstrate reserves this slot during its own `init` (before any CPU arena is created), reducing the runtime-arena count by one. Each per-domain buffer is sized via `constexpr size_t kPerDomainBufBytes` (declared in `VMSubstrateSlab.h`, computed from the layout of `TreiberHead[kNumSizeClasses]` + `MagazineTuning[kNumSizeClasses]` rounded up to page size — typically 1 page on consumer hardware). At `vmsmallocInit` time, vmsmalloc calls `VMSubstrate::reservePerDomainStaticBuffer(kPerDomainBufBytes, d)` for every `d ∈ D` (the CPU-bearing-domain subset per DEC-038); VMSubstrate allocates `ceil(byteSize / pageSize)` physical pages on domain `d`, maps them in the static-buffer region at a VA chosen by VMSubstrate, and returns the buffer VA. DomainIDs not in `D` get no buffer (their `perDomainBufs[d]` slot stays `nullptr`) — accessor calls for those domains are caller bugs that the indexing helpers can assert against. (b) **Per-CPU magazine caches** for `magazines[i][c]` live in CPU `i`'s first arena, in a structurally-reserved page placed between the occupancy buffer and the allocatable region. The arena-layout constant `kVmsmallocLocalCachePages` (= `divideAndRoundUp(kNumSizeClasses * sizeof(Magazine), arch::smallPageSize)`, declared in `VMSubstrateSlab.h`) is consumed by VMSubstrate's arena-creation code, which (i) reserves the corresponding VA slot in the arena layout, (ii) allocates the page(s) on `NUMAPolicy::domainFor(i)` via `PageAllocator::allocateSmallPage(i)`, (iii) installs the leaf PTE(s), (iv) shrinks the allocatable region by the cache page count so `freePage`/`allocPage` never touch it. vmsmalloc accesses the cache via `void* VMSubstrate::localCachePageFor(arch::ProcessorID i)`, which does pure address arithmetic — no per-call allocation. **Address arithmetic** for both halves uses VMSubstrate's existing arena-style math driven by `arch::pageTableDescriptor` (the same self-ref / level-aware primitives that already place arenas at predictable VAs); no x86-64-specific PDPT-index constants leak into vmsmalloc. **Buffer accessors** in `VMSubstrateSlab.h`: `inline TreiberHead* partialFor(DomainID d) { return reinterpret_cast<TreiberHead*>(static_cast<uint8_t*>(perDomainBufs[d]) + kPartialOffset); }` (analogous for `tuningFor`); `inline Magazine* magazineFor(arch::ProcessorID i) { return reinterpret_cast<Magazine*>(VMSubstrate::localCachePageFor(i)); }`. The `kPartialOffset` and `kTuningOffset` constants are `constexpr` in `VMSubstrateSlab.h`, derived from `sizeof(TreiberHead)` and `alignof(MagazineTuning)`. | **Supersedes DEC-020** (single-page `partial` from `PageAllocator`). **Supersedes DEC-027** (static BSS `current[]`). **Replaces the 2026-05-25 `installKernelPTE` formulation:** the prior wording put a low-level PTE-install primitive into VMSubstrate's surface, which (i) hard-coded x86-64-specific PDPT-slot conventions, (ii) forced vmsmalloc to know the page-table walking semantics, and (iii) couldn't be tested in userspace without re-implementing a page-table walker. The current split — `reservePerDomainStaticBuffer` for cross-CPU-accessed global storage, `localCachePageFor` for owner-only per-CPU storage in the existing arena — pushes the architecture-dependent details into VMSubstrate (where the arena-VA math already lives) and gives vmsmalloc two narrow, easy-to-mock entry points. **Arena layout impact:** the per-CPU cache page costs `kVmsmallocLocalCachePages` per arena (= 1 page on consumer hardware under DEC-003); the allocatable region shrinks by the same. **VA cost of the static-buffer region:** one arena-slot's worth (whatever the existing arena VA stride is — typically 1 GiB on AMD64). The physical cost remains minimal (one page per CPU-bearing domain plus one page per CPU). The init routine remains acyclic per DEC-017 (no slab calls; only `PageAllocator` from inside VMSubstrate's primitives). Decided 2026-05-21, rewritten 2026-05-25 (lightweight init + DEC-037 unified), rewritten 2026-05-26 (per-domain-buffer + per-CPU-arena-cache split, eliminating `installKernelPTE`). **Partially superseded 2026-05-27 by Phase 7 P7-DEC-010:** (i) the per-CPU "vmsmalloc local cache page" (the `kVmsmallocLocalCachePages` reservation, `localCachePageFor(i)` accessor, and `magazineFor(i)` accessor) is reframed as the unified `kernel::CpuLocal` page (`kCpuLocalPages` reservation, `cpuLocalPageFor(i)` accessor); the `Magazine[kNumSizeClasses]` array becomes a field of `kernel::CpuLocal` accessed via `kernel::cpuLocal().magazines[c]`. Per-domain-buffer storage (`partialFor` / `tuningFor`) is unaffected by P7-DEC-010 and remains as DEC-033 specifies. (ii) The arena's per-CPU page now hosts the broader kernel-wide per-CPU state (logicalID, interruptDepths, magazines, future scheduler fields) rather than vmsmalloc magazines alone. |
| DEC-031 | Settled | **Bad-pointer detection in `vmsfree` is best-effort.** When `vmsfree(p)` receives a pointer that passes the DEC-026 range check (`vmsBase ≤ p < vmsBase + 512 GiB`) but happens to land in an arena that has not yet been instantiated by `VMSubstrate::createArena`, the dirty-bitmap page that `ensureTLBEntryFresh` reads does not exist, and the call faults instead of asserting cleanly. This is accepted as a documented gap rather than tightened — `vmsmalloc` neither maintains its own active-arena list nor demands an `isInActiveArena` predicate from VMSubstrate. DEC-029's page-aligned dispatch already removes page-aligned bad pointers from this path (they go to `VMSubstrate::freePage`, which has its own validation), so the residual band is only non-page-aligned bad pointers in unpopulated arenas — a narrow corner. | RadixVM is the sole caller, and its free sites are tightly controlled — a bad pointer reaching `vmsfree` would itself be a serious caller bug. The fault inside `ensureTLBEntryFresh` is loud (not a silent corruption), just less self-explanatory than the descriptor-magic assert; an engineer hitting a page fault inside `ensureTLBEntryFresh` from `vmsfree` will recognise the diagnostic pattern. The alternative options have real costs: option (a) adds an arena-list lookup to every `vmsfree` for a rare bug case; option (b) ties vmsmalloc to VMSubstrate's arena lifecycle and demands an API addition for the same rare case. Revisit only if observed in practice. Decided 2026-05-21. |
| DEC-030 | Settled | **Caller-side preemption + migration contract.** `vmsmalloc` and `vmsfree` require the calling thread to be (a) non-preemptible and (b) pinned to its current CPU for the entire duration of the call. Today CroCOS has no scheduler, so both conditions hold trivially and no caller action is required. When a preemptive scheduler is introduced, callers must explicitly disable preemption and migration before calling (using whatever primitives the scheduler exposes — analogous to Linux's `preempt_disable` / `migrate_disable`). At that point: (1) DEC-014's "not in interrupt context" debug-build assertion at function entry is extended to `assert(!preemptionEnabled() && currentCPUPinned())`; (2) this decision is amended to call out the new primitives by name; (3) any caller in the codebase is audited for compliance. Without these guarantees, the slow path's "drop current → pop / allocPage → set current" sequence is no longer atomic on a single CPU: a preempted-then-resumed thread can overwrite a successor's `current[i][c]` (resurrecting the lost-slab hazard DEC-014 was designed to prevent), and a thread migrated mid-call reads `current[getCurrentCPU()][c]` from a different row than it later writes to (cross-CPU `current[]` corruption). | Caller-side framing matches DEC-014's "no IRQ/NMI" pattern: the compiler can't enforce the rule, but debug-build asserts catch violations and the contract is small and explicit. Rejected the "disable preemption inside vmsmalloc/vmsfree" alternative because it would require inventing the preempt-disable primitive now, before the scheduler that motivates it exists — premature mechanism for hypothetical future work. Rejected the "load-bearing assumption, revisit later" alternative because it's softer than the DEC-014 precedent and doesn't tell future kernel work *what to do* when preemption lands. RadixVM (the sole caller) likely needs preemption disabled during VMM-metadata transactions for its own correctness, so the runtime cost is near-zero. Decided 2026-05-21. |
| DEC-029 | Settled | **Large-request bypass.** For `size > largestSizeClass` (= 512 B under DEC-003), `vmsmalloc(size)` skips the slab machinery entirely: it calls `VMSubstrate::allocPage()` and returns the page base address directly. No `SlabDescriptor` is constructed; the page is not added to any partial-slab Treiber stack. The maximum supported request size remains `pageSize` (DEC-004) — `size > pageSize` still asserts. `vmsfree(p)` disambiguates by alignment after the DEC-026 range check: if `(p & (pageSize - 1)) == 0` then `p` is a whole-page allocation and `vmsfree` invokes `VMSubstrate::freePage(p)`; otherwise `p` is slab-backed and follows the existing descriptor path (`ensureTLBEntryFresh` → mask → descriptor validate → bookkeeper free). The dispatch is unambiguous because every slab-backed slot sits at offset `alignUp(sizeof(SlabDescriptor), …) > 0` within its page (DEC-001 / DEC-008), so no slab slot can ever satisfy `(p & (pageSize - 1)) == 0`. Whole-page allocations carry a `pageSize`-byte alignment guarantee — strictly stronger than DEC-001's slot-class alignment — so DEC-025's `make<T>` static_assert (`alignof(T) ≤ slotAlignment(class)`) continues to hold with `slotAlignment = pageSize` for the whole-page branch. The whole-page branch does **not** call `ensureTLBEntryFresh` on the freeing CPU, because the freer never accesses the page contents — it only hands the VA to `freePage`, which mutates the PTE in-memory. Validation of "is this actually a vmsmalloc-owned whole page" is delegated to `VMSubstrate::freePage` (which already asserts on unmapped or wrong-state pages); no separate side-table is maintained at this layer. | Resolves the DEC-003/DEC-004 contradiction by carving the 513..pageSize band into a separate-but-trivial code path that reuses VMSubstrate's existing `allocPage`/`freePage` primitives. Adds no new metadata (no SlabDescriptor for whole pages, no auxiliary owned-pages bitmap), no new partial-pool structure, and only one branch (`page-aligned?`) on the free hot path. The 87%–7% size-band wastage (513 B requesting 4 KiB on the high-overhead end, 4095 B on the low end) is real but irrelevant to current consumers — RadixVM does not request in this band, and any future consumer in this band would prefer a whole page anyway (likely cache-line and TLB benefits over a hypothetical 1-slot-per-slab class). Rejected alternatives: (a) tighten DEC-004 to `size ≤ largestSizeClass` — punts the problem to consumers and means VMM metadata objects bigger than 512 B have no allocation API at all; (b) add a page-sized class with 1 slot/slab — wastes the descriptor space for no consumer benefit and complicates the size-class iteration loop. Decided 2026-05-21. |
| DEC-028 | Settled (amended 2026-05-27 — convention-via-comment) | `template<typename T, typename... Args> SafePtr<T> VMSubstrate::make(Args&&...)` and `template<typename T> void VMSubstrate::destroy(SafePtr<T>)` are the public surface for VMM-metadata allocation. Consumers thread `SafePtr<T>` for cross-CPU access (`operator*` / `operator->` invokes `ensureTLBEntryFresh` on first touch). `vmsmalloc(size)` / `vmsfree(ptr)` exist as the lower-level primitives that `make<T>` / `destroy<T>` call. **Because `make<T>` / `destroy<T>` are templates whose bodies must live in `VMSubstrate.h`, `vmsmalloc` / `vmsfree` are unavoidably declared in the same public header** (`kernel/include/mem/VMSubstrate.h:21–22`). **Convention-only "internal-ness"**: their declarations carry an in-source comment indicating that external callers should prefer `make<T>` / `destroy<T>`. No structural enforcement is possible given the template-visibility constraint. `make<T>` computes `sizeClassFor(sizeof(T))`, static_asserts `alignof(T) ≤ slotAlignment(class)` (DEC-025), calls `vmsmalloc(sizeof(T))`, placement-news a `T` into the slot, and returns `SafePtr<T>(raw)`. `destroy<T>` calls `T::~T()` through the SafePtr, then `vmsfree(raw)`. | The original 2026-05-21 framing proposed moving `vmsmalloc` / `vmsfree` to an internal header (`kernel/mm/VMSubstrateSlab.h`); the amendment recognises that this would require moving `make<T>` / `destroy<T>` out of the public header too (since templates need full visibility at instantiation sites), which transitively pulls `VMSubstrateSlab.h` (and `SlabDescriptorBase`, the size-class constants, etc.) into the public surface — defeating the original "narrow public API" goal entirely. Convention-via-comment lands at the same place with no structural cost. Reader-side TLB freshness is still type-enforced for `make<T>` consumers via `SafePtr`; callers who deliberately bypass `make<T>` to use `vmsmalloc` directly take on the freshness obligation themselves, and the comment makes this explicit. Decided 2026-05-21, amended 2026-05-27 (convention-via-comment per user direction). |
| DEC-034 | Settled (amended by DEC-041: head-linkage, no `m.tail`) | **Chained-transfer magazines.** Per-NUMA-domain partial-slab pools store chains of slabs rather than singletons; one CAS on the shared Treiber transfers an entire chain. `SlabDescriptor` gains two fields: `Atomic<SlabDescriptor*> chainNext` (linkage between elements *within* a chain) and `uint32_t chainDepth` (number of slabs in the chain, valid only on a chain head). `desc->next` is the Treiber linkage on the shared stack and is **defined on the chain head only** (per DEC-041), pointing to the next chain head; DEC-015's tagged-head encoding is unchanged. Per `(CPU, class)` a `Magazine { SlabDescriptor* head; uint32_t depth; }` (padded to one cache line) lives in the vmsmalloc metadata arena (DEC-033 extended). The magazine is one chain that the allocator drains from the head and that freers extend at the head. Flush trigger: when `m.depth` reaches `currentK` (DEC-035), one Treiber CAS transfers the chain — `m.head->next = oldHead; CAS(partial[d][c], oldHead, m.head)`. Refill: when the magazine is empty, one Treiber CAS pops a chain head from the shared stack (new shared head = `chainHead->next`); the popper subsequently walks `chainNext` for the next `chainDepth - 1` allocations without further atomics. Cross-domain gate: a freer whose `domainOf(currentCPU) != desc->numaDomain` skips its own magazine entirely and pushes `desc` as a singleton chain (`desc->chainDepth = 1; desc->chainNext = nullptr; desc->next = oldHead`) directly onto `partial[desc->numaDomain][desc->sizeClass]`. | Reduces shared-Treiber CAS rate from `≈ 2 × ops` to `≈ 2 × ops / K`. Chain elements were never independently on the shared stack — they're reachable only through the chain head, which is what the CAS removes; the K−1 sub-elements move atomically with the head without participating in the shared-stack contract. Per-CPU magazine writes (chain construction) are uncontended by DEC-014 (no IRQ/NMI) + DEC-030 (no preemption/migration mid-call). The "fishbone" alternative (substack inside each main-stack slab, no per-CPU buffer) was rejected because once a per-CPU buffer is added, fishbone's substack field is either unused or — worse — racy (the push target can be popped between read and CAS). Cross-domain gate strengthens DEC-019: magazines remain a per-CPU view of the freer's own domain. Decided 2026-05-25, amended 2026-05-26 (DEC-041). |
| DEC-035 | Settled (extended by DEC-043: policy single-runner) | **Runtime-tunable magazine depth K.** Per `(domain, class)` a `MagazineTuning { Atomic<uint32_t> currentK; Atomic<uint32_t> overflowCount; Atomic<uint32_t> starvationCount; Atomic<uint32_t> policyLock; }` (16 B, padded to one cache line) sits alongside `partial[d][c]` in the metadata arena. Freer reads `currentK` on each flush decision (RELAXED load). Outbox flush that loses the CAS retry-loop bumps `overflowCount`; allocator slow path that finds the shared stack empty bumps `starvationCount`. A policy routine (gated by event count; piggybacks on the slow path; single-runner per `(domain, class)` per DEC-043) compares the two counters and adjusts `currentK`: high overflow → grow K; high starvation → shrink K. Bounds `[Kmin, Kmax]` are implementation-time constants. The spec carries the structure and the role of each counter and the lock; it does **not** pin the initial K, the bounds, or the precise policy formula. | Bonwick-Adams (UMA) dynamic magazine sizing: lets the system find the magazine depth that minimizes shared-stack contention without requiring an a-priori workload estimate. Acceptable cost (one atomic load on every flush decision; counter writes amortized into existing slow paths). Architectural commitment is small — making K runtime-mutable now is much cheaper than refactoring a compile-time constant out later if measurement says we need it. Decided 2026-05-25. |
| DEC-036 | Settled | **Allocator-path bounded eager-free of Empty inbox slabs.** During inbox drain on the allocator slow path (DEC-034), if `magazines[i][c].head` references an Empty slab *and* `magazines[i][c].depth > 1`, the allocator calls `VMSubstrate::reclaimSlabPage(m.head)` (DEC-047; not `freePage`), advances `m.head` to its `chainNext`, decrements `m.depth`, and re-evaluates the new head. The eager-free walk repeats until the head is non-Empty or `m.depth == 1`. A chain of length 1 is **never** eager-freed — the floor preserves a fresh slab as the next allocation target and avoids thrashing back to `allocPage`. The eager-free applies only to the inbox side (post-refill); the outbox flush is unaffected (it pushes the magazine's whole chain to the shared stack regardless of state). | Drains the middle of long chains aggressively without breaking the "always have something to allocate from" property. Safe by construction: an Empty slab has zero allocated slots, so no in-flight freer can hold a pointer to it (no future `vmsfree` can fire); the chain element is in this CPU's exclusive magazine (DEC-034/037 invariant) so no other CPU holds it; the `becameAvailable()` republish trigger fires only on Full→Partial and cannot target an Empty slab. Cost per slow-path event is bounded by `Kmax - 1` `freePage` calls (DEC-035's chain-length bound). Steady-state floor of one cached slab per `(CPU, class)` is preserved. Tail-Empty pinning (when the entire magazine decays to one Empty slab on a quiet system) still requires ITEM-009's eventual `scavenge()` to reclaim, but the common case of partially-Empty chains in active systems is reclaimed at the next allocator slow path with no coordination overhead. Decided 2026-05-25. |
| DEC-039 | Settled (amended by DEC-041: no `m.tail`; justification extended by ITEM-045) | **Pre-read `chainNext` before `allocSlot` (closes the same-domain *and* cross-domain becameFull races, ITEM-041 + ITEM-045).** The DEC-037 fast-path pseudocode is amended: before calling `bookkeeper.allocSlot(m.head, transition)`, the allocator loads `nextLocal = m.head->chainNext` into a local. On `transition.becameFull()`, the synchronous pop uses the local (`m.head = nextLocal; m.depth--`) — **never** re-reading `m.head->chainNext` after `allocSlot`. The fast path becomes: `head = m.head; nextLocal = head->chainNext; transition = allocSlot(head); if (becameFull) { m.head = nextLocal; m.depth--; }; return slot`. | Closes the chain-corruption races ITEM-041 and ITEM-045 surfaced. The proof: `desc->chainNext` of a slab in CPU A's magazine is mutable only by (a) CPU A's own operations (single-threaded by DEC-014/030), (b) a freer's same-domain push (writes `desc->chainNext = m_freer.head`), or (c) a freer's cross-domain singleton push (DEC-019/034 — writes `desc->chainNext = nullptr` as part of its publish sequence). Both (b) and (c) require `transition.becameAvailable()` (Full→Partial), which requires the slab to first become Full. The slab becomes Full exclusively via CPU A's `allocSlot`. Therefore CPU A's pre-read of `chainNext` is unconditionally ordered-before any freer's mutation of that field — same-domain or cross-domain. The happens-before chain `A.pre_read [seq-cb] A.allocSlot release [sync-with] freer.freeSlot acquire [seq-cb] freer.chainNext_write` is identical for both pusher categories; the bookkeeper's acq-rel atomics (DEC-042 #4) supply the sync-with edge. The cost is one extra load per fast-path allocation (the value is already on the cache line being touched for `allocSlot`, so the load is essentially free). Alternative considered: use a CAS for the pop. Rejected — adds an atomic to the hottest path of vmsmalloc for a race that pre-reading eliminates structurally. Decided 2026-05-26, justification extended 2026-05-26 (ITEM-045). |
| DEC-040 | Settled (amended 2026-05-27: lazy first-touch freshness via Phase-4 hooks) | **Allocator-side `ensureTLBEntryFresh` at each chain-element first-touch (closes the TLB-staleness gap, ITEM-042).** Freshness fires once per slab the popper consumes, at the moment the popper first touches that slab. Two integration points: **(a) inside the Phase-4 `pop()` call**, via the `Hooks::onPreTouch(topPtr)` callback (P4-DEC-010) — fires after the acquire-load on `head` but before any read of `topPtr->next` or `topPtr->chainDepth`. This is **load-bearing for shared-stack correctness**, not just for the popper's own consumption: `topPtr->next` is published as the new shared-stack head via the pop CAS, so a TLB-stale read would corrupt the stack visibly to every CPU. The acquire-load synchronizes *value visibility* through the current PTE but does NOT refresh this CPU's cached VA→PA mapping; the explicit `ensureTLBEntryFresh` is required. **(b) inside Phase 5's allocator path, at every `m.head` transition** — call `ensureTLBEntryFresh(newHead)` immediately before assigning `m.head = newHead` whenever the magazine head advances to a previously-untouched chain element. The transitions: (i) the becameFull synchronous pop (DEC-037) advancing along `chainNext`; (ii) the eager-free walk (DEC-036) advancing along `chainNext` after `freePage`. The freshly-popped chain head is **already fresh** from the pop hook in (a), so the refill path itself adds no extra call. The fresh-allocation slow path (`allocPage` + descriptor init) does **not** need `ensureTLBEntryFresh` either: `allocPage` (per DEC-046) invalidates the calling CPU's own TLB entry and clears the dirty bit before returning. **No eager sweep.** The previous draft's "walk the entire chain at refill time calling `ensureTLBEntryFresh` on each element" is dropped — instead the calls are spread across the chain's lifetime, paid one-per-element at first touch. Same total cost; simpler reasoning; no upfront burst on the slow path. | The lazy model is functionally equivalent to the eager sweep — both ultimately do one `ensureTLBEntryFresh` per chain element consumed — but the lazy model has three concrete advantages: (i) the load-bearing pre-touch call moves inside Phase 4 via the Hook pattern (P4-DEC-010), where it sits at the *only* code site that reads pre-CAS chain-head fields; (ii) Phase 5's pre-touch on `m.head` transitions is a single call per slab consumed, paid at the natural transition point rather than batched at refill; (iii) for chains where the consumer never reaches the chain bottom (e.g., the consumer's CPU goes idle mid-chain), no work is wasted on elements that wouldn't have been touched. The dirty-bit mechanism in VMSubstrate (`setDirtyForOtherCPUs(va)` on every `allocPage`/`freePage`, per DEC-016 clarification) is the exact mechanism `ensureTLBEntryFresh` consumes — the gap was that the allocator never invoked it. Rejected alternative: require VMSubstrate to globally shoot down TLBs on every `allocPage`/`freePage`. That would push IPI cost onto every page-level operation in the kernel. Decided 2026-05-26, amended 2026-05-27 (lazy first-touch + Phase-4 hook integration, P4-DEC-010). |
| DEC-038 | Settled (clarified by ITEM-039) | **Raw DomainID indexing for `partial[d][c]` and `tuning[d][c]`, restricted to CPU-bearing domains.** Both arrays' index space is `[0, maxDomainID + 1)` — raw DomainID, no packing — so `partial[d][c] = &partialBase[d * numClasses + c]` is a flat offset computation on the freer's hot path with zero indirection. **Init only allocates physical metadata pages for the subset of DomainIDs that have at least one CPU** (`{ d : ∃ logical CPU i with NUMAPolicy::domainFor(i) == d }`); pages for CPU-less DomainIDs (HBM-only, persistent-memory-only, etc.) and for unused DomainID values within `[0, maxDomainID]` are simply not mapped. The metadata-region VA range for those rows stays unallocated, so any erroneous index into a CPU-less or unused row page-faults instead of silently writing to a wrong location — a useful safety net against bugs in domain selection. The "rows always exist" simplicity of the unrestricted version is unnecessary because both indexing sites (allocator: `partial[numaOf(currentCPU)][c]`; freer: `partial[desc->numaDomain][c]` where `desc->numaDomain` was set to the creator's home domain per DEC-018) only ever produce DomainIDs that have CPUs. Memory-less but CPU-bearing domains still get metadata pages (allocated via `PageAllocator::allocateSmallPage(cpuInDomain(d))` — the page allocator falls back to a remote pool, which is fine since the metadata page is pinned and rarely touched). Physical cost: `(numCPUBearingDomains × numClasses × 64 B) / pageSize` rounded up — typically 1 page on consumer hardware, 2–4 pages on multi-socket servers. | Matches `PageAllocator`'s `perDomainAllocs[maxDomainID + 1]` indexing convention while eliminating physical cost for domains vmsmalloc structurally cannot index. The page-fault-on-misindex behavior is a free side-benefit: a domain-selection bug surfaces loudly instead of writing into a zero-initialized but semantically wrong row. The packed-index alternative would have added a DomainID → packedIndex map lookup on every freer-side `desc->numaDomain` access — the hottest cross-CPU path in vmsmalloc, and the memory savings (a few KiB on sparse-ID systems) don't justify the per-call indirection. The compile-time `MAX_NUMA_DOMAINS` alternative was rejected for the same reasons DEC-033 rejected static BSS sizing — it conflicts with the "size from runtime NUMA discovery" philosophy. Decided 2026-05-25, clarified 2026-05-25 (ITEM-039). |
| DEC-037 | Settled (amended by DEC-041: `m.tail` removed) | **Unified magazine — collapse `current[i][c]` into `magazines[i][c].head`.** The separate per-(CPU, class) `current[]` array is eliminated. The active allocation target on CPU `i` for class `c` is `magazines[i][c].head` whenever `m.depth > 0`. The fast path becomes: if `m.depth > 0`, call `bookkeeper.allocSlot(m.head, transition)`. On success: if `transition.becameFull()`, **pop synchronously** (`m.head = m.head->chainNext; m.depth--`); return slot. Magazine state is always-valid both for freer-side pushes (Full→Partial chain extension) and for allocator-side draining. The cross-domain free gate (DEC-019/DEC-034) is unchanged — cross-domain frees still push a singleton chain directly to the home Treiber, skipping the local magazine. (DEC-041 removed the `m.tail` field; the original DEC-037 maintained it via walk-on-refill but no consumer needed it once flush was rewritten to use `m.head->next` per DEC-041.) | **Resolves ITEM-036 (dual-ownership race) structurally:** with one representation of "active slab" per `(CPU, class)`, there is no window where a slab is owned by A while simultaneously being on the shared stack — the pop-on-`becameFull` removes the slab from A's magazine before any freer's `becameAvailable` republish can race. **Saves `cpuCount × numClasses × 8 B` of metadata** by eliminating the `current[]` array (DEC-033's metadata region shrinks by one base pointer's worth of pages). **Conceptual cost:** the magazine no longer has separable "inbox" vs "outbox" temporal modes — it's one chain that the allocator drains from the head and that freers extend at the head. The flush trigger (`m.depth >= currentK`) is unchanged; flush pushes the entire current chain to the shared stack and resets the magazine. Decided 2026-05-25, amended 2026-05-26 (DEC-041). |
| DEC-043 | Settled | **Tuning-policy single-runner per `(domain, class)` via try-lock.** Extend `MagazineTuning` with an `Atomic<uint32_t> policyLock` field (zero = unlocked, one = held). A CPU that hits the event-count trigger on the slow path attempts `policyLock.exchange(1, memory_order_acquire)`; if the prior value was zero it has acquired the lock and runs the policy (read-and-reset both counters via `exchange(0)`, decide the new `currentK`, store it, then `policyLock.store(0, memory_order_release)`). If the prior value was one, another CPU is already running the policy on the same `(domain, class)`; this CPU **skips silently** — it does not spin, retry, or queue. The event-count gating ensures policy invocations are rare relative to alloc/free traffic, so a skipped invocation is just deferred until the next CPU hits the trigger. `currentK` reads on the freer hot path remain `RELAXED` (DEC-042) and are unaffected by the lock — the lock only serializes the read-reset-decide-write cycle. `MagazineTuning` becomes `{ Atomic<uint32_t> currentK; Atomic<uint32_t> overflowCount; Atomic<uint32_t> starvationCount; Atomic<uint32_t> policyLock; }` (16 B, padded to one cache line per DEC-035). | The race the lock closes: two CPUs both do `exchange(0)` on `overflowCount`; the second sees zero and infers "no overflow," shrinking K incorrectly. Without the lock the policy's adjustment becomes a function of arbitrary interleaving, defeating the closed-loop behavior DEC-035 was designed for. Rejected alternatives: (a) per-CPU local counters with periodic aggregation — adds a per-CPU `MagazineTuning` row per `(CPU, class)` and a fan-in step, more storage and more code for a feature that's already heuristic; (b) lock-free CAS-based policy that's idempotent under concurrent execution — possible but fragile, and the rare-invocation property of event-count gating makes the lock contention negligible; (c) designated runner (only the lowest-numbered CPU in the domain runs the policy) — bad on systems where that CPU is idle. The try-lock-and-skip pattern is the minimum coordination needed. Decided 2026-05-26. |
| DEC-045 | Settled (amended 2026-05-26 by ITEM-049: precise field layout deferred to LibAlloc) | **Per-size-class derived constants are compile-time tables; SlabDescriptor uses a uniform fixed prefix + class-specific bookkeeper suffix.** Slab.h's `SlabBookkeeper` is template-parameterized by `SlotCount`, so each size class instantiates a distinct bookkeeper type. The descriptor is therefore split into: **(a) a uniform fixed prefix `SlabDescriptorBase`** — pinned by the **abstract read interface vmsmalloc consumes**: `magic` (DEC-044), `sizeClass`, `numaDomain`, the Treiber linkage `next`, the magazine-chain linkage `chainNext` (Atomic<SlabDescriptorBase*>) + `chainDepth`, plus accessor semantics `slotSize(desc)` and `slotCount(desc)` (the latter used by DEC-026 step 6a's slot-range check). The **precise field layout** (whether each accessor is a direct prefix field, a denormalized cache, or a constexpr-table lookup keyed by `sizeClass`; the exact field ordering, padding, and prefix size) is **deferred to the LibAlloc implementation**. All inter-slab linkage (Treiber heads, magazine `head` field, `chainNext` walks) uses `SlabDescriptorBase*`, so chains and stacks are not class-templated. **(b) a class-specific suffix** — `SlabBookkeeper<slotCount[c]>` placed immediately after the prefix. Full descriptor type per class is `SlabDescriptor<N>` = prefix + `SlabBookkeeper<N>`. The slab-creation slow path knows `c` and `N = slotCount[c]` at compile time and constructs the correct `SlabDescriptor<N>` in place. `vmsfree` casts the masked pointer to `SlabDescriptorBase*`, reads `desc->magic` / `desc->sizeClass` / etc. via the uniform prefix, and dispatches bookkeeper operations through a `sizeClass`-keyed jump table (one entry per DEC-003 class, ≈ 8 cases — typically inlined as a switch). **Derived constants** (constexpr arrays parallel to `slabSizeClasses`): `slotCount[c]` = the largest `N` such that the prefix + `SlabBookkeeper<N>` plus `N * slotSize(c)` (with appropriate alignment slack to `max(slotSize(c), alignof(max_align_t))`) fits in `pageSize`. Because `sizeof(SlabBookkeeper<N>)` is constant for any `N` in `[(K-1)*64+1, K*64]` (the kWordCount equivalence class), the fixpoint converges in one step per class. `slot0Offset[c]` is computed symmetrically. The DEC-026 step (6a) lower-bound check (ITEM-048) uses these tables; the slab-creation slow path uses both to size the new descriptor and seed the bookkeeper's tail-bit mask (DEC-011). | Reconciles with Slab.h's existing template parameterization rather than forcing a runtime-parameter rewrite. Uniform prefix lets `vmsfree` cast generically; class-specific suffix avoids paying the max-sized bookkeeper footprint (`SlabBookkeeper<512>` is ~136 B; a max-sized descriptor would waste ~120 B on class-512 slabs that need only ~24 B of bookkeeper). The `sizeClass`-keyed dispatch in `vmsfree` is a single switch with 8 cases. The 2026-05-26 ITEM-049 amendment trades the field-layout specificity (which had drifted into an inconsistency about whether `slotSize` was a direct field or a derivation) for a cleaner interface contract — LibAlloc owns the descriptor struct, vmsmalloc owns the accessor consumers. Decided 2026-05-26, amended 2026-05-26 (ITEM-049). |
| DEC-044 | Settled | **Descriptor magic constant pinned at `0x5DAB5DABDE5CC9C0ULL`.** `SlabDescriptor::magic` is a `uint64_t` set to this constant by every slab-creation path and validated by `vmsfree` after the DEC-026 range check, DEC-016 freshness call, and DEC-029 page-aligned dispatch. The constant is declared `constexpr uint64_t kSlabDescriptorMagic` in the vmsmalloc implementation translation unit and is the only legitimate source — code sites that need to validate must reference the constant, not a literal. Choice rationale: (a) bits 63:48 = `0x5DAB` make the value non-canonical when interpreted as an AMD64 virtual address (canonical form requires bits 63:48 to be all-zero or all-one), so a buggy dereference of the magic as a pointer faults loudly; (b) ASCII-readable as `slab slab desc CroCOS` in 8 nibble-pairs (`5D AB 5D AB DE 5C C9 C0` ≈ "5DAB 5DAB DE5C C9C0"), aiding hex-dump recognition; (c) cannot be confused with common kernel constants (`0xDEADBEEF`, `0xCAFEBABE`, page-table flags, sentinel pointers); (d) cannot collide with the DEC-024 free-poison pattern `0xCCCCCCCC...` because the leading nibble differs. The validation path is `desc->magic == kSlabDescriptorMagic`; debug builds additionally verify the magic word is unaffected by the slot-offset arithmetic (slot 0 always sits at `alignUp(sizeof(SlabDescriptor), slotAlignment) > 0`, so a slot can never overlap the magic). | The earlier spec said only "pick a 64-bit constant unlikely to appear as the leading word of any legitimate VMSubstrate page" — implementation needs an actual value, and pinning it in the spec prevents drift if multiple developers implement parts independently. The constant is small enough to inline at every validation site without needing a relocation; on AMD64 it materializes as one `movabs` per check. Future changes (e.g., if the descriptor layout is reshaped) should bump the magic to a new value via a fresh DEC, so live slab descriptors from prior kernel builds (impossible across reboots but possible with kexec-style flows) trip the magic check rather than passing silently. Decided 2026-05-26. |
| DEC-042 | Settled | **Memory-ordering policy for Treiber CASes, descriptor publication, and bookkeeper dependency (resolves ITEM-044). Portable across x86, ARMv8, and RISC-V — no x86-TSO assumptions.** Pin the synchronization rules that the rest of the spec implicitly assumes: (1) **Treiber push** (flush in DEC-034/041, cross-domain singleton push in DEC-019/034, and any freer-side publish of a fresh slab) uses `head.load(memory_order_relaxed)` at the top of each retry iteration — push-side pre-CAS reads only touch the current CPU's own data (writing `desc->next = oldHead.descPtr` and computing the new tag), so the initial load needs no cross-CPU sync. The publishing CAS is `compare_exchange_strong` with `success = memory_order_release`, `failure = memory_order_relaxed`. The release publishes all prior non-atomic descriptor writes (`magic`, `sizeClass`, `numaDomain`, `chainDepth`, `next`) and all prior in-chain `chainNext` atomic stores. (2) **Treiber pop** uses `head.load(memory_order_acquire)` at the top of each retry iteration — the pop reads `chainHead->next` (and via DEC-040 also `chainDepth`, `chainNext`, and bookkeeper state) **before** the CAS, so the initial load must establish synchronizes-with against the prior owner's release-push, or those pre-CAS reads will see stale values on ARMv8/RISC-V (where loads are not LoadLoad-ordered by default). The CAS itself is `compare_exchange_strong` with `success = memory_order_acquire` (redundant under the acquire-on-initial-load pattern but harmless and consistent), `failure = memory_order_relaxed` (next iteration re-loads `head` with acquire). **Do not collapse to RELAXED on the pop-side initial load even though x86 TSO would tolerate it** — silent breakage on ARMv8/RISC-V is the exact failure mode. (3) **`Atomic<SlabDescriptor*> chainNext`** stores and loads use `memory_order_relaxed`. Synchronization comes from (a) the surrounding Treiber CAS for cross-CPU publication of chain elements (release on push, acquire on the next popper's initial head load), and (b) the bookkeeper's acq-rel atomics for the DEC-039 pre-read race — the happens-before chain `owner.pre_read [seq-cb] owner.allocSlot [sync-with] freer.freeSlot [seq-cb] freer.chainNext_store` orders the pre-read before any freer write, so RELAXED on `chainNext` is sufficient. **The `Atomic<>` wrapper itself (not the ordering) is load-bearing for C++ data-race-freedom.** Considered 2026-05-27: could `chainNext` be a plain non-atomic `SlabDescriptor*` so post-pop walks compile to raw pointer loads? **No** — there is a race window during `ChainedTreiberStack::push(element)` between a pushing CPU X and a popping CPU Z (and a subsequent same-domain freer W). Concretely: X acq-loads head → T; X reads `T->chainNext` to compute element linkage; Z pops T (CAS succeeds); X still has the iteration's reads outstanding when Z fast-paths on T, T becomes Full, W's same-domain push writes `T->chainNext`. X's read at `t2` and W's write at `t7` access the same field with no happens-before edge under C++ rules (only an after-the-fact CAS-failure on X) — data race UB without `Atomic<>`. Machine-level executions are correct (X's CAS fails anyway, so its stale read is inconsequential), but the language model requires the field to be atomic. RELAXED ordering is fine because the synchronization comes from the surrounding CAS / bookkeeper acq-rel; the `Atomic<>` wrapper is solely to make the access race-free under C++. On AMD64 and ARMv8, `Atomic<T*>` with RELAXED compiles to the same instruction as a non-atomic pointer load (`mov rax, [...]` / `ldr x0, [...]`), so the runtime cost is zero — only a small optimization-barrier cost remains. Conclusion: keep `Atomic<SlabDescriptor*> chainNext` RELAXED. Do not expose a "non-atomic post-pop accessor"; there's no per-element a-priori quiescence boundary that consumers can rely on (the same hazard re-emerges every time a chain element cycles through { popped → in-magazine → Full → republished }). (4) **Bookkeeper dependency:** `LibAlloc::SlabBookkeeper`'s `allocSlot` and `freeSlot` atomic operations must be at least `acquire`-on-read / `release`-on-write (i.e., effectively acq-rel on the CAS that drives the state transition). This is a hard requirement that DEC-039's race-freedom argument depends on; the dependency table calls this out explicitly, and `LibAlloc::Slab.h` must document the same. (5) **Tuning counters** (`currentK`, `overflowCount`, `starvationCount`) remain `memory_order_relaxed` — they are heuristic and tolerate reordering. (6) **Consumer-side slot publication** (caller obligation, already in Hazards) remains release-ordered — a consumer storing a slot pointer into a shared atomic must use at least release ordering so that downstream readers see the descriptor fields written before vmsmalloc returned. (7) **Implementation discipline:** name the orderings in compile-time constants (`kTreiberPushOrder`, `kTreiberPopLoadOrder`, `kTreiberPopCASOrder`) referenced from every CAS site, with a comment citing DEC-042 — so any downgrade is visible at one point of edit. | The policy makes every cross-CPU happens-before edge explicit and portable. Acquire on the pop-side initial `head.load()` is the standard Treiber-stack pattern (load-acquire-then-read-next), and the minimum needed on weak architectures where LoadLoad is not implicit. The earlier draft of DEC-042 specified relaxed on the pop's initial load — that worked on x86 only because TSO makes all loads acquire-like, but CroCOS aims for ARMv8 and RISC-V targets as well, where the bug would manifest as the popper reading stale `chainHead->next` (or any other descriptor field) values from a prior incarnation of the same slab descriptor. RELAXED on `chainNext` is sound for the reasons in (3); lifting it to acq-rel would be redundant cost on every freer push without strengthening correctness. The bookkeeper dependency is the most consequential addition — if LibAlloc's atomics are RELAXED (currently unspecified in `Slab.h`), DEC-039 silently breaks; documenting the requirement in both this spec and the LibAlloc header is mandatory before implementation. Verified via the userspace test harness (which targets ARMv8 dev hardware explicitly — see §Testing Approach) running under ThreadSanitizer; x86-only validation is insufficient. Decided 2026-05-26, tightened 2026-05-26 (portability). |
| DEC-046 | Settled | **Calling-CPU TLB invariant on `VMSubstrate::allocPage` (resolves ITEM-050).** Before returning a VA, `allocPage` invalidates the calling CPU's own TLB entry for that VA (via `invlpg`) and clears the calling CPU's own dirty-bitmap bit for the same VA, in addition to the `setDirtyForOtherCPUs(va)` call that propagates the dirty signal to remote CPUs. The combination guarantees that the calling CPU's next access to the returned VA observes the fresh PA installed by `allocPage`, with no explicit `ensureTLBEntryFresh` call required from the vmsmalloc slow path. Already implemented in the current `VMSubstrate.cpp`'s allocPage path; pinning the contract here so future changes to that path preserve the property. Symmetric concern doesn't arise for `freePage` since no vmsmalloc code path reads the freed VA after issuing `freePage` — DEC-029's whole-page free hands the VA off and does not touch it again, and DEC-036's eager-free reads `m.head->chainNext` into a local *before* invoking `freePage`. | The Concurrency Model's fresh-allocation branch (no freshness call on a freshly `allocPage`'d page on the alloc-side, "Fresh allocation (slow path)" pseudocode) is the load-bearing consumer of this contract. The alternative — adding an `ensureTLBEntryFresh` call on every fresh slab creation — would impose redundant dirty-bit checks (and a possibly-redundant `invlpg`) on every slow-path slab creation, doubling the work allocPage already does. Decided 2026-05-26. |
| DEC-047 | Settled | **Read-only sentinel remap for slab reclaim (`reclaimSlabPage`), fixing the cross-popper Treiber-pop #PF.** The DEC-036 eager-free walk reclaims an Empty slab that may *still* be the encoded head a concurrent `ChainedTreiberStack::pop` on another CPU is about to dereference: that pop has already acquire-loaded `head`, and between its `onPreTouch` and its read of `topPtr->next` the reclaiming CPU can free `topPtr`. `freePage` clears the leaf PTE, so the speculative read faults. **DEC-040's freshness call does not prevent this** — `ensureTLBEntryFresh` refreshes the TLB to the *current* PTE, which for a reclaimed page is exactly the unmapped (faulting) state; it only ever defended against reading *stale content* from a prior physical backing. (DEC-046's note that "the symmetric concern doesn't arise for `freePage`" reasoned only about the *freeing* CPU's own accesses; it missed the concurrent *popper* on another CPU.) Fix: a new VMSubstrate primitive `reclaimSlabPage` that, instead of clearing the leaf PTE, remaps the VA **read-only** onto a single shared kernel-image sentinel page (`sentinelPage`, phys resolved once in `init()` via `early_boot_virt_to_phys`). The real phys page is still returned to the allocator and the VA is still released to the radix tree (sharing `releaseLeafMapping` with `freePage`); a subsequent `allocPage` overwrites the sentinel PTE present→present, never leaving a non-present window. **Correctness:** the speculative `topPtr->next` read is consumed only by the pop's CAS, which succeeds only if `head` is unchanged — i.e. `topPtr` was never reclaimed and the read was real; if `topPtr` *was* reclaimed, some pop advanced `head`, so the CAS fails and the garbage value is discarded. ABA-soundness of "`head` unchanged" rests on DEC-015's tag (37-bit counter for the 512 GiB / `kWindowAddressBits == 39` window — already the maximum width, since the page-offset field must cover the whole window; the right-shift-by-`kPageShift` packing is what frees those bits, so there is no further widening to do). The sentinel contents are never consumed, so one zero-filled page shared by every reclaimed VA suffices; reads are side-effect-free (kernel RAM, never MMIO) and writes still fault (PTE read-only), catching erroneous writes to freed slabs. `onPreTouch`/`ensureTLBEntryFresh` is **retained** (still correct for the recycled-VA stale-content case) but is no longer the fault-safety mechanism for this race. The whole-page (DEC-029) free path keeps using `freePage` — those pages are never Treiber-stack nodes. | Turning a fault into a harmless discarded read is far cheaper than the alternatives: (a) quiescence/epoch reclamation to prove no popper holds a reclaimed slab adds per-pop bookkeeping to the hottest path; (b) never reclaiming (pure DEC-002 lazy) pins a page per `(CPU,class)` indefinitely; (c) a global TLB shootdown on every reclaim pushes IPI cost onto the slow path. The sentinel costs one BSS page and one extra PTE write per reclaim. Validation is deferred to the in-kernel stress test (`kernel/VmsmallocStress.cpp`), which drives concurrent pops against the eager-free walk on all CPUs; the Phase 8 userspace mock recycles like `freePage` and does not simulate the remap (no page tables in userspace). Decided 2026-05-31. |
| DEC-041 | Settled | **Treiber linkage lives on the chain head; drop `m.tail` from the Magazine struct.** Two interlocking corrections that together resolve ITEM-043: (1) **Chain-head, not chain-bottom, carries the shared-stack `next` link.** The encoding the spec was implicitly assuming all along: each slab descriptor has two orthogonal pointers — `next` (Treiber linkage, defined on the chain *head* only, points to the next chain head on the shared stack) and `chainNext` (intra-chain linkage, defined on every chain element, walks head → bottom; bottom's `chainNext == nullptr`). Flush writes `m.head->next = oldHead.descPtr` (not `m.tail->next`), so the pop CAS is genuinely single-CAS: load `oldHead = (chainHead, tag)`; the new shared head is `chainHead->next`; one CAS publishes the swap. No chain walk is needed for the pop itself. (2) **`m.tail` is removed from the `Magazine` struct.** Under (1), nothing in the design consumes `m.tail`: flush no longer reads it (flush uses `m.head->next`), freer-side push doesn't need it (push extends at head), allocator pop doesn't need it (pop advances head along `chainNext`), and eager-free walks from head without referencing the bottom. The post-pop chain walk (DEC-040) becomes a pure TLB-freshness sweep with no bookkeeping output. The `Magazine` struct shrinks to `{ SlabDescriptor* head; uint32_t depth; }` (12 B → still padded to a cache line). | **Resolves ITEM-043 structurally.** The bottom-linkage interpretation the spec drifted into produced a contradiction — single-CAS pop is unachievable under bottom-linkage because the new shared head only lives in `bottom->next`, requiring a walk before the CAS. Head-linkage is what the rest of the spec (DEC-040's "after a successful pop", Concurrency Model's "one Treiber CAS pops") was always assuming. Once head-linkage is restored, `m.tail` has no consumers, so removing it is a free simplification. Saves `cpuCount × numClasses × 8 B` from the metadata region, eliminates the walk-on-refill bookkeeping step (refill walk becomes a freshness sweep only), and removes the `if (m.tail == nullptr) m.tail = desc` conditional from the freer push. Decided 2026-05-26. |

## Hazards

<!-- Known tricky spots likely to produce bugs or subtle misbehavior. These focus adversarial review
     and inform where to concentrate testing effort. -->

- Slab transition races: an `Empty`-becoming-`Available` republish from a freer CPU can race with
  the owning allocator CPU's `flushAllocSide` — pattern already exists in `SmallPageAllocator`,
  must be carried over correctly.
- NUMA-domain slab migration: ruled out by DEC-019 — `vmsmalloc` never steals across domains.
  The only remaining cross-domain interaction is the (rare) case where `allocPage` itself falls
  back to a remote node under local exhaustion, in which case `desc->numaDomain` records the
  caller's domain (DEC-018), not the physical placement. No ping-pong is possible.
- Lazy reclamation under quiescent workloads (DEC-002, partially mitigated by DEC-036): in active
  workloads, DEC-036's allocator-path bounded eager-free reclaims Empty slabs from inbox chains
  on each slow path, so chains drain toward a one-slab floor naturally. The residual hazard is
  the tail case: a `(CPU, class)` pair whose final cached slab decays to Empty and is never
  exercised again leaves that one page pinned indefinitely. Bounded by `cpuCount × numClasses ×
  pageSize` (one page per (CPU, class)) plus whatever Empty slabs remain on the *shared* Treiber
  stack (which DEC-036 does not touch). Future `scavenge()` (ITEM-009) is the defined mitigation
  for the residual cases.
- In-page descriptor (DEC-008): the first slots of every slab are permanently reserved for the
  descriptor. Off-by-one in the `alignUp(sizeof(SlabDescriptor), slotSize)` arithmetic, or in the
  `reserveSlot` calls during init, would either (a) hand out the descriptor's storage as a slot
  (corruption) or (b) waste an extra slot per slab.
- **Treiber stack ABA (mitigated by DEC-015):** the pop → own → fill → orphan → remote-free-republishes
  pattern is the canonical ABA scenario. The 64-bit tagged head (offset + 37-bit counter) makes a
  stale CAS comparison fail because the counter has advanced on every push (push-only rule per
  ITEM-030). Three latent risks remain:
  (a) the counter wraps every 2^37 pushes — at sustained 10 M pushes/sec per domain this is a
  ~14 hour wraparound (multiplied by K under DEC-034 chain transfer), and a stalled popper would
  have to be off-CPU for that long while exactly the same descriptor cycled back — practically
  safe but worth a one-line comment in the implementation;
  (b) the offset encoding is only valid as long as the VMSubstrate range fits in a single PDPT —
  protected by a boot-time `static_assert` / runtime assert;
  (c) **the explicit `& kCounterMask` on counter increment is load-bearing** — an unchecked
  `oldCounter + 1` at the 37-bit boundary would carry into bit 27 (the descriptor-offset MSB),
  silently corrupting the next push's encoded offset and producing a head value that decodes to a
  wrong descriptor address. The mask must be applied before the shift, on every push. Stress tests
  for ABA correctness do not exercise this code path because the wraparound time is too long; a
  unit test that constructs synthetic high-counter values and verifies the mask discipline is the
  right gate.
- **IRQ/NMI/#GP/#MC-context misuse (DEC-014 amended):** the spec forbids these contexts, but the
  compiler can't see that. A future change that introduces a forbidden-context allocation will
  produce silent corruption (lost slab on slow-path reentry) rather than a loud failure. Mitigation:
  a debug-build "not in IRQ/NMI/#GP/#MC context" assert at the head of both `vmsmalloc` and
  `vmsfree`, and a documented warning in the public header. Any introduction of a forbidden-context
  consumer must be accompanied by a spec change here.
- **#PF-context transitive re-entrancy (DEC-014 amended via ITEM-052):** #PF context is
  conditionally legal — a page-fault handler may call vmsmalloc/vmsfree only if the work
  performed during that call cannot itself page-fault. The runtime cannot detect this; it's a
  caller-side discipline. If VMSubstrate or RadixVM (the two layers under vmsmalloc) ever start
  taking page faults during their own operation, a #PF handler that uses vmsmalloc transitively
  reenters and corrupts magazine state on the interrupted CPU. Mitigation: keep VMSubstrate's
  metadata and RadixVM's working set pinned (resident), and audit any future allocation site in
  those layers for paging behavior. Stress test: drive a #PF handler that calls vmsmalloc against
  a workload that exercises every VMSubstrate / RadixVM path, and assert no nested PF occurs
  inside the vmsmalloc call frame.
- **Cross-CPU TLB staleness on slab descriptors (DEC-016):** `vmsfree` calls
  `ensureTLBEntryFresh(ptr)` before any descriptor read. The trap is the *order* of operations
  in the freer: any code path that touches `desc->magic` (or any other descriptor field) before
  the freshness call defeats the guarantee. Keep the freshness call as the literal first
  statement of `vmsfree`, and add a stress-test scenario that pins each free to a CPU that has
  never previously touched the slab page.
- **NUMA-locality drift (DEC-018):** `desc->numaDomain` is the caller's home domain at slab
  creation, not the page's physical placement. Under local-exhaustion fallback, allocPage may
  return a remote page; future freers will still route the slab to the partial stack indexed by
  the original caller's domain. Acceptable graceful degradation, but if NUMA pressure becomes a
  measured problem, the lookup variant (DEC-018 option b) is the documented escape valve.
- **Bookkeeper-extension blast radius:** DEC-011's generalization of `SlabBookkeeper` and
  DEC-013's double-free detection in `SplitBitmap` are shared with the planned `LibAlloc::SlabAllocator`
  rewrite. A subtle bug introduced in those primitives affects both consumers. Treat the LibAlloc
  unit tests for these as a load-bearing gate, not a stylistic preference. **Process note:** the
  Phase-1 LibAlloc stress/unit tests are a release gate for Phase 1 itself — do not defer them
  to "after vmsmalloc lands" follow-up work. DEC-039's pre-read race-freedom argument depends
  on the bookkeeper's split-bitmap behavior under maximally adversarial schedules; a regression
  here is silently undetectable from vmsmalloc's own tests because the race window is
  microscopic. The LibAlloc tests are the only place the contract is exercised directly.
- **DEC-042 ARMv8 release gate is the default test path, not a gold-plated option:** the dev
  machine (Apple M1 MacBook Air) is itself ARMv8, and the existing userspace unit-test build
  (`tests/liballoc/CMakeLists.txt`) already runs there. ARMv8 weak-memory exposure is therefore
  available *for free* in every test run — no separate dev hardware, no remote target. The
  consequence: TSan-clean on the M1 dev machine is the natural gate from Phase 1 onward, not a
  late-stage gold-plate. Phases that introduce concurrency-sensitive code (Phase 1 bookkeeper,
  Phase 4 Treiber primitives, Phase 5 fast path, Phase 9 stress) should each include a
  TSan-instrumented userspace test variant that runs as part of `run_liballoc_tests` (or its
  successor). The existing test CMakeLists has `-fsanitize=address` but not
  `-fsanitize=thread`; adding a parallel TSan variant is a small infrastructure cost. The
  "process-flexible" framing in earlier drafts was over-defensive — given the dev hardware,
  ARMv8 + TSan is cheaper than not having it.
- **DEC-039 pre-read is positional discipline (refactor-fragile):** the pre-read of
  `m.head->chainNext` *must* precede `bookkeeper.allocSlot(m.head, ...)` in CPU program order.
  A future refactor that helpfully consolidates the two operations into a single helper call,
  or that re-reads `chainNext` after `allocSlot`, silently reintroduces the chain-corruption
  race ITEM-041/045 surfaced. The implementation must call out the pre-read with a comment
  citing DEC-039 at the load site, and a stress test (Verification Targets: "Same-domain
  becameFull chain integrity") must be part of every change-rate-gated CI run, not a one-off.
  The pre-read pattern looks like an obvious optimization to remove — it isn't.
- **Magic-field collisions:** the descriptor magic catches non-slab pointers only probabilistically.
  DEC-044 pins the value at `0x5DAB5DABDE5CC9C0ULL` — non-canonical when dereferenced as an
  AMD64 address, ASCII-recognizable in hex dumps, and disjoint from the DEC-024 free-poison
  pattern. If the descriptor layout is ever moved or resized, bump the magic to a fresh
  value via a new DEC so live descriptors from prior builds trip the check rather than passing
  silently.
- **Descriptor-publication memory ordering:** the owner CPU constructs `desc->magic`,
  `desc->sizeClass`, `desc->numaDomain`, and the bookkeeper state on the slow path, then returns
  a pointer to slot 0 (or some later slot) to its caller. A remote freer reads those descriptor
  fields. Visibility of the descriptor writes to the freer is *not* provided by `vmsmalloc`
  itself — it relies on the caller publishing the slot pointer via a release-ordered store (e.g.,
  RadixVM storing it into a shared atomic) before any other CPU can observe it. If a future caller
  publishes slot pointers without release ordering, freers may read partially-initialized
  descriptor fields and miscompute the magic / size class / NUMA-domain routing. This is a
  *caller-side* obligation but worth pinning in the contract.
- **TLB stale-PA on recycled pages:** mitigated by VMSubstrate's design (DEC-016 clarification of
  ITEM-026). The hazard is gated by `setDirtyForOtherCPUs` firing on every PTE mutation in
  VMSubstrate. If a future VMSubstrate change removes that call from `allocPage` or `freePage`, the
  vmsfree path silently begins reading from stale PAs. Watch for: any patch to `VMSubstrate.cpp:585`
  (allocPage) or `VMSubstrate.cpp:624` (freePage) that touches the `setDirtyForOtherCPUs` call.
- **Preemption / migration contract (DEC-030):** DEC-030 pushes the "no preemption, no migration
  mid-call" obligation to callers. CroCOS has no scheduler so the obligation is currently
  vacuous, but a future preemptive scheduler must (a) land the preempt-disable / migrate-disable
  primitives, (b) extend the debug-build entry assertions in `vmsmalloc`/`vmsfree`, and (c) audit
  every caller. Skipping any of these silently corrupts magazine state: a preempted slow path
  could resume on a different CPU and write to a magazine row indexed by the new CPU (cross-CPU
  write to a struct expected to be CPU-private), and a thread migrated between the becameFull
  atomic and the corresponding magazine pop would pop on the wrong CPU's magazine (the
  bookkeeper transition fired on slab X, but the pop targets the new CPU's `m.head` which may
  be Y — chain corruption).
- **Reader-side TLB freshness (eliminated by DEC-028):** demoting `vmsmalloc`/`vmsfree` to
  implementation-internal and making `make<T>` / `destroy<T>` the only public surface forces every
  consumer through `SafePtr<T>`, whose `operator*` / `operator->` calls `ensureTLBEntryFresh` on
  first cross-CPU access. The tripwire remains only if the implementation TU itself leaks a bare
  pointer to a non-allocating CPU — e.g., a misguided refactor that exposes the raw `void*` from
  `make<T>`. Keep `vmsmalloc`'s declaration out of any public header (and ideally `static` /
  anonymous-namespace inside the implementation TU) so this can't happen accidentally.
- **False sharing on `partial[d][c]` Treiber heads (mitigated by DEC-032):** each head is wrapped
  in an `alignas(64) TreiberHead` from day one, so cross-class CAS within a NUMA domain hits
  separate cache lines. Residual hazard: if the cache-line size constant `arch::cacheLineSize`
  ever drifts (a different microarchitecture, an L2-line-coherent design), the alignment must be
  audited against the new value. The padding overhead is absorbed by DEC-033's metadata arena
  storage, so there's no "save bytes by un-padding" temptation in the future.
- **VMSubstrate-range bad pointer landing in an unpopulated arena (DEC-031):** documented gap.
  DEC-026's range check is `vmsBase ≤ p < vmsBase + 512 GiB`, far coarser than "in an active
  arena." A non-page-aligned bad pointer in a never-instantiated arena reaches
  `ensureTLBEntryFresh`, which page-faults reading the nonexistent dirty-bitmap page. The
  diagnostic is loud (a page fault inside `ensureTLBEntryFresh` from `vmsfree` is a recognisable
  pattern) but less self-explanatory than the descriptor-magic assert. DEC-029's page-aligned
  dispatch already removes page-aligned cases from this path. Accepted because the residual band
  is narrow and RadixVM's free sites are tightly controlled. Revisit if observed.
- **Bad `vmsfree(p)` with `p` in the per-domain static-buffer region (DEC-033):** the static-buffer
  region is part of the VMSubstrate VA window (passes DEC-026's range check) but has no arena
  machinery (it sits in the slot a runtime arena would otherwise occupy; VMSubstrate skips
  arena construction for that slot). Diagnostic shape: a non-page-aligned bad pointer reaches
  `ensureTLBEntryFresh`, which page-faults reading the nonexistent dirty-bitmap page; a
  page-aligned bad pointer goes through DEC-029's bypass to `VMSubstrate::freePage`, which
  asserts (the static-buffer page was never registered with VMSubstrate's normal allocation
  tracking). Either path is loud. Accepted as part of the static-buffer trade-off — adding
  arena bookkeeping for the static-buffer region just to improve the diagnostic on a caller
  bug is the wrong cost/benefit. **Note:** the per-CPU magazine cache pages do NOT raise this
  concern — they live inside regular arenas (between the occupancy buffer and allocatable
  region) with the full arena's dirty-tracking and `ensureTLBEntryFresh` support. A bad
  `vmsfree(p)` pointing at a cache page passes `ensureTLBEntryFresh` cleanly and then fails
  the descriptor-magic check at the cache-page contents — loud assert, no fault.
- **Whole-page validation gap (DEC-029):** the slab-backed free path validates the descriptor via
  magic + slot arithmetic (DEC-013), but the whole-page branch has no such structured signature —
  there's nowhere to put a magic byte without corrupting caller data. A bug that calls
  `vmsfree(p)` with a page-aligned `p` pointing at an *active slab page* would bypass the slab
  path entirely and call `VMSubstrate::freePage` on that page, unmapping the slab and its live
  slot contents. Under correct caller usage this is impossible (no slot is ever page-aligned per
  DEC-001/DEC-008), but a buggy caller passing a stray page-aligned pointer is the canonical way
  to hit this. Mitigation depends entirely on `VMSubstrate::freePage`'s own validation — it must
  reject a free of a page that vmsmalloc never allocated as a "whole page" (which it cannot
  distinguish from a slab page today). Long-term mitigations if the gap proves costly: (a)
  side-bitmap of "whole-page allocations owned by vmsmalloc" (16 MiB at full VMSubstrate
  occupancy); (b) reserve the first 16 bytes of every whole-page allocation for a magic header
  (consumers see `pageSize - 16` usable, alignment relaxed to 16 B).
- **Chain encoding invariant (DEC-034, refined by DEC-041):** `desc->next` is meaningful only on
  the chain *head* (set at flush time to the previous shared-stack head, consumed by the next
  popper as the new shared-stack head); `desc->chainNext` is meaningful only between elements
  of a chain (head walks down to bottom; bottom's `chainNext == nullptr`). Mixing these — e.g.,
  reading `desc->next` on a non-head chain element, writing `desc->next` on the chain bottom, or
  walking `chainNext` from the bottom — corrupts the shared-stack linkage and silently merges or
  splits chains. Mitigation: the flush operation is the *only* code site that sets `desc->next`,
  and only on `m.head`; the magazine-refill operation is the only site that walks `chainNext`
  from a popped chain head; both stay confined to the magazine implementation. Stress tests
  should cover concurrent flushes and refills to surface encoding-mistake regressions early.
- **Magazine tuning counter overflow (DEC-035):** `overflowCount` and `starvationCount` are
  `Atomic<uint32_t>`; under sustained heavy contention they can wrap. The tuning policy must
  read-and-reset both counters atomically (e.g., `xchg` / `exchange(0)`) so the ratio reflects only
  the most recent measurement window. A simple `load` + `store(0)` race would lose increments and
  may bias the policy. Mitigation: policy uses exchange-with-zero; the policy is the sole writer
  of `currentK` so adjusts based on the snapshot exchanged in.
- **Magazine left below flush threshold (DEC-034/037):** a low-fanout class on an idle system may
  accumulate freer-side pushes that never cross `currentK`, leaving the chained slabs reachable
  only through `magazines[i][c]`, not through any shared structure. Under lazy reclamation
  (DEC-002) this is benign — the slabs are simply available for the next allocator call on the
  same CPU (the magazine head IS the allocation target under DEC-037). But scavenge (ITEM-009,
  deferred) must drain magazines before sweeping the shared stacks, or it will miss slabs
  eligible for reclamation.
- **Eager-free of Empty inbox slabs (DEC-036) — UAF amplification:** under correct caller usage,
  an Empty slab has no live slot pointers and `freePage` is safe. Under a use-after-free bug
  where a caller dereferences a stale slot pointer *after* the slab has decayed to Empty in some
  other CPU's inbox, DEC-036 may freePage the slab between the bug-callsite's read and the
  `vmsfree` validation, escalating a "double-free assert at the bookkeeper" outcome (correct
  caller) into a "page fault in ensureTLBEntryFresh / failed descriptor magic check" outcome
  (buggy caller). Both diagnostics are loud, but the latter is slightly less self-explanatory.
  Accepted: the eager-free's correctness for well-behaved callers and steady-state memory
  recovery outweigh the cosmetic diagnostic shift for caller bugs. Mitigation: stress tests
  should exercise concurrent Empty-slab eager-free against deliberate UAF patterns to ensure the
  fault diagnostic is recognisable.
- **Dual-ownership race on `becameFull` — resolved structurally by DEC-037:** the original
  ITEM-036 hazard was that a slab filled to Full while still being CPU A's `current[i][c]`
  could be republished by a remote freer before A dropped it, producing dual ownership. DEC-037
  dissolves this by eliminating `current[]` entirely — the magazine's head is the sole
  representation of "active allocation target", and `becameFull` triggers a synchronous pop
  before vmsmalloc returns. Stress test for it nonetheless: pin one CPU to fill a slab to
  Full, immediately have another CPU free a slot, then have a third CPU pop the partial stack
  and try to allocate — assert that only one of (first CPU, third CPU) successfully claims the
  slab. This stress test must pass under the DEC-037 implementation; if it fails, the pop-on-
  becameFull discipline is misimplemented.
- **Per-domain static-buffer TLB-staleness — non-issue by construction (DEC-033):** the
  static-buffer region is allocated and mapped exactly once at init via
  `VMSubstrate::reservePerDomainStaticBuffer`, and never re-mapped. Before init, no PTE
  exists for those VAs — APs that touch them after init TLB-miss into a fresh, immutable PTE.
  Because the mapping never changes for the lifetime of the kernel, no cross-CPU
  TLB-freshness concern arises despite the static-buffer slot lacking arena-style dirty-page
  tracking. The per-CPU magazine cache pages are NOT subject to this concern — they live in
  regular arenas and use the existing dirty-bitmap machinery; they're also never re-mapped,
  so the dirty bits stay clean and `ensureTLBEntryFresh` is a no-op for those VAs. If a
  future change ever introduces re-mapping in either region, the static-buffer side needs an
  explicit shootdown mechanism added.
- **`reservePerDomainStaticBuffer` is single-threaded by design (DEC-033):** the primitive is
  callable only during VMSubstrate's `init` and `vmsmallocInit` paths (both running on the
  BSP before APs come up). It allocates physical pages and walks VMSubstrate's existing
  arena-style page-table math to install leaf PTEs; the intermediate-PT writes are
  non-atomic and **not safe for concurrent invocation**. If a future kernel component needs
  to reserve additional static buffers from a multi-CPU context, the primitive must either
  be hardened or wrapped — out of scope for this spec.
- **Same-domain becameFull chain-corruption race (DEC-039):** the most subtle correctness hazard
  in the entire design. The window is small (instructions between owner's `allocSlot` returning
  `becameFull` and owner's pop reading `chainNext`) but real on multi-CPU systems. Any future
  refactor that moves the `chainNext` load to after `allocSlot`, or that introduces a re-read of
  `m.head->chainNext` post-`allocSlot`, silently reintroduces the corruption. The fix is
  positional: the load must precede `allocSlot` on the CPU's program order. A comment at the
  pre-read site naming DEC-039 is the strongest defense. Stress test that drives concurrent
  same-domain alloc/free on a shared small-class slab to surface regressions: pin CPU A
  repeatedly allocating, pin CPU B (same domain) repeatedly freeing the just-allocated pointer,
  assert that A's magazine never points to a slab from B's magazine after thousands of cycles.
- **Allocator-side TLB freshness on refill (DEC-040):** the refill walk's per-element
  `ensureTLBEntryFresh` is structurally easy to forget. Any future refactor that moves the
  walk into a tight loop without the freshness call silently reintroduces the stale-PA-on-read
  hazard ITEM-042 surfaced. Mitigation: the spec's Concurrency Model pseudocode embeds the
  freshness call in the walk; reviewers should treat its absence as a defect. Stress test that
  pins each refill to a CPU that had previously held a freed slab at the same VA (constructed
  by deliberate freePage/allocPage churn on multiple CPUs) and verifies the popped descriptor
  is read correctly.
- **Static-buffer-region arena-slot collision (DEC-033):** VMSubstrate reserves the topmost
  arena-equivalent VA slot for the static-buffer region during its own `init`, and shifts
  CPU-owned arenas to start at the slot below. This reservation is centralized inside
  VMSubstrate (not "by convention" — it's a structural property of arena enumeration).
  Hazard residue: if a future VMSubstrate refactor reverts or alters the slot reservation,
  the static-buffer region collides with a runtime arena's VA. Defense: an assertion in
  VMSubstrate's `init` that the arena count plus the static-buffer slot fit within the
  available VMSubstrate VA window; an assertion in `reservePerDomainStaticBuffer` that the
  returned VA lies in the reserved slot. Both are runtime-cheap and surface the bug
  immediately.
- **Chain encoding: head-linkage vs bottom-linkage confusion (resolved by DEC-041):** the spec
  briefly drifted into a bottom-linkage interpretation (`m.tail->next` carrying the Treiber
  link) that was incompatible with the single-CAS pop the rest of the spec assumed. DEC-041
  restored head-linkage (`m.head->next` carries the link) and dropped the now-vestigial
  `m.tail` field. A future refactor that re-introduces a notion of "chain bottom carries the
  shared-stack link" silently reintroduces ITEM-043's contradiction; any code that writes
  `next` on a non-head chain element is a defect.
- **Treiber CAS and chain-bookkeeping memory ordering (resolved by DEC-042):** the policy is
  push initial load = relaxed, push CAS = release; **pop initial load = acquire** (load-bearing
  on ARMv8/RISC-V — the pop reads `chainHead->next` before the CAS), pop CAS = acquire;
  `chainNext` atomic ops = RELAXED (synchronization piggybacks on surrounding acq-rel); and
  `LibAlloc::SlabBookkeeper`'s `allocSlot`/`freeSlot` must be at least acq-rel. Residual
  hazard: a refactor that downgrades any of these (e.g., switching the pop initial load to
  relaxed because "x86 doesn't need it") silently breaks cross-CPU descriptor visibility on
  ARMv8/RISC-V. The bug is undetectable on x86 dev hardware. Mitigation: the implementation
  must `#define` the orderings in named constants (`kTreiberPushOrder`, `kTreiberPopLoadOrder`,
  `kTreiberPopCASOrder`) referenced from every site, with comments citing DEC-042 — so a
  downgrade is visible at one point of edit rather than scattered. Validation requires the
  userspace test harness running under ThreadSanitizer on ARMv8 dev hardware (see Testing
  Approach). The LibAlloc-side requirement is the most fragile link: `Slab.h` must document
  the acq-rel contract; a future LibAlloc refactor that relaxes the bookkeeper atomics
  silently breaks DEC-039's pre-read race-freedom on weak architectures.
- **Mixed metadata-page diagnostic gap (DEC-038 / ITEM-047):** DEC-038's "erroneous index
  page-faults" claim is page-granular, not row-granular. Pages that *exclusively* cover
  CPU-less or unused DomainID rows are unmapped (a misindex faults cleanly). Pages that
  cover *both* CPU-bearing and CPU-less rows are mapped — the CPU-less rows on those pages
  silently read/write zero-initialized memory rather than faulting. Under DEC-018 the
  freer's `desc->numaDomain` is always set to the slab creator's home domain (which by
  construction has a CPU), so this only manifests under a deeper bug that corrupts
  `desc->numaDomain`. Mitigation if it ever surfaces: add a `domainHasCPU(d)` predicate
  branch on the freer's hot path before indexing `partial[d][c]`. Out of scope today —
  RadixVM is the only caller and its descriptor writes are tightly controlled. Watch for:
  a future change that broadens trust of `desc->numaDomain` (e.g. taking it from caller
  input rather than computing it from `NUMAPolicy::domainFor(currentCPU())` at slab
  creation).
- **Magazine-head `chainDepth` / `next` are undefined while in a magazine (ITEM-046):** the
  chain-encoding invariant `chainDepth == n` and `next == nextSharedHead` holds only for
  chains on the shared Treiber stack (and at flush time, after the flushing CPU writes
  both fields just before the publishing CAS). Pop, eager-free, freer-push, and fast-path
  consumption all leave the magazine-head's `chainDepth` / `next` in a stale state. The
  authoritative depth in the magazine is `m.depth`. A future change that reads
  `m.head->chainDepth` to infer magazine depth — instead of using `m.depth` directly —
  will see stale values and corrupt the chain. Mitigation: never read `m.head->chainDepth`
  or `m.head->next` while in the magazine; treat `m.depth` as the only valid depth source
  pre-flush.
- **Per-CPU magazine pages and NUMA locality (resolved by DEC-033 rewrite):** under the
  2026-05-26 DEC-033 rewrite, each CPU's magazine cache page lives in that CPU's own first
  arena, mapped on `NUMAPolicy::domainFor(i)` as part of arena creation. Perfect NUMA
  locality by construction — no cross-domain straddling, no shared-page contention. The
  previous concern (a single global metadata page covering multiple CPUs' magazines) is
  obsolete; this entry remains only to flag any future regression that pulls the magazine
  storage back into a global region.
- **LibAlloc `SlabBookkeeper` read-side API surface dependency (ITEM-055 candidate):** the
  dependency table enumerates three required *write-side* edits (DEC-011 generalization,
  DEC-013 double-free detection, DEC-042 #4 acq-rel doc), but vmsmalloc also reads from the
  bookkeeper on hot paths: DEC-036's eager-free needs an `isEmpty(desc)` (or equivalent
  occupancy-state query), and the slot-range check (DEC-026 step 6a, ITEM-048) reads
  `desc->slotCount` if that field is denormalized — pending ITEM-049's resolution. If
  `slotCount` is derived from `sizeClass` via the `constexpr` table (DEC-045 alternative
  interpretation), then the bookkeeper need not expose it, but the eager-free `isEmpty`
  query is unconditional. A LibAlloc refactor that renames or removes either accessor
  silently breaks vmsmalloc. Mitigation: pin the read-side surface in the dependency table
  alongside the write-side edits, and add a Slab.h header comment naming the consumer.
- **AP-startup memory-ordering visibility of BSP-installed metadata pages:** vmsmalloc's
  metadata storage is written exclusively by the BSP during `memory_management` (DEC-021)
  via `VMSubstrate::reservePerDomainStaticBuffer` (per-domain buffers) and via
  `createArena`'s cache-page setup + `vmsmallocInit`'s zero-pass (per-CPU caches). APs come
  up later in `smp_bringup`. An AP's first read of `kernel::cpuLocal().magazines[c]`
  (or `cpuLocalPageFor(apID)`-derived state per P7-DEC-010), `partialFor(d)`, or
  `tuningFor(d)` must observe BSP's writes — including the page-table
  writes that map the per-domain buffers and the cache page inside the AP's own arena. This
  relies on the AP-bringup path issuing the appropriate memory barriers (or on the
  page-table installation writes being globally visible by the time APs execute their
  first instruction).
  Today CroCOS's AP-bringup sequence is presumably correct on x86 (writes serialize via the
  startup IPI), but the dependency is implicit and would break silently on ARMv8 / RISC-V if
  the AP-init path didn't issue a strong barrier before the AP's first vmsmalloc-touching
  instruction. Mitigation: stress test the userspace harness (DEC-042 portability target) with
  a "BSP writes metadata, then signals AP to start, AP immediately calls vmsmalloc" pattern
  on ARMv8 to surface barrier gaps.
- **Size-class schema extension can produce `slotCount[c] == 0` (DEC-045 fixpoint corner):**
  DEC-045 derives `slotCount[c]` as the largest `N` such that
  `alignUp(sizeof(SlabDescriptorBase) + sizeof(SlabBookkeeper<N>), max(slotSize(c), 16)) + N * slotSize(c) ≤ pageSize`.
  Under the current DEC-003 schema (max 512 B) every class admits at least one slot. A
  schema extension that adds a class with `slotSize` close to `pageSize` (e.g., a 3072 B
  class) could yield `slotCount = 0` — the fixpoint converges to "no slots fit after
  descriptor overhead". `sizeClassFor(size)` would still route requests to such a class, and
  the slab-creation slow path would build a zero-slot slab from which `allocSlot` cannot
  succeed, deadlocking the allocator. Mitigation: the `constexpr` derivation table must carry
  a `static_assert(slotCount[c] > 0)` for every class index; a schema-extension that violates
  it fails to compile rather than producing a runtime stall. The DEC-029 large-request bypass
  already handles `size > largestSizeClass`, so the right resolution for any "doesn't fit a
  slab" class is to omit it from the schema entirely and rely on the bypass for that size.

## Verification Targets

<!-- Properties to actively confirm. Specify the intended method: unit test, integration test,
     stress test, formal verification, or manual review. -->

| Property | Method |
|---|---|
| Single-CPU alloc/free conservation | Unit test |
| Multi-CPU concurrent free correctness | Stress test |
| Pointer-to-size-class lookup correctness | Unit test |
| NUMA-local allocation preference under contention | Stress test + counter |
| Arena-exhaustion behavior matches contract | Unit test |
| Treiber stack ABA-safety under DEC-015 (counter advance prevents stale-head CAS success) | Stress test with crafted pop/repush interleavings |
| DEC-015 counter mask discipline: counter wraparound at 2^37 does not corrupt the encoded descriptor offset | Unit test that constructs synthetic high-counter heads, drives a push to force the wrap, and verifies the resulting head decodes to the correct descriptor address (i.e., bit 27 was not corrupted) |
| `vmsfree` correctness when freer's CPU has never touched the slab page (DEC-016) | Stress test that pins each free to a fresh CPU |
| Magazine amortization (DEC-034): shared-stack CAS rate ≈ `2 × ops / K` under all-CPUs-one-class load | Stress test + `overflowCount` / `starvationCount` observation |
| Cross-domain free gate (DEC-019/DEC-034): cross-domain frees never visit any outbox | Stress test with CPUs in domain A freeing slabs from domain B; assert magazine state unchanged |
| Chain encoding invariant (DEC-034): inbox `chainDepth` matches actual `chainNext` walk length | Debug-build assertion on every inbox refill |
| Tuning policy convergence (DEC-035): K stabilizes within `[Kmin, Kmax]` under steady-state load | Stress test with varying fanout; observe K trajectory |
| Eager-free behavior (DEC-036): inbox chains with intermixed Empty/Partial slabs drain Empties first, never freePage the last cached slab | Stress test that constructs synthetic chains with crafted Empty placement, then drives a slow-path allocation and observes the resulting freePage call sequence + final inbox state |
| **Dual-ownership avoidance (DEC-037)**: a slab transitioning Full inside its owner's magazine does not become any other CPU's magazine head concurrently | Stress test: pin CPU A to fill slab X to Full on a small class, immediately pin CPU B to free a slot in X, immediately pin CPU C to refill its magazine from the partial stack. Assert that no two of {A, C} concurrently hold X as their magazine head while calling `allocSlot`. The pop-on-`becameFull` discipline (DEC-037) is what makes this pass. |
| **Same-domain becameFull chain integrity (DEC-039)**: owner's pop after `becameFull` uses the pre-read `chainNext`, not a post-`allocSlot` re-read | Stress test: pin CPU A to a small-class slab with at least 2 slots and a chain of depth ≥ 2; concurrently pin CPU B (same domain) to repeatedly free slots in A's chain head. A drives the head Full → B observes `becameAvailable` → B's same-domain push mutates `head->chainNext`. Assert A's chain successor after the pop matches the pre-allocSlot value, not B's overwritten value. Run thousands of cycles; failure manifests as A's magazine pointing into B's chain (verifiable by chain-domain consistency check). |
| **Cross-domain becameFull chain integrity (ITEM-045 / DEC-039 case (c))**: same race as DEC-039 but with a cross-domain freer overwriting `head->chainNext = nullptr` as part of the singleton-push setup (DEC-019/034) | Stress test variant of the above, with at least two NUMA domains in the test harness: pin CPU A (domain 0) to a small-class slab with a chain of depth ≥ 2; pin CPU C (domain 1) to repeatedly free slots in A's chain head. A drives the head Full → C observes `becameAvailable` → C's cross-domain singleton-push setup writes `head->chainNext = nullptr` before CASing onto `partial[0][c]`. Assert A's chain successor after the pop is the pre-allocSlot value, not `nullptr`. Failure manifests as A's magazine prematurely emptying after one allocation. The userspace test harness must mock `NUMAPolicy` to expose at least two CPU-bearing domains. |
| **Allocator-side TLB freshness (DEC-040)**: refill on a CPU that previously held a freed slab at the same VA reads the new slab's descriptor correctly | Stress test: pin CPU 1 to pop+use+flush slab S at VA X; pin CPU 2 to pop S, eager-free S via DEC-036, then create slab S' (which lands at VA X by deliberate page-allocator priming); pin CPU 1 to pop S' from the Treiber. Assert CPU 1's read of `desc->numaDomain` / `desc->chainDepth` matches S' (not S). Without DEC-040 the test reads S's stale values from CPU 1's TLB. |
| **Weak-memory portability (DEC-042)**: pop-side initial load is acquire, push CAS is release, bookkeeper atomics are acq-rel — all required for correctness on ARMv8/RISC-V | Userspace test harness under `tests/kernel/vmsmalloc/`, run under ThreadSanitizer on both x86 and **ARMv8 dev hardware**. TSan-clean across the full stress matrix is a release gate. Tests must include the concurrent flush-vs-pop pattern (push writer's release pairs with pop reader's acquire on `chainHead->next` and `chainHead->chainDepth`); a regression that downgrades the pop's initial load to relaxed should surface as a TSan data race on ARMv8 even if x86 runs clean. |
| **vmsfree slot-range lower-bound check (ITEM-048 / DEC-026 step 6a)**: a `vmsfree(p)` with `p` in the descriptor region (offsets `0..slot0Offset−1`) that coincidentally satisfies `(p - slotZeroAddr) % slotSize == 0` via unsigned underflow is rejected before the bookkeeper is touched | Unit test that feeds `vmsfree` a crafted pointer in the descriptor region for each size class (e.g. for class 8: `p = pageBase + 8` where `slot0Offset == 64`, so the modulo passes but the lower-bound check fails). Assert that the lower-bound check fires *before* `bookkeeper.freeSlot` is reached. Covers all DEC-003 classes since `slot0Offset[c]` varies per class. |

## Testing Approach

<!-- Mock interfaces needed, overall test strategy, stress test story, formal verification scope if any. -->

- **Bookkeeper unit tests** for the DEC-011 generalization and DEC-013 double-free detection live in
  `tests/liballoc/SlabTest.cpp` alongside the existing 15 cases.
- **In-kernel stress test** modelled on `naiveTest` (the existing VMSubstrate stress) drives
  multi-CPU concurrent `vmsmalloc`/`vmsfree` across all size classes, validates pointer integrity,
  and exercises the Full→Partial push race, the magazine fill/spill/refill cycle (DEC-034), and
  the chain-eager-free walk (DEC-036). Stress coverage in QEMU is the primary signal.
- **Userspace mockable harness, targeting ARMv8 dev hardware (Apple M1 — already the unit-test
  dev machine).** Built incrementally across the phases: Phase 1 adds the bookkeeper-side
  TSan suite, Phase 4 adds the Treiber-stack-side TSan suite, and Phase 8 stands up the full
  vmsmalloc-integration harness in `tests/kernel/vmsmalloc/`. The external surface vmsmalloc
  depends on is intentionally narrow — `VMSubstrate::reservePerDomainStaticBuffer`,
  `VMSubstrate::localCachePageFor`, `VMSubstrate::allocPage` / `freePage` / `ensureTLBEntryFresh`,
  `PageAllocator::allocateSmallPage`, and `NUMAPolicy` queries. All are mockable by a userspace
  test harness via header-substitution or link-time stubs. The Phase-8 harness exercises the
  layout logic, magazine state machine, chain eager-free policy, and ABA-counter mask
  discipline under crafted concurrent schedules without depending on QEMU. **ARMv8 coverage
  is required, not optional** — the memory-ordering policy (DEC-042) is portable by
  construction, and x86-only validation cannot surface weak-memory bugs in the pop's initial
  load, the bookkeeper's acq-rel dependency, or any
  future refactor that accidentally downgrades a CAS to RELAXED. The harness should run under
  ThreadSanitizer (TSan) on both x86 and ARMv8; TSan-clean is a release gate. The Treiber-stack
  base-pointer arithmetic (DEC-015) needs a mockable VMSubstrate base — the harness provides
  one via a configurable constant rather than the kernel's fixed VA window.

## Implementation Phases

<!-- Leaf specs only. Delete this section for subsystem specs — decomposition is tracked via components
     in frontmatter. Ordered breakdown of implementation steps. -->

<!-- All review-blocking ITEMs resolved as of 2026-05-21. Phase 0 (review-resolution gate)
     deleted; implementation proceeds with Phase 1 below. -->

1. **LibAlloc prerequisites — see [`vmsmalloc-phase-1.md`](vmsmalloc-phase-1.md) for the
   elaborated implementation spec.** Summary: lift the artificial `SlotCount % 64 == 0`
   restriction in `libraries/LibAlloc/include/liballoc/Slab.h` (DEC-011) by switching to
   ceiling division for `kWordCount` and masking tail bits; document the acq-rel ordering
   contract on `allocatedCount` (DEC-042 #4); verify DEC-013's double-free assertion (already
   implemented at `SplitBitmap.h:180`) propagates through `SlabBookkeeper::freeSlot` and is
   covered by tests. The Phase-1 sub-spec elaborates concrete edits, test coverage matrix
   `SlotCount ∈ {1, 7, 15, 63, 65, 127, 137}`, and the SmallPageAllocator regression gate.
2. **Slab descriptor layout, size-class schema, derived constants — see
   [`vmsmalloc-phase-2.md`](vmsmalloc-phase-2.md) for the elaborated implementation spec.**
   Summary: create `kernel/mm/VMSubstrateSlab.h` (per DEC-028); define `SlabDescriptorBase`
   (32 B prefix, no `slotSize` denormalization — accessor via constexpr table); pin
   `kSlabDescriptorMagic = 0x5DAB5DABDE5CC9C0ULL` per DEC-044; mirror DEC-003's size-class
   schema; compute `slotCount[c]` / `slot0Offset[c]` via the DEC-045 fixpoint (2-iteration
   convergence with `bookkeeperSize(kWordCount)` dispatch); compose `SlabDescriptor<N>` as
   prefix + `SlabBookkeeper<N>`. Compile-time validation only — no runtime code in this
   phase. The Phase-2 sub-spec elaborates the field layout (P2-DEC-001), the fixpoint
   algorithm (P2-DEC-004), the `sizeClassFor` sentinel contract (P2-DEC-005), and the
   `SlabLayoutTest.cpp` `static_assert` matrix.
3. **VMSubstrate modifications for vmsmalloc metadata storage — see
   [`vmsmalloc-phase-3.md`](vmsmalloc-phase-3.md) for the elaborated implementation spec.**
   Summary (per DEC-033 rewrite, 2026-05-26): (a) modify VMSubstrate's arena enumeration to
   reserve the topmost arena-equivalent VA slot for vmsmalloc's per-domain static-buffer
   region, shifting CPU-owned arenas down by one slot; (b) modify VMSubstrate's arena layout
   to include a structurally-reserved per-CPU vmsmalloc cache page between the occupancy
   buffer and the allocatable region, allocated on `NUMAPolicy::domainFor(i)` during
   `createArena(i)`; (c) add two new VMSubstrate primitives — `reservePerDomainStaticBuffer(byteSize, d)`
   (allocates pages on domain `d`, maps them in the reserved static-buffer slot via the
   existing arena-style page-table math, returns the buffer VA) and `cpuLocalPageFor(i)`
   (pure-arithmetic accessor returning the per-CPU CpuLocal-page VA from arena `i`'s
   layout — renamed from the earlier `localCachePageFor` per P7-DEC-010);
   (d) extend `VMSubstrateSlab.h` with storage types `TreiberHead`, `Magazine`,
   `MagazineTuning` (each `alignas(arch::cacheLineSize)`), layout constants
   `kPerDomainBufBytes` (for the per-domain buffer) and `kCpuLocalPages` (in
   `kernel/include/cpu_local.h`, per P7-DEC-010 — the per-CPU page now hosts the unified
   `kernel::CpuLocal` struct including magazines), and accessor helpers `partialFor(d)`
   / `tuningFor(d)` (magazines accessed via `kernel::cpuLocal().magazines[c]` rather
   than a separate `magazineFor(i)`); (e) implement `vmsmallocInit()` in
   `kernel/mm/VMSubstrateSlab.cpp` — iterate CPU-bearing domains and call
   `reservePerDomainStaticBuffer`; the per-CPU CpuLocal pages are zeroed and have their
   `logicalID` field set during VMSubstrate's own `createArena` (per Phase 7 step 4);
   register `[VMSubstrateSlab]` in `kernel/general.icd`. **No `installKernelPTE`** —
   eliminated in favor of the higher-level primitives. Address arithmetic stays
   architecture-portable through VMSubstrate's existing `arch::pageTableDescriptor`-driven
   arena math. Testing is primarily in-kernel boot smoke (the API is single-threaded init
   code with no concurrency to mock); minimal userspace coverage where convenient.
4. **Concurrent Treiber stack primitives in LibCore — to be elaborated in
   [`vmsmalloc-phase-4.md`](vmsmalloc-phase-4.md).** Add
   `libraries/Core/include/core/atomic/TreiberStack.h` containing
   `Core::TreiberStack<T, LinkageTraits, HeadEncoding>` (basic intrusive concurrent stack)
   and `Core::ChainedTreiberStack<T, HeadLinkage, ChainLinkage, HeadEncoding>` (chained
   variant where push/pop transfers a whole chain atomically per DEC-034/041 head-linkage).
   Linkage traits supply `getNext` / `setNext` accessors; `HeadEncoding` is a policy
   parameter that defines the head storage type and `encode` / `decode` / `advanceTag`
   operations — the DEC-015 packed-tagged-head encoding becomes a vmsmalloc-supplied policy
   when the primitive is consumed in Phase 5, and the Core library ships a sensible default
   (e.g. `__uint128_t` head with separate pointer + counter words for consumers without a
   fixed-VA-window constraint). Memory-ordering per DEC-042: push CAS = release, pop initial
   load = acquire (load-bearing on ARMv8), `chainNext` ops = relaxed (synchronization
   piggybacks on the surrounding Treiber CAS). The phase ships with a concurrent stress test
   suite in `tests/core/TreiberStackTest.cpp` running under TSan on the M1 dev machine.
   Strictly independent of Phases 1–3 in terms of code dependencies; placed between Phase 3
   (metadata region) and Phase 5 (fast path) for narrative-flow reasons.
5. **`vmsmalloc` happy path (DEC-037/DEC-039/DEC-040/DEC-041):** size-class dispatch first — for
   `size > largestSizeClass` go straight to `allocPage` per DEC-029. Otherwise enter the
   magazine loop: if `m.depth > 0`, **pre-read `nextLocal = m.head->chainNext` (DEC-039) then**
   `allocSlot(m.head, transition)`. On success: if `transition.becameFull()`, pop using the
   pre-read local (`m.head = nextLocal; m.depth--`) — do not re-read `chainNext`. Return slot.
   If `m.depth == 0`: refill via one Treiber CAS from `partial[localDomain][c]` (uses the
   Phase-4 `Core::ChainedTreiberStack::pop()` primitive; no cross-domain steal per DEC-019);
   the CAS uses `chainHead->next` as the new shared head (DEC-041 head-linkage); **call
   `ensureTLBEntryFresh` on the popped chain head and on each chain element during the
   chainNext walk (DEC-040)** — the walk is a pure freshness sweep with no bookkeeping
   output (DEC-041). On shared-stack empty bump `starvationCount` and fall to fresh
   allocation via `allocPage` + init descriptor (`desc->numaDomain = localDomain`,
   `chainNext = nullptr`, `chainDepth = 1`, `m.head = desc; m.depth = 1`). Run eager-free
   walk (DEC-036): while `m.depth > 1` and `m.head` is Empty, `freePage(m.head)` and advance.
   Retry the loop.
6. **`vmsfree` happy path:** nullptr assert (DEC-023) → debug "not in IRQ/NMI/#GP/#MC context"
   (DEC-014) → VMSubstrate-range assert (DEC-026) → **page-aligned dispatch (DEC-029)**: if
   `(p & (pageSize - 1)) == 0`, `freePage(p)` and return. Otherwise `ensureTLBEntryFresh`
   (DEC-016) → mask → magic check (DEC-013/044) → **slot-range check
   `slotZeroAddr(desc) ≤ p < slotZeroAddr(desc) + slotSize * slotCount` (ITEM-048)** →
   slot-modulo check (DEC-013) → debug-only poison (DEC-024) → `freeSlot` →
   on Full→Partial: **cross-domain gate (DEC-019/DEC-034) — if `desc->numaDomain != numaOf(currentCPU)`,
   push a singleton chain directly to the home Treiber via the Phase-4
   `Core::ChainedTreiberStack::push()` primitive (setting `desc->next = oldHead.descPtr` per
   DEC-041 head-linkage). Same-domain: extend magazine head (`desc->chainNext = m.head;
   m.head = desc; m.depth++`); on `depth >= currentK` (DEC-035), flush the magazine via one
   `ChainedTreiberStack::push()` of the full chain — write `m.head->next = oldHead.descPtr`
   (DEC-041) — bumping `overflowCount` on retry.**
7. **Assertion paths (DEC-012, DEC-013, DEC-014, DEC-023):** wire panics, double-free /
   pointer-sanity asserts, edge-input asserts (`vmsmalloc(0)`, `vmsfree(nullptr)`), and
   debug-build "not in IRQ/NMI/#GP/#MC context" assert at the head of both functions (#PF is permitted per DEC-014 amendment).
8. **Userspace test harness on ARMv8 (DEC-042 portability gate):** stand up
   `tests/kernel/vmsmalloc/` with mocked `VMSubstrate::reservePerDomainStaticBuffer` / `localCachePageFor` / `allocPage` /
   `freePage` / `ensureTLBEntryFresh`, `PageAllocator::allocateSmallPage`, and `NUMAPolicy`.
   Run the multi-CPU concurrent alloc/free matrix under ThreadSanitizer on the M1 dev
   machine — TSan-clean on ARMv8 is a release gate, not a follow-up. Cover the
   memory-ordering-sensitive paths explicitly: pop reading `chainHead->next` immediately after
   acquire-load; cross-CPU descriptor publication via flush; DEC-039 pre-read race; bookkeeper
   acq-rel contract. (The Core Treiber primitive's TSan coverage from Phase 4 is a
   prerequisite; this phase tests the integration with vmsmalloc's magazine and bookkeeper
   state machines.) A regression that downgrades any DEC-042 ordering should surface as a
   TSan race on ARMv8.
9. **In-kernel stress test:** drive from `naiveTest`-style harness; include ABA, cross-CPU
   first-touch, **all-CPUs-one-class magazine throughput (DEC-034), and cross-domain free gate
   (DEC-019/DEC-034)** scenarios. Sample `overflowCount` / `starvationCount` to confirm
   amortization (≈ `2 × ops / K`).
10. **Magazine tuning policy (DEC-035/DEC-043):** implement the read-and-reset of
    overflow/starvation counters and the `currentK` adjustment within `[Kmin, Kmax]`. Gate
    entry on `policyLock.exchange(1, acquire) == 0` per DEC-043 (try-lock; skip on contention,
    release with store of 0); the lock is the only safe way to read-reset-decide-write the
    per-`(domain, class)` counter pair. Piggyback on the existing slow path; gate by event count.
    Initial K and bounds are tuning knobs. Optional in the first cut — the system is correct
    with `currentK == kInitialK` held constant and `policyLock` untouched; the policy is an
    optimization.
11. **(Deferred — ITEM-009)** explicit `scavenge()` for Empty-slab reclamation if quiet-workload
    pinning becomes measurable. Must drain per-CPU magazines (DEC-034/041) before sweeping the
    shared stacks.

## References

<!-- Relevant papers, prior art, related CroCOS components. -->

- `kernel/include/mem/VMSubstrate.h` — public API including `vmsmalloc`/`vmsfree` declarations.
- `kernel/mm/VMSubstrate.cpp:585` — current `allocPage` implementation; see ITEM-016.
  `ensureTLBEntryFresh` lives at line 694 and is invoked freer-side per DEC-016.
- `libraries/LibAlloc/include/liballoc/Slab.h` — generic slab primitive this layer composes.
- `kernel/include/mem/PageAllocator.h:118-165` — `BigPageMetadata` / `SmallPageAllocator`: closest
  existing example of per-NUMA partial-slab pooling.
- `kernel/include/mem/PageAllocator.h:222-234` — `LocalPool::paPage1`/`paPage2`: existing
  precedent for a per-CPU two-slot cache (structurally a degenerate magazine of K=2 without
  chain-transfer semantics).
- `libraries/LibAlloc/include/liballoc/SlabAllocator.h` — current LibAlloc slab design (to be
  replaced); contrast for API surface.
- `kernel/mm/VMSubstrate.cpp` — current radix-tree leaf bookkeeping pattern.
- Bonwick, J. & Adams, J. (2001), "Magazines and Vmem: Extending the Slab Allocator to Many
  CPUs and Arbitrary Resources" — origin of the per-CPU magazine + global-depot pattern adapted
  here as DEC-034 (chain transfer) and DEC-035 (runtime-tunable K via contention feedback).
- `/Users/spencer/.claude/projects/-Users-spencer-Documents-CroCOS/memory/project_slab_abstraction_plan.md`
  — Phase B context and the partial-slab steal open question.

---

## Concurrency Model

**Per-CPU state.** Each CPU `i` owns:
- `Magazine magazines[i][class]` (DEC-034/037/041) — padded to one cache line per `(CPU, class)`,
  holding `{ SlabDescriptor* head; uint32_t depth; }`. The magazine's `head` is the **active
  allocation target** whenever `m.depth > 0` (DEC-037 — no separate `current[]` array exists).
  The chain starting at `m.head` and walking `chainNext` for `m.depth - 1` hops to a slab with
  `chainNext == nullptr` is the per-CPU cache of partial-or-empty slabs for this `(CPU, class)`.
  Allocator-side pops drain from the head; freer-side pushes extend the head. Both happen only
  on CPU `i` and are uncontended (DEC-014 forbids IRQ/NMI; DEC-030 forbids preemption/migration
  mid-call), so no atomicity is needed on magazine fields. (DEC-041 removed `m.tail` — no
  operation reads the chain bottom.)

**Per-NUMA-domain partial-slab Treiber stacks (chained — DEC-002 / DEC-015 / DEC-034).**
`partial[domain][class]` is the head of a single-linked stack of *chain heads*. Each chain head's
`chainNext` chain holds the remaining `chainDepth - 1` slabs in the chain; chain elements are
private to the head and never appear independently on the shared stack. The shared-stack link
between chain heads is `desc->next` (Treiber linkage); push and pop are CAS-on-head against a
64-bit tagged head (DEC-015). Cross-domain stealing is **not done** — local miss goes to a fresh
`allocPage` (DEC-019).

**Per-(domain, class) magazine tuning (DEC-035).** `tuning[domain][class]` holds
`{ Atomic<uint32_t> currentK; Atomic<uint32_t> overflowCount; Atomic<uint32_t> starvationCount; }`.
The freer reads `currentK` on each flush decision; the tuning policy adjusts `currentK` against
the counter ratio.

**Allocator path on CPU `i` (called as `vmsmalloc(size)`):**
0. Assert `size > 0` and `size ≤ pageSize` (DEC-023, DEC-004) and "not in IRQ/NMI/#GP/#MC context"
   (DEC-014 amended, debug builds; #PF is permitted) at the head of `vmsmalloc`.
1. **If `size > largestSizeClass`** (DEC-029): call `VMSubstrate::allocPage()` (panic on failure
   per DEC-012) and return the page base address directly. Skip all slab machinery.
2. Let `c = sizeClassFor(size)` and `m = magazines[i][c]`. Loop:
   - **Fast path** (DEC-037/DEC-039): if `m.depth > 0`:
     - `head = m.head; nextLocal = head->chainNext;` **(DEC-039 pre-read: capture `chainNext`
       before `allocSlot` so a concurrent same-domain freer's push can't corrupt the pop.)**
     - Call `bookkeeper.allocSlot(head, transition)`. The slab can only be Partial or Empty
       here (magazine occupancy invariant), so allocSlot always succeeds.
     - If `transition.becameFull()`: **pop synchronously using the pre-read local**:
       `m.head = nextLocal; m.depth--; if (m.depth > 0) ensureTLBEntryFresh(m.head)` —
       lazy first-touch per DEC-040 amended; `nextLocal` is a previously-untouched chain
       element on this CPU since the refill pop. The just-filled slab is now orphaned-Full.
       Do **not** re-read `head->chainNext` here — `chainNext` may have been overwritten
       by a same-domain freer's push that observed `becameAvailable` on this slab.
     - Return the slot's address.
   - **Magazine empty — refill (DEC-034/037/DEC-040 amended 2026-05-27/DEC-041/DEC-042):**
     `m.depth == 0`. Call `partial[numaOf(i)][c].pop()` (Phase-4 `ChainedTreiberStack::pop`).
     The pop's internals: acquire-load `head`; if pointer is null, fire `Hooks::onEmptyStack`
     (vmsmalloc's hook bumps `tuning[numaOf(i)][c].starvationCount` RELAXED) and return
     `{nullptr, 0}`; otherwise fire `Hooks::onPreTouch(topPtr)` (vmsmalloc's hook calls
     `ensureTLBEntryFresh(topPtr)` — load-bearing because the next reads of `topPtr->next`
     and `topPtr->chainDepth` are published via CAS and must come from the current PA);
     read `topPtr->next` and `topPtr->chainDepth`; pop CAS (success=acquire, failure=relaxed
     per DEC-042); on CAS failure, fire `Hooks::onCasFailure` (vmsmalloc's hook bumps
     `overflowCount` RELAXED) and retry from the acquire-load.
     - On pop success returning `{chainHead, depth}`: `m.head = chainHead; m.depth = depth`.
       `chainHead` is already TLB-fresh from the pre-touch hook; no additional sweep walks
       the rest of the chain (chain elements get freshness lazily, at each `m.head`
       transition below). Pop counter is not advanced (DEC-015).
     - On pop returning `{nullptr, 0}`: shared stack empty — fall through to fresh
       allocation. (Counter bump already done inside the hook.)
   - **Eager-free walk (DEC-036)** (runs after refill, before retrying fast path): while
     `m.depth > 1` and `m.head`'s bookkeeper reports Empty, read `next = m.head->chainNext`
     into a local, call `VMSubstrate::freePage(m.head)`, set `m.head = next`, decrement
     `m.depth`, and **call `VMSubstrate::ensureTLBEntryFresh(m.head)` if `m.depth > 0`**
     (lazy first-touch per DEC-040 amended — the new `m.head` is a previously-untouched
     chain element on this CPU). Bounded by `Kmax - 1` iterations. After this, either
     `m.head` is non-Empty or `m.depth == 1` (single cached slab preserved as the floor).
     The first iteration's `m.head` is already fresh from the refill pop hook; subsequent
     iterations need the explicit call.
   - **Fresh allocation (slow path):** `VMSubstrate::allocPage()` (panic on failure per DEC-012),
     construct a new `SlabDescriptor` in-place at offset 0 with `desc->numaDomain = numaOf(i)`
     (DEC-018), `desc->chainNext = nullptr`, `desc->chainDepth = 1`; seed its bookkeeper with
     reserved slots covering the descriptor's storage and any trailing unused space (DEC-011);
     set `m.head = desc; m.depth = 1`. Retry the loop. (No `ensureTLBEntryFresh` needed on a
     freshly `allocPage`'d page for the calling CPU — `allocPage` clears the calling CPU's
     own dirty bit and installs the PTE; any prior stale entry was cleared at the previous
     `freePage`.)

**Freer path on any CPU (called as `vmsfree(p)`):** ordering pinned by DEC-026 and DEC-029.
1. Assert `p != nullptr` (DEC-023) and "not in IRQ/NMI/#GP/#MC context" (DEC-014 amended, debug builds; #PF is permitted).
2. Assert `vmsBase ≤ p < vmsBase + vmsSize` (DEC-026) — rejects stack, kernel-image, and other
   non-VMSubstrate VAs before any TLB-freshness machinery touches them.
3. **If `(p & (pageSize - 1)) == 0`** (DEC-029): `VMSubstrate::freePage(p)`. Done — no
   `ensureTLBEntryFresh`, no descriptor read, no Treiber-stack interaction. `freePage` is
   responsible for asserting on free-of-non-allocated-page.
4. `VMSubstrate::ensureTLBEntryFresh(p)` (DEC-016) — must precede any descriptor read so that a
   first-touch on this CPU does not see a stale or unmapped TLB entry.
5. `desc = (SlabDescriptor*)(p & ~(pageSize - 1))`.
6. Validate: `desc->magic`, `p` within slab data range, `(p - slotZeroAddr(desc)) % desc->slotSize == 0`.
   Assert on failure (DEC-013).
7. **Debug builds only:** poison the slot — `memset(p, 0xCC, desc->slotSize)` (DEC-024). Skipped
   in release builds. Runs *before* the bookkeeper free so the poison touches memory that the
   bookkeeper still considers allocated, which is the correct ordering for stress tests that
   want to detect both UAF and poison-then-realloc races.
8. `desc->bookkeeper.freeSlot(slotIndex, transition)`. Double-free assert fires inside the
   bookkeeper (DEC-013).
9. **If `transition.becameAvailable()` (Full → Partial), publish under the magazine layer
   (DEC-034/037):**
   - **Cross-domain gate (DEC-019/DEC-034, routing settled 2026-05-27 per Phase 6 P6-DEC-002):**
     if `desc->numaDomain != numaOf(currentCPU())`, call
     `partial[desc->numaDomain][desc->sizeClass].push(*desc)` — Phase-4's `push(element)`.
     The push **may extend the home-domain stack's existing top chain** if its depth is below
     `maxChainLength`, or start a new singleton chain otherwise; the freer doesn't choose. This
     produces fewer-but-longer chains on the shared stack than the earlier-draft "always
     publish singleton" formulation (which would have called `pushChain(desc, 1)` instead).
     The freer never touches its own magazine. The Phase-4 hooks (P4-DEC-010) bound to
     `tuningFor(desc->numaDomain)[c]` fire `onPreTouch` (ensureTLBEntryFresh on the existing
     top chain head, if any) and `onCasFailure` (bumps `overflowCount` on retries). Done.
   - **Same-domain case:** let `i = currentCPU()`, `c = desc->sizeClass`. Extend the magazine
     at the head: `desc->chainNext = m.head; m.head = desc; m.depth++`. **No atomics.**
   - **Flush check:** read `K = tuning[numaOf(i)][c].currentK.load(RELAXED)`. If `m.depth >= K`,
     **flush the magazine**: set `m.head->chainDepth = m.depth`; loop
     `{ oldHead = partial[numaOf(i)][c].load(); m.head->next = oldHead.descPtr;
     newTag = (oldHead.tag + 1) & kCounterMask;
     CAS(partial[numaOf(i)][c], oldHead, encode(m.head, newTag)) }` (push-only counter advance
     with explicit 37-bit mask per DEC-015; head-linkage per DEC-041); each CAS retry bumps
     `tuning[numaOf(i)][c].overflowCount` (RELAXED add_fetch). On success, clear the magazine:
     `m.head = nullptr; m.depth = 0`. The flush pushes the entire current chain — including
     the slab that was the active allocation target — to the shared stack. The next allocator
     call on this CPU refills from the shared stack.

**Why no allocHolder.** Multi-CPU concurrent allocation from the same slab is impossible by
construction. Pop-from-shared-stack transfers exclusive ownership of the chain (including the
head slab) to the popping CPU's magazine. The owning CPU is the only caller of `allocSlot` on
any slab in its magazine (DEC-014/030 prevent concurrent calls on the same CPU; no other CPU
can reach this magazine). When a slab transitions Full inside `allocSlot`, DEC-037's
pop-on-`becameFull` removes it from the magazine before any freer can republish it. So the
bookkeeper's owner-only single-claimer assumption holds without an atomic stamp — the dual-
ownership race that allocHolder solves for PageAllocator's eager-free path simply doesn't
arise here (vmsmalloc never eager-frees in-use slabs; DEC-036 only freePages Empty slabs that
are inbox-private by construction).

**Cross-CPU race surfaces.**
- *Owner allocating while remote CPU frees:* covered by `SlabBookkeeper`'s split alloc/free
  bitmap halves — already validated under stress in the SmallPageAllocator migration.
- *Owner becameFull race with remote freer's becameAvailable (DEC-037/DEC-039):* after the
  owner's `allocSlot` atomic drives a slab to Full, a remote freer's `freeSlot` atomic may
  drive it Partial→back-to-Partial-after-Full (i.e., become Partial again) and observe
  `becameAvailable`. The freer then publishes the slab.
    - *Cross-domain freer case (ITEM-045):* the freer's domain ≠ `desc->numaDomain`, so the
      DEC-019/034 gate forces a singleton push directly onto the home Treiber stack — the
      freer never extends any magazine, but its singleton-push prep **does write
      `desc->chainNext = nullptr`** (along with `desc->chainDepth = 1` and `desc->next =
      oldHead.descPtr`). This is the same class of mutation as the same-domain case below
      and is handled by the same DEC-039 pre-read: the owner captured `head->chainNext`
      before `allocSlot`, so the cross-domain freer's later overwrite to `nullptr` does
      not corrupt the owner's pop target. End state: the slab is on the shared stack (from
      the freer's publish) and no longer in the owner's magazine (from the owner's pop).
      No dual-ownership, no chain corruption.
    - *Same-domain freer case (ITEM-041):* the freer's domain == `desc->numaDomain`, so the
      freer pushes onto **its own** magazine via `desc->chainNext = m_B.head; m_B.head = desc`.
      This **mutates the just-Full slab's `chainNext` field** between the owner's `allocSlot`
      and the owner's pop. A naive pop reading `m.head->chainNext` post-`allocSlot` reads the
      freer's overwritten pointer (`m_B`'s previous head) instead of the owner's expected
      successor, splicing the freer's chain elements into the owner's chain and orphaning the
      owner's real successor. **DEC-039 closes this** by mandating that the owner pre-read
      `m.head->chainNext` into a local before calling `allocSlot`; the pre-read is
      unconditionally ordered-before any freer mutation because the slab must first transition
      Full (via the owner's own `allocSlot`) before any same-domain freer can fire
      `becameAvailable` and touch `chainNext`. With DEC-039, end state: slab is in the
      freer's magazine (from the freer's push), and the owner's chain advances to its
      correctly-pre-read successor. No dual-ownership, no chain corruption.
- *Magazine flush concurrent with another CPU's flush (DEC-034):* both CASes target
  `partial[d][c]`; whichever loses bumps `overflowCount` and retries. Chain integrity is preserved
  because each CPU's magazine is private — neither CPU touches the other's chain.
- *Magazine flush concurrent with a remote CPU's refill:* refill pops a chain head; the new
  shared-stack head exposes a different chain. The flushing CPU's CAS against the *old* head
  fails and is retried against the new head; the flushed chain ends up below the refilled-CPU's
  consumed chain. No data is lost.
- *Pop concurrent with freer making a chain element Empty:* the freer that drives a slab Full→Partial
  is the only freer who can push it; subsequent frees on the same slab (now in someone's magazine
  chain) do not generate publish events. When the chain element transitions to Empty during freer
  activity *after* it has been popped into someone's magazine, it stays in the magazine — lazy
  reclamation per DEC-002, plus opportunistic eager-free at the head per DEC-036.
- *ABA on the Treiber head (DEC-015):* the chain-head pointer is what the CAS targets; the
  64-bit tagged head's counter advances on every successful push (including flushes), so a stale
  head value cannot succeed even when the same chain head reappears at the top.
- *Cross-domain free (DEC-019/DEC-034 gate):* a freer whose domain ≠ `desc->numaDomain` pushes
  a singleton chain to the home domain's `partial[]` directly, never visiting any magazine. This
  preserves the "magazines contain same-domain slabs only" invariant without ping-pong.

<!-- Performance Envelope can be added when latency targets are known. -->