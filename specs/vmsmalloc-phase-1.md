---
kind: leaf
status: ready
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 1 — LibAlloc prerequisites

> Lift the artificial `SlotCount % 64 == 0` restriction in `LibAlloc::SlabBookkeeper`, document
> the acq-rel memory-ordering contract on `allocatedCount`, and verify (via tests) that
> `SplitBitmap::releaseBit`'s existing double-free assertion propagates through
> `SlabBookkeeper::freeSlot`. Strict prerequisite to all later vmsmalloc phases.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- Not building any vmsmalloc code — that's Phase 4+ in the parent spec.
- Not modifying `SmallPageAllocator` or any current `SlabBookkeeper` consumer's call sites —
  the API surface remains source-compatible for the existing `SlotCount % 64 == 0` callers.
  Existing consumers do not need to change.
- Not implementing the `LibAlloc::SlabAllocator` rewrite (a separate, parallel consumer of the
  same primitives).
- Not adding a new bitmap primitive — `Core::SplitBitmap` is unchanged in this phase. Its
  `releaseBit` already asserts double-free at `SplitBitmap.h:180` (verified during phase
  planning); the parent-spec phase 1 description's "Extend `Core::SplitBitmap::releaseBit` to
  detect releasing an already-free bit" turned out to be already done. Phase 1's DEC-013 work
  is to *verify* and *test* the existing detection, not to add new code.

## Consumer Contract

- **Lifted restriction:** `LibAlloc::SlabBookkeeper<N, Storage, UseHint>` accepts any positive
  `N`, not just multiples of 64. For `N % 64 != 0`, the tail-word bits in
  `[N, kWordCount * 64)` are permanently masked as occupied at construction and never appear in
  either bitmap half (alloc or free).
- **Backwards-compatible behavior:** for any `N % 64 == 0`, every externally observable
  behavior of `SlabBookkeeper<N>` after Phase 1 is identical to pre-Phase-1. The
  `SmallPageAllocator` build and its existing `naiveTest` stress are gates on this property.
