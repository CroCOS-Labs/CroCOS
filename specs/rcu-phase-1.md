---
kind: leaf
status: complete
parent: specs/rcu.md
---

# RCU Phase 1 — Core Epoch Engine

> The epoch-based reclamation algorithm as a pure, kernel-free component in `Core::rcu`:
> epoch counter, reader slots, the four-state limbo-bag machine, and the advance/drain logic.

## Non-Goals

- **No kernel dependencies of any kind.** No `arch.h`, no `CpuLocal`, no `VMSubstrate`, no
  `InterruptContextDepths`. Include set is capped at `stddef.h`, `stdint.h`, `assert.h`,
  `core/atomic.h`, `core/utility.h`, `core/TypeTraits.h` — exactly what `TreiberStack.h` allows
  itself.
- **No storage ownership.** The engine is constructed over a caller-supplied slot array. It never
  allocates and has no opinion on where slots live.
- **No slot-identity policy.** The caller passes a slot index. Binding that index to a CPU is
  Phase 2's job.
- **No context enforcement.** Forbidden-context checks need `InterruptContextDepths`, so they are
  Phase 2's.
- **No `SafePtr` integration.** The engine deals in raw pointers (RCU-DEC-002, RCU-DEC-011).
- **No stall detection.** It needs a clock; Phase 2 supplies one.

## Consumer Contract

`libraries/Core/include/core/rcu/EpochDomain.h`, `namespace Core::rcu`. Header-only, matching
`TreiberStack.h`.

**As shipped 2026-08-01.** Two spellings differ from the pre-implementation draft and both are
load-bearing: `CROCOS_RCU_NOEXCEPT` in place of bare `noexcept` (P1-DEC-011) and
`is_default_constructible_v<Hooks>` in place of `__is_constructible(Hooks)` (P1-DEC-016).

```cpp
// noexcept everywhere except the Core test build, where assert(...) throws and a
// noexcept entry point would turn every negative test into std::terminate — P1-DEC-011.
#if defined(CORE_LIBRARY_TESTING)
#define CROCOS_RCU_NOEXCEPT
#else
#define CROCOS_RCU_NOEXCEPT noexcept
#endif

struct RetireHead {                     // intrusive; 16 bytes
    RetireHead* next    = nullptr;      // also the double-retire marker — see Failure Modes
    void      (*deleter)(RetireHead*) = nullptr;
};

inline constexpr size_t kBagCount              = 4;                        // P1-DEC-006
inline constexpr size_t kRetireAdvanceThreshold = 64;                      // ITEM-005, provisional
inline constexpr size_t kUnboundedDrainBatch   = static_cast<size_t>(-1);  // RCU-DEC-033

// Effective epoch width — P1-DEC-014.
inline constexpr unsigned kStateEpochBits     = 63;
inline constexpr unsigned kBagTagBits         = 62;
inline constexpr unsigned kEffectiveEpochBits = kBagTagBits;
inline constexpr uint64_t kMaxEpoch           = (uint64_t{1} << kEffectiveEpochBits) - 1;

enum class BagState : uint64_t { Free = 0, Open = 1, Sealed = 2, Claimed = 3 };

struct alignas(64) ReaderSlot {         // bare 64 — Core cannot include arch.h
    Atomic<uint64_t>    state{kInactive};      // bit 0 active, bits 63..1 epoch snapshot
    uint64_t            nesting      = 0;      // owner-only, plain (I5)
    uint64_t            openBagIndex = 0;      // owner-only bookkeeping
    uint64_t            retireCount  = 0;      // owner-only; advance threshold (I5)
    bool                inDrain      = false;  // owner-only; I14 + the RCU-DEC-038 assert
    Atomic<RetireHead*> bagHead[kBagCount]     = {};
    Atomic<uint64_t>    bagTagState[kBagCount] = {};   // (tag : 62, state : 2)
};

// The P1-DEC-009 protocol-point set, fixed at implementation. WINDOW-INTERIOR
// points sit inside the RCU-DEC-024 masked, fault-free transition window; a
// kernel Hooks must leave those three empty.
struct NoopRcuHooks {                   // zero-size default
    void onAfterEpochLoad(uint64_t) const noexcept {}       // WINDOW-INTERIOR: readLock, epoch load -> activation store (the I3 stall point)
    void onAfterActivation(uint64_t) const noexcept {}      // WINDOW-INTERIOR: readLock, after kReaderActivationFence
    void onBeforeDeactivation() const noexcept {}           // WINDOW-INTERIOR: readUnlock, before kStateRetire
    void onAfterScanEpochLoad(uint64_t) const noexcept {}   // tryAdvance, after kScanEpochLoad, before kScanFence (the I10 seam)
    void onBeforeEpochAdvance(uint64_t) const noexcept {}   // tryAdvance, scan clean, before the CAS
    void onAfterRetireEpochLoad(uint64_t) const noexcept {} // retire, after kRetireFence + epoch load
    void onBeforeSeal(size_t, size_t) const noexcept {}     // (slot, bag) before Open -> Sealed
    void onAfterClaim(size_t, size_t) const noexcept {}     // (slot, bag) after winning the claim CAS, before touching head — the HAZARD-1 window
    void onPreTouch(RetireHead*) const noexcept {}          // RCU-DEC-017
};

template <typename H> concept RcuHooks = /* the nine points above */;

template <typename Hooks = NoopRcuHooks>     // RCU-DEC-017
requires RcuHooks<Hooks>
class EpochDomain {
public:
    EpochDomain(ReaderSlot* slots, size_t slotCount) noexcept
        requires is_default_constructible_v<Hooks>;          // NOT __is_constructible — P1-DEC-016
    EpochDomain(ReaderSlot* slots, size_t slotCount, Hooks h) noexcept;
    EpochDomain(const EpochDomain&) = delete;
    EpochDomain& operator=(const EpochDomain&) = delete;
    ~EpochDomain();                         // debug-asserts full quiescence — RCU-DEC-034

    void     readLock(size_t slot) CROCOS_RCU_NOEXCEPT;   // caller guarantees no reentry — RCU-DEC-024
    void     readUnlock(size_t slot) CROCOS_RCU_NOEXCEPT;
    void     retire(size_t slot, RetireHead* node) CROCOS_RCU_NOEXCEPT;   // issues kRetireFence
    bool     tryAdvance(size_t slot) CROCOS_RCU_NOEXCEPT;     // scan + CAS + sweep; needs slot for inDrain (I14)
    size_t   sweepExpired(size_t slot) CROCOS_RCU_NOEXCEPT;   // drain all slots' claimable bags
    void     synchronize(size_t slot) CROCOS_RCU_NOEXCEPT;    // grace period only — RCU-DEC-031
    void     barrier(size_t slot) CROCOS_RCU_NOEXCEPT;        // seals own bag, drains to completion — RCU-DEC-031
    size_t   drainAllQuiescent() CROCOS_RCU_NOEXCEPT;         // teardown only; universal owner — RCU-DEC-035
    void     setDrainBatchBound(size_t bound) CROCOS_RCU_NOEXCEPT;   // RCU-DEC-033; default kUnboundedDrainBatch
    [[nodiscard]] size_t   getDrainBatchBound() const CROCOS_RCU_NOEXCEPT;
    [[nodiscard]] uint64_t currentEpoch() const CROCOS_RCU_NOEXCEPT;
    [[nodiscard]] size_t   getSlotCount() const CROCOS_RCU_NOEXCEPT;

private:
    ReaderSlot*      slots;
    size_t           slotCount;
    Atomic<uint64_t> globalEpoch{0};
    size_t           drainBatchBound = kUnboundedDrainBatch;
    bool             teardownActive  = false;
    [[no_unique_address]] Hooks hooks{};
};
```

