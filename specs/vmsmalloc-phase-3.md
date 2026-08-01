---
kind: leaf
status: ready
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 3 — VMSubstrate modifications for vmsmalloc metadata storage

> Modify VMSubstrate enough to give vmsmalloc the storage environment it needs without
> exposing low-level page-table primitives. Two interlocking changes: (a) reserve the
> topmost arena-equivalent VA slot for a "per-domain static-buffer region" and add a
> `reservePerDomainStaticBuffer(byteSize, DomainID)` primitive that places NUMA-distributed
> pinned buffers there; (b) extend the existing arena layout with one structurally-reserved
> "vmsmalloc local cache" page per arena, allocated on the arena owner's NUMA domain,
> exposed via `localCachePageFor(ProcessorID)`. Extend `VMSubstrateSlab.h` with the storage
> types (`TreiberHead`, `Magazine`, `MagazineTuning`), the `constexpr` size constants both
> VMSubstrate and vmsmalloc consume, and the accessor helpers
> (`partialFor` / `tuningFor`). The per-CPU `Magazine[kNumSizeClasses]` array is folded
> into the `kernel::CpuLocal` struct (Phase 7 / P7-DEC-010 amendment 2026-05-27) — access
> is via `kernel::cpuLocal().magazines[c]`, not a separate `magazineFor(i)` accessor.
> Implement `vmsmallocInit()` to populate the per-domain buffers; the per-CPU CpuLocal
> pages are zeroed and have their `logicalID` set during VMSubstrate's own init (Phase 7
> step 4). All address arithmetic flows
> through VMSubstrate's existing `arch::pageTableDescriptor`-driven arena math, so the
> implementation is architecture-portable by construction.

## Amendment banner (2026-05-27 — P3-DEC-004 + P7-DEC-010)

**Naming substitutions throughout this spec:**

| Pre-amendment name | Canonical name (post-2026-05-27) |
|---|---|
| `kVmsmallocLocalCacheBytes` / `kPerCpuCacheBytes` | `kCpuLocalBytes` (declared in `kernel/include/cpu_local.h`, not `VMSubstrateSlab.h`) |
| `kVmsmallocLocalCachePages` | `kCpuLocalPages` |
| `localCachePageFor(i)` | `cpuLocalPageFor(i)` |
| `magazineFor(i)` (separate accessor) | `kernel::cpuLocal().magazines[c]` (field of `kernel::CpuLocal`) |
| "vmsmalloc local cache page" / "per-CPU cache page" | "CpuLocal page" |

The per-CPU `Magazine[kNumSizeClasses]` array is a **field of `kernel::CpuLocal`** (introduced by Phase 7), not a standalone allocation. `kernel::CpuLocal` includes `{ logicalID; InterruptContextDepths; Magazine magazines[kNumSizeClasses]; }` and lives in the per-CPU arena metadata page. Some implementation-step prose below still uses pre-amendment names; mentally substitute per the table.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No Treiber stack operations** — Phase 4 (`Core::TreiberStack` / `Core::ChainedTreiberStack`)
  defines those. Phase 3 declares `TreiberHead` as storage only.
- **No magazine state-machine** — Phase 5 implements push / pop / flush. Phase 3 declares
  `Magazine` as storage only.
- **No `vmsmalloc` / `vmsfree` entry points** — Phase 5 and Phase 6 build these.
- **No general-purpose PTE-install primitive** — the previous Phase-3 draft proposed
  `installKernelPTE`; it has been replaced by the two narrow, higher-level primitives
  above. `installKernelPTE` will not be added.
- **No `MagazineTuning::policyLock` logic** — the lock storage is initialized to zero;
  Phase 10 (`magazine tuning policy`) implements try-lock and the read-reset-decide-write
  cycle.
- **No teardown** — `vmsmallocInit` is one-shot; the static-buffer region and arena cache
  pages are pinned for the kernel's lifetime.
- **No CPU / memory hotplug support** (parent-spec non-goal). Both the per-domain buffer
  set and the per-CPU cache pages are sized once at init.

## Consumer Contract

### `VMSubstrate::reservePerDomainStaticBuffer(size_t byteSize, kernel::mm::DomainID d) -> void*`

- Declared in `kernel/include/mem/VMSubstrate.h`. Implemented in `kernel/mm/VMSubstrate.cpp`.
- **Allocates `divideAndRoundUp(byteSize, arch::smallPageSize)` physical pages** on
  NUMA domain `d` via `PageAllocator::allocateSmallPage(cpuOnDomain(d))` where `cpuOnDomain(d)`
  is the lowest logical CPU ID with `NUMAPolicy::domainFor(i) == d` (deterministic across
  boots; per ITEM-051 the PageAllocator pool is valid for AP CPU IDs before SMP bringup).
- **Maps the pages contiguously** at a VA chosen by VMSubstrate from the static-buffer
  region (the topmost arena-equivalent VA slot reserved during VMSubstrate's own `init`,
  see Arena Enumeration Change below). The returned VA is page-aligned and lies entirely
  within the static-buffer region.
- **Zero-fills** the allocated pages before returning (callers can rely on
  newly-returned buffers being all zeros).
- **Returns** the buffer's base VA. Subsequent calls return non-overlapping VAs within
  the static-buffer region; VMSubstrate maintains an internal bump pointer.
- **Single-threaded init only.** Asserts pre-SMP context in debug builds. Panics if
  PageAllocator fails or if the static-buffer region is exhausted.

### `VMSubstrate::cpuLocalPageFor(arch::ProcessorID i) -> void*`

(Renamed from the earlier `localCachePageFor` per the P7-DEC-010 unification 2026-05-27.)

- Declared in `kernel/include/mem/VMSubstrate.h`. Pure address arithmetic — no allocation
  side effect.