- **Acq-rel contract pinned (DEC-042 #4):** `SlabBookkeeper::allocSlot` /
  `freeSlot` / `allocSlots` / `freeSlotsBulk` use at least acquire-on-read / release-on-write
  ordering on `allocatedCount`. The current `ACQ_REL` `fetch_add` (Slab.h:101) and `fetch_sub`
  (Slab.h:137) satisfy this. A header comment block at each public declaration site states the
  contract so any future LibAlloc refactor sees it before downgrading.
- **DEC-013 double-free detection:** a `freeSlot(s)` where slot `s` is already free fires the
  existing assertion at `SplitBitmap.h:180` (debug builds; assertions compile to no-ops in
  release). `freeSlotsBulk` has the symmetric assertions at `SplitBitmap.h:193-197`. Both
  propagate through `SlabBookkeeper`'s API.

## Dependencies

<!-- Upstream interfaces this phase relies on. -->

| Dependency | Role | Must be stable first? |
|---|---|---|
| `Core::SplitBitmap` (`libraries/Core/include/core/atomic/SplitBitmap.h`) | Bitmap primitive under `SlabBookkeeper`. `releaseBit` (line 175) asserts double-free at line 180; `releaseBitsBulk` (line 186) asserts at lines 193-197 (`allocBitmap` side) and 196-197 (`freeBitmap` side). `reserveBit` (line 203) masks a bit in both halves — invoked indirectly by `SlabBookkeeper::reserveSlot` (Slab.h:159) for tail-bit masking under the amended P1-DEC-001. | Yes — live. No changes needed in this phase. |
| `SmallPageAllocator` (existing consumer) | Sole in-kernel consumer of `SlabBookkeeper` today (512-slot instance, a multiple of 64). Build target `Kernel`; stress target `naiveTest`. | Yes — must continue building and stress-passing without source changes. |
| `tests/liballoc/SlabTest.cpp` and `tests/liballoc/CMakeLists.txt` | Test infrastructure for `SlabBookkeeper` / `Slab`. Build target `LibAllocTestRunner`; run target `run_liballoc_tests`. | Yes — live. Add new tests; do not regress existing 15. |
| Test harness assertion-trap support | Phase 1 adds double-free tests that expect an assertion failure. The current `tests/harness/TestHarness.h` may not have a `EXPECT_ASSERT_FAILURE` / death-test macro. **Modification permitted** per user latitude (LibAlloc/Core modifications allowed to streamline implementation) — extend the harness if it lacks the capability. | Yes — verify or extend during step 4. |
| ThreadSanitizer toolchain on the dev machine (Apple M1, ARMv8) | Phase 1's concurrent stress tests run under TSan to surface weak-memory regressions in `SlabBookkeeper`. The existing `LibAllocTestRunner` CMakeLists uses ASan; a parallel TSan build variant is added. Apple Clang ships with TSan support; no separate toolchain needed. | Yes — verify at the start of Step 6. |

## Invariants

<!-- Conditions that must hold at all times in Phase 1's output. -->

- After Phase 1: `kWordCount == (SlotCount + 63) / 64` for any `SlotCount > 0`. Storage
  footprint grows in 64-bit-word increments and only when crossing a 64-slot boundary.
  Static-assertable property: `sizeof(SlabBookkeeper<63>) == sizeof(SlabBookkeeper<64>)`,
  `sizeof(SlabBookkeeper<64>) < sizeof(SlabBookkeeper<65>)`.
- For `SlotCount % 64 != 0`: the bits in `[SlotCount, kWordCount * 64)` are marked unavailable
  in *both* `allocBitmap` and `freeBitmap` at construction and remain so for the lifetime of
  the bookkeeper. `bitmapAvailableCount()` returns exactly `SlotCount` (not `kWordCount * 64`)
  on a freshly seeded bookkeeper. `allocSlot` never returns a slot index outside `[0, SlotCount)`.
- **After `seedAllAvailable()` / `seedAllAvailable(usableCount)`: `reservedCount == kTailBits`**
  (= `kWordCount * 64 - usableCount`, a compile-time constant via `usableCount = SlotCount` for
  the no-arg form). `isEmpty()` is amended to check `allocatedCount == 0 && reservedCount == kTailBits`
  — i.e., "no live allocations *beyond* the structural tail-bit reservation". Caller-requested
  `reserveSlot` calls grow `reservedCount` past `kTailBits`, and `isEmpty()` then correctly returns
  false until those caller reservations are released (or never, in the case of permanent
  kernel-image / ACPI reservations in PageAllocator).
- `allocatedSlotCount() ≤ SlotCount - reservedCount` at all observable moments. The quantity
  `SlotCount - reservedCount` is the count of slots available to `allocSlot` — equal to
  `usableCount` immediately after init (when only tail bits are reserved), and shrinks by each
  subsequent caller `reserveSlot` call. `reservedCount` aggregates both structural tail bits
  and caller-requested carve-outs; users that need to distinguish them can subtract `kTailBits`
  to recover the caller-reservation count.
- For `SlotCount % 64 == 0`: every method's behavior is bit-identical to pre-Phase-1 (because
  `kTailBits == 0` and the `seedAllAvailable(SlotCount)` loop body is empty).

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `SlotCount == 0` at instantiation | Compile error via retained `static_assert(SlotCount > 0)` | No |
| `allocSlot` called after all `SlotCount - reservedCount - tailMaskedCount` slots are claimed | Returns -1 with `transition.before == transition.after == Full` (existing behavior) | Normal path |
| Double-free via `freeSlot(s)` of an already-free slot `s` | `SplitBitmap::releaseBit`'s assertion fires at `SplitBitmap.h:180` (debug builds). Release builds: undefined behavior at the bookkeeper level (a phantom decrement of `allocatedCount`); production callers must not double-free. | No (caller bug) |
| Double-free via `freeSlotsBulk` of an already-free bit | Symmetric assertion at `SplitBitmap.h:193-197` | No (caller bug) |
| `freeSlot(s)` with `s >= SlotCount` for an `N % 64 != 0` bookkeeper | If `s` is in `[SlotCount, kWordCount * 64)`, the bit is masked-as-occupied so `releaseBit` sees `prev & mask != 0` for the alloc-side check (mask-as-occupied → set in allocBitmap = 0, set in freeBitmap = 0) — the assertion fires because the freeBitmap's bit is 0 and `releaseBit` sets it, returning prev=0 which passes the assert; **this is a hole** unless the lower-bound check in `Slab::free` (`p < baseAddr + kRegionSize`) catches it first. Mitigation: the `Slab` wrapper's `contains()` check already enforces this at its layer; `SlabBookkeeper`'s contract documents that the caller (here: `Slab`) is responsible for keeping `s < SlotCount`. | No (caller bug) |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P1-ITEM-001 | Resolved 2026-05-27 (leave as-is) | | | Should `seedAllAvailable(usableCount)` also accept a `SlotCount`-less constructor variant so external callers can seed before knowing the usable count? | Resolved: no. The existing default ctor + post-construction `seedAllAvailable()` already handles this. No real caller has surfaced needing the variant. |
| P1-ITEM-002 | Resolved 2026-05-27 | | | Does the test harness (`tests/harness/TestHarness.h`) currently support an assertion-trap macro (`EXPECT_ASSERT_FAILURE` / `EXPECT_DEATH`)? | Resolved: yes. Per user 2026-05-27 — the test harness's `assert` throws an exception, so assertion-failure tests use a try/catch (or a macro that wraps the same) to detect the trap. No harness extension needed. Phase 7 reuses this mechanism for negative-test coverage of `vmsmalloc` / `vmsfree` entry-point asserts. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P1-DEC-001 | Settled (amended 2026-05-27 per parent-spec discussion) | **Tail-bit masking goes through `SlabBookkeeper::reserveSlot`, not `bitmap.reserveBit` directly.** `seedAllAvailable(size_t usableCount)` seeds the full bitmap to "all available", then loops `reserveSlot(i)` for each `i` in `[usableCount, kWordCount * 64)`. The no-arg `seedAllAvailable()` delegates with `SlotCount` (no tail bits in the multiple-of-64 case — the loop body is empty). **Add a static constexpr member** `static constexpr SlotCountType kTailBits = static_cast<SlotCountType>(kWordCount * 64 - SlotCount);` to `SlabBookkeeper`. **Amend `isEmpty()`** from `allocatedCount == 0 && reservedCount == 0` to `allocatedCount == 0 && reservedCount == kTailBits` — the right-hand side is a compile-time constant; the compiler folds the comparison. | The original draft routed tail bits through `bitmap.reserveBit` directly, bypassing `SlabBookkeeper`'s own bookkeeping — a layering smell. Routing through `reserveSlot` keeps the abstraction intact: every change to the bitmap state flows through the bookkeeper, and `reservedCount` consistently tracks "all bits not available for allocation regardless of cause" (structural tail bits + caller-requested carve-outs). The `kTailBits` offset in `isEmpty()` lets both consumers continue to use the same predicate without semantic drift: PageAllocator (where `kTailBits == 0` because `SlotCount % 64 == 0`) sees identical behavior to the pre-Phase-1 era; vmsmalloc sees `isEmpty()` correctly fire when `allocatedCount` returns to zero after a chain of slot frees. Other accessors (`isFull`, `freeSlotCount`) already use `SlotCount - reservedCount` for their "usable count", which becomes `usableCount` for vmsmalloc slabs and `SlotCount` for PageAllocator — both correct without modification. Alternative considered (initial draft): keep `bitmap.reserveBit` direct, leave `isEmpty()` unchanged. Rejected because the bypass layering smell would propagate hazards into Phase 5 ("watch for refactors that switch to `reserveSlot`") — fixing the layering upstream is cheaper. Original 2026-05-26 decision: see git history. |
| P1-DEC-002 | Settled | Test coverage for sub-word slot counts uses a parameterized helper rather than copy-paste of every existing test. The test set is `SlotCount ∈ {1, 7, 15, 63, 65, 127, 137}` — spanning < 64, near-boundary, > 64, and crossing each `kWordCount` equivalence class (1, 2, and 3 words). The existing literal-64 / literal-128 tests stay as-is so behavioral parity for the originally-supported sizes remains visible. | Cheapest path to broad coverage. The chosen set spans the equivalence classes of `kWordCount = (N + 63) / 64`. |
| P1-DEC-003 | Settled | The acq-rel ordering contract is documented as a single Doxygen-style comment block above each of `allocSlot`, `allocSlots`, `freeSlot`, `freeSlotsBulk` declarations in `Slab.h`. The block names DEC-039 and DEC-042 #4 (from the parent vmsmalloc spec) so a future maintainer can trace the constraint back to its origin. No code change to the `ACQ_REL` orderings themselves. | A focused comment matches the existing project style (see Slab.h:39–45 for the existing TODO block being retired). A separate "ordering contract" header file proved over-engineered for a single constraint. |
| P1-DEC-004 | Settled | The `SlabBookkeeper::SlotCountType` typedef uses `SmallestUInt_t<log2ceil(SlotCount + 1)>` (existing behavior). For `SlotCount == 1`, `log2ceil(2) == 1`, so `SlotCountType` is `uint8_t` — valid. No special-case needed for `SlotCount == 1`. | Verified the existing `SmallestUInt_t` traits handle the edge case. The original `% 64 == 0` restriction had hidden `SlotCount >= 64`, so the existing code never exercised `SlotCount == 1`; this decision is the explicit confirmation that no extra handling is needed. |