Three contract points that follow from the parent's resolutions:

- **`readLock` / `readUnlock` are not reentrancy-safe on their own.** The engine documents that
  the caller must guarantee the outermost transition is not reentered on the same slot; the
  Phase-2 veneer provides that with `arch::InterruptDisabler`, and the userspace harness gets it
  for free by having no signal handlers (RCU-DEC-024). Keeping the guarantee at the veneer is what
  lets the engine stay arch-free.
- **`retire` issues `kRetireFence` (SEQ_CST) before its epoch load** (RCU-DEC-018) and
  debug-asserts `nesting > 0 || slot.inDrain || domain.teardownActive` (RCU-DEC-019 as amended by RCU-DEC-038 —
  deleter-retires from sweep paths legitimately run outside sections; writers still require a
  section unconditionally). The fence is the entire fix for ITEM-007 and must not be moved,
  elided, or made conditional.
- **`onPreTouch` fires per node, before any read of that node's `RetireHead` fields**
  (RCU-DEC-017, rephrased with RCU-DEC-033 — the pop-run-pop drain has no wholesale detach) —
  the drainer may be a different CPU than the retirer, dereferencing links in slab memory whose
  TLB entry may be stale.
- **Drains are batch-bounded** (RCU-DEC-033). At most `drainBatchBound` deleters run per
  `tryAdvance`/`sweepExpired` invocation, counted across all bags it claims; hitting the bound
  mid-bag stores the remainder's head back and re-seals (`Claimed → Sealed`, tag unchanged —
  amended I13). The bound is a plain per-domain value read only by the draining CPU; it must be
  set before concurrent use. Default `SIZE_MAX` (effectively unbounded).
- **Completion primitives are graded** (RCU-DEC-031, RCU-DEC-035). `synchronize(slot)`: grace
  period elapsed, nothing more. `barrier(slot)`: asserts `!inDrain`, seals **and rotates** the
  caller's own open bag, then advances and sweeps until no `Sealed`/`Claimed` bag tagged `≤` the
  entry snapshot is non-empty (RCU-DEC-036 — *not* "until globally empty", which livelocks) —
  full guarantee for retires issued from this CPU (per-slot; RCU-DEC-040); remote *open*-bag
  residents excluded, evaluated at entry. `drainAllQuiescent()`: **precondition — a
  happens-before edge from every CPU's last domain operation to the call** (thread join
  qualifies; a relaxed flag does not — RCU-DEC-035 as corrected), debug-asserted: no active
  sections *and* no `Claimed` bag; sets the domain-global `teardownActive` flag, acts as
  universal owner, force-seals every open bag, drains to empty ignoring `drainBatchBound`;
  returns the number of deleters run (RCU-DEC-037). The only entry point allowed to perform
  another slot's owner-side transitions, and the only completion primitive whose caller needs no
  bound slot — `synchronize`/`barrier` carry `retire`'s same-CPU slot binding for the call's
  duration. Deleter-retires during the teardown drain pass any index `< slotCount` (slot 0 is
  conventional for slotless callers); all are legal under `teardownActive` (final review F11).
- **`~EpochDomain` debug-asserts full quiescence** (RCU-DEC-034): every bag `Free` and empty, no
  active section, no in-flight drain. It never drains and never waits; release trusts the caller.

Caller obligations: `slot < slotCount`; `readLock`/`readUnlock` balanced and on the same slot;
the object owning a `RetireHead` is already unlinked and unreachable before `retire`.

`readLock` / `readUnlock` are loop-free, allocation-free, and legal in any context *at the engine
level* — the kernel veneer narrows this to "any context except NMI/#MC" (RCU-DEC-024), a
narrowing an engine-level reader must not miss. Every other entry point may run deleters and is
therefore governed by Phase 2's context rules; `drainAllQuiescent` additionally inherits the
strict blocking-primitive mask whenever a kernel veneer for it appears (it runs deleters →
`vmsfree`).

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| `Atomic<T>` (global namespace, `core/atomic.h`) | All atomic state | Yes |
| `thread_fence(MemoryOrder)` (`core/atomic.h:455`) | Both SEQ_CST fences | Yes |
| `assert(cond, "msg")` (`libraries/Core/include/assert.h`) | Debug-only checks; throws under `CORE_LIBRARY_TESTING` | Yes |

Note the spellings, which differ from `RCU-proposal.md`: `Atomic` is **global**, not `Core::Atomic`;
the fence is `thread_fence`, not `atomicThreadFence`; strong CAS is `compare_exchange` with no
`_strong` suffix; the header is snake_case throughout while the surrounding code is camelCase, the
same mix `TreiberStack.h` already lives with.

## Invariants

Inherits I1–I14 from `specs/rcu.md` (I7 retired; I13 as amended by RCU-DEC-033). Phase-local
additions:

