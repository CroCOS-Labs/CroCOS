---
kind: leaf
status: ready
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 2 — Slab descriptor layout, size-class schema, derived constants

> Create the vmsmalloc-internal header `kernel/mm/VMSubstrateSlab.h` (per DEC-028). Define
> `SlabDescriptorBase` (uniform prefix with the read-side fields vmsmalloc consumes), the
> `kSlabSizeClasses` constexpr array (mirror of `LibAlloc::InternalAllocator::slabSizeClasses`),
> the derived `slotCount[c]` / `slot0Offset[c]` constexpr tables (DEC-045 fixpoint), and pin
> `kSlabDescriptorMagic = 0x5DAB5DABDE5CC9C0ULL` per DEC-044. Compile-time validation only —
> no runtime code, no slab creation, no allocation paths.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- No runtime allocation/free paths — those start in Phase 4. Phase 2's output is types,
  constants, and `static_assert` validation.
- No metadata-region setup, magazine state, or Treiber stack — Phase 3 (VMSubstrate
  modifications + per-domain buffer / per-CPU cache allocation) and Phase 5 (fast path)
  cover those.
- No `vmsmalloc` / `vmsfree` declarations — DEC-028 keeps those implementation-internal; they
  appear when Phase 4 builds the entry points.
- No `VMSubstrate::make<T>` / `destroy<T>` either — they're public API, built once the
  underlying primitives exist (Phase 4+).
- No descriptor *instantiation* on real pages. Phase 2 defines the *types* and *constants*; a
  `SlabDescriptor<N>` instance is built in-place by the slab-creation slow path in Phase 4.

## Consumer Contract

- **New header `kernel/mm/VMSubstrateSlab.h`** (implementation-internal; not exported from
  `kernel/include/mem/`). Contains:
  - `namespace kernel::mm::vmsmalloc { ... }` (internal namespace, symmetric with the
    existing `kernel::mm::VMSubstrate` namespace in `VMSubstrate.cpp`).
  - `constexpr uint64_t kSlabDescriptorMagic = 0x5DAB5DABDE5CC9C0ULL;` (DEC-044).
  - `constexpr Core::ConstexprArray kSlabSizeClasses = {8ul, 16, 32, 64, 96, 128, 256, 512};`
    (DEC-003 schema, mirror of `LibAlloc::InternalAllocator::slabSizeClasses` —
    InternalAllocator.cpp:26).
  - `constexpr size_t kNumSizeClasses = kSlabSizeClasses.size();`
  - `struct SlabDescriptorBase` — the uniform fixed prefix (see Decisions for fields).
  - Per-class `constexpr size_t slotCount(size_t c);` accessor returning the DEC-045 fixpoint
    value for class `c`.
  - Per-class `constexpr size_t slotSize(size_t c)` returning `kSlabSizeClasses[c]`.
  - Per-class `constexpr size_t slot0Offset(size_t c)` returning the DEC-045 derived offset.
  - `constexpr size_t sizeClassFor(size_t size)` — maps a request size to its class index
    (used by `vmsmalloc` and `VMSubstrate::make<T>`'s compile-time class selection in later
    phases).
  - `constexpr size_t slotAlignment(size_t c)` — `slotSize(c)` for power-of-two classes,
    `alignof(max_align_t)` for non-pow2 classes (DEC-001).
  - `template <size_t N> struct SlabDescriptor : SlabDescriptorBase { LibAlloc::SlabBookkeeper<N> bookkeeper; };`
    — the per-class full descriptor type (DEC-045).
  - **Read accessors** that take a `SlabDescriptorBase*` (or const reference) and produce
    `slotSize(desc)`, `slotCount(desc)`, `slot0Offset(desc)`, `slotZeroAddr(desc)` — each does
    a constexpr-table lookup keyed by `desc->sizeClass`. These accessors are how `vmsfree`
    consumes the descriptor in Phase 5 (DEC-026 step 6a/6b uses them per the ITEM-049
    amendment).
- **Field layout of `SlabDescriptorBase`** is fixed in P2-DEC-001 (see Decisions). The fields
  in declaration order are: `magic`, `next`, `chainNext`, `chainDepth`, `sizeClass`,
  `numaDomain` — chosen to pack tightly to 32 B without internal padding under AMD64 ABI.