## Hazards

<!-- Known tricky spots in Phase 1's implementation. -->

- **Tail-bit masking must happen at seed time, not at first-use.** If `seedAllAvailable(SlotCount)`
  does not reserve tail bits, `allocSlot`'s underlying `tryClaimBitNoDrain` may return a bit
  index in `[SlotCount, kWordCount * 64)` — out-of-range for the caller's slot data. Defense:
  a unit test that allocates all slots from a bookkeeper with `SlotCount % 64 != 0` and
  asserts every returned slot index is `< SlotCount`.
- **`releaseBitsBulk`'s allocBitmap-side check is non-atomic** (`atomic_load(a[w])` at
  `SplitBitmap.h:193` reads the alloc side without coordination). This is fine for Phase 1 —
  the alloc side is single-owner per the SplitBitmap contract — but a future change that
  breaks the single-owner property silently breaks the assertion's value. Document the
  ownership assumption near the `releaseBitsBulk` site (latitude granted by the user for
  Core modifications).
- **Storage-footprint static_asserts may surprise existing consumers** if their `SlotCount`
  happens to be in a `kWordCount` equivalence class with smaller values than expected.
  Verify `SmallPageAllocator`'s instantiation (`SlotCount == 512`, exactly 8 words) is
  unchanged in footprint before merging.