- **Returns the base VA** of CPU `i`'s `kernel::CpuLocal` page, a structurally-reserved
  `kCpuLocalPages`-sized region inside CPU `i`'s first arena. The region sits between
  the existing occupancy buffer and the existing allocatable region.
- **The page(s) are allocated and mapped** by VMSubstrate's `createArena(i)` on
  `NUMAPolicy::domainFor(i)`, before this function is ever called.
- Safe to call from any context after `VMSubstrate::init` returns. Used both at init
  time (BSP writes each AP's CpuLocal initial state through this accessor; BSP also
  points its own GSBase at `cpuLocalPageFor(0)`) and at runtime by VMSubstrate-internal
  diagnostic code if needed. Per DEC-014/030 only the owning CPU `i` should write to
  its own CpuLocal at runtime; VMSubstrate exposes the accessor unguarded — the
  use-discipline lives in the consumers.

### Arena Layout Modification

The existing arena layout (in `VMSubstrate.cpp`) is:
```
arenaBase
  | self-reference page tables       (kSelfRefSize)
  | occupancy buffer                  (kOccupancyBufferSize)
  | --- allocatableBase ---
  | allocatable region                (kArenaPageCount * smallPageSize)
arenaEnd
```

Phase 3 inserts the vmsmalloc local cache page(s):
```
arenaBase
  | self-reference page tables       (kSelfRefSize)
  | occupancy buffer                  (kOccupancyBufferSize)
  | CpuLocal page                     (kCpuLocalBytes = kCpuLocalPages * smallPageSize; hosts kernel::CpuLocal incl. vmsmalloc magazines)
  | --- allocatableBase ---
  | allocatable region                (kArenaPageCount' * smallPageSize)  [shrunk by kVmsmallocLocalCachePages]
arenaEnd
```

- `kCpuLocalBytes` is declared in `kernel/include/cpu_local.h` (introduced by Phase 7)
  as `constexpr size_t kCpuLocalBytes = divideAndRoundUp(sizeof(kernel::CpuLocal), arch::smallPageSize) * arch::smallPageSize;`
  — VMSubstrate.cpp `#include`s `cpu_local.h` to consume the constant. (The earlier
  draft had this constant live in `VMSubstrateSlab.h` under the name
  `kVmsmallocLocalCacheBytes`; per the P7-DEC-010 amendment 2026-05-27 the constant
  moves to `cpu_local.h` since the page now hosts the kernel-wide CpuLocal struct, not
  just the vmsmalloc magazines.)
- `kArenaPageCount` shrinks by `kCpuLocalPages`. The existing
  `static_assert(kArenaPageCount % kBranchFactor == 0)` continues to hold after the shrink —
  VMSubstrate's arena page count was already chosen with bitmap-radix branching constraints
  in mind; the cache-page reservation must preserve this. If it doesn't, the static_assert
  fires at compile time and the cache page count is bumped to the next multiple. **Action
  item during implementation:** verify the multiple-of-`kBranchFactor` constraint holds for
  the DEC-003-induced cache page count; if not, round up (cheap — one page max).
- `createArena(i)` is modified to (1) compute the cache page VA(s) via the new layout,
  (2) allocate the physical page(s) via `PageAllocator::allocateSmallPage(i)` (the existing
  `allocateSmallPage` already places on `domainFor(i)` via `nearestPool`), (3) install the
  leaf PTE(s) via the same self-ref arena-style math the arena already uses for its
  occupancy buffer pages (`ensureLeafBitmapPageMapped`-style; the cache pages are
  structurally similar — pinned, init-time-only). The cache pages are not entered into the
  leaf alloc/free bitmaps (they sit outside the allocatable region).

### Arena Enumeration Change (Static-Buffer Region Reservation)

- `VMSubstrate::init` reserves one arena-equivalent VA slot — the topmost arena slot in the
  existing enumeration — for the per-domain static-buffer region. This slot does **not**
  receive a `createArena` call; instead, it gets a thinner setup: a self-reference root
  page table installed (so leaf-PTE installation through `reservePerDomainStaticBuffer`
  can use the same arena-style math), and bump-pointer state for tracking the next free VA
  within the slot. No occupancy buffer, no per-CPU dirty tracking, no allocatable region.
- VMSubstrate's existing `for (i = 0; i < processorCount(); i++) createArena(i)` loop is
  modified so the index space avoids the reserved slot. Concretely: the static-buffer slot
  occupies index 0 (or whatever index sits at the top of the existing arena enumeration);
  CPU arenas occupy indices 1..processorCount(). The exact slot index is an internal
  VMSubstrate detail; vmsmalloc sees only the API.
- Asserts at boot that `processorCount() + 1 <= VMSubstrate_max_arenas` (the existing arena
  count limit minus one for the static-buffer slot). Panics on collision.

### `vmsmallocInit() -> bool`

- Declared in `kernel/mm/VMSubstrateSlab.h`. Implemented in
  `kernel/mm/VMSubstrateSlab.cpp` (new file).
- ICD registration in `kernel/general.icd`:
  ```
  [VMSubstrateSlab]
  phase = "memory_management"
  required = true
  per_cpu = false
  depends_on = ["VMSubstrate"]
  routine = "kernel::mm::vmsmalloc::vmsmallocInit"
  ```