- **P1-I1 (corrected 2026-08-01).** `static_assert(kBagCount >= 2)` — rotation needs a second
  bag (`maybeRotate`'s acquire-before-release). The old `>= 4` floor was I7's, which is retired;
  4 remains the P1-DEC-006 default as tuning slack, not correctness.
- **P1-I2.** `ReaderSlot` is exactly cache-line aligned and its `state` word shares no line with
  another slot's, so the scan's per-slot loads do not false-share with a reader's activation store.
- **P1-I3.** A zero-initialized `ReaderSlot` array is a valid empty domain: `state == 0` decodes
  as inactive, `bagTagState == 0` decodes as `(tag 0, Free)`.
- **P1-I4 (amended by RCU-DEC-034; membership corrected, final review F10).** `EpochDomain`
  holds no state other than the epoch, the slot-array pointer/count, the drain batch bound, the
  domain-global `teardownActive` flag (RCU-DEC-037), and the `[[no_unique_address]] hooks`
  member. Construction is trivial; the destructor is a debug-only quiescence assert (no draining,
  no waiting) and compiles to nothing in release.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `slot >= slotCount` | Debug: assert. Release: out-of-bounds slot access. | No (caller bug) |
| Unbalanced `readUnlock` | Debug: assert on `nesting == 0`. Release: `nesting` underflows and the slot stays active forever, stalling the domain. | No (caller bug) |
| `retire` of an already-linked `RetireHead` | Debug: assert on `node->next == nullptr` — **partial coverage**, misses a bag's tail node (see Hazards). Release: bag list corruption (cycle). | No (caller bug) |
| Owner reenters `retire` on its own open bag (e.g. via #PF) | Debug: assert — `retire` re-reads `bagTagState` after the push and compares it against the value `prepareOpenBag` left there, catching a bump or rotation that happened underneath. This is the class no invariant covers (parent Hazards). Release: undefined; the non-atomic I5 bookkeeping is corrupted. | No (caller-contract violation) |
| `synchronize` / `barrier` from inside a section on this domain | Debug: assert on `nesting == 0`. Release: spins forever. | No (caller bug) |
| `synchronize` / `barrier` from deleter context | Debug: assert on `!inDrain` (without it: an undiagnosed hang — every inner `tryAdvance` hits the I14 early-out and the epoch never moves, even in debug). Release: spins forever. | No (caller bug) |
| `drainAllQuiescent` with any concurrent use of the domain | Debug: assert (active section observed). Release: data race, undefined. The precondition is external and cannot be fully checked. | No (caller bug) |
| `~EpochDomain` on a non-quiescent domain | Debug: assert (RCU-DEC-034). Release: retired objects leak; deleters never run. | No (caller bug) |
| Drain hits `drainBatchBound` mid-bag | Remainder re-sealed (`Claimed → Sealed`, tag unchanged); immediately re-claimable by any CPU. Benign — RCU-DEC-033. | Yes |
| Concurrent `tryAdvance` on many CPUs | All but one lose the epoch CAS; all sweep. Benign. | Yes |
| Owner's open bag stale while all other bags `Sealed`/`Claimed` | `maybeRotate` finds no `Free` bag and keeps the current open bag one epoch longer — never blocks (RCU-DEC-027). Benign. | Yes |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P1-ITEM-001 | Resolved 2026-08-01 → parent RCU-DEC-027 | | | Parent ITEM-001: what does the owner do when its index-matching bag is `Claimed` by another CPU mid-drain? | Dissolved: bag selection is `openBagIndex`, not `epoch mod kBagCount`, so the owner never *needs* a specific index. `maybeRotate` is opportunistic — if no bag is `Free` it keeps the current open bag one epoch longer (tag-bump is always safe by I11). The owner never blocks and never waits on a `Claimed` bag. |
| P1-ITEM-002 | Resolved 2026-08-01 → P1-DEC-008 | | | Should `sweepExpired` visit slots in a rotated order keyed by the calling slot? | Yes — own slot first, then rotated. Order cannot complicate I2: expiry guards the claim CAS per-bag, so safety is visit-order-free. |
| P1-ITEM-003 | Resolved 2026-08-01 → P1-DEC-013 | | | Should `retire` return a bool indicating whether it drained, for test observability? | No. `DebugIntrospection` covered it, exactly as this row anticipated. |
| P1-ITEM-004 | Deferred | No | | Should `EpochDomain` be a template on `kBagCount`? | Non-type template parameter would let tests use `kBagCount = 4` while production tunes higher, at the cost of code bloat and a templated `ReaderSlot`. Defer to ITEM-005's tuning pass. |
| P1-ITEM-005 | Resolved 2026-08-01 → RCU-DEC-024 | | | Parent ITEM-008: how is the outermost transition made atomic w.r.t. interrupts? | The *veneer* masks maskable interrupts across the transition (`arch::InterruptDisabler`) and NMI/#MC sections are forbidden. **The engine itself needs no change** — it documents the precondition and stays arch-free. The transition window is fault-free by construction, so no exception can fire inside it. |
| P1-ITEM-006 | Resolved 2026-08-01 → RCU-DEC-018, RCU-DEC-019 | | | Parent ITEM-007: must `retire` be pinned? | Yes, but the safety property comes from `kRetireFence`, not from pinning. Engine gains a debug assert that the caller's slot is active, and a SEQ_CST fence at the top of `retire` before the epoch load. |
| P1-ITEM-007 | Resolved 2026-08-01 → RCU-DEC-017 | | | Parent ITEM-009: what is the freshness hook's shape? | `Hooks` policy template parameter with `onPreTouch(RetireHead*)`, defaulting to `NoopHooks`. **`EpochDomain` becomes a template**, per the `ChainedTreiberStack` precedent. |
| P1-ITEM-008 | Resolved 2026-08-01 → P1-DEC-014 | | | Parent ITEM-013: pin the effective epoch/tag widths and `static_assert` them. | State word carries 63 epoch bits, `bagTagState` 62 tag bits. The expiry comparison must be width-consistent. Pinned at implementation; parent ITEM-013 can now close against P1-DEC-014. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P1-DEC-001 | Settled | **Header-only in `core/rcu/EpochDomain.h`.** No `.cpp`. | Matches `TreiberStack.h` / `SplitBitmap.h`. Keeps the engine usable from both the kernel target and the userspace test build without a library target — which matters because `CoreUser` does not exist (it is commented out at `libraries/Core/CMakeLists.txt:39-49`) and tests compile Core sources directly. Decided 2026-08-01. |
| P1-DEC-002 | Settled | **Cache-line alignment is a bare `alignas(64)` with an explanatory comment**, not `arch::CACHE_LINE_SIZE`. | Core cannot include `arch.h` without breaking RCU-DEC-002. This is the existing convention in `TreiberStack.h:206-209` and `SplitBitmap.h:37-38`. Phase 2's kernel-side types use `arch::CACHE_LINE_SIZE` instead. Decided 2026-08-01. |
| P1-DEC-003 | Settled | **`RetireHead` carries the deleter as a plain function pointer taking `RetireHead*`.** Type recovery is the caller's job. | Keeps the engine free of templates and of any knowledge of the retired type. Phase 2's `retire<T, Head>` wrapper synthesizes the thunk that recovers `T*` from the head. Decided 2026-08-01. |
| P1-DEC-004 | Settled | **The scan never skips the calling CPU's own slot.** | A writer may legitimately retire from inside a read-side section, so the caller's own slot can be active and must block its own advance. Skipping it is a plausible-looking optimization and a genuine safety bug — called out in the parent spec's Hazards for exactly this reason. Decided 2026-08-01. |
| P1-DEC-005 | Settled (**superseded in mechanism by RCU-DEC-033, 2026-08-01**) | **Deleters run on the draining CPU, popped one at a time from the `Claimed` bag** — pop-run-pop, not wholesale `head.exchange(nullptr)` detach, so the batch bound can stop mid-bag and re-seal the remainder. | Exclusivity now comes from the `Claimed` state itself (I6: only the claim winner touches `head`), not from detachment — pops and the exit store are plain ops. Deleter-reenters-`retire` stays safe: the deleter's pushes go to its *own* slot's open bag (I6) and recursion into the drain is blocked by `inDrain` (RCU-DEC-030). The no-ABA argument survives — `head` has no concurrent writers in any state. Original detach form retained here for history; do not reintroduce it, it makes the RCU-DEC-033 remainder invisible to `barrier`. Decided 2026-08-01. |
| P1-DEC-006 | Provisional | **`kBagCount = 4`.** | Structural minimum is 2 (rotation; P1-I1 as corrected — I7's `≥ 4` floor is retired); 4 buys slack between sealing and reclamation. Real tuning is parent ITEM-005, deferred until a RadixVM-shaped workload exists — the vmsmalloc Phase 10 lesson. Decided 2026-08-01. |
| P1-DEC-007 | Settled | **`DebugIntrospection.h` exposes epoch, slot state, bag contents, and `assertQuiescent()` behind `CROCOS_RCU_TEST_HARNESS`.** | The vmsmalloc Phase-8 pattern (`kernel/mm/vmsmalloc.cpp:532-559`). Bag algebra is otherwise untestable without making internals public; `assertQuiescent()` is how the quiescence check is unit-tested without throwing through the `noexcept` destructor. Decided 2026-08-01. |
| P1-DEC-008 | Settled | **`sweepExpired(slot)` visits slots starting at the caller's own, then `(slot+1) % slotCount` onward, wrapping.** | Resolves P1-ITEM-002. Safety is visit-order-free — I2's expiry check guards each claim CAS individually, and `tryAdvance`'s epoch scan must visit *all* slots regardless of order — so the order is pure policy: rotation spreads claim-CAS contention that all-CPUs-start-at-slot-0 would concentrate, and own-slot-first adds locality (own expired bags drain with no cross-CPU traffic). P1-DEC-004 is unaffected: rotation permutes order, never membership. Decided 2026-08-01. |
| P1-DEC-009 | Settled (**set fixed 2026-08-01**) | **The `Hooks` policy (RCU-DEC-017) additionally carries named protocol-point callbacks** (`onAfterEpochLoad`, `onBeforeActivation`, `onBeforeSeal`, …; exact set fixed at implementation, each documented at its call site), no-ops in `NoopHooks`. **The set as implemented is the nine points listed in the Consumer Contract**; `onBeforeActivation` was dropped as redundant with `onAfterEpochLoad` (same position in the sequence) and `onAfterActivation` / `onBeforeDeactivation` added so the harness can gate both ends of the transition. Window-interior points are `onAfterEpochLoad`, `onAfterActivation`, `onBeforeDeactivation`; each is tagged `WINDOW-INTERIOR` at its call site as this decision requires. | Resolves P3-ITEM-001 at the engine layer: the injection mechanism already exists and is already proven zero-cost (`[[no_unique_address]]` + inlined empty bodies — a *provable* zero, stronger than an `#ifdef`). The torture harness supplies a `Hooks` whose points spin on atomic gates; kernel and release builds compile them away. A no-op or spinning call perturbs no atomic ordering — delay is exactly what I3 licenses and what the tests induce. **Masked-window contract (final review F7):** points named `onAfterEpochLoad`/`onBeforeActivation` sit *inside* the RCU-DEC-024 interrupt-masked, fault-free `readLock` transition window in the kernel veneer. The implementation must tag each point's window position at its call site, and **kernel `Hooks` must be empty at window-interior points** — a point that touches anything (a counter in vmsmalloc memory, a log, MMIO) breaks the audited no-instruction-can-fault claim exactly as the evicted `monoTimens` stall-stamp did, and a spinning point would spin with interrupts masked. (A non-inlined *empty* call is fine: the kernel stack is in RCU-DEC-024's audited region list.) Decided 2026-08-01. |
| P1-DEC-010 | Settled | **`retire` rotates FIRST and bumps the open bag's tag only as the fallback when rotation is impossible** — not the parent's sketch order, which bumps unconditionally and then calls `maybeRotate`. Concretely: read `t = tag(bag[openBagIndex])`; if `t < e`, look for a `Free` bag (manufacturing one by draining our own expired bags if `!inDrain`); if one is found, open it at tag `e` **then** seal the old bag at **its own tag `t`** (acquire before release) and push there; if none is found, bump the current bag's tag to `e` and push there. | **The parent's sketch is degenerate as written.** `rcu.md`'s `retire` pseudocode performs the I11 tag bump (`if (t < e) tagState.store(pack(e, Open))`) *before* calling `maybeRotate(slot, e)`, whose documented trigger is "`e` exceeds the open bag's tag" — which the bump has just made permanently false. Rotation could never fire, no bag would ever seal, and every retire for the life of the domain would pile into a single bag that no CPU could ever drain. Rotate-first is what RCU-DEC-027 and P1-ITEM-001's *prose* actually describe ("if no `Free` bag it keeps the current open bag one epoch longer (tag-bump is always safe by I11)" — i.e. the bump is the failed-rotation fallback, not an unconditional step). It is also strictly better than the bump-then-rotate-with-a-stale-flag repair, which is equally correct but seals the old bag at `e` instead of `t`, delaying its expiry by a full epoch for no gain. Safety is unchanged: the sealed bag's nodes all have `r(n) <= t` because every push into it either matched the tag or bumped it, so sealing at `t` satisfies I11; the new bag opens at `e = r(n)` for the node being pushed. Confirmed with the spec author 2026-08-01. Decided 2026-08-01. |
| P1-DEC-011 | Settled | **Entry points are declared `CROCOS_RCU_NOEXCEPT`, a macro expanding to `noexcept` everywhere except under `CORE_LIBRARY_TESTING`.** The destructor stays unconditionally `noexcept`. | The Consumer Contract declares every entry point `noexcept`, but under the Core test harness `assert(...)` **throws** (`libraries/Core/include/assert.h`), and a throw out of a `noexcept` function is `std::terminate` — not a catchable failure. Every `EXPECT_ASSERT_FAILURE` negative test this phase's Verification Targets require (`!inDrain` from a deleter, retire outside a section, out-of-range slot, the `drainAllQuiescent` precondition) would therefore have killed the runner instead of passing. The spec caught this hazard for `~EpochDomain` — that is exactly why P1-DEC-007 routes the quiescence test through `DebugIntrospection::assertQuiescent()` — and then did not generalize it to the other eleven members. Kernel and release builds are bit-for-bit what the contract specifies; only the test build relaxes. Confirmed with the spec author 2026-08-01. Decided 2026-08-01. |
| P1-DEC-012 | Settled | **Unit tests heap-allocate their `EpochDomain` and destroy it only on the success path**, via a local `Owned<>` wrapper whose `finish()` does `drainAllQuiescent()` then `delete`. A test that fails part-way deliberately leaks its domain. | Follows from P1-DEC-011 and RCU-DEC-034 together, and it is sharper than the Phase-3 rule it generalizes. `~EpochDomain` asserts quiescence; a test that fails an `ASSERT_*` mid-scenario leaves retired objects in limbo, so the stack-allocated domain's destructor runs **during unwinding**, throws, and terminates the whole runner — converting one reported test failure into a total loss of the suite's results. This was not hypothetical: it happened on the first run, and the underlying test bug (P1-DEC-015's cause) was invisible until the domain was moved to the heap. `specs/rcu-phase-3.md` P3-DEC-007 already reached "failed scenarios skip teardown and leak deliberately, domains heap-allocated" for the torture suite; the same reasoning applies to every test that constructs a domain, so it belongs here rather than one phase later. Decided 2026-08-01. |
| P1-DEC-013 | Settled | **`retire` returns `void`.** Test observability comes from `DebugIntrospection` and from the existing return values instead. | Resolves P1-ITEM-003. `sweepExpired` already returns its deleter count, `drainAllQuiescent` returns its own, and `BagSnapshot.nodeCount` / `totalResidue` / `openBagResidue` gave every Phase-1 test what a `bool` would have — including the batch-bound and quiet-residue cases that motivated the question. Adding a test-only signal to the hottest write-side entry point to duplicate information already available was not worth it. Decided 2026-08-01. |
| P1-DEC-014 | Settled | **Epoch widths are pinned as named constants with `static_assert`s: `kStateEpochBits = 63`, `kBagTagBits = 62`, `kEffectiveEpochBits = kBagTagBits = 62`, `kMaxEpoch = 2^62 - 1`.** All comparisons — including `globalEpoch >= tag + 2` — run at the effective width, and `retire` debug-asserts `e <= kMaxEpoch`. | Resolves P1-ITEM-008 and gives parent ITEM-013 something to close against. I9 as originally written said 64 bits; the state word actually carries 63 (bit 0 is the active flag) and `bagTagState` 62 (two bits of state), so the effective width is the narrower of the two. The assertions are written against intermediate constexpr variables rather than literals, per this phase's own `-Wtautological-compare` hazard. Unreachable in practice at grace-period rates, so this is documentation-with-teeth rather than a live concern. Decided 2026-08-01. |
| P1-DEC-015 | Settled | **Three ordering constants beyond the parent's table are named and documented as additions**: `kBagOpen` (RELAXED — `Free → Open` and the I11 tag bump), `kBagDrainAccess` (RELAXED — `head` reads/writes by the CPU holding a bag `Claimed`), `kEpochObserve` (ACQUIRE — diagnostic and termination-predicate epoch reads). | RCU-DEC-010 requires every atomic site to carry a named ordering, but the parent's table covers only the sites its proofs turn on, leaving three real accesses unnamed. Each is justified in the header at its definition: `kBagOpen` is an owner-side transition on a bag no other CPU may write, with visibility to a future thief riding the subsequent `kBagSeal` RELEASE; `kBagDrainAccess` is RCU-DEC-033's "plain ops" under I6 exclusivity, RELAXED rather than non-atomic only because `head` is an `Atomic<>` that concurrent observers (`barrier`'s termination predicate, `DebugIntrospection`) may legally load; `kEpochObserve` is not on any proof path. Naming them keeps the audit-trigger discipline total rather than partial. Decided 2026-08-01. |
| P1-DEC-016 | Settled | **The default constructor's constraint is `requires is_default_constructible_v<Hooks>`, a new variable template in `core/TypeTraits.h`, not the built-in `__is_constructible(Hooks)` the draft contract named.** `core/TypeTraits.h` gains `is_constructible_v` and `is_default_constructible_v`. | **GCC 15 — the kernel compiler — rejects a built-in trait used directly in a function signature** ("error: use of built-in trait `__is_constructible(Hooks)` in function signature; use library traits instead"). Clang, which builds `tests/`, accepts it. So the contract's literal spelling compiled clean and passed all 441 tests before failing the cross build outright. Two things follow. First, `TypeTraits.h` was missing the trait entirely, so this is a real gap filled rather than a workaround. Second — the generalizable lesson — **a new Core header is not validated until it has been compiled with `x86_64-crocos-g++ -ffreestanding -Werror` in both `-DDEBUG_BUILD` and release**; the harness compiler is not a proxy for the kernel compiler, and this phase's existing `-Wtemplate-body` hazard is the same class of divergence. Decided 2026-08-01. |
| P1-DEC-017 | Settled (**correction found by Phase 3, 2026-08-01**) | **`drainAllQuiescent` visits each slot's `openBagIndex` bag LAST, and moves the cursor off it — to a `Free` bag — before claiming it.** The per-bag body is factored into `drainBagAsUniversalOwner`, and `findFreeBagExcept(s, except)` is added alongside `findFreeBag`. | **The original loop was fatal on a path RCU-DEC-038 explicitly legalises.** It force-sealed and Claimed every bag in index order, including the one `openBagIndex` designates. A deleter that retires — legal inside a drain, and the shape RadixVM's node deleters have — reaches `prepareOpenBag`, which reads `bagTagState[openBagIndex]`, finds `Claimed`, and trips its `Free`-or-`Open` assert. Under teardown the drainer has no bound slot, so *every* deleter-retire targets one fixed slot, and that slot's open bag is always among the ones drained: the failure is not an unlucky interleaving but a certainty whenever a deleter retires during teardown. The engine's own body comment ("deleters may have retired into bags already visited this pass — legal, and collected by the next one") documented the intended behaviour all along. Found by `rcuTortureContendedTeardown`; confirmed by disarming deleter-retires immediately before the drain, which makes the scenario pass. Two alternatives were rejected: relaxing `prepareOpenBag`'s assert under `teardownActive` (it is the assert that catches RCU-DEC-027's owner-reentrancy class, and weakening it for one caller blinds it for all), and narrowing the contract to forbid deleter-retires during teardown (unenforceable selectively, and a landmine for the one consumer the framework exists for). **Termination note:** a `Free` bag always exists by the time the cursor moves, because the epoch cannot advance during teardown (`tryAdvance` early-outs on `teardownActive`), so `prepareOpenBag`'s `t < e` rotation trigger can fire at most once per slot per pass; with `kBagCount >= 2` static-asserted, at least one bag is left `Free`. Decided 2026-08-01. |
| P1-DEC-018 | Settled (**bug found by the Phase 4 in-kernel spike, 2026-08-01**) | **`EpochDomain::retire` takes the deleter and calls `hooks.onPreTouch(node)` as its FIRST statement**, before the double-retire read, the deleter store and `pushNode`. The Phase-2 veneer no longer writes `node->deleter`; the engine is the single owner of every `RetireHead` access. `onPreTouch`'s contract is reworded from "before any READ" to "before any ACCESS, read or write". | **RCU-DEC-017 was implemented on the read side only.** `drainClaimedBag` refreshed before reading `next`/`deleter`, but the veneer's `retireNode` wrote `node->deleter` through a raw pointer with no freshness at all. Under RCU-DEC-006 stealing the retiring CPU is routinely not the allocating one, so the store landed on this CPU's stale mapping and the drainer — which does refresh — then read a null deleter off the real page. In a debug kernel that is the "retired node has no deleter" assert; **in release the assert is compiled out and it is a call through a null function pointer**, since the faulty write sat outside `CROCOS_RCU_DEBUG_CHECKS`. Diagnosed by a three-arm experiment rather than inspection: baseline panics within ~1 iteration; a `SafePtr` probe before retire (refresh + read) survives 327K iterations/CPU to a clean shutdown; the same read through a RAW pointer (no refresh) panics identically — isolating `ensureTLBEntryFresh` as the discriminator. **The deleter store had to move into the engine, not just gain a refresh:** a hook call at the top of the engine's `retire` would have landed AFTER the veneer's write. Cost: one `ensureTLBEntryFresh` per retire in the kernel, currently unmeasured (the userspace harness's is a no-op) — carried to the Phase 4 spec. The original "read" wording is why this survived three phases and 1261 green tests. Decided 2026-08-01. |