- **`reservedCount` now aggregates structural tail bits + caller reservations.** Under the
  amended P1-DEC-001, `reservedCount == kTailBits` immediately after seeding, and grows past
  `kTailBits` only on caller `reserveSlot` calls. The `isFull()` and `freeSlotCount()` formulas
  (`SlotCount - reservedCount` as the usable-count denominator) remain correct without change
  — for vmsmalloc slabs `SlotCount - reservedCount == usableCount` after seeding, which is the
  expected "max claimable slots"; for PageAllocator (where `kTailBits == 0`) it remains
  `SlotCount - callerReservations` as today. **Watch for any new accessor or invariant that
  subtracts `reservedCount` from `SlotCount` and expects "caller reservations only"** — the
  formula returns "usable slot count net of tail bits", not "caller reservations". Existing
  users (`isFull`, `freeSlotCount`, `seedAllUsed`) all want the usable-count semantic, so the
  change is a strict improvement for them; the only new code that needs to distinguish is
  diagnostic code that wants to report "how many caller carve-outs are live" — those can
  subtract `kTailBits` explicitly.
- **Double-free assertion is debug-only.** Release builds (no `NDEBUG` opt-out, but
  `assert()` may compile to a no-op depending on the test/build config) will not fire on
  double-free. Phase-1 tests verify the assertion fires in the test build (which has
  assertions enabled). The DEC-013 contract is satisfied with debug-only enforcement.
- **The user's latitude for LibAlloc/Core modifications** (granted explicitly) means Phase 1
  can also include: minor refactors to `SplitBitmap` that improve `releaseBit`'s comment
  clarity around double-free; cleanup of the TODO block at `Slab.h:39-45`; small
  ergonomics changes to `SlabBookkeeper`'s public API where they help. Boundaries: do not
  rename existing public methods, do not change `OccupancyTransition`'s semantics, and do
  not alter `ACQ_REL` orderings.

## Verification Targets