- Algorithm:
  1. Derive CPU-bearing-domain set `D = { NUMAPolicy::domainFor(i) : i ∈ [0, processorCount()) }`.
  2. For each `d ∈ D`:
     `void* buf = VMSubstrate::reservePerDomainStaticBuffer(kPerDomainBufBytes, d);`
     `perDomainBufs[d] = buf;`
     Walk the `MagazineTuning` entries in the buffer and set `currentK = kInitialK` on each.
     (Buffer is already zero-filled by `reservePerDomainStaticBuffer`; `TreiberHead.head`
     stays zero, which decodes to the "empty stack" tagged head per DEC-015.)
  3. **(Folded into VMSubstrate's arena-creation loop per Phase 7 step 4 — no longer
     part of `vmsmallocInit`.)** Zeroing the per-CPU `kernel::CpuLocal` page and
     writing `logicalID = i` happens during `createArena(i)`. `vmsmallocInit` reads
     `cpuLocal().magazines` after the fact; the magazines start zero (head = nullptr,
     depth = 0) by the page-zero step in `createArena`.
  4. Capture `VMSubstrate`'s `vmsBase` and `vmsSize` constants for `vmsfree`'s
     DEC-026 range check. (Provided by VMSubstrate as `getKernelMemRegionStart(slot)` style
     accessors against the topmost arena slot; specifics during implementation.)
  5. `klog::info("VMSubstrateSlab init: perDomainBufs=N, perCpuCaches=M");`
  6. Return `true`.

### `VMSubstrateSlab.h` extensions

Phase 3 adds to the header established in Phase 2:

```cpp
namespace kernel::mm::vmsmalloc {

    // ─── Storage types (per-CPU-only structures get cache-line alignment) ───

    struct alignas(arch::cacheLineSize) TreiberHead {
        uint64_t head;  // tagged-head encoding per DEC-015; consumed by Phase 4 Core::ChainedTreiberStack
    };

    struct alignas(arch::cacheLineSize) Magazine {
        SlabDescriptorBase* head;
        uint32_t depth;
        // (no m.tail per DEC-041)
    };

    struct alignas(arch::cacheLineSize) MagazineTuning {
        Atomic<uint32_t> currentK;
        Atomic<uint32_t> overflowCount;
        Atomic<uint32_t> starvationCount;
        Atomic<uint32_t> policyLock;
    };

    // ─── Tuning bounds (provisional per P3-DEC-002; revisit in Phase 10) ───

    inline constexpr uint32_t kInitialK = 8;
    inline constexpr uint32_t kMinK     = 2;
    inline constexpr uint32_t kMaxK     = 64;
    static_assert(kMinK <= kInitialK && kInitialK <= kMaxK);

    // ─── Per-domain buffer layout ───

    inline constexpr size_t kPartialOffset = 0;
    inline constexpr size_t kPartialBytes  = kNumSizeClasses * sizeof(TreiberHead);
    inline constexpr size_t kTuningOffset  = roundUpToNearestMultiple(
        kPartialOffset + kPartialBytes, alignof(MagazineTuning));
    inline constexpr size_t kTuningBytes   = kNumSizeClasses * sizeof(MagazineTuning);
    inline constexpr size_t kPerDomainBufBytes = roundUpToNearestMultiple(
        kTuningOffset + kTuningBytes, arch::smallPageSize);

    // ─── Per-CPU CpuLocal layout (P7-DEC-010 amendment 2026-05-27) ───
    //
    // The CpuLocal-page size constants live in `kernel/include/cpu_local.h` (introduced
    // by Phase 7) — they're consumed by VMSubstrate.cpp's arena layout math. The
    // Magazine[kNumSizeClasses] array is a field of `kernel::CpuLocal`, not a
    // standalone allocation; access at runtime is `kernel::cpuLocal().magazines[c]`.
    // No `kPerCpuCacheBytes` / `kVmsmallocLocalCachePages` constants are defined here
    // anymore.

    // ─── Module-local storage (defined in VMSubstrateSlab.cpp) ───

    extern void* perDomainBufs[];  // sized to maxDomainID + 1 at definition site;
                                   // null slots indicate non-CPU-bearing domains

    // ─── Accessors ───

    inline TreiberHead* partialFor(DomainID d) {
        assert(perDomainBufs[d] != nullptr, "partialFor: domain has no buffer");
        return reinterpret_cast<TreiberHead*>(
            static_cast<uint8_t*>(perDomainBufs[d]) + kPartialOffset);
    }

    inline MagazineTuning* tuningFor(DomainID d) {
        assert(perDomainBufs[d] != nullptr, "tuningFor: domain has no buffer");
        return reinterpret_cast<MagazineTuning*>(
            static_cast<uint8_t*>(perDomainBufs[d]) + kTuningOffset);
    }

    // No `magazineFor(i)` accessor — magazines live in kernel::CpuLocal per P7-DEC-010.
    // Same-CPU access: `kernel::cpuLocal().magazines[c]`.
    // Cross-CPU access (init only, from BSP): write directly to
    //   `static_cast<kernel::CpuLocal*>(VMSubstrate::cpuLocalPageFor(i))->magazines[c]`.

    // ─── Init entry point ───

    bool vmsmallocInit();

}  // namespace kernel::mm::vmsmalloc
```

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 2 (`vmsmalloc-phase-2.md`) | `VMSubstrateSlab.h` exists with `kNumSizeClasses`, `kSlabSizeClasses`, `SlabDescriptorBase`, the magic constant. Phase 3 extends the same header. | Yes — Phase 2 must be in. |
| `kernel/mm/VMSubstrate.cpp` (existing arena machinery) | Phase 3 modifies the arena enumeration (reserve one slot) and the arena layout (add the cache page reservation). The existing self-ref page-table math, `kSelfRefSize`, `kOccupancyBufferSize`, `kBranchFactor`, and `kArenaPageCount` constants are read/modified; the existing `ensureLeafBitmapPageMapped`-style PTE install pattern is the model for cache-page mapping. | Yes — live. |
| `kernel/include/kmemlayout.h` | Provides `VMM_SUBSTRATE_ROOT_INDEX`, `getKernelMemRegionStart`, `getKernelMemRegionSize`, `bootPageTable`, `early_boot_phys_to_virt`. | Yes — live. |
| `kernel/include/mem/PageAllocator.h` | `PageAllocator::allocateSmallPage(cpu)` for static-buffer pages and cache pages. Per ITEM-051, callable for AP CPU IDs before SMP bringup. | Yes — live. |
| `kernel/include/mem/NUMA.h` | `NUMAPolicy::domainFor(cpu)` and the iteration support to derive set `D`. | Yes — live. |
| `kernel/include/arch/amd64/amd64.h` | `arch::cacheLineSize`, `arch::smallPageSize`, `arch::processorCount`, `arch::pageTableDescriptor`, `arch::invlpg`, `arch::ProcessorID`, `arch::PTE<level>`, `arch::PageTable<level>`. | Yes — live. |
| `kernel/include/mem/MemTypes.h` | `virt_addr`, `phys_addr`, `DomainID`. | Yes — live. |
| `libraries/Core/include/core/math.h` | `roundUpToNearestMultiple`, `divideAndRoundUp`. | Yes — live. |
| `kernel/general.icd` and `tools/gen_init_registry.py` | Init-registry source and processor. | Yes — live. |