## Hazards

- **HAZARD-1 lives here.** The four-state bag machine is implemented in this phase; if the
  transitions are wrong, objects are freed a grace period early. Test the interlock deliberately.
- **`kReaderActivationFence` and `kScanFence` must both stay SEQ_CST.** A downgrade passes every
  x86 test and fails rarely on ARMv8. Both sites carry the named constant and an inline citation.
- **`nesting` is deliberately non-atomic** (I5). Correct because only the owner touches it and
  interrupt nesting on one CPU is serialized — but it will look like a bug to a reviewer and to
  TSan if a test ever drives one slot from two threads. The harness must bind one thread per slot.
- **The `-Wtemplate-body` trap.** `TreiberStack.h:151-163` documents a non-dependent
  `static_assert` that GCC evaluates at parse time, breaking any freestanding TU that merely
  *includes* the header. Any `static_assert` here on lock-freedom or platform capability needs the
  same value-dependent treatment.
- **`-Werror` + `-Wtautological-compare` on pinned constants.** A `static_assert` comparing two
  constexpr constants can trip this; vmsmalloc Phase 2 hit it. Use intermediate constexpr
  variables.
- **Unused-variable warnings in release** — **corrected 2026-08-01, the original wording named the
  wrong build.** There are three `assert` expansions, not two. The release *kernel* build
  (`kassert.h`, no `DEBUG_BUILD`) expands to `(void)(condition)`, which still **evaluates** the
  condition — so the variable is used and no warning fires, but an expensive assert predicate
  costs real work in release. It is the third branch — neither `KERNEL` nor `CORE_LIBRARY_TESTING`,
  i.e. `assert.h`'s `#define assert(condition, ...)` — that discards the condition entirely and
  produces the warning. `synchronize` hit exactly this and carries an explicit `(void)s;`, the
  pattern at `TreiberStack.h:388-390`. The practical consequence of the release-kernel behaviour:
  keep assert predicates cheap, or accept paying for them in release. `retire`'s owner-reentrancy
  re-check is written to reuse a value `prepareOpenBag` already returned rather than issuing a
  second load, for this reason.