| Property | Method |
|---|---|
| `SlotCount ∈ {1, 7, 15, 63, 65, 127, 137}` supports the seed → alloc-all → free-all cycle with unique slot indices in `[0, SlotCount)` | Parameterized unit test in `SlabTest.cpp` |
| For `SlotCount % 64 != 0`, no claimed slot index is `≥ SlotCount` | Unit test: claim all slots, assert each return value `< SlotCount` |
| For `SlotCount % 64 != 0`, `bitmapAvailableCount()` after `seedAllAvailable()` equals `SlotCount`, not `kWordCount * 64` | Unit test |
| `freeSlot` of an already-free slot triggers an assertion | Unit test using harness assertion-trap macro |
| `freeSlotsBulk` with a pending mask that includes an already-free bit triggers an assertion | Unit test, same pattern |
| Storage-footprint identity `sizeof(SlabBookkeeper<63>) == sizeof(SlabBookkeeper<64>) < sizeof(SlabBookkeeper<65>)` | `static_assert` in unit test file |
| `SmallPageAllocator` builds and `naiveTest` passes in QEMU with no regression in alloc rate or pattern | Compile + boot + stress |
| Existing 15 `SlabTest.cpp` cases pass unchanged | `run_liballoc_tests` |
| Single-owner + multi-freer concurrent stress: N freer threads concurrently `freeSlot` slots a single owner thread allocated; `allocatedSlotCount()` ends at exactly zero, no double-free assertions fire | New concurrent test in `SlabConcurrentTest.cpp` (or sibling file), run under TSan on the M1 dev machine |
| Observer-thread consistency: a reader thread polling `isEmpty()` / `isFull()` / `allocatedSlotCount()` while owner+freer mutate never observes a count outside `[0, SlotCount - reservedCount]` | New concurrent test, run under TSan |
| `OccupancyTransition` balance: across a concurrent alloc/free workload, the count of `becameFull` transitions equals the count of `becameAvailable` transitions for the same slab | New concurrent test (counts transitions across all threads, asserts balance at quiescence) |
| TSan-clean (no data races reported) on the M1 ARMv8 dev machine across all Phase-1 concurrent tests | TSan build variant of `LibAllocTestRunner`; clean output is the release gate |
| TSan-clean on x86 (CI / cross-check) — sanity check that TSan agrees across architectures | Same build variant on any x86 development machine, when available |

## Testing Approach

- All Phase-1 unit tests live in `tests/liballoc/SlabTest.cpp` alongside the existing 15
  cases. Parameterized tests use a small helper (templated lambda or `TEST_P`-equivalent if
  the harness supports it; otherwise a plain template function called from per-SlotCount
  `TEST(...)` cases).
- Assertion-trap tests rely on the harness's death-test or assertion-expectation mechanism.
  Verify presence at the start of Step 4; extend the harness if absent (permitted under user
  latitude).
- The kernel-side smoke gate is `naiveTest`'s existing run in QEMU. No new kernel-side test
  is required for Phase 1 — the parent vmsmalloc spec's stress tests start in Phase 8 and
  cover the bookkeeper indirectly through vmsmalloc itself.
- **ARMv8 / weak-memory testing is a Phase-1 gate.** The dev machine (Apple M1 MacBook Air) is
  itself ARMv8 and already runs `LibAllocTestRunner`, so TSan-on-ARMv8 coverage is free and
  natural for Phase 1 onward. The parent vmsmalloc spec's hazard "DEC-042 ARMv8 release gate is
  the default test path" pins this. Phase 1's concurrent stress tests must run TSan-clean on
  the M1 dev machine before Phase 1 is considered complete.
- **Concurrent stress tests** are added in a new file `tests/liballoc/SlabConcurrentTest.cpp`
  (kept separate from the single-threaded `SlabTest.cpp` so unit-test runs can stay fast and
  the concurrent suite can be wired to a dedicated TSan-instrumented runner). The concurrent
  tests use `std::thread` and standard atomics for orchestration (the harness already pulls in
  `std::set` and standard headers per `SlabTest.cpp` line 20-21).
- **TSan build variant.** Add a second test runner target `LibAllocTestRunnerTSan` in
  `tests/liballoc/CMakeLists.txt`, mirroring `LibAllocTestRunner` but with
  `-fsanitize=thread` instead of `-fsanitize=address` / `-fsanitize=leak`. (TSan and ASan are
  mutually exclusive at link time, so a parallel binary is the standard pattern.) The
  `run_liballoc_tests` custom target invokes both runners; both must pass for Phase 1 to
  ship. The TSan runner uses lower optimization (`-O1`) to keep stack traces readable —
  `-O3 -ffast-math` from the ASan variant is dropped for TSan.