## Invariants

- After VMSubstrate's `init` (which now includes the static-buffer slot setup) and before
  any vmsmalloc call:
  - The topmost arena-equivalent VA slot is reserved as the static-buffer region; its
    self-reference root page table is installed; no leaf PTEs are populated yet.
  - Every CPU's first arena has its layout shifted to include the
    `kCpuLocalBytes` reservation between the occupancy buffer and the
    allocatable region; the cache page(s) are mapped to physical memory on the arena
    owner's NUMA domain.
- After `vmsmallocInit` returns:
  - For every `d ∈ D` (CPU-bearing domain): `perDomainBufs[d]` is a non-null,
    page-aligned VA pointing into the static-buffer region; its first `kPerDomainBufBytes`
    bytes are zero-initialized except every `MagazineTuning::currentK` equals `kInitialK`.
  - For every `d ∉ D`: `perDomainBufs[d]` is `nullptr`.
  - For every `i ∈ [0, processorCount())`: the `kernel::CpuLocal` struct at
    `VMSubstrate::cpuLocalPageFor(i)` is zero-initialized (logicalID gets set to `i`
    during VMSubstrate's arena creation; interruptDepths and magazines remain zero per
    the page-zero step).
- `kPerDomainBufBytes % arch::smallPageSize == 0` (static_assert).
- `kCpuLocalBytes % arch::smallPageSize == 0` (static_assert in `cpu_local.h`).
- `kCpuLocalPages >= 1` (static_assert in `cpu_local.h`; the struct must produce at
  least one page reservation).
- The new `kArenaPageCount` value (after subtracting cache pages) is still a multiple of
  `kBranchFactor` (existing VMSubstrate invariant; static_assert).