- **`SlabBookkeeper<N>` placement**: immediately after the prefix, with the necessary
  alignment slack handled by `slot0Offset[c]`'s computation. The `SlabDescriptor<N>` template
  uses default class-member layout (no `[[no_unique_address]]`, no manual offset arithmetic);
  C++'s rules guarantee the bookkeeper sits at offset `sizeof(SlabDescriptorBase)` with
  natural alignment, which the derived `slot0Offset[c]` already accounts for.
- **No public surface change**. `VMSubstrate.h` is unmodified in Phase 2. `kernel/mm/mm.h`
  and the other public headers are unmodified. The new header is `#include`d only by future
  vmsmalloc TUs.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 1 (`vmsmalloc-phase-1.md`) | `LibAlloc::SlabBookkeeper<N>` accepts any positive `N`; tail-bit masking via `seedAllAvailable(usableCount)`. Phase 2's `SlabDescriptor<N>` instantiations need this. | Yes — Phase 1 must be complete before Phase 2 can pass its `static_assert`s for non-pow2 classes. |
| `libraries/LibAlloc/include/liballoc/Slab.h` | Provides `SlabBookkeeper<N>` template that occupies the suffix of `SlabDescriptor<N>`. | Yes — Phase 1 output. |
| `libraries/Core/include/core/atomic.h` | Provides `Atomic<T>` for the `chainNext` field. | Yes — live. |
| `libraries/Core/include/core/math.h` | Provides `roundUpToNearestMultiple` (the alignUp equivalent), `divideAndRoundUp`, `max(a, b, ...)`. All `constexpr`. | Yes — live. |
| `libraries/Core/include/core/ds/ConstexprArray.h` | Backing the size-class array (mirror of LibAlloc's pattern at InternalAllocator.cpp:26). | Yes — live. |
| `kernel/include/arch/amd64/amd64.h` | Provides `kSmallPageSize` (= 4096) — needed for the DEC-045 fixpoint that fits descriptor + slots in one page. | Yes — live. |
| `kernel::mm::NUMA::DomainID` (or wherever the strong-typed DomainID lives) | The `numaDomain` field's type. | Yes — live. |

## Invariants

<!-- Compile-time invariants validated via static_assert. -->

- `sizeof(SlabDescriptorBase) <= 32` (target). The packing of fields in P2-DEC-001 achieves
  32 B; a static_assert pins the size so an accidental field addition is loud.
- `alignof(SlabDescriptorBase) == 8` (forced by the `uint64_t magic` field and the pointer
  fields).
- `slotCount(c) > 0` for every `c ∈ [0, kNumSizeClasses)` — closes ITEM-054 / parent-spec
  hazard "Size-class schema extension can produce `slotCount[c] == 0`".
- `slot0Offset(c) >= sizeof(SlabDescriptorBase) + sizeof(LibAlloc::SlabBookkeeper<slotCount(c)>)`
  for every `c` (slot 0 must sit *after* the bookkeeper).
- `slot0Offset(c) + slotCount(c) * slotSize(c) <= kSmallPageSize` for every `c` (slots fit in
  one page — DEC-007).
- `slot0Offset(c) % slotAlignment(c) == 0` for every `c` (DEC-001 alignment contract).
- `kSlabDescriptorMagic`'s bits 63:48 are `0x5DAB` (non-canonical when interpreted as AMD64
  VA — DEC-044 rationale (a)).
- `kSlabDescriptorMagic`'s leading byte (`0x5D`) differs from the DEC-024 poison pattern's
  leading byte (`0xCC`) — DEC-044 rationale (d).
- For every `c`, `kSlabSizeClasses[c] <= kSmallPageSize` (sanity; DEC-004 caps at pageSize).
- `kSlabSizeClasses` is monotonically increasing (so `sizeClassFor(size)` can be a simple
  upper-bound scan).