- The kernel-side smoke gate (`naiveTest` in QEMU) remains a regression gate on the existing
  in-kernel `SlabBookkeeper` consumer (SmallPageAllocator). No new kernel-side test is required
  for Phase 1.

## Implementation Phases

<!-- Concrete ordered steps for Phase 1 itself. -->

1. **Confirm starting state.**
   - Verify `SplitBitmap::releaseBit`'s assertion at `SplitBitmap.h:180`
     (`assert(!(prev & mask), "Double free: bit already set in freeBitmap")`) and
     `releaseBitsBulk` assertions at `SplitBitmap.h:193-197` are intact.
   - Verify `SlabBookkeeper::freeSlot` (Slab.h:134) and `freeSlotsBulk` (Slab.h:146) call
     into the asserting primitives.
   - Verify `allocatedCount.fetch_add(1, ACQ_REL)` at Slab.h:101 and
     `.fetch_sub(1, ACQ_REL)` at Slab.h:137. No code change here.

2. **Edit `Slab.h` — restriction relaxation.**
   - Delete or rewrite the TODO block at Slab.h:39–45. Replace with a brief historical note:
     `// DEC-011 (vmsmalloc): SlotCount may be any positive value; tail bits in [SlotCount,`
     `// kWordCount * 64) are masked-occupied at init via seedAllAvailable(SlotCount).`
   - Change `static_assert(SlotCount > 0 && (SlotCount % 64) == 0, ...)` at Slab.h:50-52 to
     `static_assert(SlotCount > 0, "SlabBookkeeper SlotCount must be positive")`.
   - Change `static constexpr size_t kWordCount = SlotCount / 64;` at Slab.h:54 to
     `static constexpr size_t kWordCount = (SlotCount + 63) / 64;`.

3. **Edit `Slab.h` — `seedAllAvailable(usableCount)` overload + `kTailBits` + `isEmpty()` amendment (P1-DEC-001 amended 2026-05-27).**
   - Add a static constexpr member to `SlabBookkeeper`:
     ```
     static constexpr SlotCountType kTailBits =
         static_cast<SlotCountType>(kWordCount * 64 - SlotCount);
     ```
   - Add a new public method:
     ```
     void seedAllAvailable(size_t usableCount) {
         assert(usableCount <= SlotCount, "usableCount must not exceed SlotCount");
         bitmap.seedAllAvailable();
         allocatedCount.store(0, RELEASE);
         for (size_t i = usableCount; i < kWordCount * 64; i++) {
             reserveSlot(i);  // increments reservedCount per tail bit
         }
     }
     ```
     Note ordering: `allocatedCount.store(0, RELEASE)` must run **before** the `reserveSlot`
     loop, because `reserveSlot` asserts `allocatedCount.load(RELAXED) == 0` (Slab.h:160).
   - Modify the existing no-arg `seedAllAvailable()` to delegate:
     `void seedAllAvailable() { seedAllAvailable(SlotCount); }`. For SlotCount % 64 == 0 the
     loop body is empty; `reservedCount` stays at 0 — bit-identical to pre-Phase-1 behavior.
   - **Amend `isEmpty()`** (Slab.h:179) from
     `return allocatedCount.load(ACQUIRE) == 0 && reservedCount == 0;`
     to
     `return allocatedCount.load(ACQUIRE) == 0 && reservedCount == kTailBits;`.
     The right-hand side folds to a compile-time constant; for SlotCount % 64 == 0 it
     remains `reservedCount == 0` (unchanged behavior).
   - Update the `Slab` wrapper's constructors (Slab.h:228, 235) — currently they call the
     no-arg `seedAllAvailable()`, which now correctly masks tail bits for any SlotCount.
     No source change needed in `Slab`'s constructors.