- Cache pages are NOT entered in the arena's leaf alloc/free bitmaps; calling `allocPage` /
  `freePage` on a cache-page VA would be a bug (the existing assertions would fire on the
  bitmap-state mismatch).

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `PageAllocator::allocateSmallPage` fails during `createArena`'s cache-page allocation | Panic (DEC-012) | No |
| `PageAllocator::allocateSmallPage` fails during `reservePerDomainStaticBuffer` | Panic (DEC-012) | No |
| `reservePerDomainStaticBuffer` byteSize exceeds the static-buffer region's remaining VA | Panic — "static-buffer region exhausted". Indicates a sizing error in Phase 3 (the region's slot is sized to accommodate the realistic maximum). | No |
| Caller indexes `perDomainBufs[d]` with `d ∉ D` | The accessor's `assert(perDomainBufs[d] != nullptr)` fires. By DEC-018 / DEC-038 the indexing sites only produce CPU-bearing DomainIDs, so this fault indicates a real bug. | No |
| `processorCount() + 1 > VMSubstrate_max_arenas` (the static-buffer slot would overflow the arena enumeration space) | Boot-time assertion in `VMSubstrate::init` panics. CroCOS's max-processor count is 256 (per CLAUDE.md) and the VMSubstrate VA window holds far more arena slots than that, so this is a structural sanity check rather than a runtime concern. | No |
| `kArenaPageCount` (after cache-page shrinkage) is not a multiple of `kBranchFactor` | Compile-time `static_assert` in `VMSubstrate.cpp`; implementer must round up the cache page count to preserve the invariant. | No |
| `vmsmallocInit` called more than once | Asserted at entry via a static `bool kInitialized` flag; second call panics. | No |
| `vmsmallocInit` called from a non-BSP context | Asserted (pre-SMP context is required); panics. | No |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P3-ITEM-001 | Resolved 2026-05-27 (implementation-time choice) | | | Where exactly does the static-buffer slot sit in VMSubstrate's arena enumeration — index 0 or index `processorCount()`? | Resolved: pick whichever index makes the existing math read cleanly. Choice doesn't affect correctness; defer to implementation. |
| P3-ITEM-002 | Resolved 2026-05-27 (new internal accessor) | | | Does VMSubstrate's existing `arenaVirtualBase(index)` accessor need to be augmented, or is a new internal accessor enough? | Resolved: new internal accessor. `arenaVirtualBase` is in the public VMSubstrate.h; extending it has surface-area implications. The static-buffer-slot accessor is internal to VMSubstrate.cpp. |
| P3-ITEM-003 | Resolved 2026-05-27 (provisional → Phase 10) | | | Are `kInitialK = 8`, `kMinK = 2`, `kMaxK = 64` the right starting values? | Resolved as provisional per P3-DEC-002; Phase 10's magazine tuning policy revisits with RadixVM workload data. Constants live in `VMSubstrateSlab.h` and are trivially adjustable. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P3-DEC-001 | Settled | **The two new VMSubstrate APIs are declared in the public `VMSubstrate.h`** (alongside `allocPage`, `freePage`, `mapMMIOPage`), but both are documented as single-threaded init-only. Debug builds assert pre-SMP context at function entry. | Reusable for future init-time consumers that want NUMA-aware pinned buffers; demoting to a private header would force re-export later. The single-threaded contract is enforced by debug assertion, not by include-visibility. |
| P3-DEC-002 | Provisional | `kInitialK = 8`, `kMinK = 2`, `kMaxK = 64`. Pinned in `VMSubstrateSlab.h` with a `static_assert` chain. | `K = 8` amortizes shared-stack CAS over ~8 slabs while keeping the per-CPU cache size bounded (at full magazine, 8 slabs × pageSize = 32 KiB resident per (CPU, class)). `kMinK = 2` is the minimum useful depth (anything smaller defeats chain-transfer). `kMaxK = 64` keeps `chainDepth` within `uint32_t` headroom; in practice the policy will land far below it. All three revisitable in Phase 10. |
| P3-DEC-003 | Settled | **Static-buffer region implementation:** the topmost arena-equivalent VA slot. VMSubstrate's `init` installs a self-reference root page table for the slot (matching the existing arena-root pattern, so leaf-PTE installation goes through the same arena-style math), maintains a bump pointer for the next free VA within the slot, and exposes `reservePerDomainStaticBuffer` as the only way to add mappings. No occupancy buffer, no allocatable-region tracking, no `freePage` support. | Reuses VMSubstrate's existing page-table walking machinery without inheriting the runtime-arena overhead (dirty bitmap, leaf alloc/free bitmap, per-CPU concurrency). Self-ref root keeps the leaf-PTE-install math identical to the arena case, which keeps the implementation architecture-portable through `arch::pageTableDescriptor`. The bump pointer is simple and correct for init-only single-threaded use. |
| P3-DEC-004 | Settled (amended 2026-05-27 — folded into CpuLocal per P7-DEC-010) | **Per-CPU CpuLocal page placement:** between the occupancy buffer and the allocatable region. The arena's per-CPU page hosts a `kernel::CpuLocal` struct (defined in `kernel/include/cpu_local.h`, introduced by Phase 7) — **the vmsmalloc magazine array is folded into this struct as `cpuLocal().magazines[c]` rather than living in its own dedicated page**. The page is sized via `kCpuLocalPages` (replacing the earlier `kVmsmallocLocalCachePages`), declared in `cpu_local.h` and consumed by `VMSubstrate.cpp`'s arena-layout math. One page is sufficient on consumer hardware (CpuLocal size envelope: 1 + 64 + 8 × 64 ≈ 640 B with cache-line padding). | The user's original framing was "just map in an extra page after this occupancy buffer to keep the CPU's local cache" — which is still what Phase 3 does. The amendment generalizes the page's role from "vmsmalloc local cache" to "CpuLocal (kernel-wide per-CPU state including the vmsmalloc local cache)". Including the magazines in `CpuLocal` rather than in a parallel structure unifies "per-CPU kernel state" under one accessor (`cpuLocal()`), removes the separate `magazineFor(i)` API surface, and gives the future scheduler (which wants `preempt_count`, `current_thread`, etc. in per-CPU storage) a natural place to add fields. Phase 7's P7-DEC-010 records the unified design. |
| P3-DEC-005 | Settled | **Static-buffer pages zero-fill is `reservePerDomainStaticBuffer`'s responsibility.** vmsmalloc relies on receiving zero-initialized buffers. | Pushes the zero-fill to the place that has the just-allocated physical page in hand (avoiding a redundant zero in vmsmallocInit). `MagazineTuning::currentK = kInitialK` is the only non-zero seeding vmsmallocInit does; everything else relies on the zero-fill guarantee. |
| P3-DEC-006 | Settled | **Per-CPU cache pages are NOT zero-filled by VMSubstrate.** vmsmallocInit explicitly zeroes them after the fact. | Per the user's clarification, the arena's cache page is just "mapped in" — VMSubstrate doesn't know it's vmsmalloc-owned beyond reserving the space and placing it on the right NUMA domain. Zeroing it would impose vmsmalloc's semantics on a structurally-anonymous reservation. vmsmallocInit's explicit zero-pass is cheap (one `memset` per CPU). |
| P3-DEC-007 | Settled | **DomainID iteration uses the lowest-CPU-ID-on-domain rule.** When `reservePerDomainStaticBuffer` is called for domain `d`, the underlying `allocateSmallPage` uses `cpuOnDomain(d) = min { i : NUMAPolicy::domainFor(i) == d }`. Deterministic across boots. | Reproducible init output simplifies tests and debugging. No performance consequence (PageAllocator's `nearestPool` cares about NUMA domain, not CPU identity). |

## Hazards

- **Arena enumeration shift breaks existing arena-index consumers.** Any kernel code that
  reads `arenaVirtualBase(0)` (or similar fixed-index access) and expects it to be CPU 0's
  arena will silently get the static-buffer region instead after Phase 3. Defense: audit
  every existing call site to `arenaVirtualBase` / `createArena` / arena-index lookups
  before merging. The current callers I see are `VMSubstrate::init` itself (the
  enumeration loop) — straightforward to fix.
- **`kArenaPageCount` shrinkage violates the multiple-of-`kBranchFactor` invariant.** The
  existing `static_assert(kArenaPageCount % kBranchFactor == 0)` at `VMSubstrate.cpp:171`
  must continue to pass after subtracting cache pages. Defense: compute the cache page
  count and immediately verify the new `kArenaPageCount` satisfies the assert at compile
  time; if not, round the cache page reservation up to the next multiple of
  `kBranchFactor`. Cost: at most `kBranchFactor - 1` extra pages per arena (a few KiB at
  most).
- **Cache page mapped but never used.** Phase 3 reserves and maps the cache page; Phase 5
  is the first phase to actually read/write it. Between Phase 3 and Phase 5 implementation
  the cache page sits idle. Mitigation: nothing needed — it's just memory; the
  `vmsmallocInit` zero-pass writes it, so a smoke-test that boots the kernel can verify
  the mapping is at least live (the write doesn't fault).
- **Header-include direction `VMSubstrate.cpp → VMSubstrateSlab.h`.** Slightly unusual:
  VMSubstrate is the lower layer, but it imports a constant from a higher-layer header.
  Acceptable because (a) the dependency is purely on a `constexpr` size, not on
  vmsmalloc's runtime functions; (b) the alternative — duplicating the size constant in
  VMSubstrate.cpp — risks drift if the schema ever changes. A `static_assert` at the
  consumption site that the constant matches `kPerCpuCacheBytes` adds a safety belt.
- **Static-buffer region sized for the topmost arena slot — but the buffer set may exceed
  that.** If the kernel boots on a NUMA system with many domains (say, 256 CPU-bearing
  domains), the total per-domain buffer bytes = `256 × kPerDomainBufBytes` ≈ 256 pages on
  consumer-style schemas. The topmost arena slot's VA capacity is one arena's worth —
  typically 1 GiB on AMD64 — which is dramatically larger. **Probably safe** on all
  realistic systems, but worth an explicit assertion in `reservePerDomainStaticBuffer`:
  bump-pointer-exceeds-slot panics.
- **Asymmetry between domain and CPU iteration in vmsmallocInit.** The init iterates
  domains via `D` (a derived set) but iterates CPUs via `[0, processorCount())`. A future
  refactor that introduces sparse CPU IDs (the same way DomainIDs are sparse) would break
  the CPU loop silently. CroCOS's existing arch model uses contiguous logical CPU IDs (per
  `arch::MAX_PROCESSOR_COUNT = 256` and the existing init loops in `VMSubstrate::init`), so
  the asymmetry is currently fine.
- **The cache page allocation happens during `createArena(i)` on the BSP.** The BSP calls
  `PageAllocator::allocateSmallPage(i)` for AP CPU IDs to pin the cache page on the AP's
  home domain — Phase 1's ITEM-051 contract pins this works. If that contract is ever
  weakened (e.g., a refactor that requires the target CPU to be running), Phase 3 breaks.
  Defense: `static_assert` or runtime check that ITEM-051 still holds, included in
  `VMSubstrateSlab.cpp`'s init.
- **Static-buffer region pages have no dirty-bitmap entries.** Mapped once and never
  re-mapped, so no concurrent-mapping-change risk (per the parent-spec hazard rewrite).
  If a future change introduces re-mapping in the static-buffer region, that hazard
  surfaces and an explicit shootdown mechanism becomes necessary. Per-CPU cache pages
  *do* have dirty-bitmap coverage (they sit inside regular arenas) and don't need
  special handling.

## Verification Targets

| Property | Method |
|---|---|
| `kPerDomainBufBytes`, `kPerCpuCacheBytes`, `kVmsmallocLocalCachePages` are all positive and page-aligned | `static_assert` in `VMSubstrateSlab.h` |
| Arena layout's `kArenaPageCount` (after cache-page shrinkage) is a multiple of `kBranchFactor` | `static_assert` in `VMSubstrate.cpp` |
| `processorCount() + 1 <= VMSubstrate_max_arenas` | Runtime assert in `VMSubstrate::init` |
| `vmsmallocInit` completes without panic on single-CPU single-domain QEMU boot | Boot-side smoke; klog line "VMSubstrateSlab init: perDomainBufs=1, perCpuCaches=N" |
| `vmsmallocInit` completes on `run_numa` (3-domain) and `run_numa_hmat` configurations; per-domain buffer count matches the CPU-bearing-domain count | Boot-side smoke + klog inspection |
| Each per-domain buffer's `MagazineTuning::currentK` equals `kInitialK` after init | Add a debug-only post-init scan that reads `tuningFor(d)` for every `d ∈ D` and asserts; behind `#ifdef CROCOS_VMSMALLOC_DEBUG_INIT` |
| Each per-CPU CpuLocal page is zero-initialized except for `logicalID` after init | Same debug scan, reading `static_cast<kernel::CpuLocal*>(cpuLocalPageFor(i))` for every CPU |
| Existing `naiveTest` continues to pass — arena allocation/free unaffected by the cache-page shrinkage and static-buffer-slot reservation | In-kernel boot + stress (QEMU) |
| `allocPage` / `freePage` regression: total allocatable pages match `processorCount() * kArenaPageCount` (the new, shrunk value) | Add a klog line at the end of `VMSubstrate::init` reporting the total |

## Testing Approach

- **Primary signal:** in-kernel boot in QEMU. Phase 3 is single-threaded init code; the
  parent-spec previously flagged this as not ideal for userspace mocking, and the user
  accepted that gap. Boot smoke covers the entire init path; klog lines verify per-domain
  buffer / per-CPU cache counts and `currentK` seeding.
- **Multi-NUMA boot configurations:** `make run`, `make run_numa`, `make run_numa_hmat`.
  Each should produce a klog line confirming the expected per-domain buffer count
  (1 / 3 / 3 respectively, assuming all NUMA nodes have CPUs).
- **Optional userspace component (low-effort):** the constexpr layout math in
  `VMSubstrateSlab.h` (`kPerDomainBufBytes`, `kTuningOffset`, `kVmsmallocLocalCachePages`)
  can be `static_assert`-verified in a Phase-2-style test file (extending
  `tests/kernel/vmsmalloc/SlabLayoutTest.cpp` with the Phase-3 constants). No runtime
  test logic needed. Add this if it falls out cheaply during implementation; skip
  otherwise.
- **No TSan variant.** Init is single-threaded; concurrent stress resumes in Phase 4.

## Implementation Phases

<!-- Concrete ordered steps for Phase 3 itself. -->

1. **Confirm starting state.**
   - Phase 2 merged; `VMSubstrateSlab.h` defines `SlabDescriptorBase`, magic constant,
     size-class schema.
   - Read `VMSubstrate.cpp` arena-init code; identify `kSelfRefSize`,
     `kOccupancyBufferSize`, `kArenaPageCount`, `kBranchFactor`, the `createArena` body,
     the `init()` enumeration loop, and the arena-VA accessor pattern. Sketch the
     modifications.
   - Identify the existing `arenaVirtualBase` accessor in `VMSubstrate.h` and any
     external consumers (likely just `VMSubstrate.cpp` itself).

2. **Extend `VMSubstrateSlab.h` with the Phase-3 type and constant definitions.**
   - Add `TreiberHead`, `Magazine`, `MagazineTuning` struct definitions (per the contract
     section above), each `alignas(arch::cacheLineSize)`.
   - Add `kInitialK`, `kMinK`, `kMaxK` constants with `static_assert(kMinK <= kInitialK && kInitialK <= kMaxK)`.
   - Add `kPartialOffset`, `kPartialBytes`, `kTuningOffset`, `kTuningBytes`,
     `kPerDomainBufBytes` `constexpr` constants (per the contract).
   - Add `kPerCpuCacheBytes`, `kVmsmallocLocalCachePages` `constexpr` constants.
   - Add `static_assert(kPerDomainBufBytes % arch::smallPageSize == 0)` and
     `static_assert(kPerCpuCacheBytes % arch::smallPageSize == 0)` and
     `static_assert(kVmsmallocLocalCachePages >= 1)`.
   - Declare `extern void* perDomainBufs[]` (definition in `VMSubstrateSlab.cpp`).
   - Add `inline` accessors `partialFor`, `tuningFor`, `magazineFor` (each calling the
     null-check assert).
   - Declare `bool vmsmallocInit();`.

3. **Modify `VMSubstrate.cpp`'s arena layout to reserve the cache page.**
   - `#include "VMSubstrateSlab.h"` at the top of `VMSubstrate.cpp` (or in
     `VMSubstrateHelper.h` if one exists; otherwise inline `#include` near the layout
     constants).
   - Introduce a new layout constant: `constexpr size_t kVmsmallocLocalCacheSize = kVmsmallocLocalCacheBytes;`
     placed between `kOccupancyBufferSize` and `allocatableBase`'s offset.
   - Adjust the `allocatableBase` accessor (line 189-191 area) to add
     `+ kVmsmallocLocalCacheSize` to its offset.
   - Shrink `kArenaPageCount` to `(arenaSize - kSelfRefSize - kOccupancyBufferSize - kVmsmallocLocalCacheSize) / arch::smallPageSize`,
     keeping the multiple-of-`kBranchFactor` invariant (round the cache reservation up if
     needed; assert).
   - Add a new accessor `inline kernel::mm::virt_addr localCachePageBase(virt_addr arenaBase) { return arenaBase + kSelfRefSize + kOccupancyBufferSize; }`.

4. **Modify `createArena(cpu)` to allocate and map the cache page(s).**
   - After the existing arena root-table setup but before the function returns: for each
     of `kVmsmallocLocalCachePages` pages, call
     `phys_addr p = PageAllocator::allocateSmallPage(cpu);`, then install the leaf PTE at
     the corresponding VA via the same arena self-ref math the rest of the arena uses
     (model on `ensureLeafBitmapPageMapped` at `VMSubstrate.cpp:362` but without the
     bitmap-content init — the cache page is left uninitialized; vmsmallocInit zeroes it
     later).
   - `arch::invlpg(cachePageVA)` after each install (calling-CPU is the BSP for every
     `createArena` call per the existing `VMSubstrate::init` loop).
   - `setDirtyForOtherCPUs(cachePageVA)` for symmetry with `allocPage` — even though
     remote CPUs won't touch the cache page (DEC-014/030 forbids cross-CPU magazine
     access), keeping the dirty-bitmap discipline consistent across all arena pages
     means `ensureTLBEntryFresh` on a cache-page VA continues to work without surprises.