- For every consecutive pair `c, c+1`: `kSlabSizeClasses[c+1] > kSlabSizeClasses[c]`
  (no duplicates).

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| Schema change introduces a class with `slotCount(c) == 0` | Compile error via `static_assert(slotCount(c) > 0)` for every class index | No |
| Schema change introduces a class with `slot0Offset(c) + slotCount(c) * slotSize(c) > kSmallPageSize` | Compile error via the page-fit `static_assert` | No |
| `sizeof(SlabDescriptorBase) > 32` (accidental field addition / reordering) | Compile error via size `static_assert`. (The 32-byte target is a soft cap — bumping it is fine if intentional, but the assert forces a deliberate change.) | No |
| Non-pow2 class has `slot0Offset` not aligned to 16 B | Compile error via alignment `static_assert` | No |
| `kSlabDescriptorMagic` is changed in a way that violates DEC-044's rationale | Compile error via the magic-properties `static_assert`s | No |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P2-ITEM-001 | Resolved 2026-05-27 → P2-DEC-005 (sentinel) | | | Should `sizeClassFor(size)` return `kNumSizeClasses` (a sentinel) or assert for `size > largestSizeClass`? | Resolved: sentinel return. P2-DEC-005 already pins `sizeClassFor` returning `kNumSizeClasses` for oversized inputs; Phase 5 / Phase 7 consume the sentinel via the DEC-029 whole-page bypass dispatch. |
| P2-ITEM-002 | Resolved 2026-05-27 (verify at impl) | | | Does `Core::ConstexprArray`'s deduction work the same way `InternalAllocator.cpp:26` uses it? | Resolved direction: verify at the start of Phase 2 implementation. If the deduction doesn't carry to `kSlabSizeClasses`, fall back to `constexpr size_t kSlabSizeClasses[] = {8, 16, ...}` with a `kNumSizeClasses` macro/template. Either form satisfies the consumers; the fallback is mechanical. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P2-DEC-001 | Settled | **`SlabDescriptorBase` field layout: 32 B, six fields, packed in this order:** `uint64_t magic` (8 B, offset 0); `SlabDescriptorBase* next` (8 B, offset 8 — Treiber linkage, head-only per DEC-041); `Atomic<SlabDescriptorBase*> chainNext` (8 B, offset 16 — intra-chain linkage per DEC-034); `uint32_t chainDepth` (4 B, offset 24 — valid on chain head only); `uint16_t numaDomain` (2 B, offset 28 — `DomainID`-typed); `uint8_t sizeClass` (1 B, offset 30 — index into `kSlabSizeClasses`); `uint8_t _padding[1]` (1 B, offset 31). Total 32 B, no internal padding under the AMD64 SysV ABI. Magic at offset 0 keeps DEC-026's `desc->magic` check on the first cache line of the descriptor without an offset computation. | Tight packing minimizes prefix bytes that don't carry usable slots. The chosen field types match the read interface DEC-045 pins (`magic`, `next`, `chainNext`, `chainDepth`, `sizeClass`, `numaDomain`); no `slotSize` denormalization per P2-DEC-002. `uint8_t sizeClass` admits 256 classes (vastly more than DEC-003's 8); `uint16_t numaDomain` matches `PageAllocator.h`'s DomainID width (verified during phase planning). Decided 2026-05-26. |
| P2-DEC-002 | Settled | **No `slotSize` denormalization on `SlabDescriptorBase`. `slotSize(desc)`, `slotCount(desc)`, `slot0Offset(desc)` are constexpr-table accessors keyed by `desc->sizeClass`.** The accessors live in `VMSubstrateSlab.h` as `inline constexpr` free functions:<br>`inline constexpr size_t slotSize(const SlabDescriptorBase* d) { return kSlabSizeClasses[d->sizeClass]; }` (and analogous for `slotCount`, `slot0Offset`). | Resolves ITEM-049 with the cleaner of the two options (the parent spec deferred the field-vs-accessor choice to implementation). The "denormalization saves a measurable indirection" claim from the earlier DEC-045 draft was never measured; the table is a constexpr `ConstexprArray` in `.rodata` and the compiler can constant-fold every accessor through `sizeClass`. Saves up to 8 B per descriptor (across thousands of slabs in steady state) and removes the risk of `slotSize` / `sizeClass` drift. The descriptor stays 32 B. Decided 2026-05-26. |
| P2-DEC-003 | Settled | **Namespace: `kernel::mm::vmsmalloc`** for all vmsmalloc-internal symbols (types, constants, accessors). Symmetric with `kernel::mm::VMSubstrate` (the existing namespace in `VMSubstrate.cpp:552`). The header is `#pragma once` / standard include guards (matching the project convention in `kernel/include/mem/*.h`). | Matches existing project style. Internal namespace prevents accidental ADL collisions with `VMSubstrate`'s symbols. Decided 2026-05-26. |
| P2-DEC-004 | Settled | **DEC-045 fixpoint computed via a 2-step convergence with explicit `bookkeeperSize(kWordCount)` dispatch.** Algorithm: (1) compute an upper-bound `N_upper = (kSmallPageSize - sizeof(SlabDescriptorBase)) / slotSize(c)`; (2) compute `kWC_upper = divideAndRoundUp(N_upper, 64)`; (3) look up `book_size = bookkeeperSize(kWC_upper)` via a `switch` over `kWordCount` that returns `sizeof(LibAlloc::SlabBookkeeper<kWC*64>)` for `kWC ∈ [1, 8]`; (4) compute `slot0 = roundUpToNearestMultiple(sizeof(SlabDescriptorBase) + book_size, max(slotSize(c), 16))`; (5) compute `N = (kSmallPageSize - slot0) / slotSize(c)`; (6) if `divideAndRoundUp(N, 64) != kWC_upper`, repeat once with the smaller `kWC`. Two iterations always suffice (the iteration only shrinks `kWC`, and shrinking by one step is bounded). | The `sizeof(SlabBookkeeper<N>)` template instantiation requires `N` at compile time, so a direct fixpoint isn't possible. Dispatching on `kWordCount` lets us instantiate `SlabBookkeeper<kWC*64>` (a representative of the equivalence class) for each `kWC ∈ [1, 8]` and compute `bookkeeperSize` as a `constexpr` switch. Cap `kWC` at 8 because DEC-003's largest class (8 B) with one page yields at most ~490 slots = `kWC = 8`; any future schema needing larger `kWC` will fail a `static_assert` and prompt extension of the switch. Decided 2026-05-26. |
| P2-DEC-005 | Settled | **`sizeClassFor(size)` returns sentinel `kNumSizeClasses` for `size > largestSizeClass`.** Concretely: linear-scan `kSlabSizeClasses` for the smallest `c` such that `kSlabSizeClasses[c] >= size`; return `kNumSizeClasses` if no class fits. Callers (DEC-029 large-request bypass in Phase 4) check the sentinel and route to `VMSubstrate::allocPage()` directly. | A sentinel return is the minimal contract change that supports DEC-029's whole-page bypass without making `sizeClassFor` panic or assert (which would require a separate "trySizeClassFor" path). The 8-element scan is constant-time at the compiler's option (likely loop-unrolled). Resolves P2-ITEM-001. Decided 2026-05-26. |

## Hazards

- **`bookkeeperSize` switch must enumerate every `kWordCount` value that any class might
  produce.** If a future schema change pushes `kWC > 8`, the switch returns 0 (or a default
  case that asserts) and `slot0Offset` computes wrong values. Defense: a `static_assert` that
  every class's computed `kWC <= 8`, plus a comment on the switch noting the equivalence-class
  representative pattern.
- **`SlabDescriptor<N>` template instantiation cost.** Each DEC-003 class spawns a distinct
  template instance (`SlabDescriptor<7>`, `SlabDescriptor<15>`, `SlabDescriptor<31>`, etc.).
  This is fine — at most 8 instances for the DEC-003 schema. But each instance's
  `SlabBookkeeper<N>` is also a distinct type, so `vmsfree`'s `sizeClass`-keyed dispatch in
  Phase 5 will be a switch over up to 8 cases. Phase 2 lays the foundation; the dispatch
  ergonomics are Phase 5's concern.
- **Constexpr table layout cross-check vs. LibAlloc.** The size-class array mirrors LibAlloc's
  `InternalAllocator::slabSizeClasses`. If LibAlloc's array ever changes — e.g., InternalAllocator
  tunes its classes — vmsmalloc's array silently diverges. Defense: add a `static_assert` in
  Phase 2 that the two arrays are identical *as long as InternalAllocator's array is exposed
  in a header*. Currently it's in InternalAllocator.cpp (not a header), so the assertion can't
  be added directly. Mitigation: a comment in `VMSubstrateSlab.h` referencing
  `InternalAllocator.cpp:26` as the source of truth for the schema, with a TODO to move
  InternalAllocator's array to a header so the cross-check can be enforced. Permitted under
  user latitude.
- **DEC-001 alignment for non-pow2 classes:** the 96-byte class needs `slot0Offset` aligned
  to 16 (`alignof(max_align_t)`). The fixpoint uses `max(slotSize, 16)` for alignment so this
  holds for both pow2 and non-pow2 classes; a static_assert verifies it explicitly for class
  96 in case the fixpoint formula gets refactored.
- **`sizeof(SlabBookkeeper<N>)` includes `Storage`, which defaults to `InlineSplitBitmapStorage<N/64>`.**
  After Phase 1, `kWordCount = (N + 63) / 64`, so the default Storage's word count tracks
  `kWordCount` directly. Defense: a `static_assert` in `VMSubstrateSlab.h` that
  `sizeof(SlabBookkeeper<64>) == sizeof(SlabBookkeeper<63>) + sizeof(uint64_t) * 0` — i.e.,
  the kWordCount = 1 equivalence class's bookkeeper size matches expectations.
- **`Atomic<SlabDescriptorBase*>` size on AMD64:** 8 B (pointer-sized atomic). Standard, but a
  `static_assert(sizeof(Atomic<SlabDescriptorBase*>) == 8)` pins it so a Core refactor that
  inflates atomic pointers is loud.

## Verification Targets

| Property | Method |
|---|---|
| `sizeof(SlabDescriptorBase) == 32` | `static_assert` in `VMSubstrateSlab.h` |
| `alignof(SlabDescriptorBase) == 8` | `static_assert` |
| Every field's offset matches P2-DEC-001's layout (e.g., `offsetof(SlabDescriptorBase, magic) == 0`, `offsetof(_, chainDepth) == 24`) | `static_assert` per field |
| `slotCount(c) > 0` for every class | `static_assert` in a parameter pack expansion |
| `slot0Offset(c) % slotAlignment(c) == 0` for every class | `static_assert` per class |
| `slot0Offset(c) + slotCount(c) * slotSize(c) <= kSmallPageSize` for every class | `static_assert` per class |
| `kSlabDescriptorMagic` bits 63:48 == `0x5DAB` | `static_assert` |
| `kSlabDescriptorMagic` leading byte (`0x5D`) differs from poison `0xCC` | `static_assert` |
| `sizeClassFor(0)` → undefined behavior (caller bug — DEC-023 vmsmalloc rejects size 0); `sizeClassFor(1) == 0`; `sizeClassFor(8) == 0`; `sizeClassFor(9) == 1`; `sizeClassFor(512) == 7`; `sizeClassFor(513) == kNumSizeClasses` | Unit tests in `tests/kernel/vmsmalloc/SlabLayoutTest.cpp` (new file) |
| Computed `slotCount[c]` table matches hand-computed values (8 → 490, 16 → 249, 32 → 125, 64 → 63, 96 → 42, 128 → 31, 256 → 15, 512 → 7) — exact numbers depend on `sizeof(SlabBookkeeper<N>)` for each `N`; the test pins what the implementation produces and any change is loud | `static_assert` in the test file |
| `VMSubstrateSlab.h` includes cleanly from both kernel and userspace test builds | Kernel build (`cmake --build cmake-build-debug --target Kernel`) succeeds; userspace test build (`tests/kernel` target) succeeds |

## Testing Approach

- Phase 2 is **compile-time-validated**. The header itself carries `static_assert`s for every
  invariant. A passing build is the primary gate.
- Add `tests/kernel/vmsmalloc/SlabLayoutTest.cpp` (new directory) with:
  - Additional `static_assert`s that pin the computed `slotCount[c]` values per class. These
    duplicate the in-header asserts but make any unexpected change of the table loud in test
    output (rather than just kernel build output).
  - Runtime tests for `sizeClassFor(size)` covering edges: 0 (undefined, not exercised), 1, 8,
    9, 16, 17, 95, 96, 97, 512, 513, 4096, 4097 (overflow).
  - `static_assert(offsetof(SlabDescriptorBase, field) == expected_offset)` for each of the six
    fields.
- Wire `tests/kernel/vmsmalloc/` into `tests/kernel/CMakeLists.txt`: add a new `KernelTests`
  source `vmsmalloc/SlabLayoutTest.cpp`, plus the include path
  `../../kernel/mm/` so the test can `#include "VMSubstrateSlab.h"`.
- No TSan variant for Phase 2 (no concurrent code). The Phase-1 TSan runner stays as-is.

## Implementation Phases

<!-- Concrete ordered steps for Phase 2 itself. -->

1. **Confirm starting state.**
   - Phase 1 is merged; `LibAlloc::SlabBookkeeper<N>` accepts any positive `N`.
   - `tests/liballoc/SlabConcurrentTest.cpp` is passing under TSan on the M1.
   - `LibAlloc::InternalAllocator::slabSizeClasses` at `InternalAllocator.cpp:26` is the
     authoritative size-class schema.

2. **Create `kernel/mm/VMSubstrateSlab.h`.** Header skeleton:
   ```cpp
   #ifndef CROCOS_VMSUBSTRATE_SLAB_H
   #define CROCOS_VMSUBSTRATE_SLAB_H

   #include <stdint.h>
   #include <stddef.h>
   #include <core/atomic.h>
   #include <core/ds/ConstexprArray.h>
   #include <core/math.h>
   #include <liballoc/Slab.h>
   #include <arch/amd64/amd64.h>     // kSmallPageSize
   #include <mem/NUMA.h>             // DomainID (or wherever it lives)

   namespace kernel::mm::vmsmalloc {

   // DEC-044: 64-bit magic constant — non-canonical AMD64 VA, disjoint from
   // 0xCC poison, ASCII-readable in hex dumps.
   inline constexpr uint64_t kSlabDescriptorMagic = 0x5DAB5DABDE5CC9C0ULL;
   static_assert((kSlabDescriptorMagic >> 48) == 0x5DAB,
                 "kSlabDescriptorMagic must keep its non-canonical signature");
   static_assert((kSlabDescriptorMagic >> 56) != 0xCC,
                 "kSlabDescriptorMagic must not collide with DEC-024 poison byte");

   // DEC-003 size-class schema; mirror of LibAlloc::InternalAllocator::slabSizeClasses
   // (InternalAllocator.cpp:26 — TODO: move LibAlloc's array to a header so we can
   // cross-check with static_assert).
   inline constexpr Core::ConstexprArray kSlabSizeClasses =
       { 8ul, 16, 32, 64, 96, 128, 256, 512 };
   inline constexpr size_t kNumSizeClasses = kSlabSizeClasses.size();

   // ... (struct SlabDescriptorBase, accessors, bookkeeperSize, slotCount, slot0Offset,
   //      sizeClassFor, SlabDescriptor<N>, static_asserts)

   }  // namespace kernel::mm::vmsmalloc

   #endif
   ```

3. **Define `SlabDescriptorBase` and pin its layout.** Add the struct per P2-DEC-001:
   ```cpp
   struct SlabDescriptorBase {
       uint64_t magic;                                  // offset 0
       SlabDescriptorBase* next;                        // offset 8
       Atomic<SlabDescriptorBase*> chainNext;           // offset 16
       uint32_t chainDepth;                             // offset 24
       uint16_t numaDomain;                             // offset 28 — DomainID-typed
       uint8_t  sizeClass;                              // offset 30
       uint8_t  _padding[1];                            // offset 31
   };
   static_assert(sizeof(SlabDescriptorBase) == 32);
   static_assert(alignof(SlabDescriptorBase) == 8);
   static_assert(offsetof(SlabDescriptorBase, magic) == 0);
   static_assert(offsetof(SlabDescriptorBase, next) == 8);
   static_assert(offsetof(SlabDescriptorBase, chainNext) == 16);
   static_assert(offsetof(SlabDescriptorBase, chainDepth) == 24);
   static_assert(offsetof(SlabDescriptorBase, numaDomain) == 28);
   static_assert(offsetof(SlabDescriptorBase, sizeClass) == 30);
   static_assert(sizeof(Atomic<SlabDescriptorBase*>) == 8);
   ```
   Verify against the actual `DomainID` type — if it's not `uint16_t`, either change the field
   width or document the cast.

4. **Define the size-class accessors.** Per P2-DEC-002:
   ```cpp
   inline constexpr size_t slotSize(size_t c) { return kSlabSizeClasses[c]; }
   inline constexpr size_t slotAlignment(size_t c) {
       // DEC-001: pow2 classes align to size; non-pow2 to alignof(max_align_t) = 16 (DEC-022).
       return (slotSize(c) & (slotSize(c) - 1)) == 0 ? slotSize(c) : 16;
   }
   inline constexpr size_t slotSize(const SlabDescriptorBase* d) {
       return slotSize(d->sizeClass);
   }
   ```

5. **Implement `bookkeeperSize(kWordCount)` and the fixpoint** per P2-DEC-004:
   ```cpp
   inline constexpr size_t bookkeeperSize(size_t kWordCount) {
       switch (kWordCount) {
           case 1: return sizeof(LibAlloc::SlabBookkeeper<64>);
           case 2: return sizeof(LibAlloc::SlabBookkeeper<128>);
           case 3: return sizeof(LibAlloc::SlabBookkeeper<192>);
           case 4: return sizeof(LibAlloc::SlabBookkeeper<256>);
           case 5: return sizeof(LibAlloc::SlabBookkeeper<320>);
           case 6: return sizeof(LibAlloc::SlabBookkeeper<384>);
           case 7: return sizeof(LibAlloc::SlabBookkeeper<448>);
           case 8: return sizeof(LibAlloc::SlabBookkeeper<512>);
           default: return 0;  // static_assert below catches kWC > 8
       }
   }

   inline constexpr size_t slot0Offset(size_t c);  // forward decl

   inline constexpr size_t slotCount(size_t c) {
       const size_t ss = slotSize(c);
       const size_t align = max(ss, size_t{16});
       // Upper bound assuming zero bookkeeper:
       size_t N_upper = (arch::kSmallPageSize - sizeof(SlabDescriptorBase)) / ss;
       size_t kWC = divideAndRoundUp(N_upper, size_t{64});
       size_t book = bookkeeperSize(kWC);
       size_t slot0 = roundUpToNearestMultiple(sizeof(SlabDescriptorBase) + book, align);
       size_t N = (arch::kSmallPageSize - slot0) / ss;
       // Second iteration in case kWC shrunk:
       if (divideAndRoundUp(N, size_t{64}) != kWC) {
           kWC = divideAndRoundUp(N, size_t{64});
           book = bookkeeperSize(kWC);
           slot0 = roundUpToNearestMultiple(sizeof(SlabDescriptorBase) + book, align);
           N = (arch::kSmallPageSize - slot0) / ss;
       }
       return N;
   }

   inline constexpr size_t slot0Offset(size_t c) {
       const size_t align = max(slotSize(c), size_t{16});
       const size_t kWC = divideAndRoundUp(slotCount(c), size_t{64});
       return roundUpToNearestMultiple(sizeof(SlabDescriptorBase) + bookkeeperSize(kWC), align);
   }
   ```

6. **Add per-class fold-expression `static_assert`s.** Validate every class index against the
   invariants:
   ```cpp
   namespace detail {
       template <size_t... Cs>
       constexpr bool validateAllClasses(index_sequence<Cs...>) {
           return ((slotCount(Cs) > 0) && ...)
               && ((slot0Offset(Cs) % slotAlignment(Cs) == 0) && ...)
               && ((slot0Offset(Cs) + slotCount(Cs) * slotSize(Cs) <= arch::kSmallPageSize) && ...)
               && ((divideAndRoundUp(slotCount(Cs), size_t{64}) <= 8) && ...);
       }
   }
   static_assert(detail::validateAllClasses(
       make_index_sequence<kNumSizeClasses>{}),
       "DEC-045 fixpoint invariants violated for at least one class");
   ```
   If `make_index_sequence` / `index_sequence` aren't already in the kernel-compatible Core
   surface, add them (permitted under user latitude) or expand the assertion manually for
   each of the 8 classes.

7. **Define `SlabDescriptor<N>` and `sizeClassFor(size)`.**
   ```cpp
   template <size_t N>
   struct SlabDescriptor : SlabDescriptorBase {
       LibAlloc::SlabBookkeeper<N> bookkeeper;
   };

   inline constexpr size_t sizeClassFor(size_t size) {
       for (size_t c = 0; c < kNumSizeClasses; c++) {
           if (kSlabSizeClasses[c] >= size) return c;
       }
       return kNumSizeClasses;  // sentinel: large-request bypass per DEC-029
   }
   ```

8. **Add the test file `tests/kernel/vmsmalloc/SlabLayoutTest.cpp`.**
   - Include `<core/ds/ConstexprArray.h>`, `<liballoc/Slab.h>`, the new `VMSubstrateSlab.h`,
     and the standard test harness headers (per CLAUDE.md test convention).
   - Pin the computed `slotCount[c]` values via `static_assert` (these are the values the
     fixpoint produces given the actual `sizeof(SlabBookkeeper<N>)`; record what the
     implementation outputs so any change is loud).
   - Runtime tests for `sizeClassFor` edge values per the Verification Targets table.
   - Run via `tests/kernel/CMakeLists.txt`'s test runner (add the file to `KernelTests`'s
     source list and add the `kernel/mm/` include path).