4. **Edit `Slab.h` — acq-rel documentation block (P1-DEC-003).**
   - Add a comment block immediately above the `allocSlot` declaration (currently Slab.h:87):
     ```
     // Atomic ordering on `allocatedCount` is at least acquire-on-read /
     // release-on-write (ACQ_REL on the fetch_add below). Required by vmsmalloc
     // (parent spec DEC-039 / DEC-042 #4): downgrading to RELAXED breaks the
     // pre-read race-freedom argument for becameFull on ARMv8/RISC-V.
     ```
   - Add a symmetric block above `allocSlots`, `freeSlot`, and `freeSlotsBulk` (Slab.h:111,
     134, 146).
   - Do **not** change the `ACQ_REL` constants on the `fetch_add` / `fetch_sub` calls.

5. **Add tests in `SlabTest.cpp`.**
   - Decide on a parameterization strategy: if the harness supports parameterized tests, use
     it; otherwise add a templated helper invoked from per-SlotCount `TEST(...)` cases:
     ```
     template <size_t N>
     void testSeedAndExhaust() { ... }
     TEST(SlabBookkeeper_SubWord_SlotCount_1)   { testSeedAndExhaust<1>(); }
     TEST(SlabBookkeeper_SubWord_SlotCount_7)   { testSeedAndExhaust<7>(); }
     TEST(SlabBookkeeper_SubWord_SlotCount_15)  { testSeedAndExhaust<15>(); }
     TEST(SlabBookkeeper_SubWord_SlotCount_63)  { testSeedAndExhaust<63>(); }
     TEST(SlabBookkeeper_SubWord_SlotCount_65)  { testSeedAndExhaust<65>(); }
     TEST(SlabBookkeeper_SubWord_SlotCount_127) { testSeedAndExhaust<127>(); }
     TEST(SlabBookkeeper_SubWord_SlotCount_137) { testSeedAndExhaust<137>(); }
     ```
   - The helper:
     - Seeds via `seedAllAvailable()` (no-arg, which now delegates with `SlotCount`).
     - Asserts `isEmpty()` true, `isFull()` false, `freeSlotCount() == N`,
       `bitmapAvailableCount() == N`.
     - Allocates all `N` slots, collects indices into a `std::set<int>` (or equivalent),
       asserts each is in `[0, N)` and unique.
     - Asserts `isFull()` true.
     - Frees all slots, asserts `isEmpty()` true at the end.
   - Add storage-footprint static_asserts at the top of the file:
     ```
     static_assert(sizeof(SlabBookkeeper<63>) == sizeof(SlabBookkeeper<64>));
     static_assert(sizeof(SlabBookkeeper<64>) < sizeof(SlabBookkeeper<65>));
     static_assert(sizeof(SlabBookkeeper<128>) < sizeof(SlabBookkeeper<129>));
     ```
   - Add double-free tests:
     ```
     TEST(SlabBookkeeper_DoubleFree_Asserts) {
         SlabBookkeeper<64> sb;
         sb.seedAllAvailable();
         OccupancyTransition t{};
         int slot = sb.allocSlot(t);
         sb.freeSlot(slot, t);
         EXPECT_ASSERT_FAILURE(sb.freeSlot(slot, t));
     }
     TEST(SlabBookkeeper_DoubleFreeBulk_Asserts) {
         // analogous, using freeSlotsBulk
     }
     ```
     (If `EXPECT_ASSERT_FAILURE` doesn't exist in the harness, add it as the first sub-task
     of this step — see P1-ITEM-002.)