5. **Add the static-buffer slot reservation to `VMSubstrate::init`.**
   - Before the `for (i = 0; i < processorCount(); i++) createArena(i)` loop: reserve
     the topmost arena-equivalent VA slot for the static-buffer region. Set up its
     self-reference root page table (matching the arena root pattern). Initialize a
     module-local bump pointer `staticBufferNextVA` to the slot's base.
   - Modify the arena-enumeration loop so it skips the reserved slot. Concretely: if the
     existing layout puts arena `i` at `arenaVirtualBase(i)`, shift to
     `arenaVirtualBase(i + 1)` (or whatever index avoids the reserved slot).
   - Add an assertion: `processorCount() + 1 <= maximumArenaSlots`.

6. **Implement `VMSubstrate::reservePerDomainStaticBuffer`.**
   - Declared in `VMSubstrate.h`. Body in `VMSubstrate.cpp`.
   - Compute `size_t pages = divideAndRoundUp(byteSize, arch::smallPageSize);`.
   - Get the lowest CPU on domain `d` for the placement intent
     (`NUMAPolicy::lowestCpuOnDomain(d)` — add this helper if it doesn't already exist;
     small change in `NUMA.h`/`NUMA.cpp`).
   - For each of `pages` pages: `phys_addr p = PageAllocator::allocateSmallPage(cpuOnDomain);`,
     install the leaf PTE at `staticBufferNextVA`, `arch::invlpg`, advance
     `staticBufferNextVA` by one page, zero-fill the just-mapped page.
   - Return the original `staticBufferNextVA` (before any of the advances above).
   - Assert `staticBufferNextVA + pages * smallPageSize <= staticBufferSlotEnd`.