- **`tryAdvance` sweeps unconditionally, so nothing can advance the epoch "quietly".** Any test or
  helper that drives the epoch forward *also drains every expired bag in the domain*, on every
  call. This is load-bearing behaviour (see the parent's note on why the sweep must not sit under
  the scan's early return), but it makes test choreography order-sensitive in a way that is easy
  to get wrong: three separate Phase-1 test bugs traced to it, including a batch-bound test that
  set `drainBatchBound` *after* the advance that expired the bag and therefore measured an
  unbounded drain. Any test that wants to observe a bag in a particular state must reach that
  state without an intervening `tryAdvance`, or account for the sweep it performs. Phase 3's
  torture scenarios will hit this harder than Phase 1 did.
- **`__is_constructible` and friends cannot appear in a function signature under GCC 15** — see
  P1-DEC-016. Same divergence class as the `-Wtemplate-body` trap above, and equally invisible to
  the harness build: it passed 441 tests under clang before failing the cross compile.
- **The double-retire assert has partial coverage, by construction.** `retire` asserts
  `node->next == nullptr` and the drain clears `next` before handing a node to its deleter, which
  catches a re-retire of any node linked to a successor and of a bag's current head. It cannot
  catch re-retiring a node that is a bag's *tail*, whose `next` is legitimately `nullptr` while it
  is still in the bag. Closing the gap needs a dedicated linked-bit, which `RetireHead` has no room
  for at 16 bytes. Recorded so the check is not mistaken for total.

## Verification Targets

**All targets below are covered and green as of 2026-08-01** — 441/441 under both `CoreTestRunner`
(ASan + leak) and `CoreTestRunnerTSan` (ARMv8), the latter with zero ThreadSanitizer reports.

| Property | Method | Covered by |
|---|---|---|
| I1 — advance blocked by an active stale slot | Unit test | `rcuActiveSlotWithStaleSnapshotBlocksAdvance` |
| I2 — no drain before `tag + 2` | Unit test on bag algebra via `DebugIntrospection` | `rcuNoBagDrainedBeforeTagPlusTwo` |
| I3 — stale snapshot delays, never permits | Unit test with an injected stall between epoch load and activation store | `rcuStaleSnapshotDelaysAdvancementButNeverPermitsIt` (stall injected at `onAfterEpochLoad`) |
| I6 / HAZARD-1 — owner-push never collides with drainer-take | Targeted concurrent test with the stall placed at the exact race window, under TSan | `rcuConcurrentOwnerPushWhileThievesDrain` (stall at `onAfterClaim` — after the claim CAS is won, before the winner touches `head`) |
| `static_assert(kBagCount >= 2)` (P1-I1 as corrected) | Compile time | `EpochDomain.h`, asserted at namespace scope |
| Re-seal hand-off: bound-hit remainder claimed by a *different* thread sees fresh `head` (`kBagReseal`/`kBagClaim` pairing) | Concurrent test under TSan on ARMv8 | `rcuConcurrentResealHandoff` (`drainBatchBound = 1`, so every node is its own claim/drain/re-seal cycle) |
| `synchronize`/`barrier` from deleter context assert (`!inDrain`) instead of hanging | Unit test (`EXPECT_ASSERT_FAILURE` from a deleter) | `rcuBlockingPrimitivesAssertFromDeleterContextInsteadOfHanging` — the expectation is checked **inside** the deleter, so the throw never unwinds through the engine and strands `inDrain` |
| Nesting, incl. simulated interrupt-nested sections | Unit test | `rcuReadLockNestingPublishesOnceAndRetiresOnce` |
| `synchronize` advances exactly two epochs | Unit test | `rcuSynchronizeAdvancesExactlyTwoEpochs` |
| `synchronize` does **not** imply destruction (weaker guarantee is real) | Unit test: retire on slot B, `synchronize` on A, assert B's object not yet destroyed | `rcuSynchronizeDoesNotImplyDestruction` |
| `barrier` destroys all of the caller's own pre-call retirees (incl. still-Open bag) | Unit test | `rcuBarrierDestroysTheCallersOwnRetireesIncludingItsOpenBag` |
| `barrier` excludes remote Open-bag residents, drains remote Sealed bags | Unit test: B retires and goes quiet without sealing; A's `barrier` returns with B's open bag intact | `rcuBarrierExcludesRemoteOpenBagsButDrainsRemoteSealedOnes` |
| `barrier` terminates under continued remote retire traffic (RCU-DEC-036's entry-snapshot bound) | Unit test | `rcuBarrierTerminatesUnderContinuedRemoteRetireTraffic` |
| Batch bound: bound hit mid-bag re-seals; remainder re-claimable; unbounded ≡ `kUnboundedDrainBatch` | Unit test with `drainBatchBound = 1` via `DebugIntrospection` | `rcuDrainBatchBoundResealsRemainderWithTagUnchanged` — note the bound must be set *before* the advance that expires the bag, or `tryAdvance`'s unconditional sweep empties it first |
| `drainAllQuiescent` empties a domain incl. remote Open bags and deleter-retires | Unit test | `rcuDrainAllQuiescentEmptiesEverythingIncludingRemoteOpenBags`, `rcuDrainAllQuiescentCollectsDeleterRetiresTargetingAnySlot` (children retired to a *different* slot than the drainer, per RCU-DEC-037), `rcuDrainAllQuiescentWithAnActiveSectionAsserts` |
| Quiescence check fires on a non-quiescent domain | Unit test via `DebugIntrospection::assertQuiescent()` with `EXPECT_ASSERT_FAILURE` — **not** through `~EpochDomain`: a throwing assert cannot escape a `noexcept` destructor (subagent review; the destructor calls the same internal check) | `rcuQuiescenceCheckFiresOnANonQuiescentDomain` (both the objects-in-limbo and the active-section cases) |
| Retire from inside own section (P1-DEC-004) | Unit test asserting the caller's own slot blocks its advance | `rcuRetireFromOwnSectionBlocksOwnAdvance` |
| Deleter that itself retires — incl. from a `synchronize`/`barrier` sweep, outside any section (RCU-DEC-038) | Unit test | `rcuDeleterThatItselfRetires` — also pins RCU-DEC-031's coverage boundary: the children retired *during* the barrier are deliberately **not** collected by it |
| Quiet-system residue via stealing: A's **sealed** bags drain on B's calls; A's open bag is the exact residue (I13, ITEM-014) | Unit test: slot A retires with a choreographed rotation, then only slot B ever calls `tryAdvance` (see P3-DEC-007 for the choreography — "assert *all* A's bags drain" fails on a correct build) | `rcuQuietSlotSealedBagsDrainElsewhereAndOpenBagIsTheResidue` |
| `onPreTouch` fires exactly once per drained node (RCU-DEC-017) | Unit test with a counting `Hooks` | `rcuOnPreTouchFiresOncePerDrainedNode` |
| Rotation leaves exactly one `Open` bag per slot at all times (the structural basis of I6) | Unit test across more rounds than `kBagCount` | `rcuRotationKeepsExactlyOneOpenBagPerSlot` |
| Encoding round trips; a zero-initialized slot array is a valid empty domain (P1-I3) | Unit test + `static_assert` | `rcuStateWordAndBagEncoding`, `rcuZeroInitialisedSlotArrayIsAValidEmptyDomain` |
| Contract violations assert rather than corrupt | `EXPECT_ASSERT_FAILURE` negative tests | `rcuRetireOutsideASectionAsserts`, `rcuOutOfRangeSlotAsserts`, `rcuUnbalancedReadUnlockAsserts`, `rcuSynchronizeAndBarrierFromInsideASectionAssert` |
| No use-after-grace-period under real concurrency | Mini-rcutorture: readers dereference a protected pointer while writers swap and retire; the deleter genuinely `delete`s, so a UAGP is a hard ASan trap | `rcuConcurrentReadersAndWriters` |
| No data races | TSan on ARMv8 | All three concurrent tests; zero reports |
| Header is valid in a freestanding kernel TU under `-Werror` (P1-DEC-016) | Cross-compile probe, `-DDEBUG_BUILD` and release | `x86_64-crocos-g++ -ffreestanding -nostdlib -mcmodel=kernel -Werror`; kernel target also builds and links |

## Testing Approach

`tests/core/RcuEpochDomainTest.cpp`, plus `tests/core/RcuConcurrentTest.cpp` for the multithreaded
cases. First two includes must be `#include "../test.h"` then `#include <harness/TestHarness.h>`.

Because the engine has no kernel dependencies, these are plain Core tests — no mocks, no
`bindThreadToCpu`, no shadowed headers. Slots are a stack or heap array in the test; one
`std::thread` per slot index.

**Both source lists must be updated:** `CoreTests` (`tests/core/CMakeLists.txt:12-32`) and
`CoreTestsTSan` (`:166-184`) are maintained separately, and updating only one silently drops
coverage from the release gate.

Concurrent tests use `TEST_WITH_TIMEOUT_NO_TRACKING` so the leak tracker does not attribute
`std::thread` machinery to the test. The harness already scales per-test timeouts ×10 under TSan
(`tests/harness/TestHarness.cpp:66-75`).

**Note on running:** `run_all_tests` does **not** include `CoreTestRunnerTSan`, so `unit_tests`
alone will not exercise this phase under TSan. Use `run_core_tests`, which runs both.

## Implementation Phases

**All seven steps completed 2026-08-01.** Shipped as
`libraries/Core/include/core/rcu/EpochDomain.h` (engine) and
`.../core/rcu/DebugIntrospection.h` (test window), with
`tests/core/RcuEpochDomainTest.cpp` (26 tests) and `tests/core/RcuConcurrentTest.cpp`
(3 tests). Both `CoreTests` and `CoreTestsTSan` source lists were updated, as the
Testing Approach warns. `core/TypeTraits.h` gained `is_constructible_v` /
`is_default_constructible_v` (P1-DEC-016). Six decisions were forced during
implementation — P1-DEC-010 through P1-DEC-016 — of which P1-DEC-010 (rotate-first)
is the only one that changes the *algorithm* rather than its packaging.

1. **Skeleton + orderings.** Header, namespace, guard, file-header design block, the **full
   post-review** ordering-constant set — including `kRetireFence`, `kSweepEpochLoad`,
   `kSynchronizeFence`, `kProtectedLinkLoad`, and `kStatePublish` at **RELEASE** not RELAXED —
   plus `RetireHead`, `ReaderSlot`, the `Hooks` parameter, and the `static_assert`s.
2. **Read side.** `readLock` / `readUnlock` with nesting, documenting the caller's
   no-reentry precondition. Unit tests for nesting and state encoding.
3. **Epoch advance.** `tryAdvance` with the exact-match check and both fences. **The epoch load
   must be sequenced before the fence (I10)** — comment it at the site, since reversing it breaks
   safety without changing any constant. Unit tests for I1 and I3, including the injected-stall
   case.
4. **Bag state machine.** The four states, seal, claim, the pop-run-pop drain with the
   `drainBatchBound` re-seal exit (RCU-DEC-033, amended I13), reopen, plus the per-node
   `onPreTouch` hook call. This is the HAZARD-1 phase; P1-ITEM-001 is resolved (RCU-DEC-027).
5. **`retire` + `sweepExpired`.** `kRetireFence` before the epoch load; the caller-is-pinned
   debug assert; bag selection via `openBagIndex`; the advance threshold. Unit tests for I2, the
   bag algebra, the stealing/residue case, and the batch-bound re-seal.
6. **Completion primitives.** `synchronize`: `kSynchronizeFence`, then spin on `tryAdvance` until
   the epoch has advanced twice past entry. `barrier`: assert `!inDrain`, seal-**and-rotate** own open bag, snapshot
   `e0`, then advance-and-sweep until `globalEpoch ≥ e0 + 2` and no **`Sealed`/`Claimed`** bag
   tagged `≤ e0` is non-empty (RCU-DEC-031, RCU-DEC-036 — both the "until globally empty" form
   and the unqualified-state form livelock).
   `drainAllQuiescent`: full quiescence assert (incl. no `Claimed` bag), `inDrain` set,
   universal-owner force-seal, drain to empty (RCU-DEC-035, RCU-DEC-037). `~EpochDomain`
   quiescence assert (RCU-DEC-034).
7. **`DebugIntrospection.h`** and the concurrent test file; TSan green on ARMv8.

## References

- **Implementation:** `libraries/Core/include/core/rcu/EpochDomain.h`,
  `libraries/Core/include/core/rcu/DebugIntrospection.h`,
  `tests/core/RcuEpochDomainTest.cpp`, `tests/core/RcuConcurrentTest.cpp`,
  `tests/core/CMakeLists.txt` (both source lists),
  `libraries/Core/include/core/TypeTraits.h` (`is_default_constructible_v`).
- `specs/rcu.md` — parent. RCU-DEC-001 through RCU-DEC-040; invariants I1–I14 (I13 as amended by
  RCU-DEC-033; I14's enforcement enumeration); the ordering table incl. `kBagReseal`.
- `libraries/Core/include/core/atomic/TreiberStack.h` — house style, policy-struct layering, the
  `-Wtemplate-body` workaround, and the DEC-042 ordering-constant presentation.
- `libraries/Core/include/core/atomic.h` — `Atomic<T>` (`:461`), `thread_fence` (`:455`),
  `compare_exchange` (`:486`), `exchange` (`:583`).
- K. Fraser, *Practical Lock-Freedom*, ch. 5.