6. **Add concurrent stress tests + TSan build variant (ARMv8 weak-memory gate).**
   - Verify Apple Clang's `-fsanitize=thread` works on the dev machine: a trivial
     `int main() {}` linked with `-fsanitize=thread` should build and run. If it doesn't,
     install / repair the toolchain before proceeding (Apple ships TSan; this is sanity-only).
   - Create `tests/liballoc/SlabConcurrentTest.cpp`. Test cases:
     ```
     TEST(SlabBookkeeper_Concurrent_MultiCpuFree_AllocatedCountReturnsToZero) {
         // Owner thread allocates all N slots (collect indices).
         // Spawn K freer threads (default K=4); shard indices across them.
         // Each freer thread calls sb.freeSlot(idx, t) for its shard.
         // Join all; assert sb.allocatedSlotCount() == 0 and sb.isEmpty().
         // Repeat for SlotCount ∈ {64, 137, 256}.
     }
     TEST(SlabBookkeeper_Concurrent_ObserverConsistency) {
         // Owner thread alternates alloc/free in a loop (random pattern).
         // K-1 freer threads free slots the owner has allocated.
         // 1 observer thread polls allocatedSlotCount() / isEmpty() / isFull() in a
         // tight loop. Assert observed value is always in [0, SlotCount - reservedCount].
     }
     TEST(SlabBookkeeper_Concurrent_TransitionBalance) {
         // Aggregate `becameFull` and `becameAvailable` counts across threads
         // via atomic counters. After the run, assert the two are equal (every Full
         // is reached by exactly one alloc and left by exactly one free).
     }
     ```
   - Add `LibAllocTestRunnerTSan` target in `tests/liballoc/CMakeLists.txt`:
     ```
     add_executable(LibAllocTestRunnerTSan ../TestMain.cpp)
     target_sources(LibAllocTestRunnerTSan PRIVATE
         InternalAllocTest.cpp
         SlabTest.cpp
         SlabConcurrentTest.cpp)
     target_link_libraries(LibAllocTestRunnerTSan PRIVATE TestHarness)
     target_compile_options(LibAllocTestRunnerTSan PRIVATE
         -std=c++2c -fexceptions -Wall -Wextra
         -DCROCOS_TESTING -DCORE_LIBRARY_TESTING
         -O1 -g -fsanitize=thread)
     target_link_options(LibAllocTestRunnerTSan PRIVATE -fsanitize=thread)
     ```
     Add to `run_liballoc_tests`:
     ```
     add_custom_target(run_liballoc_tests
         COMMAND LibAllocTestRunner
         COMMAND LibAllocTestRunnerTSan
         DEPENDS LibAllocTestRunner LibAllocTestRunnerTSan
         ...)
     ```
   - Run `make run_liballoc_tests` on the M1. Both runners must complete with no failures
     and no TSan reports. **A TSan data race in the bookkeeper paths is a release-gate
     failure** — investigate before proceeding.

7. **Build and regression-gate (kernel side).**
   - `cmake --build build --target LibAllocTestRunner` and `make run_liballoc_tests`. All
     existing 15 tests pass; new tests pass; TSan-clean.
   - `cmake --build cmake-build-debug --target Kernel` — kernel builds without changes.
   - `cmake --build cmake-build-debug --target run` (or `run_numa`) — boot to QEMU, run
     `naiveTest`-style stress. SmallPageAllocator's allocation pattern and rate must match
     pre-Phase-1 (eyeball the printed counts; a perf-counter regression detector is out of
     scope).

8. **Optional cleanup (permitted by user latitude).**
   - If the SplitBitmap assertions' wording would benefit from referencing DEC-013, update
     the strings at `SplitBitmap.h:180`, `:193`, `:196` to mention "Double free (vmsmalloc
     DEC-013)" so the diagnostic is self-explanatory.
   - If `SplitBitmap::releaseBit`'s comment about double-free can be improved, do so.
   - These are nice-to-haves; gate Phase 1 on functional correctness, not on cleanup.

## References

- `libraries/LibAlloc/include/liballoc/Slab.h` — file under edit.
- `libraries/Core/include/core/atomic/SplitBitmap.h` — `releaseBit` at line 175, double-free
  assertion at line 180; `releaseBitsBulk` at line 186 with assertions at 193-197; `reserveBit`
  at line 203 (used for tail-bit masking).
- `tests/liballoc/SlabTest.cpp` — existing 15 tests; Phase 1 adds parameterized variants and
  double-free coverage.
- `tests/liballoc/CMakeLists.txt` — `LibAllocTestRunner` build target, `run_liballoc_tests`
  custom target.
- `tests/harness/TestHarness.h` — assertion-trap macros (verify presence at Step 4).
- Parent spec `specs/vmsmalloc.md`:
  - DEC-011 — restriction-lift requirement and `slotCount[c]` derivation rationale.
  - DEC-013 — double-free contract.
  - DEC-042 #4 — acq-rel requirement on bookkeeper atomics.
  - DEC-039 — race that the acq-rel contract underpins.
  - Hazard "Bookkeeper-extension blast radius" — Phase-1-tests-as-release-gate process note.
  - Hazard "DEC-039 pre-read is positional discipline" — why the acq-rel doc is load-bearing.