7. **Implement `VMSubstrate::localCachePageFor`.**
   - Pure address arithmetic: return `localCachePageBase(arenaVirtualBase(processorCount()->arenaIndex(i)))`
     cast to `void*`. (Specifics depend on how arena indexing is shifted in step 5.)

8. **Create `kernel/mm/VMSubstrateSlab.cpp` with `vmsmallocInit`.**
   - Define `void* perDomainBufs[arch::MAX_NUMA_DOMAINS + 1] = {};` (or use the
     `NUMAPolicy::maxDomainID() + 1` runtime size if a runtime-sized array helper exists;
     otherwise a sufficient static cap).
   - `vmsmallocInit` body per the contract section: derive `D`, call
     `reservePerDomainStaticBuffer` for each `d ∈ D`, walk the `MagazineTuning` entries to
     seed `currentK`, zero each CPU's cache via `localCachePageFor`, set the
     `kInitialized` flag, log.

9. **Register `[VMSubstrateSlab]` in `kernel/general.icd`.**
   - Add the section with `phase = "memory_management"`, `required = true`,
     `per_cpu = false`, `depends_on = ["VMSubstrate"]`,
     `routine = "kernel::mm::vmsmalloc::vmsmallocInit"`.
   - Run `tools/gen_init_registry.py` (or rebuild) to confirm the entry is accepted.