9. **Build and regression-gate.**
   - `cmake --build cmake-build-debug --target Kernel` succeeds.
   - `cmake --build build --target KernelTests` (or its runner) succeeds and the new
     `SlabLayoutTest` passes.
   - Existing kernel build / boot / `naiveTest` regression is unaffected (Phase 2 doesn't add
     any runtime).

10. **Optional follow-up (permitted by user latitude).**
    - Move `LibAlloc::InternalAllocator::slabSizeClasses` from InternalAllocator.cpp:26 to a
      public LibAlloc header (e.g., a new `liballoc/SlabSizeClasses.h`) so vmsmalloc can
      `static_assert` that its `kSlabSizeClasses` matches LibAlloc's by element.

## References

- `kernel/mm/VMSubstrate.cpp` — `namespace kernel::mm::VMSubstrate` at line 552 (style precedent).
- `kernel/include/mem/VMSubstrate.h` — public VMSubstrate interface (unchanged in Phase 2).
- `libraries/LibAlloc/include/liballoc/Slab.h` — `SlabBookkeeper<N>` (Phase-1-extended).
- `libraries/LibAlloc/InternalAllocator.cpp:26` — `slabSizeClasses = {8, 16, 32, 64, 96, 128, 256, 512}` — the schema vmsmalloc mirrors.
- `libraries/Core/include/core/math.h` — `roundUpToNearestMultiple`, `divideAndRoundUp`, `max`, `log2ceil`.
- `libraries/Core/include/core/atomic.h` — `Atomic<T>` for `chainNext`.
- `libraries/Core/include/core/ds/ConstexprArray.h` — backing the size-class array.
- `kernel/include/arch/amd64/amd64.h` — `kSmallPageSize` (= 4096).
- `kernel/include/mem/NUMA.h` — `DomainID` (the type backing `numaDomain`).
- Parent spec `specs/vmsmalloc.md`:
  - DEC-028 — implementation-internal header location (`kernel/mm/VMSubstrateSlab.h`).
  - DEC-044 — magic constant value `0x5DAB5DABDE5CC9C0ULL` and its rationale.
  - DEC-045 (amended by ITEM-049) — uniform prefix + class-specific bookkeeper suffix; abstract read interface; fixpoint formula.
  - DEC-003 — size-class schema mirror.
  - DEC-001, DEC-022 — alignment contract.
- Phase 1 spec `specs/vmsmalloc-phase-1.md` — `SlabBookkeeper<N>` with arbitrary `N`.