10. **Build and smoke-test in QEMU.**
    - `cmake --build cmake-build-debug --target Kernel` succeeds.
    - `make run` boots; serial log shows "VMSubstrateSlab init: perDomainBufs=1, perCpuCaches=8".
    - `make run_numa` boots; log shows "perDomainBufs=3" (or whatever the QEMU NUMA topology
      produces).
    - `make run_numa_hmat` boots; log matches.
    - `naiveTest` continues to pass (no regression from arena layout shrinkage).

11. **Optional follow-up (under user latitude).**
    - Add a debug-only post-init scan (under `#ifdef CROCOS_VMSMALLOC_DEBUG_INIT`) that
      walks every per-domain buffer and per-CPU cache, asserting the expected initial
      state.
    - Extend `tests/kernel/vmsmalloc/SlabLayoutTest.cpp` with the Phase-3 `static_assert`s
      duplicated as runtime checks (for visibility in test output).
    - Add the `VMSubstrate.cpp`-side assertion that no future arena creation lands in the
      static-buffer slot index (defense against the "static-buffer-region arena-slot
      collision" hazard).

## References

- `kernel/mm/VMSubstrate.cpp` — existing arena machinery; Phase 3 modifies:
  - The arena layout constants (lines around `kSelfRefSize`, `kOccupancyBufferSize`,
    `kArenaPageCount`).
  - `createArena` (the per-CPU arena init).
  - `init` (the enumeration loop and the new static-buffer slot reservation).
  - `allocatableBase` accessor.
- `kernel/include/mem/VMSubstrate.h` — public API; Phase 3 adds
  `reservePerDomainStaticBuffer` and `localCachePageFor`.
- `kernel/include/kmemlayout.h` — `VMM_SUBSTRATE_ROOT_INDEX`, `getKernelMemRegionStart`,
  `early_boot_phys_to_virt`, `bootPageTable`.
- `kernel/include/mem/PageAllocator.h` — `allocateSmallPage(cpu)`; per ITEM-051 valid for
  AP CPU IDs during memory_management phase.
- `kernel/include/mem/NUMA.h` — `NUMAPolicy::domainFor`, `domainCount`, plus a possibly-new
  `lowestCpuOnDomain(d)` helper.
- `kernel/include/arch/amd64/amd64.h` — `arch::cacheLineSize`, `arch::smallPageSize`,
  `arch::pageTableDescriptor`, `arch::processorCount`, `arch::invlpg`,
  `arch::MAX_PROCESSOR_COUNT`, `arch::ProcessorID`.
- `libraries/Core/include/core/math.h` — `roundUpToNearestMultiple`, `divideAndRoundUp`.
- `kernel/general.icd` and `tools/gen_init_registry.py`.
- Parent spec `specs/vmsmalloc.md`:
  - DEC-021 (rewritten 2026-05-26) — `vmsmallocInit` algorithm.
  - DEC-033 (rewritten 2026-05-26) — per-domain-buffer + per-CPU-arena-cache split.
  - DEC-018 — slab `desc->numaDomain = NUMAPolicy::domainFor(currentCPU)` contract that
    keeps `partial[d]` indexing safe (domains never null).
  - DEC-038 (clarified by ITEM-039) — CPU-bearing-domain subset.
  - Hazard "Bad `vmsfree(p)` with `p` in the per-domain static-buffer region".
  - Hazard "Per-domain static-buffer TLB-staleness — non-issue by construction".
  - Hazard "Static-buffer-region arena-slot collision".
  - Hazard "Per-CPU magazine pages and NUMA locality (resolved by DEC-033 rewrite)".
- Phase 1 spec `specs/vmsmalloc-phase-1.md` — ITEM-051 (PageAllocator per-CPU pool
  availability for AP IDs at memory_management phase).
- Phase 2 spec `specs/vmsmalloc-phase-2.md` — `VMSubstrateSlab.h` location, namespace,
  Phase-2 type definitions Phase 3 extends.
