---
kind: subsystem
status: drafting
parent: ~
components:
  - specs/rcu-phase-1.md
  - specs/rcu-phase-2.md
  - specs/rcu-phase-3.md
---

# RCU — Safe Memory Reclamation

> An epoch-based reclamation framework providing wait-free read-side critical sections and
> deferred object destruction, correct under a tickless kernel with no scheduler and no
> reliance on IPIs.

## Terminology

| Term | Meaning |
|---|---|
| **Domain** | One independent grace-period universe. Each protected data structure gets its own (R6). |
| **Slot** | One CPU's per-domain reader state: activation word plus its limbo bags. One slot per possible CPU. |
| **Epoch** | A monotonically increasing 64-bit per-domain counter. Advances only when every active slot has caught up. |
| **Read-side critical section** | The region between `readLock` and `readUnlock`. An object loaded inside a section is guaranteed not to be recycled before the section ends. |
| **Retire** | Hand an unlinked object to the framework for destruction after a grace period. Does not destroy it. |
| **Limbo bag** (bag) | A per-slot, epoch-tagged intrusive list of retired-but-not-yet-destroyed objects. |
| **Drain** | Take a bag's contents and run the deleters. Performed by the owning CPU *or by any other CPU* (RCU-DEC-006). |
| **Grace period** | The interval after which every section that could have held a pointer to a retired object has ended. Here: two epoch advances. |
| **UAGP** | Use-after-grace-period — the bug class this framework exists to prevent. |

## Requirements and Constraints

Carried verbatim from `RCU-proposal.md` §1 and §3, which this spec supersedes. They are cited
throughout the Decisions table, so they belong here rather than in a superseded document.

**Hard constraints — properties of the project, not negotiable in this design:**

- **C1 — Tickless kernel.** No periodic timer interrupt. Grace-period detection cannot be driven
  by tick-time sampling of per-CPU state. Idle CPUs may sit in `hlt` indefinitely and must not
  stall grace periods.
- **C2 — No scheduler dependency for correctness.** There is no scheduler, and when one exists
  the framework may consume *hints* but must remain correct without it. Context switches cannot
  be the quiescent-state signal.
- **C3 — Testable in isolation on the dev machine.** Must build and run in the userspace harness
  (ARMv8 M1, TSan as the primary release gate, ASan alongside).
- **C4 — Freestanding kernel C++26.** No std library, `Atomic` plumbing, no exceptions/RTTI,
  camelCase naming, debug-only runtime checks.
- **C5 — IPIs as garnish, never load-bearing.** Correctness must hold unchanged if any IPI is
  dropped, delayed indefinitely, or never sent. *(RCU-DEC-007 satisfies this in its strongest
  form: no IPI exists in the design at all.)*

**Requirements on the framework:**

- **R1 — Read side is cheap and wait-free.** O(1) CPU-local work per section — a couple of
  CPU-local atomic ops plus at most one full fence — with no stores to shared cache lines and no
  CAS loops.
- **R2 — Read side is legal in (almost) any context.** Entering and exiting a section must be
  safe from page faults, IRQ handlers, and nested within itself. **Narrowed by RCU-DEC-024:** NMI
  and #MC are excluded. The original R2 said "any context up to and including NMI"; that promise
  is withdrawn.
- **R3 — Grace periods complete without universal participation.** A CPU that is idle, halted, or
  never touches the subsystem must not delay grace periods. Only CPUs *inside* sections may block
  one, and only until they exit.
- **R4 — Deferred reclamation with bounded writer cost.** Writers retire in O(1) CPU-local work;
  the framework invokes the deleter after a grace period. Blocking `synchronize` exists for slow
  paths, but `retire` is the workhorse.
- **R5 — No-ABA-within-a-section guarantee.** A pointer loaded inside a section refers to an
  object whose memory is not recycled until after the section ends. This is what lets a consumer's
  read algorithm skip per-node revalidation.
- **R6 — Multiple independent domains**, so grace periods don't couple across consumers and a
  domain can be constructed and torn down in a unit test.
- **R7 — Engine testable as a pure algorithm** over `Atomic` and an abstract slot array, with zero
  kernel dependencies.
- **R8 — Scheduler/timer integration is additive.** Future hooks may accelerate reclamation but
  must never be required for safety; their absence may cost only latency.

## Non-Goals

- **Not a replacement for TLB freshness.** RCU guarantees an object's memory is not recycled.
  It says nothing about whether *this CPU's* view of that memory is current. The
  `SafePtr` / `ensureTLBEntryFresh` discipline remains mandatory and orthogonal
  (RCU-DEC-011).
- **Not a general reference-counting or ownership system.** Objects have exactly one retiring
  writer. Shared mutable ownership is out of scope.
- **Not sleepable / preemptible.** Read-side sections are non-blocking and CPU-pinned
  (RCU-DEC-009). SRCU-shaped preemptible domains are deferred (see ITEM-004).
- **No reclamation daemon, kernel thread, or timer callback.** Forward progress is pulled from
  the retire path (RCU-DEC-005). This is a structural choice, not a temporary one.
- **No IPI on any correctness path.** IPIs may be added later purely to shorten reclamation
  latency, and the framework must be fully correct without them (C5, RCU-DEC-007).
- **Does not bound memory against a non-terminating reader.** A section that never exits stalls
  reclamation indefinitely. That is a caller-contract violation, detected in debug builds
  (RCU-DEC-013), not designed around.
- **RadixVM itself is out of scope.** This spec covers the reclamation framework only; the
  consumer gets its own spec.

## Consumer Contract

### Read side

```cpp
{
    kernel::rcu::ReadGuard guard(domain);          // enter section
    auto node = kernel::rcu::protect(domain, parent->child);   // SafePtr<Node>
    // ... traverse; every dereference is freshness-checked by SafePtr ...
}                                                   // exit section
```

Any pointer loaded via `protect` inside a section refers to an object whose memory will not be
recycled before the section ends (R5). No revalidation, no version counters, no ABA handling is
required of the consumer — this is the property that keeps the radix tree's read algorithm
simple.

A read-side critical section:

1. **Does not block.** No `synchronize`, no waiting on another CPU's work, no lock acquisition
   that a writer could hold across a grace period.
2. **Is CPU-pinned.** Begins and ends on the same CPU. Trivially true today — there is no
   scheduler and no context switch anywhere in the kernel — and asserted against the
   `preemptionDisabled()` / `cpuPinned()` predicates so the check becomes real for free when a
   scheduler lands (RCU-DEC-009).
3. **Is bounded.** Debug builds stamp entry with `timing::monoTimens()` and warn past a generous
   threshold, naming the stalling slot (RCU-DEC-013).
4. **May nest freely**, including from an interrupt handler that interrupts a section on the
   same CPU (R2) — but **not** from NMI or #MC context, which are forbidden and debug-asserted
   (RCU-DEC-024). This narrows the original R2, which promised legality "in any context up to and
   including NMI". `readLock` / `readUnlock` remain a handful of CPU-local instructions with no
   allocation and no loops; the outermost transition additionally masks maskable interrupts for
   its duration, which is why the NMI carve-out exists.

### Write side

```cpp
{
    kernel::rcu::ReadGuard guard(domain);           // REQUIRED — see RCU-DEC-019
    unlink(parent, child);                          // publish the removal first
    kernel::rcu::retireDestroy(domain, child);      // then retire (issues kRetireFence)
}
```

**The writer must itself be inside a section** (RCU-DEC-019). This is not a formality: the writer
traverses the shared structure to perform the unlink, so an unpinned writer can have `parent`
reclaimed underneath it — a use-after-free in the *writer*. Note this is a different property
from reclamation safety, which RCU-DEC-004 establishes without pinning.

`retire` is O(1) amortized, CPU-local, and never allocates. It may drain expired bags and may
attempt an epoch advance — so it is *not* legal in every context.

`retire`, `drain`, and `tryAdvance` follow vmsmalloc's DEC-014 forbidden-context rule unchanged:
**forbidden in IRQ / NMI / #UD / #DF / #GP / #MC context; conditionally legal in #PF**. The rule
is inherited rather than invented because deleters bottom out in `vmsfree` (RCU-DEC-012). The #PF
carve-out is load-bearing, not incidental: RadixVM legitimately unlinks and retires from the
fault path.

**Completion is three primitives, graded by guarantee** (RCU-DEC-031, RCU-DEC-035).
`synchronize()` promises only that a grace period has elapsed. `barrier()` promises that every
object retired before the call has been destroyed — *except* objects still (at barrier entry) in
other CPUs' `Open` bags. It seals and rotates the caller's own open bag first, so the guarantee
for retires issued *from this CPU* is complete — **per-slot, not per-thread; see RCU-DEC-040 for
the scheduler-era affinity obligation** — and it delivers by actively advancing and sweeping,
waiting only on bags mid-drain by another CPU. Both blocking primitives assert `!inDrain` at
entry: called from a deleter they would otherwise hang undiagnosed against the I14 early-out.
`drainAllQuiescent()` requires the external guarantee that no CPU will touch the domain again,
acts as universal owner (force-sealing every open bag), and drains the domain to empty — it is
the teardown primitive. Consumers pick the lightest correct option; teardown is quiesce →
`drainAllQuiescent()` → destroy, and `~Domain()` asserts quiescence rather than draining
(RCU-DEC-034). Both are forbidden inside a read-side section on the same domain
(deadlock), inside a drain (I14), and under the **strict** forbidden-context mask — no #PF
carve-out: a grace-period wait inside a fault handler spins on other CPUs' progress from a
context that may itself be blocking them. Both are contractually slow-path-only. Interrupt
readers arriving while either primitive spins are explicitly fine — they delay the advance for
the duration of the handler and nothing more (I3).

**Ordering obligation on the writer:** **every** store that makes the object unreachable must be
sequenced before `retire` — not merely "the unlink", since a splice or a two-step removal has
several, and Lemma A needs the one covering whichever location the reader loaded. `retire` then
issues `kRetireFence` itself; the caller does not need its own barrier.

**Publication obligation:** a writer installing a new node must initialize it and publish the link
with RELEASE. This is the other half of what makes `kProtectedLinkLoad = ACQUIRE` meaningful —
without it a reader can observe an uninitialized node.

**Deleters may run on any CPU** (RCU-DEC-006), at any time after the grace period, in the context
of an unrelated CPU's `retire`, and — since RCU-DEC-038 — **outside any read-side section**. They
must not assume CPU or NUMA locality, must not touch retirer-CPU-local state such as
`cpuLocal()`, and everything they dereference is subject to the `SafePtr` freshness discipline —
`onPreTouch` covers each node's `RetireHead` fields, **not** the object body or anything the
deleter reaches through. **A deleter may read, modify, or retire only state reachable solely
from the retired object; it must not load any pointer from the live structure** (RCU-DEC-039 —
no assert can catch a violation; cross-structure cleanup belongs at unlink time, inside the
writer's section).

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| `Atomic<T>`, `thread_fence` (`libraries/Core/include/core/atomic.h`) | Every atomic operation and the two SEQ_CST fences | Yes |
| `VMSubstrate::reservePerDomainStaticBuffer` | NUMA-placed, kernel-lifetime slot storage | Yes |
| `VMSubstrate::make<T>` / `destroy<T>` / `SafePtr<T>` | Node allocation and the terminal deleter | Yes |
| `kernel::cpuLocal()` / `getLogicalProcessorID()` | Slot identity | Yes |
| `kernel::interrupts::inForbiddenContext(mask)` + the policy masks | Forbidden-context asserts | Yes — **landed 2026-08-01**, hoisted out of `vmsmalloc.cpp`'s anonymous namespace with packed counters (RCU-DEC-026) |
| `arch::InterruptDisabler` | Masks the outermost `ReadGuard` transition (RCU-DEC-024/025) | Yes — **fixed 2026-08-01**; previously disabled nothing |
| `arch::processorCount()`, `arch::CACHE_LINE_SIZE` | Slot array sizing and alignment | Yes |
| `kernel::timing::monoTimens()` | Debug-only stall detection | No — debug diagnostics only; needs a userspace mock (none exists) |
| vmsmalloc DEC-014 context rules | Inherited discipline | Yes |
| IPI subsystem | **None.** Explicitly not a dependency | N/A |
| Scheduler | **None.** Explicitly not a dependency | N/A |

## Invariants

- **I1 (advance gate).** The global epoch advances `e → e+1` only after a SEQ_CST-ordered scan
  observed every *active* slot's epoch snapshot to be exactly `e`.
- **I2 (reclaim gate).** A bag tagged `t` is drained only when the global epoch is `≥ t+2`.
- **I3 (exact match).** An active slot whose snapshot is any value other than the current epoch
  blocks advancement unconditionally. Staleness may only *delay* advancement, never permit it.
- **I4 (slot ownership).** A slot's `state` word is written only by its owning CPU. All other
  CPUs only ever load it.
- **I5 (nesting locality).** A slot's `nesting` counter and bag-index bookkeeping are read and
  written only by the owning CPU, and only with interrupts able to nest — never concurrently
  from another CPU. They therefore require no atomics.
- **I6 (bag disjointness).** At any instant, the owner may push only into a bag in state `Open`,
  and a drainer may take only a bag in state `Claimed` that it transitioned from `Sealed`.
  Owner-push and drainer-take therefore never target the same bag. *(This is the resolution of
  HAZARD-1; see RCU-DEC-008.)*
- **I7 — RETIRED by RCU-DEC-027.** It read: *"Bags are indexed `epoch mod kBagCount` with
  `kBagCount ≥ 4`; two epochs mapping to the same index therefore differ by at least 4, which
  exceeds the `+2` expiry threshold, so a bag encountered under a different tag is always already
  expired and safe to drain."* The arithmetic was correct but held only under the unstated premise
  *tag ≡ index (mod kBagCount)* — and linear probing, the obvious fix for ITEM-001, destroys that
  premise while leaving the conclusion licensing an unconditional drain. That combination frees
  objects a live reader still holds. Bag selection is now `openBagIndex` (RCU-DEC-027), so the
  invariant is unnecessary and `kBagCount` is a pure batching tunable rather than a correctness
  floor. Retained here rather than deleted because the retired reasoning is the kind that gets
  reinvented.
- **I8 (no retire-time allocation).** `retire` performs no allocation. The tracking node is
  intrusive to the retired object.
- **I9 (epoch monotonicity).** The global epoch is monotonically non-decreasing and never wraps
  in practice. Note the *effective* width is not 64: the slot state word spends bit 0 on the
  active flag (63 epoch bits) and `bagTagState` spends 2 bits on the state (62 tag bits). All
  comparisons — including `globalEpoch ≥ tag + 2` — must be performed on a consistent width, which
  RCU-DEC-041 pins at **62** (the narrower of the two) and `static_assert`s.
- **I11 (tag dominance).** For every node `n` in a bag tagged `t`, `t ≥ r(n)`, where `r(n)` is the
  epoch value returned by the load sequenced after `n`'s `kRetireFence`. I2's `globalEpoch ≥ t+2`
  gate is sound **only in combination with I11**. Note the asymmetry: a tag *larger* than `r` is
  always safe (it only delays the drain); a tag *smaller* is a UAGP. This is what makes
  RCU-DEC-027's tag-bump sound and the "push into an older open bag" shortcut fatal. It is also
  the precise sense in which a stale (low) epoch read in `retire` is safe — Lemma C bounds every
  unlink-missing reader by `s ≤ r`, so a low `r` errs in the conservative direction.
- **I12 (unit epoch advance).** The global epoch advances **by exactly one, via an RMW, and never
  skips a value** — exempted only inside `drainAllQuiescent` (RCU-DEC-035), whose no-concurrency
  precondition leaves Lemma B with no reader to protect. Lemma B relays through `L_W --fr--> CAS(r+1) --rf--> L_S2`, which requires the
  write of `r+1` to *exist* and to be the one the scan read. A batched advance, a catch-up fast
  path, or a plain `store(e+2)` would let `L_W` and `L_S2` read the same write, breaking the
  coherence chain **with no ordering constant changed** — the same invisible-to-DEC-042 failure
  class as I10.
- **I13 (Free is terminal until the owner acts) — amended by RCU-DEC-033.** Thieves may only
  enter the bag state machine at `Sealed` and only leave at `Claimed → Free` (bag emptied) or
  `Claimed → Sealed` (drain batch bound hit; remainder intact, tag unchanged). Every owner-side
  transition (`Free → Open`, `Open → Sealed`, the tag bump) still has no concurrent writer and is
  a **store, not a CAS** — "no CAS" is the claim; the *ordering* of each store is still whatever
  the RCU-DEC-010 table assigns (`kBagSeal` is RELEASE — a relaxed seal would sever the claimer's
  happens-before to the owner's pushes, the same race class `kBagReseal` fixes on the thief
  side). The owner only touches `Free`/`Open` bags, and a re-sealed bag never passes through
  `Free`. This is why the retire fast path is cheap; hardening those stores into CASes would be
  pure cost.
- **I14 (no drain reentry) — enforcement enumerated 2026-08-01, discipline corrected same day
  (final review F2/F3).** A CPU inside a drain does not enter another. The owner-only `inDrain`
  flag is **set, for its duration, by every site that runs deleters directly**: `sweepExpired`
  (around its drain loop), `maybeRotate`'s manufacture drain, and `barrier`'s manufacture step —
  **`tryAdvance` only checks; it must not set** (if it did, its own unconditional call into
  `sweepExpired` would hit the top-of-function check and the mandatory sweep would silently never
  run). Checked at the top of `tryAdvance` / `sweepExpired` (RCU-DEC-030), which also check the
  domain-global `teardownActive` (RCU-DEC-037). **Reentrancy discipline: a drain site runs its
  drain body only on a clear→set transition and only the setter clears** — concretely,
  `maybeRotate` *skips* manufacture entirely when `inDrain` is already set (its documented
  keep-the-current-bag fallback is always legal), and `barrier` asserts `!inDrain` at entry then
  clears its manufacture-step flag *before* entering the main loop. This closes both the
  unbounded deleter → retire → manufacture-drain recursion and the early-clear bug where a nested
  site's exit clears the outer drain's flag, turning a later legal deleter-retire into a spurious
  RCU-DEC-038 panic. The flag is load-bearing for that assert in both directions.
- **I10 (scan load precedes scan fence).** In `tryAdvance`, the epoch load (`kScanEpochLoad`) is
  **sequenced before** the scan's SEQ_CST fence (`kScanFence`), never after. This ordering is
  load-bearing and non-obvious: the proof that a scan must observe a reader's activation
  (Lemma B, RCU-DEC-004) fires C++20 [atomics.order]/4 with the scan's epoch load on the
  *happens-before-the-fence* side. Reversing the two as a cosmetic cleanup collapses the entire
  safety argument **while changing no ordering constant** — RCU-DEC-010's naming discipline would
  not catch it, and x86-TSO would not fail. Any edit to `tryAdvance` that moves the epoch load
  relative to the fence is a correctness change.

## Concurrency Model

### Slot state word

One 64-bit word per slot, written only by its owner (I4):

```
 bit  0      : active flag
 bits 63..1  : epoch snapshot at the time the outermost section was entered
```

`Inactive` is the all-zero encoding, so a zero-initialized slot array is correctly "no readers"
(matching the zero-init discipline `reservePerDomainStaticBuffer` already provides).

### Read lock / unlock

```
readLock(slot):                                  // outermost entry only
    if (slot.nesting++ != 0) return;             // I5: plain, owner-only
    e = globalEpoch.load(kEpochLoadOnEnter)       // RELAXED
    slot.state.store(makeActive(e), kStatePublish)// RELEASE — RCU-DEC-020, NOT relaxed
    thread_fence(kReaderActivationFence)          // SEQ_CST  <-- the entire hot-path cost

readUnlock(slot):
    if (--slot.nesting != 0) return;
    slot.state.store(kInactive, kStateRetire)     // RELEASE — orders all section reads before
```

The RELAXED-store-then-SEQ_CST-fence shape is deliberate: it is the cheaper half of the
Dekker-style pairing with the scan's fence, and it is what makes I3 sufficient. An arbitrarily
long delay between the epoch load and the activation store is safe precisely because I3 makes a
stale snapshot *block* rather than permit.

### Epoch advance

```
tryAdvance(self):                                 // any CPU, any time, concurrently safe
    if (slot[self].inDrain || teardownActive)      // I14 — no reentry from a deleter;
        return false;                              // RCU-DEC-037 — no fenced-protocol entry
                                                   // mid-universal-owner-mode (final review F1)
    advanced = false
    e = globalEpoch.load(kScanEpochLoad)           // ACQUIRE — MUST precede the fence (I10)
    // Own-slot early-out: if OUR slot is active with a stale snapshot, the scan
    // below is guaranteed to fail, so skip the fence and the O(P) remote-line
    // walk. NOTE: this is NOT P1-DEC-004's forbidden optimization — that one
    // omits the own slot FROM the scan; this one uses the own slot to decide
    // not to scan at all. Skipping an advance is always safe; skipping a slot
    // is not.
    st0 = slot[self].state.load(kScanLoad)
    if (!(isActive(st0) && epochOf(st0) != e)):
        thread_fence(kScanFence)                   // SEQ_CST — pairs with kReaderActivationFence
        blocked = false
        for s in [0, slotCount):                   // never skips self — P1-DEC-004
            st = slot[s].state.load(kScanLoad)      // ACQUIRE
            if (isActive(st) && epochOf(st) != e):  // I3: exact match
                blocked = true; break
        if (!blocked):
            // Exactly +1, never a store, never a skip — I12.
            globalEpoch.compare_exchange(e, e + 1, kEpochAdvance, kEpochAdvanceFailure)
            advanced = true   // CAS failure is benign: another CPU advanced concurrently
    sweepExpired()      // ALWAYS — see below. ALL slots, not just our own (RCU-DEC-006)
    return advanced
```

**The sweep is unconditional, and that is load-bearing.** An earlier draft returned `false` from
inside the scan loop, above the sweep. That silently defeated RCU-DEC-006: while any one CPU
blocked advancement, *no* CPU swept, so bags that had expired before the stall — provably safe to
drain — were held anyway, and the only remaining drain path was the owner's own `reopenBag`, i.e.
owner-only draining. Expiry is gated per bag on `globalEpoch ≥ tag + 2` and has no dependency
whatsoever on this call's advance succeeding. Both independent reviewers found this defect.

`tryAdvance` is idempotent and safe to call from any number of CPUs simultaneously. A losing CAS
is not an error — it means the work was already done.

### Limbo bags and the steal protocol

Each slot owns `kBagCount` bags. Each bag is:

```cpp
Atomic<RetireHead*> head;      // intrusive list of retired objects
Atomic<uint64_t>    tagState;  // (tag : 62 bits, state : 2 bits)
```

`state ∈ { Free, Open, Sealed, Claimed }`. The state machine — and this is the load-bearing part
of the whole design:

| Transition | Performed by | Precondition |
|---|---|---|
| `Free → Open` (tag := e) | **Owner only** | bag index `= e mod kBagCount`, bag empty |
| push onto `head` | **Owner only** | state is `Open` and tag `= e` |
| `Open → Sealed` | **Owner only** | owner has moved to a different epoch; it will never push here again |
| `Sealed → Claimed` | **Owner or any other CPU** | `globalEpoch ≥ tag + 2` (I2) |
| pop-run-pop under `Claimed`, then `Claimed → Free` (emptied) or `Claimed → Sealed` (batch bound hit — RCU-DEC-033) | whichever CPU won the claim | pops are plain ops (`Claimed` grants exclusive `head` ownership, I6); **both exit stores are RELEASE** (`kBagRelease` / `kBagReseal`) — the thief-side exits hand the bag to a future claimer/owner and are *not* covered by I13's owner-only plain-store license |

The `Sealed → Claimed` transition is a CAS, and it is what serializes drainers: exactly one CPU
wins, and only the winner touches `head`. Because the owner pushes only into `Open` bags and
drainers take only `Claimed` bags, the two never collide (I6). This is what closes HAZARD-1: the
window is closed *structurally* by the state machine rather than by timing or by discipline.

### Retire

**Corrected 2026-08-01 (Phase 1 implementation; `rcu-phase-1.md` P1-DEC-010).** The earlier form of
this block performed the I11 tag bump *unconditionally* and only then called `maybeRotate(slot, e)`
— whose own trigger is "`e` exceeds the open bag's tag", which the bump had just made permanently
false. Rotation could never fire, no bag would ever seal, and every retire for the life of the
domain would pile into a single bag that no CPU could ever drain. **The bump is the fallback for a
failed rotation, not a step that precedes it** — which is what RCU-DEC-027 and ITEM-001's
resolution always said in prose. See Hazards.

```
retire(slot, node):
    // The caller's unlink(s) are already sequenced before this point.
    thread_fence(kRetireFence)                     // SEQ_CST — RCU-DEC-018. Unconditional.
    e = globalEpoch.load(kEpochLoadOnRetire)        // ACQUIRE — RCU-DEC-021

    i = prepareOpenBag(slot, e)                    // rotate FIRST, bump only as fallback
    pushNode(slot.bag[i], node)                    // kBagPush = RELEASE
    if (++slot.retireCount crosses threshold) tryAdvance(slot)


// Returns a bag that is Open and tagged >= e. Never blocks, never fails, never
// allocates. Bag selection is openBagIndex, NOT `e mod kBagCount` — RCU-DEC-027.
prepareOpenBag(slot, e):
    loop:                                          // at most twice — manufacture is one-shot
        i      = slot.openBagIndex
        (t,st) = slot.bag[i].tagState              // kBagTagLoad
        if (st == Free):
            slot.bag[i].tagState.store(pack(e, Open))    // kBagOpen
            return i
        assert(st == Open)                         // openBagIndex never designates Sealed/Claimed
        if (t >= e): return i                      // already this epoch's bag — nothing to do

        j = findFreeBag(slot)
        if (j != none):                            // ── ROTATE ──
            slot.bag[j].tagState.store(pack(e, Open))     // kBagOpen  — acquire...
            slot.bag[i].tagState.store(pack(t, Sealed))   // kBagSeal  — ...before release
            slot.openBagIndex = j
            return j                               // NB: sealed at t, NOT at e — expires at t+2

        if (manufactureAllowed && !slot.inDrain):  // I14: never a drain within a drain
            manufactureFreeBag(slot)               // claim + drain OUR OWN expired bags; bounded
            manufactureAllowed = false
            continue                               // openBagIndex may have moved under a
                                                   // deleter-retire that rotated the slot

        // ── FALLBACK: the ONLY place the tag bump happens ──
        slot.bag[i].tagState.store(pack(e, Open))  // bump, never match (I11)
        return i
```

**Naming.** Elsewhere in this spec tree the rotation step is called `maybeRotate` (I14, RCU-DEC-033,
the Failure Modes table, `rcu-phase-3.md` P3-DEC-007). It is folded into `prepareOpenBag` above
because the decision it makes and the bump it falls back to cannot be separated without
reintroducing the defect; every statement made about `maybeRotate` elsewhere applies to the rotate
branch here unchanged.

Rotation is *opportunistic and never blocking*. Three properties are load-bearing:

- **Acquire before release.** The replacement bag is opened *first*, so the owner is never left
  without an open bag — and in particular `openBagIndex` never designates a `Sealed` bag, which is
  the I11-fatal "push into a sealed bag" case.
- **The old bag is sealed at its own tag `t`, not at `e`.** Every node in it has `r(n) ≤ t` (each
  push either matched the tag or bumped it), so sealing at `t` satisfies I11 and the bag expires at
  `t+2` rather than `e+2`. Sealing at `e` is also *correct*, just needlessly late.
- **The fallback never fails.** If no bag is `Free` and none can be manufactured, the owner keeps
  its current bag one epoch longer by bumping the tag — safe by I11, since raising a tag only ever
  delays a drain. Degradation under sustained pressure is therefore one bag growing with a rising
  tag: reclamation *latency*, never blocking.

`manufactureFreeBag` claims and drains the slot's *own* expired `Sealed` bags to make a `Free` one.
It is **skipped entirely when `inDrain` is already set** (I14 discipline: a deleter's retire must
not open a drain within a drain), **losing a claim CAS to a thief is a no-op**, and it respects
`drainBatchBound` (RCU-DEC-033 — it sits on the retire/#PF fast path, the exact latency spike that
bound targets). In every failure mode the owner simply falls through to the tag bump. Note the
`continue`: a deleter run by the manufacture drain may itself retire and rotate the slot, so the
decision has to be re-derived rather than resumed from stale locals.

Forward progress is driven entirely from this path (RCU-DEC-005) — the DEC-036 pattern vmsmalloc
already uses. No daemon, no tick, no IPI.

### Why memory stays bounded with no scheduler input

This is the requirement that drove RCU-DEC-006, and it deserves an explicit argument.

Let *U* be the set of retired-but-unreclaimed objects.

1. *U* grows only via `retire`. Every `retire` is also an opportunity to drain and to advance, so
   growth carries its own remedy.
2. A bag stops accepting pushes once sealed, and becomes claimable as soon as the epoch passes
   `tag + 2`. Advancement is blocked only by an active reader with a stale snapshot (I3), and by
   contract every section terminates.
3. **Crucially, a sealed expired bag is drainable by *any* CPU** (RCU-DEC-006). So a CPU that
   retires a pile of objects **into bags that have sealed** and then goes permanently idle does not strand them: the next
   `tryAdvance` on *any* CPU sweeps them.

Hence on a system that is merely *lightly* used — any CPU still calling into the domain
occasionally — residue converges to the objects in the owners' currently-`Open` bags, because
every call sweeps every slot. Under the owner-drains-only design the bound would instead have
been O(*P* × threshold) held permanently, with no mechanism short of an IPI to recover it.

**Precision, because the difference matters.** On a system that goes *completely* quiet — no CPU
ever calls `retire`, `tryAdvance`, `drain`, or `synchronize` on this domain again — nothing
advances and nothing sweeps, so the last sealed and `Open` bags are held indefinitely. That
residue is bounded by O(*P* × `kBagCount` × threshold) and is a fixed high-water mark, not a
growing leak: a system doing no work creates no new garbage. Stealing does not eliminate this
case, it eliminates the far worse one where a *single* idle CPU strands memory while the rest of
the system runs. Claims that residue reaches literally zero should be read with that
qualification — see ITEM-014.

The one genuinely unbounded case is a non-terminating read-side section, which is a contract
violation (see Non-Goals) and is surfaced by RCU-DEC-013's stall detector.

### Memory ordering constants (RCU-DEC-010)

Every atomic site references these by name, and re-cites the ordering inline at the call site.
Any downgrade is a one-point edit and an audit trigger. This mirrors vmsmalloc's DEC-042 policy
verbatim; the ARMv8/TSan release gate is what makes it load-bearing rather than cosmetic.

| Constant | Order | Site |
|---|---|---|
| `kEpochLoadOnEnter` | `RELAXED` | epoch snapshot at section entry — the following fence does the work |
| `kStatePublish` | `RELEASE` | activation store. **Not RELAXED** — see RCU-DEC-020 |
| `kReaderActivationFence` | `SEQ_CST` | full fence after activation — **do not downgrade** |
| `kStateRetire` | `RELEASE` | deactivation store at section exit |
| `kProtectedLinkLoad` | `ACQUIRE` | the protected link load inside `protect` — see RCU-DEC-022 |
| `kScanEpochLoad` | `ACQUIRE` | epoch load at the top of `tryAdvance`. **Must be sequenced BEFORE `kScanFence`** — see I10 |
| `kScanFence` | `SEQ_CST` | full fence before the slot scan — pairs with the activation fence |
| `kScanLoad` | `ACQUIRE` | per-slot state load during the scan |
| `kEpochAdvance` | `ACQ_REL` | epoch CAS success. The RELEASE half is load-bearing for RCU-DEC-021 |
| `kEpochAdvanceFailure` | `RELAXED` | epoch CAS failure |
| `kRetireFence` | `SEQ_CST` | **full fence sequenced between the caller's unlink and the retire-path epoch load** — see RCU-DEC-018 |
| `kEpochLoadOnRetire` | `ACQUIRE` | epoch load on the retire path. ACQUIRE is required by RCU-DEC-021, not by RCU-DEC-018 |
| `kSweepEpochLoad` | `ACQUIRE` | epoch load driving the expiry decision in the sweep — see RCU-DEC-021 |
| `kSynchronizeFence` | `SEQ_CST` | full fence before `synchronize`'s epoch read — see RCU-DEC-023 |
| `kBagTagLoad` | `ACQUIRE` | bag `tagState` load |
| `kBagSeal` | `RELEASE` | `Open → Sealed` |
| `kBagClaim` | `ACQ_REL` | `Sealed → Claimed` CAS success |
| `kBagClaimFailure` | `RELAXED` | `Sealed → Claimed` CAS failure |
| `kBagRelease` | `RELEASE` | `Claimed → Free` |
| `kBagPush` | `RELEASE` | push onto a bag head |
| `kBagPushFailure` | `RELAXED` | push CAS retry |
| `kBagTake` | *retired by RCU-DEC-033* | Was `head.exchange(nullptr)`; the pop-run-pop drain reads `head` with plain ops under `Claimed` exclusivity (I6), visibility riding `kBagClaim` / `kBagReseal` / `kBagRelease` on `tagState` |
| `kBarrierEntryFence` | `SEQ_CST` | In `barrier`, sequenced after the seal-and-rotate and before the `e0` snapshot load (RCU-DEC-036) — the RCU-DEC-023-class guarantee for the `unlink(); barrier(); reuse()` idiom. The termination predicate's per-bag scans use `kBagTagLoad` |
| `kBagReseal` | `RELEASE` | The `Claimed → Sealed` re-seal store on batch-bound hit (RCU-DEC-033). Pairs with the next claimer's `kBagClaim` ACQUIRE — **plain is a data race**: the next claimer would read a stale remainder `head` and re-run freed deleters; the tagState value is bit-identical before/after the drain, so coherence rescues nothing (subagent review 2026-08-01) |

### Why no ABA tag on the bag head

Bag heads are plain `Atomic<RetireHead*>` with no counter, unlike `Core::TreiberStack`. The
justification: a node cannot reappear as a bag head until it has actually been reclaimed and
reallocated, and by then the epoch has moved on, so it lands in a different bag. Within one
bag's `Open → Free` lifetime the same address can appear at most once.

This matters concretely rather than aesthetically: the kernel target has **no lock-free 16-byte
CAS**, which is why vmsmalloc had to invent a packed 64-bit head encoding
(`kernel/mm/vmsmalloc.cpp:81-104`) rather than use `Core::Uint128HeadEncoding`. Avoiding the tag
avoids inheriting that whole problem. See ITEM-003 — this argument must survive adversarial
review, because if it is wrong the fix is expensive.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| Reader section never exits | Epoch stops advancing; retired memory accumulates. Reads remain correct. Debug build warns with the stalling slot ID after a threshold. | No (caller bug) |
| `retire` called from forbidden context | Debug: assertion failure naming the context. Release: undefined — deleters may reenter vmsmalloc on the same CPU's magazine. | No (caller bug) |
| `synchronize` called inside a section on the same domain | Debug: assertion failure. Release: deadlock (spins forever waiting for itself). | No (caller bug) |
| Object retired twice | Debug: assertion on the intrusive head's already-linked state. Release: bag list corruption. | No (caller bug) |
| Object retired before being unlinked | UAGP — a reader may still reach it through the live structure. Not detectable by the framework. | No (caller bug) |
| All `kBagCount` bags simultaneously non-`Free` on retire | `maybeRotate` finds no `Free` bag and keeps the current open bag one epoch longer (tag-bump safe by I11); it may claim-and-drain its own expired `Sealed` bags to manufacture a `Free` one, losing that CAS to a thief harmlessly. Never blocks, never fails, never allocates (RCU-DEC-027; **corrected 2026-08-01** — this row previously described the retired pre-DEC-027 index-matching design and cited retired I7). | Yes (by construction) |
| `tryAdvance` CAS lost to a concurrent advancer | Benign. The advance happened; the caller proceeds to sweep. | Yes |
| Epoch wraparound | 62 effective bits (RCU-DEC-041) at grace-period rate — unreachable. No handling; debug builds assert `e ≤ kMaxEpoch` in `retire` so an impossible value is loud rather than silently truncated. | N/A |
| Maskable interrupt arrives inside the outermost `readLock` publish window | Impossible — `ReadGuard` holds `arch::InterruptDisabler` across the transition (RCU-DEC-024), and the window is fault-free by construction so no exception can fire either. | N/A (prevented) |
| NMI or #MC handler enters a read-side section | Contract violation. Debug: assert via `inForbiddenContext(kRcuReadSideForbiddenDepthMask)`. Release: the nested section runs unprotected — silent UAGP. | No (caller bug) |
| Thief dereferences a stolen bag node through a stale TLB entry | `onPreTouch` fires once per node before any read of that node's `RetireHead` fields (RCU-DEC-017), so the read is safe. A `Hooks` policy that omits the freshness call is a Phase-2 bug, not a design gap. | N/A (prevented) |
| `synchronize` returns while the caller's retired objects are still undrained | **Specified and intended** (RCU-DEC-031): `synchronize` promises only the grace period. A caller wanting destruction uses `barrier` (own retirees, per RCU-DEC-040) or `drainAllQuiescent` (teardown). | Yes (by contract) |
| `Domain` destroyed with objects still in limbo | Debug: destructor quiescence assert fires (RCU-DEC-034). Release: objects leak, deleters never run. The sanctioned teardown is quiesce → `drainAllQuiescent()` → destroy (RCU-DEC-035). | No (caller bug) |
| Sweep runs an unbounded number of deleters on the caller's CPU | Accepted latency cost of RCU-DEC-006. See ITEM-011. | Yes |
| Domain used before `init()` | Debug: every veneer entry asserts the `initialized` flag (P2-DEC-009 — with opaque engine storage there is no engine pointer to null-check). Release: the veneer launders storage holding no object — UB; observed behavior on zeroed storage is a null slot-array deref or vacuous scans, i.e. reclamation with no reader protection. | No (init-order bug) |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| ITEM-001 | Resolved 2026-08-01 → RCU-DEC-027 | | | Is the `Free/Open/Sealed/Claimed` state machine minimal, and is the owner's `reopenBag` fallback wait-free when another CPU holds a `Claimed` bag at the same index? | The owner cannot block. Candidate resolutions: (a) owner scans forward for any usable index, (b) owner pushes to an overflow bag, (c) prove the collision cannot occur given `kBagCount ≥ 4`. Must be settled before Phase 1 code. |
| ITEM-002 | Resolved 2026-08-01 → RCU-DEC-029 | | | Does the two-fence pairing (`kReaderActivationFence` / `kScanFence`) need a herd7 AArch64 litmus proof, or is the manual case analysis in RCU-DEC-004 sufficient? | Recommendation: litmus. The protocol core is small, ARMv8 is the release gate, and TSan exercises but does not prove fence pairings. |
| ITEM-003 | Resolved 2026-08-01 → RCU-DEC-028 | | | Is the no-ABA-tag argument for bag heads airtight under `retire`-from-inside-a-section and under a deleter that itself retires? | If it fails, the fallback is a packed 64-bit encoding in the style of vmsmalloc DEC-015 — expensive, so this needs adversarial review rather than assertion. |
| ITEM-004 | Deferred | No | | Should a preemptible (SRCU-shaped, A3) domain flavor be offered for sleepable sections? | Only if a future consumer needs them. The consumer-facing API is engine-agnostic by design, so this is additive. |
| ITEM-005 | Deferred | No | | Values for `kBagCount` and the retire-count advance threshold. | `kBagCount = 4` provisional (I7 needs ≥ 4). Threshold ~64 provisional. Real values need a RadixVM-shaped workload — the vmsmalloc Phase 10 lesson: do not tune before a representative workload exists. |
| ITEM-006 | Deferred | No | | Should `tryAdvance` sweep *all* slots' expired bags on every call, or a bounded number per call? | Full sweep is O(slotCount × kBagCount) tag loads. Cheap at 8-16 CPUs; revisit only if slot counts grow. Note ITEM-011 raises the sharper version of this: the cost that matters is running the *deleters*, not loading the tags. |
| ITEM-007 | Resolved 2026-08-01 → RCU-DEC-018, RCU-DEC-019 (fence in `retire`; pinning required but for different reasons) | | | **What ordering must the retire path carry so that a reader pinning at `r+1` or later is guaranteed to observe the unlink?** Sub-question: must `retire` be called from inside a read-side section, as every reference EBR implementation (Fraser; crossbeam-epoch's `defer_destroy(&Guard)`) requires? | **Analysis 2026-08-01.** Readers pinned at snapshot ≤ `r` are covered: I3's exact match makes them block the `r+1 → r+2` advance. The exposure is a reader pinning at `r+1` or later, which does *not* block that advance — so safety requires such a reader to see the unlink. The happens-before chain that would guarantee it is: unlink → [writer does something with RELEASE] → [scanner ACQUIREs it] → scanner's epoch CAS (RELEASE) → reader's epoch load (ACQUIRE) → reader's link load. **With an unpinned writer the second link does not exist** — the writer owns no slot, so the scan reads nothing the writer wrote, and the chain is severed. Pinning supplies a slot but does *not* by itself close it: if the scan observes the writer still *active*, the value it acquires was stored at pin time, i.e. *before* the unlink. **The load-bearing requirement appears to be fence placement, not pinning:** with an SC fence (or an SC-ordered epoch load) between the unlink and the retire's epoch read, assume a reader pins at `r+1` without seeing the unlink; then in E's modification order the writer's load (`r`) precedes the scanner's CAS (`r+1`), which precedes the reader's load (`r+1`), forcing `F_writer <sc F_reader`, and the SC-fence property then forbids the reader's later load from reading a value older than the unlink — contradiction. **This means `kEpochLoadOnRetire = RELAXED` is itself the defect.** Recommended resolution: (a) strengthen `kEpochLoadOnRetire` to `SEQ_CST` or insert an explicit fence, *and* (b) require pinning anyway, since it is canonical, costs RadixVM nothing (writers already traverse), and independently bounds the epoch to `{w, w+1}` while the writer holds its section. The final SC-order step in (a) is exactly the kind of reasoning that should be machine-checked rather than trusted — see ITEM-002. Must be settled before Phase 1 step 3. |
| ITEM-008 | Resolved 2026-08-01 → RCU-DEC-024 (mask interrupts across the outermost transition; forbid NMI/#MC sections) | | | The `readLock` / `readUnlock` nesting sequence is not atomic with respect to interrupts on the same CPU. How is it made so? | Concrete failure: `readLock` does `if (slot.nesting++ != 0) return;` and only then publishes. An interrupt in that window finds `nesting != 0`, returns immediately, and runs its "protected" traversal with the slot still **inactive**. Reordering to publish-then-increment fails symmetrically. I5's "naturally serialized" claim was the error — serialized is not atomic. Resolved per user direction; see RCU-DEC-024. |
| ITEM-009 | Resolved 2026-08-01 → RCU-DEC-017 (onPreTouch hook) | | | A CPU draining a *stolen* bag walks an intrusive list living in another CPU's retired slab memory. Does each `RetireHead->next` read need `ensureTLBEntryFresh` first? | Yes. Resolved per user direction: reuse the `ChainedTreiberStack` hook pattern verbatim. See RCU-DEC-017. |
| ITEM-010 | Resolved 2026-08-01 → RCU-DEC-031 (`synchronize` = grace period only; `barrier` = drive-to-completion destruction), RCU-DEC-032 (execution-context assumptions) | | | Does `synchronize` guarantee that the caller's previously retired objects have been **destroyed**, or only that a grace period has **elapsed**? | These differ: two epoch advances can complete while the caller's bag is sealed-but-unswept. Linux's `synchronize_rcu` promises only the grace period; `rcu_barrier` is the separate primitive that waits for callbacks. Consumers doing teardown almost certainly want the stronger one. Resolved per user direction: mimic Linux's *split*, not the sweep-in-`synchronize` recommendation originally recorded here — the consumer chooses the lightest correct option. |
| ITEM-011 | Resolved 2026-08-01 → RCU-DEC-033 (node-granular batch bound; `Claimed → Sealed` early exit; I13 amended) | | | Should the number of deleters run per sweep be bounded? | A CPU calling `tryAdvance` from its retire path sweeps *every* slot (RCU-DEC-006) and may inherit thousands of deleters queued by other CPUs, each bottoming out in `vmsfree`. That is an unbounded latency spike on a path a page-fault handler may sit on. Bounding the batch trades residue for tail latency. Interacts with ITEM-006, and with RCU-DEC-032: under a future scheduler the batch bound doubles as the preemption-latency bound, so one knob resolves both. |
| ITEM-012 | Resolved 2026-08-01 → RCU-DEC-034 (destructor asserts quiescence; never drains, never waits) | | | What are the semantics of destroying a `Domain` that still has retired-but-undrained objects? | Phase 2 forbids dynamic teardown in the kernel, but R6 explicitly wants domains constructible and destructible in unit tests, and Phase 3 will do exactly that between scenarios. Candidates: assert the domain is quiescent and all bags empty; force a full drain in the destructor; leak deliberately. Without a decision the torture suite's leak tracker will make the choice by accident. Teardown shape settled as quiesce → `drainAllQuiescent()` (RCU-DEC-035) → destroy, with the destructor asserting rather than draining (RCU-DEC-034). |
| ITEM-013 | Resolved 2026-08-01 → RCU-DEC-041 | | | Pin the effective epoch width and the exact comparison used for expiry. | I9 as originally written said 64 bits; the state word actually carries 63 and `bagTagState` 62. `globalEpoch ≥ tag + 2` compares a 62-bit truncated tag against a wider counter. Unreachable in practice, but the widths should be stated and `static_assert`ed rather than left implicit. Pinned during Phase 1 implementation; see also `rcu-phase-1.md` P1-DEC-014. |
| ITEM-015 | Open | No | | Should bags be opened **untagged** and tagged at *seal* time (crossbeam's shape) instead of tagged at open time? | RCU-DEC-018 costs one `DMB ISH` per `retire` because each `retire` re-reads the epoch. crossbeam pushes into an untagged local bag and assigns the tag at seal, so one fence covers every object in the bag. Sound either way; purely an amortization question, and only worth doing if the per-retire fence shows up in a profile. **Explicitly forbidden shortcut:** skipping the fence when the freshly-read epoch already equals the open bag's tag. That leaves the second object's unlink un-fenced relative to the tag and reintroduces ITEM-007. This is exactly the plausible-looking refactor RCU-DEC-010's discipline is meant to catch, so it is recorded here rather than left to be rediscovered. **Re-examined 2026-08-01 against RCU-DEC-027 and the shipped Phase-1 code; stays Open.** RCU-DEC-027's rationale claimed to answer this item "for free"; it does not, and that clause has been corrected. What RCU-DEC-027 changed is *bag selection* (`openBagIndex` rather than `epoch mod kBagCount`) and *tag assignment* (bump rather than match) — a different axis. The cost this item is about is untouched: `retire` still executes `kRetireFence` and re-reads the epoch **unconditionally on every call**, exactly as RCU-DEC-018 requires and as the Hazards entry on the "skip the fence when the epoch already matches the tag" shortcut forbids weakening. The convergence is only that a bag's *final* tag is now settled at seal time; crossbeam's actual win is that the bag is opened **untagged** so `retire` never needs the epoch at all, and that is the half not adopted. What RCU-DEC-027 *did* buy this item is real but narrower: under `epoch mod kBagCount` tag-at-seal was structurally impossible, because the bag index was itself epoch-derived and so the epoch was needed at open time regardless; now nothing but the tag needs it, so the restructuring is available whenever it is wanted. **Trigger caveat:** the "only worth doing if the per-retire fence shows up in a profile" condition is harder to reach than it reads — per RCU-DEC-025 the QEMU/TCG profiler counts basic-block executions and models neither privileged-instruction cost nor serialization, so a `DMB ISH` is invisible to the only profiler this project has. Deciding this item will need real hardware measurement or an analytic argument, not a profile. |
| ITEM-016 | Open | No | | Does ITEM-002's litmus suite include a **negative control**? | Review's recommendation, and it is the test that matters most: test 2 (the full protocol) with `kRetireFence` **removed** must come back **allowed** on AArch64. If it comes back forbidden, the model or the encoding is wrong and the positive results prove nothing. Distinguishes "the fence is load-bearing" from "herd7 agreed with me". Also: the model must be RC11/C++20, not C11 — the C11 model does not forbid the relay and would give a false negative. |
| ITEM-017 | Deferred | No | | Should the I1 advance-gate scan be accelerated by a **sticky participation bitmap** — bit *i* = "CPU *i* has ever entered a section on this domain" — so a domain touched by 4 CPUs on a 64-core machine scans 4 slots instead of 64? | User proposal 2026-08-01, design pinned now because it is cheap to record and expensive to re-derive. **Viable shape:** set-once by the owner before its first activation (a slot-local "my bit is set" flag keeps steady-state entries off the shared line entirely); read by `tryAdvance` as a filter for both the epoch check and the bag sweep; **never cleared** in v1. Safety: skipping a slot is a correctness decision, not a hint — stale-*set* merely delays, stale-*clear* is a UAGP; sticky-set-once makes stale-clear structurally impossible, and visibility rides the existing machinery (bit-set sequenced before the reader's first `F_R`, so the Lemma-B relay that forces a scan to observe the activation forces it to observe the bit). That is a **fourth SC-fence-pairing proof obligation** and must join the ITEM-002 herd7 suite with its own negative control (ITEM-016's discipline). **Forbidden variant, recorded so it is not reinvented:** per-section *activity* bits — a shared line written by every reader on every entry, the same shape RCU-DEC-024 already rejected, worse than the scan it saves. **Known degradation and its pinned fix (user refinement 2026-08-01):** thread migration accumulates bits toward all-set over a process's lifetime. Clearing is made safe by widening to **2 bits per slot** — `NotParticipating` / `Participating` / `PendingClear` (fourth state spare) — turning the clear into a correctly-formed Dekker pairing over fences both sides already execute. Scanner: demote `Participating → PendingClear` freely (both states still scanned; demotion alone never skips anyone); finalize `PendingClear → NotParticipating` only after its SC fence then observing the slot inactive. Reader, outermost entry: activate, `F_R`, then load participation state post-fence; on anything but `Participating`, promote via CAS, **re-fence, and re-snapshot the epoch** before proceeding — the re-snapshot is load-bearing (without it a reader can pin at a stale epoch behind a completed finalize). Skip-safety then mirrors the main theorem: scanner fence first ⟹ observed inactive and the reader's re-snapshot pins co-later; reader fence first ⟹ scanner sees `Participating` restored. Structurally "a second activation word at coarser granularity" — a fifth litmus family for ITEM-002, same shape as the first four. Cost: one post-fence load + predictable branch per outermost entry, inside the RCU-DEC-024 masked window (the word lives in the same pinned domain storage, so the fault-free claim survives); the line is read-mostly, categorically unlike the forbidden per-entry-written variant. **Demotion policy is optional and safety-irrelevant** (any policy is correct; worst case is promote churn) — a domain that never demotes always reads `Participating`, so the check can be config-gated where degradation doesn't matter. Wider fields (4 bits) buy only churn-damping (idle-age saturating counter) — tuning, not capability; noted as extension only. The previously-sketched scheduler context-switch-out clear remains a policy *trigger* option under this protocol rather than a distinct mechanism. **Two obligations from subagent review 2026-08-01:** (i) the participation word must live in `reservePerDomainStaticBuffer` storage (pinned, always mapped) or the RCU-DEC-024 fault-free-window claim breaks — the same audit that caught the `monoTimens` stamp; P2-DEC-002 reserves per NUMA domain, so the shared word sits in one domain's reservation (still pinned; the cross-node access latency is a cost note, not a correctness issue). (ii) The promote path's honest cost is a CAS **plus re-publishing the activation word with the fresh snapshot** — "one post-fence load + branch" describes only the common already-`Participating` path. **Why deferred:** the scan runs once per threshold-crossing retire, and at the target 8–16 cores a full scan is a handful of cache lines (ITEM-006's arithmetic); the win appears at high core counts with per-process domains (RadixVM). Revisit alongside ITEM-006 if slot counts grow or the scan shows in a profile. |
| ITEM-018 | Resolved 2026-08-01 → RCU-DEC-036 (seal-and-rotate; entry-snapshot termination) | | | Pin `barrier`'s exact mechanics: the seal-and-rotate obligation and the termination condition. | Adversarial review 2026-08-01, two sub-defects in the RCU-DEC-031 sketch. **(a) Seal leaves no open bag:** after `barrier` seals the caller's open bag, `openBagIndex` designates a `Sealed` bag; a deleter-retire during `barrier`'s own sweeps would push into it — the I11-fatal "push into a sealed bag". `barrier` must seal-*and-rotate*, manufacturing a `Free` bag first if needed (drain own expired bags; advance until one expires — terminates since the barrier itself drives advances). **(b) The naive loop livelocks:** "sweep until no `Sealed`/`Claimed` bag is non-empty" chases a moving target under sustained remote retire traffic. Correct shape: after seal+fence, snapshot `e0`; drive until `globalEpoch ≥ e0 + 2` **and** no bag tagged `≤ e0` is non-empty — once new opens are tagged `> e0` that set strictly shrinks, so it terminates, and every pre-call retiree outside remote open bags is covered (pre-call sealed bags have tags `≤ e0`). Also: `barrier(slot)` performs owner-side transitions on `slot`, so it carries the same same-CPU binding obligation as `retire`. |
| ITEM-019 | Resolved 2026-08-01 → RCU-DEC-037 (full assert; `inDrain`; any-slot deleter-retires) | | | Pin `drainAllQuiescent`'s interior: the full quiescence assert, `inDrain` suppression, and deleter-retire slot targeting. | Adversarial review 2026-08-01. **(a)** The debug assert must cover *no bag `Claimed`*, not just no active sections — a `Claimed` bag is an in-flight drain, i.e. concurrent use, and universal-owner plain ops on it would race the thief. **(b)** Deleters run during the teardown drain may retire and cross the advance threshold; without `inDrain` set, that re-enters `tryAdvance` mid-universal-owner-mode, mixing the fenced protocol with plain stores. Set `inDrain` (or an equivalent teardown flag) for the duration. **(c)** Deleter-retires during teardown may target any slot (the caller is universal owner; no concurrency exists) and are collected by the continuing drain loop — this also answers how a harness main thread with no bound slot tears down. |
| ITEM-020 | Resolved 2026-08-01 → RCU-DEC-038 (assert becomes `nesting > 0 \|\| inDrain`; RCU-DEC-019 amended) | | | RCU-DEC-019's pinning assert contradicts deleter-retires from sweep paths whose callers are forbidden from being in a section. | Adversarial review 2026-08-01; **pre-existing** (since RCU-DEC-019 + RCU-DEC-030 coexisted: `synchronize` spins on `tryAdvance`, which sweeps, which runs deleters, which may retire — with the caller *required* to be outside any section), widened by `barrier`/`drainAllQuiescent`. Recommended resolution: the assert becomes `nesting > 0 \|\| inDrain`. Principled, not expedient: RCU-DEC-019 itself locates reclamation safety in `kRetireFence`, not pinning; pinning's real justification is writer-traversal safety, and a deleter retiring a child of an already-dead parent traverses nothing live — the parent left the structure ≥ 1 grace period ago. RCU-DEC-019's prose should gain the carve-out explicitly so the exemption is not misread as weakening the writer rule. |
| ITEM-014 | Deferred | No | | Is the completely-quiet-system residue (O(*P* × `kBagCount` × threshold), held indefinitely) acceptable, or is a last-gasp drain wanted? | See §Concurrency Model. It is a fixed high-water mark, not a growing leak, and the natural home for a fix is the idle-loop hook already deferred as `rcu-phase-2.md` P2-ITEM-005. Recording it so the claim "residue reaches zero" is not repeated without its qualification. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| RCU-DEC-001 | Settled | **Epoch-based reclamation (A4) is the engine.** Rejected: tick-driven RCU (A1, needs the tick and the scheduler), explicit QSBR (A2), SRCU-shaped two-phase counters (A3), hazard pointers (A5), per-node refcounts (A6), type-stable + version revalidation (A7), seqlocks (A8). | A4 is the only candidate that needs nothing from tick or scheduler, leaves uninvolved CPUs entirely out of the protocol, keeps the consumer's reader contract trivial, and reduces to a small slot-array-plus-atomics algorithm testable in userspace. A3 is the viable runner-up, rejected only because its migration tolerance buys nothing under the CPU-pinned reader contract while its sum-balance safety argument is materially harder to model-check. A2 was rejected as anti-modular: correctness of the radix tree would hinge on every unrelated long-running kernel loop remembering to announce quiescence. A5 costs a store+fence *per node visited* — wrong trade for a deep read-mostly traversal. Decided 2026-08-01. |
| RCU-DEC-002 | Settled | **Engine in `Core`, binding in the kernel.** `Core::rcu` holds the pure algorithm over `Atomic` and an abstract slot array with zero kernel includes; `kernel::rcu` supplies storage, slot identity, context asserts, and the `SafePtr` integration. | Mirrors the proven `TreiberStack` split (Core algorithm + kernel-supplied policy structs). It is what makes R7 — engine testable as a pure algorithm in userspace under TSan — achievable at all. Decided 2026-08-01. |
| RCU-DEC-003 | Settled | **Consumer-facing API names no epochs.** `Domain` / `ReadGuard` / `protect` / `retire` / `synchronize`. | An A2- or A3-shaped engine could back a `Domain` later without touching a single consumer call site (R8). Naming the mechanism in the API would foreclose that. Decided 2026-08-01. |
| RCU-DEC-004 | Settled (**rewritten 2026-08-01** — the original argument was unsound, not merely under-argued) | **Safety rests on I1 + I2 + I3 + I10 and *three* SC fences: the reader's activation fence, the scan's fence, and `kRetireFence`.** The governing rule is C++20 [atomics.order]/4 bullet 4 (SC fence `X` hb `A`, `A` coherence-ordered-before `B`, `B` hb SC fence `Y` ⟹ `X` precedes `Y` in `S`). Notation: `U` = unlink; `F_W` = `kRetireFence`; `L_W` = retire's epoch load returning `r`; `Sc2` = the scan whose CAS writes `r+2`, with epoch load `L_S2` then fence `F_Sc2`; `R` = a reader with epoch load `L_R` returning `s`, activation store `A_R`, fence `F_R`, protected load `D_R`. **Lemma A:** if `D_R` misses `U` then `F_R <s F_W` (`D_R --fr--> U`, `F_R` hb `D_R`, `U` hb `F_W`). **Lemma B:** `F_W <s F_Sc2` (`L_W --fr--> CAS(r+1) --rf--> L_S2`; `F_W` hb `L_W`; `L_S2` hb `F_Sc2` **by I10**). **Lemma C:** if `D_R` misses `U` then `s ≤ r`. **Theorem:** A+B give `F_R <s F_Sc2`, so `Sc2` observes `A_R` or a co-later value; if `A_R` then `s ≤ r ≠ r+1` and I3 blocks `Sc2` — contradiction; if co-later then `R` already exited and the release/acquire pairing puts its whole section happens-before the drain. ∎ | The original text asserted "S's post-fence loads are SC-ordered after the unlink" — which *is* Lemma A, and which is **false without `F_W`**. Independent review (2026-08-01) produced a concrete counterexample needing only one stale reader and no `r+1` case at all: with E = 3, an unpinned writer unlinks and tags a bag 3; a reader's RELAXED epoch load returns a stale 3 while E advances 3→4→5; bag 3 is already expired at 5 and is drained; the reader then publishes `active(3)`, fences, misses the unlink, and dereferences the freed object. Nothing in C++20 or in the AArch64 model forbids this — the `ob` cycle does not close. Adding `F_W` forbids it via Lemmas A+B. Three further notes: the argument is **C++20-specific** (pre-C++20 [atomics.order]p9-12 does not forbid this WRC-with-fences relay, so any herd7 model must be RC11/C++20, not C11); it **never uses the `r+1` case split**, which was an artifact of the original analysis; and it **never requires the writer to be pinned**. Formalization method is ITEM-002. Decided 2026-08-01. |
| RCU-DEC-018 | Settled | **`retire` issues a SEQ_CST fence (`kRetireFence`) sequenced between the caller's unlink and the retire-path epoch load.** This is the fix for ITEM-007. A `SEQ_CST` *load* is explicitly **not** an acceptable substitute. | Resolves ITEM-007. Lemma B needs `F_W` hb `L_W` with `F_W` an SC **fence**; bullet 4 requires SC fences at both ends. Strengthening `kEpochLoadOnRetire` to `SEQ_CST` instead does not work: the operations that must be ordered are the **unlink** (a non-SC store sequenced before the load) and the reader's **link load** (a non-SC load), which bullets 2 and 3 do not reach. Concretely on AArch64, a `seq_cst` load is `LDAR`, which does not order a preceding store against itself in the store-buffer direction — `DMB ISH` is required. This corrects the recommendation originally recorded in ITEM-007, which offered the SC load as an equivalent option. **Corroboration:** crossbeam-epoch does exactly this in `Global::push_bag` — `atomic::fence(SeqCst)` followed by a *relaxed* epoch load, then tag. Cost is one `DMB ISH` per `retire`; amortizable to one per bag by adopting crossbeam's tag-at-seal shape if it ever matters (see ITEM-015). Decided 2026-08-01. |
| RCU-DEC-019 | Settled (**amended 2026-08-01 by RCU-DEC-038**) | **`retire` must be called from inside a read-side section on this domain *or* from inside a drain (deleter context)** — debug-asserted as `nesting > 0 \|\| slot.inDrain \|\| domain.teardownActive` (RCU-DEC-038) — but this is *not* what provides reclamation safety. The section requirement is unconditional for *writers* unlinking from the live structure; the drain carve-out exists because a deleter retiring children of an already-dead parent traverses nothing live (RCU-DEC-038). | Resolves ITEM-007's sub-question. RCU-DEC-004's theorem closes with an unpinned writer, so the original justification for pinning was wrong. The real reasons stand on their own: (i) **the writer must traverse the shared structure to perform the unlink**, and an unpinned writer can have the parent reclaimed under it — a use-after-free in the *writer*, not in a reader, and the Consumer Contract's `unlink(parent, child); retire(...)` example as originally written is unsafe for exactly this reason; (ii) it is `synchronize`'s natural precondition; (iii) it is canonical (Fraser; crossbeam's `defer_destroy(&Guard)`). **Do not re-attribute the reclamation-safety property to pinning** — that misattribution is what allowed the missing fence to hide. Decided 2026-08-01. |
| RCU-DEC-024 | Settled | **The outermost `readLock` / `readUnlock` transition runs with maskable interrupts disabled, and RCU read-side sections are forbidden in NMI and #MC context** (debug-asserted against `InterruptContextDepths`). Masking covers only the two *transitions*, never the body of the section. The mask lives in the Phase-2 `ReadGuard`, not in the Core engine. | Resolves ITEM-008. The decisive fact is that **the transition window is fault-free by construction**: it touches only the slot word and the global epoch — both in `reservePerDomainStaticBuffer` storage, which is pinned, always mapped, and never remapped (P2-I3, confirmed by the user 2026-08-01: all VMSubstrate pages stay paged in) — plus the kernel stack. So no #PF/#GP/#UD can fire inside it, and masking maskable IRQs leaves only NMI and #MC. Those are prohibited by contract: CroCOS takes no NMIs today, and the only plausible future source that might want VM structures is a sampling profiler doing stack unwinding — exactly the case where a loud assertion beats silent corruption. Masking the transition only (not the section) means an IRQ during the protected traversal is still fine: the outer's publish is complete, so the nested `readLock` correctly short-circuits on `nesting != 0`. Keeping the mask in `ReadGuard` preserves RCU-DEC-002 — the Core engine documents "the caller must ensure the outermost transition is not reentered" and stays arch-free, which the userspace harness satisfies for free by having no signal handlers. **Four corrections to the fault-free claim, from review 2026-08-01 — the claim is sound but was stated too broadly.** (i) It must cover the **domain object itself** (`slots`, `slotCount`, `&globalEpoch` are loaded from it), so P2-I3's pinned-storage requirement extends to the `Domain`, not just the slot array — a `Domain` placed in vmsmalloc memory would be subject to `reclaimSlabPage` inside the masked window. (ii) It must cover **slot-identity derivation**, which reads `CpuLocal` via GS — a second region, and one that migrates mid-boot; derive the index *before* masking. (iii) **Debug builds break it outright**: RCU-DEC-013's stall detector stamps entry with `monoTimens()`, which reads `ClockManager` state and may touch an **HPET MMIO page** — a third region, inside a window justified by "touches only pinned slot storage". Move the stamp outside the masked region. (iv) Masking IF does **not** leave "only NMI and #MC": per RCU-DEC-025, `InterruptSetup.cpp:35-44` installs **trap gates** for vectors 0,1,3,4,5,6,7,16,17,18,19,20, which do not clear IF — so a `#DB` or `#BP` taken inside the window runs with interrupts enabled. Not a production bug (those are debugger-induced, not window-induced), but the honest statement is: *the window contains no instruction that can fault; any trap-gate exception taken inside it must not enter a read-side section, and hardware breakpoints must not be set on it.* Also add `static_assert` that `Atomic<uint64_t>` is lock-free (with `TreiberStack.h:151-163`'s value-dependent treatment), since a libatomic call inside the window would be a lock acquisition. **Escalation path if NMI-context sections are ever genuinely needed:** add exactly one extra slot per CPU for NMI context, rather than paying K× slots up front. Rejected alternatives: folding nesting into the state word (correct, but relies on the implicit invariant that every nested section restores the word exactly); a split nesting-RMW + conservative scan (correct, but adds an RMW and an unconditional fence to every entry); one slot per context level (attractive, but needs a bounded-nesting argument, and the occupancy-bitmap mitigation for its scan cost reintroduces the same Dekker pairing on a shared cache line written by every reader on every entry — worse for the read path than the scan it optimizes). Decided 2026-08-01 (user direction). |
| RCU-DEC-025 | Settled (**corrected twice, 2026-08-01**) | **`ReadGuard` uses `arch::InterruptDisabler` (`kernel/include/arch.h:111`) for the outermost transition. No hand-rolled flag manipulation.** Optimizing the masking primitive, if ever wanted, is then a single-site change benefiting every consumer rather than an RCU-local trick. **PREREQUISITE, now LANDED (2026-08-01): `InterruptDisabler` was broken — its constructor recorded `wasEnabled` but never called `disableInterrupts()`** (`kernel/arch/arch.cpp:59-62`), making it a save/restore helper that disabled nothing. Fixed and verified: kernel boots on `run`/`run_numa`/`run_numa_hmat`, full test suite green. Note the fix also changed behaviour for `ClockManager::compareTimerTicks` and six `TimerQueues` paths, which had been running their critical sections with interrupts live. | Per user direction 2026-08-01: correctness and one abstraction now, optimization later. With `disableInterrupts()` present, `InterruptDisabler` is already exactly the efficient shape — `wasEnabled = areInterruptsEnabled()` in the constructor and a *conditional* restore in `release()` — so it reads the flags once and never executes `popfq`. The cost being avoided on AMD64 is specifically **`popfq`**, a multi-uop partially-serializing instruction — it rewrites the whole flags register and forces an interrupt-deliverability check — an order of magnitude costlier than a simple ALU op. Reading IF once and branching keeps the flags *read* (cheap) and drops the flags *write* (expensive), replacing it with a highly predictable branch. **Rejected first formulation:** inferring "interrupts are already masked" from a nonzero `InterruptContextDepths`. That inference is **false**. `kernel/arch/amd64/interrupts/InterruptSetup.cpp:35-44` installs **trap gates** (`0x8F`) for vectors 0,1,3,4,5,**6**,7,16,17,**18**,19,20 — and a trap gate does *not* clear IF. So #UD (vector 6) and #MC (vector 18) run with interrupts still **enabled**. A #UD handler entering a read-side section is permitted by RCU-DEC-024 (only NMI and #MC are forbidden), so the depth check would report "already masked", the mask would be skipped, and the transition window would run unprotected — reintroducing precisely the ITEM-008 defect. The converse direction fails too: normal-context code may hold `arch::InterruptDisabler`, so all-depths-zero does not imply IF was set. **Do not profile this under the QEMU/TCG profiler** — it counts basic-block executions and models neither privileged-instruction cost nor serialization, so it would report the whole thing as free. Decided 2026-08-01. |
| RCU-DEC-027 | Settled (**illustration corrected 2026-08-01** — the decision text was right, the §Concurrency Model pseudocode contradicted it) | **Bag selection is `slot.openBagIndex`, not `epoch mod kBagCount`, and an open bag's tag is BUMPED to the current epoch rather than matched against it.** The owner has exactly one `Open` bag at a time and pushes into it unconditionally; rotation is opportunistic and acquire-before-release. **Ordering clarified:** rotation is attempted *first*, and the tag bump is the fallback taken only when no `Free` bag exists and none can be manufactured — the reading this decision's own rationale already implies ("if no `Free` bag it keeps the current open bag one epoch longer"). See `rcu-phase-1.md` P1-DEC-010 and the corrected pseudocode in §Concurrency Model. | Resolves ITEM-001, and the framing of that question was the error: all three candidates tried to *handle* the claim-race collision rather than make it non-load-bearing. **Candidate (c) — "prove the collision unreachable" — is disprovable:** the owner creates the claimable bag itself, and between its own `Open → Sealed` store and its `Sealed → Claimed` CAS, any CPU inside `sweepExpired` (which visits *all* slots per RCU-DEC-006) can win the CAS. No unusual `kBagCount`, no epoch wrap. **Candidate (a) — linear probing — is a UAGP generator**: it destroys I7's premise (tag ≡ index mod kBagCount) while leaving I7's conclusion licensing an unconditional drain, so a bag opened at a probed index with tag 4 gets drained at epoch 5, freeing objects a reader pinned at 4 may still hold. It also fails to terminate: all four bags can be simultaneously unusable (three `Claimed` by different thieves running long deleter chains, one sealed too recently to be expired), which under RCU-DEC-006's sweep-all-slots is steady state, not a corner. **Benefits of (R):** wait-free by construction, so `retire` can never have nowhere to put a node; **it kills the critical deleter-retires-into-its-own-draining-bag collision** (a `Claimed` bag and the `Open` push target are never the same bag); satisfies I11 by construction; **deletes I7 entirely**, demoting `kBagCount` from a correctness floor to a pure batching tunable and removing the tuning landmine; reduces the quiet-system residue floor by a factor of `kBagCount`, since a slot now has at most one unstealable `Open` bag instead of up to four; and **removes the structural obstacle to** crossbeam's tag-at-seal (**corrected 2026-08-01 — this clause originally read "converges with crossbeam's tag-at-seal, answering ITEM-015 for free", which it does not; see ITEM-015**). Degradation under sustained pressure with no `Free` bag is one bag growing with a rising tag — reclamation *latency*, never blocking, never unbounded beyond what the workload retires during the stall. Decided 2026-08-01. |
| RCU-DEC-028 | Settled | **Bag heads carry no ABA tag, justified structurally rather than by epoch arithmetic: there is no ABA-sensitive CAS in the design.** | Resolves ITEM-003. The recorded justification was true but fragile — it depended on the tag↔epoch binding that RCU-DEC-027 has now changed. The structural argument depends on nothing: (1) the classic Treiber ABA bug lives in **CAS-pop** (read `head=A`, read `A->next=B`, CAS `head: A→B`), and **this design has no CAS on `head` in the take path** — originally `head.exchange(nullptr)`; since RCU-DEC-033, plain pops under `Claimed` exclusivity (I6: single writer, no comparison against a possibly-recycled value) — structurally ABA-immune either way; (2) push is ABA-immune on its own, since a successful CAS means `head == old` *now* and `node->next == old`, making the resulting list consistent regardless of the location's history. Push-only Treiber has never had an ABA problem. The strong form of the reuse argument likewise needs no epochs: **an address cannot be pushed twice into a bag without being removed in between, and removal requires the bag to be `Claimed`, which terminates its `Open` lifetime.** Also confirmed (re-derived for the RCU-DEC-033 drain): the claimer's visibility of the owner's pushes rides seal-RELEASE → `kBagClaim`-ACQUIRE on `tagState`, with every push sequenced before the seal; plain pops under `Claimed` exclusivity are then sound, and the re-claim case rides `kBagReseal` the same way. (The original wording cited a RELEASE-push / ACQUIRE-exchange pairing on `head`; the exchange no longer exists.) Decided 2026-08-01. |
| RCU-DEC-039 | Settled | **Deleter contract clause: a deleter may read, modify, or retire only state reachable *solely from the retired object* — it must not load any pointer from the live structure.** Stated in the Consumer Contract; violation is undetectable by assert (see RCU-DEC-038's coverage note) and is a UAF. | Subagent review 2026-08-01: RCU-DEC-038's carve-out is justified by "a deleter traverses nothing live," which was an *assumption*, not an obligation — and a plausible deleter violates it: lazy secondary-index cleanup, where the deleter walks a still-live list to remove a back-reference. Run from a `synchronize`/`barrier` sweep it executes with `nesting == 0`, its traversal unprotected, and if it unlinks-and-retires it is a *writer* by RCU-DEC-019's own definition while the assert waves it through via `inDrain`. The clause is the discipline the assert cannot provide. Consumers needing cross-structure cleanup must do it at *unlink* time (inside the writer's section), not at deleter time. Decided 2026-08-01. |
| RCU-DEC-040 | Settled | **`barrier`'s own-retirees guarantee is per-*slot*, not per-thread: it covers retires issued from the CPU the barrier runs on. Scheduler-era consumer obligation (extends RCU-DEC-032): the retires a `barrier` is meant to cover must have been issued from the same CPU — achieved by CPU *affinity* (migration-off) across the retire→barrier span. Preemption and blocking remain permitted throughout; only migration is constrained.** | Subagent review 2026-08-01. Slots are CPU-indexed (P2-I1), and RCU-DEC-032 blesses migration outside sections and engine calls — which includes the span between a `retire` and the `barrier` covering it. A thread that retires on CPU 0, migrates, and barriers on CPU 1 seals slot 1's bag while its retirees sit in slot 0's *open* bag — the excluded class — and the "complete" promise breaks silently in the free-the-backing-store case. Vacuous today (no scheduler; thread↔CPU↔slot is a bijection) and unrepresentable in the harness (P3-I2 binds one thread per slot — recorded as untestable, not covered). Per-thread slots rejected: unbounded thread counts vs. static `processorCount()`-sized storage, and no API can compensate — even a ticket-style barrier cannot seal a remote open bag (the I13 wall, RCU-DEC-035's option-(b) rejection). Affinity-not-preemption keeps the obligation cheap: a pinned-but-preemptible thread is an ordinary scheduler concept, and teardown-shaped callers pin briefly anyway. Decided 2026-08-01. |
| RCU-DEC-029 | Settled | **The manual case analysis in RCU-DEC-004 is the correctness argument; herd7 litmus is demoted to a regression fixture, not a correctness gate.** ITEM-016's negative control remains mandatory, and the model must be RC11/C++20. | Resolves ITEM-002. Independent review verified all four [atomics.order]/4 bullet applications against N4861, including the direction of "fence `X` happens-before `A`" — the step easiest to get backwards, which the spec has right. Three attacks failed: the non-atomic scan (the contradiction is derived in the SC total order `S`, not from timing, so *when* in the scan a slot was read is irrelevant); the co-later-value branch under P0982 (the only writer of a slot is its owner, so any co-later value is `Inactive` or a subsequent activation, both RELEASE — which is exactly why RCU-DEC-020 is load-bearing rather than cosmetic); and Lemma C's RELAXED epoch load (it needs only coherence on E plus sequenced-before, both of which RELAXED provides). Conditional on I11 and I12, which were the two unstated side conditions the argument rested on and are now numbered. Decided 2026-08-01. |
| RCU-DEC-030 | Settled | **An owner-only `inDrain` flag in `ReaderSlot` guards against sweeper recursion.** `tryAdvance` and `sweepExpired` return immediately when it is set; `retire` still pushes normally. | RCU-DEC-006 opened an unbounded recursion that owner-only draining did not have: `retire` → threshold → `tryAdvance` → `sweepExpired` → claim a bag → run a foreign deleter → deleter calls `retire` → threshold → `tryAdvance` → … Each level runs an arbitrary number of *other CPUs'* deleters (the ITEM-011 volume concern, now with recursion on top), and in the kernel an unbounded stack is `#DF` and a panic. One plain byte and one predictable branch; the flag is I5-class owner-only state needing no atomic. This is also what makes the "deleter that retires" verification target testable rather than accidentally recursive. Decided 2026-08-01. |
| RCU-DEC-031 | Settled (**amended 2026-08-01** — original `barrier` contract was unsatisfiable; see RCU-DEC-035) | **Two primitives, Linux-shaped in the split but not in the mechanism: `synchronize()` promises only that a grace period has elapsed; `barrier(slot)` promises that every object retired to the domain *before the call* has been destroyed — except objects still in *other* CPUs' `Open` bags.** `barrier` first seals the **caller's own** open bag (legal: the caller is that slot's owner), then is **drive-to-completion, not a passive wait**: it actively advances epochs and sweeps until everything sealed-or-claimed is destroyed, waiting only on bags currently `Claimed` by another CPU. The caller's-own-retirees guarantee is therefore *complete* — the common mid-life use ("wait for my retirees before freeing their backing store") gets the full promise; only remote open-bag residents are excluded, which is exactly the ITEM-014 residue class. Full-domain destruction is `drainAllQuiescent` (RCU-DEC-035), not `barrier`. Both primitives are forbidden inside a read-side section on the domain, inside a drain (I14), and — unlike `retire` — under the **strict** forbidden-context mask with **no #PF carve-out**. Interrupt readers arriving mid-primitive are explicitly permissible. | Resolves ITEM-010, user direction 2026-08-01: mimic Linux's split so the consumer can choose the lightest correct option. The mechanism cannot be Linux's, though: `rcu_barrier` waits passively because kthreads/softirqs guarantee callbacks eventually run, whereas here nothing runs deleters except a CPU that sweeps — on a quiet system a passive wait would never terminate (the ITEM-014 residue). Hence active sweep: the teardown caller pays for its own teardown. **Coverage boundary:** objects retired *during* the call — including by deleters that themselves retire — are not covered, and neither are remote open-bag residents (the amendment): `Open → Sealed` is an owner-only plain store (I13), an owner seals only during its own subsequent `retire` calls, and the residue model (§Why memory stays bounded) already states open-bag contents are recoverable only by their owner — the original "every object retired before the call" promise contradicted the spec's own memory model. Teardown paths use `drainAllQuiescent` (RCU-DEC-035), not a `barrier` loop. **Liveness edge:** a bag `Claimed` by another CPU cannot be taken (I6), so `barrier` waits on it — deleter-bounded today, scheduler-bounded under future preemption; same dependency class as waiting on a reader, acceptable for a blocking primitive, and RCU-DEC-013's stall detector must cover a barrier stalled on a foreign `Claimed` bag. **Why the strict mask:** `retire`'s #PF carve-out exists because RadixVM legitimately unlinks and retires from the fault path, but a grace-period *wait* inside a fault handler is a spin on other CPUs' progress from a context that may itself be blocking them. Interrupt readers mid-primitive are safe by I3 — an interrupt-context `readLock` on the spinning CPU merely delays the advance for the (bounded) duration of the handler. Naming: `barrier` keeps the Linux analogy findable; `flush` was the considered alternative. Decided 2026-08-01. |
| RCU-DEC-032 | Settled | **Execution-context assumptions are now explicit rather than implied by C1: every engine entry point (`readLock`/`readUnlock`, `retire`, `tryAdvance`, `sweepExpired`, the drain of a claimed bag) assumes it executes on one CPU from start to finish; read-side sections are non-preemptible under any future scheduler; the blocking wait loops of `synchronize`/`barrier` are the only regions that tolerate preemption and migration.** | Raised by the user 2026-08-01 while resolving ITEM-010. I4/I5/I14 are all keyed to *the executing CPU's* slot — owner-only non-atomic bag bookkeeping and the `inDrain` flag. Under C1 (no scheduler) this was silently safe; a future preemptible `synchronize` migrating mid-`tryAdvance` would clear a different CPU's `inDrain` than it set, and a migrated read-side section would `readUnlock` someone else's slot. The wait loops are exempt because the waiter holds no slot (it is forbidden from being in a section) and its wait targets — `globalEpoch ≥ start+2`, domain bags empty — are domain-global, not per-CPU. Future shape when a scheduler exists: `barrier` as `loop { preemption-off: tryAdvance + bounded sweep batch; preemption point }`, so ITEM-011's batch bound doubles as the preemption-latency bound — one knob resolving both. Sleepable sections remain ITEM-004's deferred SRCU flavor, not a relaxation of this rule. Recording it now means the scheduler work later trips over a written rule instead of a silent one. **Amended 2026-08-01 (RCU-DEC-040): migration tolerance in the wait loops has one cross-call exception — the retire→barrier span needs CPU affinity when the barrier is meant to cover those retires.** Decided 2026-08-01. |
| RCU-DEC-033 | Settled | **The number of deleters run per engine call (`tryAdvance` / `sweepExpired` invocation, counted across all bags it claims) is bounded by `drainBatchBound`, a per-domain **runtime** tunable (earlier drafts wrote `kDrainBatchBound`; the k-prefix is reserved for compile-time/ordering constants and falsely signaled constancy — final review F12). The bound is node-granular: a drain that hits it mid-bag stores the remainder's head back and transitions the bag `Claimed → Sealed`, tag unchanged.** Setting the bound to `SIZE_MAX` (≈2⁶³) is the supported way to run effectively unbounded — the bound is read only by the CPU inside the drain, so the tunable is a plain per-domain value with no atomicity cost; `SIZE_MAX` is the default until tuning (parent authority; ITEM-005). **I13 is amended accordingly**: thieves leave the bag state machine at `Claimed → Free` (emptied) *or* `Claimed → Sealed` (bound hit, remainder intact). **Two corrections from subagent review 2026-08-01:** (1) the bound-hit exit store is **`kBagReseal` RELEASE, not plain** — the original "one plain store" phrasing was a data race (see the ordering table); (2) the bound's scope is **all deleter execution transitively triggered by one engine entry** — including `maybeRotate`'s manufacture drain (which sits on the retire/#PF fast path, the exact spike ITEM-011 targets) and `barrier`'s manufacture step (which may therefore need multiple rounds, since a bound-hit re-seal leaves the bag non-`Free`) — not merely "per `tryAdvance`/`sweepExpired` invocation". `drainAllQuiescent` is exempt (RCU-DEC-035). | Resolves ITEM-011, user direction 2026-08-01. The unbounded spike is real: a retire-path `tryAdvance` sweeps every slot (RCU-DEC-006) and may inherit thousands of foreign deleters, each bottoming out in `vmsfree`, on a path a #PF handler may sit on. **Why node-granular:** the tempting bag-granularity bound (stop *claiming* after K deleters, drain claimed bags to completion) fails because a bag is not size-bounded — the owner keeps pushing into the same `Open` bag until rotation, and rotation waits on epoch advance, so one stalled reader grows every other CPU's open bag without limit. **Why re-seal rather than carry the remainder:** while `Claimed`, the drainer owns `head` exclusively (I6), so incremental pop-run-pop needs only plain ops, and the bound-hit exit is one plain store; the remainder stays in a *visible*, still-expired, immediately re-claimable bag, so `barrier`'s all-bags-empty accounting (RCU-DEC-031) stays sound. Carrying the remainder in a drainer-private list was rejected: invisible to `barrier` and incompatible with RCU-DEC-032 if drains are ever preemptible. Detach-then-restash was rejected as equivalent to re-seal with extra motion. **Invariant audit for the `Claimed → Sealed` transition:** owner plain-store privileges survive (the owner only touches `Free`/`Open` bags, and a re-sealed bag never passes through `Free`); `Sealed → Claimed` remains the sole serializing CAS; no ABA since the tag never moves; I11 unaffected. The per-engine-call scope is deliberately the unit RCU-DEC-032 bounds for future preemption — the batch bound doubles as the preemption-latency bound. ITEM-006 (bounding *tag loads* per sweep) remains deferred; deleter execution is the cost that matters. Decided 2026-08-01. |
| RCU-DEC-034 | Settled | **`~Domain()` performs no draining and no waiting. It debug-asserts full quiescence: every bag `Free` and empty, no active section on any slot, no in-flight drain. The consumer teardown contract is: externally guarantee no CPU will touch the domain again, call `drainAllQuiescent()` (RCU-DEC-035), then destroy. In release the assert compiles out and the caller is trusted** — destroying a non-quiescent domain is a caller bug whose consequence (leaked objects whose deleters never run, or worse if readers are live) is not defended. | Resolves ITEM-012, user direction 2026-08-01 (leaned assertion over force-drain; deliberate-leak not seriously in contention). Force-drain rejected on four grounds: (i) it converts "destroyed with live readers" — an unambiguous caller bug — into an infinite spin *inside a destructor*, where the assertion makes it a loud, attributable panic; (ii) it cannot actually rescue the caller, because destruction already requires an external no-new-users guarantee (otherwise the `Domain` object itself is a use-after-free no drain can fix), so the consumer is already writing a deliberate teardown path and the explicit `drainAllQuiescent()` call costs one line; (iii) a destructor that can block indefinitely poisons every containing type's destructor; (iv) the project safety stance is debug-check + trust-release with forcing-function asserts, and this is that stance verbatim. Deliberate-leak rejected: leak-tracker noise in the torture suite and accumulation at process teardown, the one real kernel use case named (per-process structures torn down at exit — a controlled operation). Side benefit for Phase 3: scenario teardown becomes a *tested* exercise of `drainAllQuiescent`, cross-checked by two independent detectors — the destructor assert and the harness leak tracker. **Amended 2026-08-01 with RCU-DEC-035:** the teardown contract is externally guarantee no-new-users, then `drainAllQuiescent()`, then destroy — not a `barrier` loop, which cannot seal remote open bags and could spin forever at exactly teardown time. Decided 2026-08-01. |
| RCU-DEC-035 | Settled | **`drainAllQuiescent()` is the teardown drain: precondition is RCU-DEC-034's external no-new-users guarantee, **respecced (subagent review 2026-08-01) as a happens-before obligation**: an hb edge must exist from every CPU's last operation on the domain to the call — a thread join qualifies; a RELAXED done-flag does not. Debug-asserted: no active section on any slot *and* no bag in `Claimed` (RCU-DEC-037). Under that precondition the caller acts as *universal owner* with plain operations — force-seal every `Open` bag, advance the epoch freely (I12's unit-advance rule is exempted here and only here: with no reader to protect, Lemma B has no obligation), drain every bag, looping until all bags are `Free` and empty; returns the number of deleters run.** It is the only entry point permitted to perform another slot's owner-side transitions, and it is legal precisely because the precondition removes all concurrency — **the hb edge, not any fence in the entry path, is what makes the plain reads sound: an entry fence with no prior observing read pairs with nothing, and the drained data (`RetireHead` fields, object bodies) is non-atomic, so only genuine happens-before covers it. The original "the entry still fences once to order against the last pre-quiescence activity" claim was unsound and is withdrawn.** | Closes the gap found propagating RCU-DEC-031 into Phase 1 (2026-08-01, option (a) of four cataloged): `barrier` cannot seal another CPU's `Open` bag — `Open → Sealed` is owner-only (I13) and owners seal only on their own retire path — so pre-call retirees in a remote open bag whose owner never retires again are unreachable by `barrier`, and RCU-DEC-034's original `loop { barrier() }` teardown could spin forever in exactly its target scenario. Rejected alternatives: **(b)** CAS-hardening `Open → Sealed` so thieves can seal — destroys I13's plain-store fast path and opens a push-vs-seal race where an in-flight push lands in a just-sealed, possibly-expired bag with `tag < r(n)`, an I11 violation (UAGP) fixable only by per-push re-verification; **(c)** a seal-request flag honored on the owner's next retire — unbounded for the idle-owner case that motivated stealing in the first place; **(d)** dropping `barrier` — loses the caller's-own-retirees guarantee that (a) keeps. Termination: once no new users exist, total retire volume (including deleter-retires, which land in the *caller's* slot) is finite, so the loop empties. The universal-owner claim needs no fences for the *owner-side* stores — no concurrent observer by precondition, and visibility of prior remote activity comes from the precondition's happens-before edge, per the corrected decision text. Decided 2026-08-01 (user direction). |
| RCU-DEC-036 | Settled | **`barrier(slot)` mechanics: (1) seal-*and-rotate* the caller's open bag — never seal without opening a replacement, manufacturing a `Free` bag first if none exists (drain own expired bags; advance until one expires); (2) after the seal, `kBarrierEntryFence` (SEQ_CST) then snapshot `e0 = globalEpoch`; (3) loop `tryAdvance` + sweep until `globalEpoch ≥ e0 + 2` **and** no bag **in state `Sealed` or `Claimed`** tagged `≤ e0` is non-empty — `Open` bags are outside the predicate, exactly as they are outside the promise (**corrected 2026-08-01, subagent review: the unqualified form waits on remote Open bags — unclaimable, unsealable — and livelocks; it even self-livelocks, since the caller's own rotated bag opens tagged `e0` and collects deleter-retires**). `barrier` asserts `!inDrain` at entry (a deleter calling it would otherwise hang undiagnosed: every inner `tryAdvance` hits the I14 early-out and the epoch never moves); its manufacture step sets `inDrain` (cleared before the main loop — I14 discipline) and respects `drainBatchBound`. `barrier` carries `retire`'s same-CPU slot-binding obligation for the duration of the call; the *cross-call* coverage rule is RCU-DEC-040.** | Resolves ITEM-018. The rotate is mandatory because deleter-retires during `barrier`'s own sweeps push into the bag `openBagIndex` designates — sealing without rotating makes that a push into a `Sealed` bag, the I11-fatal case. The manufacture step terminates because `barrier` itself drives advances, so its own sealed bags expire. The entry-snapshot bound is what makes the loop terminate under sustained remote traffic: once the epoch passes `e0`, newly opened bags are tagged `> e0` (`Free → Open` sets `tag := e`), while "until globally empty" chases a moving target and livelocks. **Termination precisely stated (corrected twice — final review F13): the `Sealed/Claimed`-tagged-`≤ e0` set is *not* monotone — a remote owner that rotates converts its `Open ≤ e0` bag into a *new* member arbitrarily late, and may do so more than once (its epoch load can lawfully return values `≤ e0` for a while) — but each slot's successive open-bag tags strictly increase, so per slot only finitely many `≤ e0` conversions exist; each claimed bag drains finite nodes, so the loop terminates.** A bag sealed just after the final check stays undrained; this is permissible because RCU-DEC-031's exclusion ("still in Open bags") is evaluated **at barrier entry** — worth stating since it is what makes the exit race benign. Coverage is exactly RCU-DEC-031's amended promise: every pre-call sealed bag has tag `≤ e0`, and remote open bags are excluded by contract. Decided 2026-08-01. |
| RCU-DEC-037 | Settled | **`drainAllQuiescent` interior: the debug quiescence assert covers *no active section on any slot* **and** *no bag in `Claimed`*; the caller sets a **domain-global `teardownActive` flag** (plain — sound under the quiescence precondition) for the duration (**corrected 2026-08-01: originally "its own slot's `inDrain`", which contradicts both the any-slot rule below and the blessed no-bound-slot caller**); deleter-retires during the teardown drain may target any slot and are collected by the continuing loop.** | Resolves ITEM-019. A `Claimed` bag *is* concurrent use — an in-flight drain on another CPU — and universal-owner plain ops on it would race the thief, so the precondition must observably exclude it. `inDrain` suppression is required because a deleter-retire crossing the advance threshold would otherwise re-enter `tryAdvance` mid-universal-owner-mode, interleaving the fenced concurrent protocol with the plain-store quiescent one on the same state. Any-slot targeting is sound because the precondition removed all concurrency (slot ownership is a concurrency discipline, and there is no concurrency to discipline); it also answers how a harness main thread with no bound slot tears down. Decided 2026-08-01. |
| RCU-DEC-038 | Settled | **`retire`'s pinning assert becomes `nesting > 0 \|\| slot.inDrain \|\| domain.teardownActive`: a retire is legal from inside a read-side section, from inside a drain (deleter context), or during the teardown drain (RCU-DEC-037's any-slot case — the flag is domain-global precisely because the retire may target a slot other than the drainer's). RCU-DEC-019's prose is amended to carry the carve-out explicitly. The assert's coverage loss is real and documented: it can no longer distinguish a legal child-retire from a deleter illegally acting as a *writer* — that discipline now rests on the deleter contract clause (RCU-DEC-039), not on an assert.** | Resolves ITEM-020, a contradiction pre-existing since RCU-DEC-019 + RCU-DEC-030 coexisted: `synchronize` spins on `tryAdvance`, which sweeps, which runs deleters, which may retire (RCU-DEC-030 explicitly supports this) — while `synchronize`'s caller is *required* to be outside any section. `barrier` and `drainAllQuiescent` made the path unavoidable. The exemption is principled, not expedient: RCU-DEC-019 itself locates reclamation safety in `kRetireFence` (RCU-DEC-018), not in pinning; pinning's real justification is *writer-traversal* safety, and a deleter retiring children of an already-dead parent traverses nothing live — the parent left the structure ≥ 1 grace period ago, so nothing the deleter touches can be reclaimed under it. The carve-out must not be misread as weakening the writer rule: a *writer* unlinking from the live structure still requires a section, unconditionally. Decided 2026-08-01. |
| RCU-DEC-026 | Settled — **implemented and verified 2026-08-01** | **`InterruptContextDepths`' per-kind counters narrow from `uint32_t` to `uint8_t`, packed into the struct's first 8 bytes** (7 kinds + one explicit reserved byte), queried by one aligned 64-bit load via `__builtin_memcpy` rather than type-punning. Note the struct remains `alignas(64)`, so `sizeof` is 64 — it is the *first eight bytes* that are packed and read. | User proposal 2026-08-01. Makes all three forbidden-context policies a single load plus a mask plus a test, where today each is a sequence of separate loads and branches: *any* interrupt context = `w != 0`; vmsmalloc/`retire` DEC-014 = `w & ~pfMask` (#PF conditionally legal); RCU read-side RCU-DEC-024 = `w & (nmiMask \| mcMask)`. It improves the **existing** `inForbiddenContextForVmsmalloc()` hot path (`kernel/mm/vmsmalloc.cpp:304-308`, currently six loads and six branches), not only the new RCU checks. Depth headroom is ample: `dispatchInterrupt` runs with IF clear for interrupt-gate vectors, so IRQ nesting is 0 or 1, and 255 is far beyond any reachable exception nesting before #DF. Requirements: an explicit `reserved` byte (the read must not depend on padding), `static_assert(sizeof == 8)` and on `alignof >= 8`, `static_assert` pinning each field's offset so the masks stay correct under reordering, and a debug saturation assert in `InterruptContextGuard`'s constructor since a wrapped counter would silently disable the forbidden-context checks. Provisional pending the offset-pinning and a confirmation that no caller reads the fields as `uint32_t`. Note this is **independent of RCU-DEC-025** — it does not rescue the rejected depth-based masking inference, which fails for reasons of gate type, not counter width. Decided 2026-08-01. |
| RCU-DEC-020 | Settled | **`kStatePublish` is RELEASE, not RELAXED.** | Independent defect found in review, unrelated to ITEM-007. The intended pairing is `kStateRetire` (RELEASE) → `kScanLoad` (ACQUIRE). But a scan may read a *co-later* value than `Inactive` — specifically the **next** section's activation store. C++20 P0982 removed same-thread non-RMW stores from release sequences, so a RELAXED activation store is **not** in the `Inactive` store's release sequence, the acquire load synchronizes with nothing, and the previous section's dereferences are unordered with respect to the drain. That is a formal data race and TSan-visible. Failing interleaving: `A_R1(active,s)` → `D_R` dereferences O → `Inactive(REL)` → `A_R2(active,r+1)` RELAXED; `Sc2` reads `A_R2`, matches, advances, O is freed, with no happens-before edge from `D_R`. On ARMv8 the fix is `STLR` in place of `STR`, immediately dominated by the existing `DMB ISH` — **effectively free**. Decided 2026-08-01. |
| RCU-DEC-021 | Settled | **The epoch load driving the expiry decision must be ACQUIRE**: `kSweepEpochLoad` (new) and `kEpochLoadOnRetire` (strengthened from RELAXED), since the latter feeds `reopenBag`, which drains. | Independent defect found in review. The "reader already exited" branch of RCU-DEC-004 depends on the chain `Inactive(REL)` → `Sc2` slot load (ACQ) → `Sc2` CAS on E (REL) → **drainer's epoch load** → deleter. With a RELAXED drainer load the final edge does not exist and the exited reader's dereferences are unordered with the deleter's writes. Every write to E is an RMW, so they form a release sequence and an ACQUIRE load of any value `≥ r+2` suffices. `kSweepEpochLoad` was missing from the ordering table entirely. Decided 2026-08-01. |
| RCU-DEC-022 | Settled | **`kProtectedLinkLoad = ACQUIRE`** — the protected link load inside `protect` gets a named ordering constant like every other site. | Independent gap found in review: the ordering table covered the epoch, the slots, and the bags, but not the object the reader is actually protecting. RCU's other half is *publication* — a writer that initializes a node then release-stores it into a link requires the reader's link load to be at least consume/ACQUIRE, or the reader can dereference an uninitialized node. Address dependencies happen to save this on ARMv8, but not against compiler value-speculation, and the spec must not rely on that silently. Decided 2026-08-01. |
| RCU-DEC-023 | Settled | **`synchronize` carries the same `SEQ_CST` fence as `retire`** (`kSynchronizeFence`), before its epoch read. | `unlink(); synchronize(); free();` is a teardown idiom a consumer will write, and it has structurally the same shape as `retire` — so it needs the same fence for the same reason. `synchronize` appeared nowhere in the ordering table. Compounds ITEM-010. Decided 2026-08-01. |
| RCU-DEC-005 | Settled | **Grace-period progress is pulled from the retire path.** No daemon, no kernel thread, no timer callback. | C1 and C2 forbid the alternatives outright, and the timer route is independently blocked: timer callbacks run in IRQ context (`kernel/timing/TimerQueues.cpp:255-257`) where `vmsfree` is forbidden by DEC-014, so deleters could not run there. This is the same DEC-036 pattern vmsmalloc already proves out. Decided 2026-08-01. |
| RCU-DEC-006 | Settled | **Expired bags are stealable: any CPU may drain any slot's sealed, expired bags.** Departs from `RCU-proposal.md` §5.2, which drained only from the owning CPU. | This is what makes memory bounded with **zero** scheduler or IPI input. Under owner-only draining, a CPU that retires and then idles strands O(threshold) objects permanently, and the proposal's remedy was an async drain-nudge IPI plus open question Q7. That remedy is disproportionately expensive here: the IPI subsystem is only a raw ICR write (`kernel/arch/amd64/interrupts/APIC.cpp:331`) with no vector reservation, no handler installation, no arch-neutral API, and an `updateRouting()` that carries a `TODO(SMP)` making post-SMP handler registration unsafe. Stealing costs one uncontended CAS on the retire path and dissolves Q7 entirely. Decided 2026-08-01. |
| RCU-DEC-007 | Settled | **No IPI appears on any correctness path, and none is required for the memory bound.** Given RCU-DEC-006, the drain-nudge IPI is demoted from "needed" to "optional latency optimization", and is not part of this spec. | Satisfies C5 in its strongest form: not merely "correct if an IPI is dropped" but "no IPI exists to drop". Also removes a hard dependency on substantial unbuilt IPI-subsystem work. Decided 2026-08-01. |
| RCU-DEC-008 | Settled | **HAZARD-1 (push/steal collision) is closed structurally by the four-state bag machine**, not by revalidation or timing. Owner pushes only to `Open`; drainers take only what they transitioned `Sealed → Claimed`. | The naive rule "stealable iff `globalEpoch ≥ tag+2`" admits a real safety bug: the owner reads the epoch, stalls, the epoch advances twice, a thief steals the bag, and the owner's push then lands in an already-expired bag — freeing a freshly retired object a grace period early. The alternative fix (owner revalidates after push and re-drives) is positional discipline, the same refactor-fragile category as vmsmalloc's DEC-039 hazard. A state machine is checkable; discipline is not. Decided 2026-08-01. |
| RCU-DEC-009 | Settled | **Read-side sections are non-blocking and CPU-pinned.** `ReadGuard` asserts against `preemptionDisabled()` / `cpuPinned()` even though both are currently vacuous `return true` stubs. | Trivially satisfied today (no scheduler, no context switches at all). Asserting now is the DEC-030 forcing-function pattern: when a scheduler lands, the checks become real for free and the migration is loud rather than silent. Decided 2026-08-01. |
| RCU-DEC-010 | Settled | **Every memory ordering is pinned at a named `inline constexpr MemoryOrder` constant** and re-cited inline at each call site. | Verbatim adoption of vmsmalloc's DEC-042 policy. ARMv8 is the release gate and x86 TSO would silently tolerate downgrades that break on weak memory — the exact failure mode the naming discipline exists to make auditable. Decided 2026-08-01. |
| RCU-DEC-011 | Settled | **RCU is layered on top of `SafePtr` freshness, not a replacement for it.** Protected links are traversed through `SafePtr`, and `protect<T>` returns `SafePtr<T>` at the kernel-veneer layer only (the Core engine stays raw). | These are orthogonal guarantees: RCU says the object is not recycled, `ensureTLBEntryFresh` says this CPU's view of its bytes is current. A radix walk needs the freshness check at every level regardless, so folding it into `protect` yields one call site instead of two. Keeping it out of Core preserves RCU-DEC-002. Resolves `RCU-proposal.md` Q6 as yes-at-the-veneer. Decided 2026-08-01. |
| RCU-DEC-012 | Settled | **Context rules are inherited from vmsmalloc DEC-014 unchanged**, not re-derived. Forbidden in IRQ/NMI/#UD/#DF/#GP/#MC; conditionally legal in #PF. The predicate is hoisted out of `vmsmalloc.cpp`'s anonymous namespace into a shared header. | Deleters bottom out in `vmsfree`, so RCU cannot be more permissive than vmsmalloc. One rule for consumers, enforced by the same `InterruptContextDepths` counters, beats two subtly different rules. Decided 2026-08-01. |
| RCU-DEC-013 | Settled | **All runtime checks are debug-only**, including forbidden-context, double-retire, and the section-duration stall detector. Release builds trust the caller. | Consistent with P7-DEC-001 and the project's kernel safety stance: check in debug, trust in release, and use loud assertions as forcing functions. The read side in particular must stay at one store plus one fence in release (R1). Decided 2026-08-01. |
| RCU-DEC-014 | Settled | **Slot storage lives in `reservePerDomainStaticBuffer` memory, not in `CpuLocal`.** Each CPU's slot is placed in its local NUMA domain. | Resolves `RCU-proposal.md` Q2. Decisive factor discovered during codebase validation: the BSP's `CpuLocal` **migrates mid-boot** — GSBase is re-pointed from the BSS bootstrap struct to the arena-resident page during `VMSubstrate::init` — which would silently zero any RCU state placed there. Independently, per-domain buffers keep `CpuLocal` from growing per domain and let a domain be constructed and torn down inside a unit test (R6). Decided 2026-08-01. |
| RCU-DEC-015 | Settled | **`kernel::rcu::Domain` has an inert constructor plus an explicit `init()`** called from an `.icd`-registered routine at the `memory_management` phase, `depends_on = ["VMSubstrateSlab"]`. | Global constructors genuinely run as of the 2026-07-31 toolchain change, and they run during `cpp_init` before every other subsystem. A `Domain` ctor that touched `VMSubstrate` would panic exactly as `PITEventSource`'s did. Decided 2026-08-01. |
| RCU-DEC-017 | Settled | **`EpochDomain` takes a `Hooks` policy template parameter with an `onPreTouch(RetireHead*)` member, defaulting to `NoopHooks`.** The Phase-2 veneer supplies a hook calling `VMSubstrate::ensureTLBEntryFresh`. It fires per node, before any read of that node's `RetireHead` fields (originally phrased "after the bag head is detached" — RCU-DEC-033's incremental pop-run-pop drain has no wholesale detach, but the per-node obligation is unchanged). | Resolves ITEM-009. Stealing (RCU-DEC-006) means a drainer dereferences intrusive links living in *another* CPU's retired slab memory, whose pages are subject to `reclaimSlabPage` — the DEC-047 stale-TLB bug class. The Core engine cannot call `ensureTLBEntryFresh` without breaking RCU-DEC-002, so the call is injected. This is verbatim the pattern `ChainedTreiberStack` already uses for exactly the same reason (`TreiberStack.h:33-40`, `kernel/mm/vmsmalloc.cpp:143-156`), including the `[[no_unique_address]]` zero-overhead default — so it costs nothing in the userspace harness, where the mock `ensureTLBEntryFresh` is a no-op. Decided 2026-08-01 (user direction). |
| RCU-DEC-041 | Settled | **The effective epoch width is 62 bits, pinned as named constants and `static_assert`ed: `kStateEpochBits = 63`, `kBagTagBits = 62`, `kEffectiveEpochBits = kBagTagBits = 62`, `kMaxEpoch = 2^62 - 1`.** Every comparison — including I2's `globalEpoch ≥ tag + 2` — is performed at that width, and `retire` debug-asserts `e ≤ kMaxEpoch`. I9's "64-bit" phrasing is superseded. | Resolves ITEM-013. The width is the **narrower** of the two encodings the epoch flows through, because a value that round-trips through either must survive both: the slot state word spends bit 0 on the active flag (63 left) and `bagTagState` spends 2 bits on the bag state (62 left). Stating it as a constant rather than leaving it implicit matters less for wraparound — at grace-period rates 2^62 is unreachable, and this decision adds no wraparound *handling* — than for making the truncation visible to anyone who later widens a field, changes the bag-state encoding, or adds a third place the epoch is packed. The `static_assert`s are written against intermediate constexpr variables, not literals, because `-Werror` + `-Wtautological-compare` trips on constant-vs-constant comparisons spelled inline (vmsmalloc Phase 2 hit exactly that). Implemented and asserted in Phase 1; see `rcu-phase-1.md` P1-DEC-014 for the phase-local record. Decided 2026-08-01. |
| RCU-DEC-016 | Provisional | **`RetireHead` is a 16-byte intrusive member** of the retired type (`next` + deleter pointer), recovered via a member-pointer NTTP. Retire never allocates. | Retiring runs on free paths and must not recurse into vmsmalloc's failure modes (I8). Costs idle bytes during the object's live phase — the standard `rcu_head` trade. Provisional because the exact recovery mechanism should be validated against real radix-node link types before being pinned. Decided 2026-08-01. |

## Hazards

- **HAZARD-1 — push/steal collision on a limbo bag.** Closed by RCU-DEC-008's state machine, but
  this is the single most dangerous spot in the design: getting it wrong frees live objects one
  grace period early, and the window is narrow enough that it would not reproduce under casual
  testing. Phase 1 must test the interlock directly with injected stalls, not just incidentally.
- **The two-fence pairing is the whole safety argument.** `kReaderActivationFence` and
  `kScanFence` are both SEQ_CST and neither may be downgraded. On x86 TSO a downgrade would pass
  every test; on ARMv8 it would fail rarely and non-deterministically. ARMv8 TSan is the gate
  precisely because of this.
- **The no-ABA-tag argument (ITEM-003).** If wrong, the fix is a packed 64-bit head encoding and
  a full re-derivation of the bag protocol — expensive to discover late.
- **`retire` from inside a read-side section on the same domain** is legal and expected (a writer
  may hold a section while restructuring), but it means `tryAdvance` can be called by a CPU whose
  own slot is active. That CPU's own snapshot will then block its own advance. This is correct
  but wastes work, and a naive implementation might instead *skip* its own slot — which would be
  a safety bug.
- **Deleters that themselves retire.** A destructor that retires child nodes reenters `retire`
  during a drain. Safe because the drained bag is `Claimed` (exclusively owned, I6) and the
  deleter's pushes go to its own slot's *open* bag — two different bags by construction — with
  recursion into the drain itself blocked by `inDrain` (RCU-DEC-030). Note the safety argument
  changed with RCU-DEC-033: it was "the list is already detached"; it is now `Claimed`
  exclusivity, since the pop-run-pop drain works in place. Also note these retires may occur
  outside any section — see ITEM-020.
- **`barrier` is easy to implement wrong in two independent ways (ITEM-018).** Sealing the
  caller's bag without rotating leaves `openBagIndex` pointing at a `Sealed` bag — a
  deleter-retire then pushes into it, the I11-fatal case. And "sweep until globally empty"
  livelocks under sustained remote traffic; the loop must bound itself by the entry-epoch
  snapshot. Both mistakes produce a `barrier` that passes quiet unit tests.
- **Zeroed-slot-array semantics.** An uninitialized domain scans clean and reclaims freely. The
  zero encoding is chosen to make a zeroed array mean "no readers", which is right for a
  correctly-initialized empty domain and wrong for an uninitialized one. Guarded only by the
  RCU-DEC-015 init assert.
- **`monoTimens()` is not a happens-before edge.** It is a globally-shared RELAXED accumulator
  (`kernel/timing/ClockManager.cpp:209-223`). It is fit for stall *diagnostics* and nothing else;
  it must never be used to order protocol steps.
- **RCU-DEC-024's fault-free-window argument is load-bearing and depends on a VMSubstrate
  property.** If slot storage ever becomes lazily mapped, demand-faulted, or remappable, an
  exception can fire inside the masked window and the whole justification collapses — silently,
  because masking still *looks* correct. P2-I3 is therefore not merely a convenience; it is a
  safety precondition, and anything that changes `reservePerDomainStaticBuffer`'s mapping
  behaviour must revisit this decision.
- **The NMI/#MC prohibition is a contract narrowing that will not be obvious to a future
  consumer.** R2 as originally written promised legality up to and including NMI. A profiler or
  watchdog author reaching for RCU in an NMI handler will find a debug assertion, and only in a
  debug build. Worth stating in the header comment, not just the spec.
- **Stealing a bag means dereferencing another CPU's slab memory (ITEM-009).** RCU-DEC-006 is the
  right call for memory boundedness, but it introduces a stale-TLB exposure that owner-only
  draining did not have — the same class of bug as vmsmalloc's DEC-047. Every `next` traversal in
  a stolen bag is a candidate site. This also puts a hole in RCU-DEC-002's "engine knows nothing
  about the kernel", since the freshness call must be injected from Phase 2.
- **The scan's epoch load must precede the scan's fence (I10).** This is the single most
  dangerous line in the design to "clean up". Reversing the two collapses Lemma B and the whole
  safety argument **while changing no ordering constant** — so RCU-DEC-010's naming discipline,
  which is the project's main defence against ordering regressions, does not cover it. x86-TSO
  would not fail either. It needs a comment at the site, not just this line in the spec.
- **`kRetireFence` looks removable and is not.** It sits between the caller's unlink and an epoch
  load that is only RELAXED-for-this-purpose, so a reader of the code sees a full barrier guarding
  a relaxed load and may conclude it is redundant. It is the entire fix for ITEM-007. Likewise the
  tempting "skip the fence when the epoch already matches the open bag's tag" optimization
  reintroduces the bug — recorded as ITEM-015 so it is refused with a reason rather than
  rediscovered.
- **Three of the ordering constants are correct for reasons that have nothing to do with their
  obvious purpose.** `kStatePublish` is RELEASE not because the activation needs publishing but
  because C++20 release sequences exclude same-thread relaxed stores (RCU-DEC-020);
  `kEpochLoadOnRetire` is ACQUIRE not for ITEM-007 but because it feeds the drain path
  (RCU-DEC-021); `kEpochAdvance`'s RELEASE half exists for the drainer, not the scanner. Each
  will look over-strong to someone optimizing later.
- **`retire` reentered from #PF context.** DEC-014 makes #PF *conditionally* legal, and the bag
  bookkeeping (`openBagIndex`, the retire counter) is deliberately non-atomic owner-only state
  (I5). A page fault taken in the middle of `retire` that itself retires would corrupt it. This is
  a caller-contract violation — VMSubstrate and its consumers must not fault during operation, the
  same obligation vmsmalloc already carries — but it is undetectable and worth stating.
- **Sweep cost is unbounded and lands on whoever calls (ITEM-011).** The CPU that happens to
  trigger an advance pays for every other CPU's queued deleters. Under RCU-DEC-006 this is by
  design, but it means retire-path latency depends on other CPUs' retire volume.
- **A protected pointer escaping its section is the most likely consumer bug in the framework.**
  `SafePtr` is freely copyable and nothing stops it outliving the `ReadGuard`. There is no
  mechanism, no assert, and no plausible runtime detection — only the contract. Worth a header
  comment and a torture scenario that deliberately does it under ASan.
- **Reentrancy into the owner's own bag machinery is not encoded anywhere.** RCU-DEC-028's
  push/take exclusion holds on a *three*-part precondition — owner-only sealing (in the state
  machine), one owner per slot (I4), and **the owner not being reentrant into its own bag** — and
  only the first two are structural. RCU-DEC-027 removes the deleter-retires case and RCU-DEC-030
  removes the sweeper-recursion case, but `#PF` reentrancy remains a contract obligation. Add a
  debug assert *after* the push that `tagState` is still `(expected tag, Open)`: one load, and it
  catches exactly the class no invariant covers.
- **`kBagCount` is no longer a correctness floor** (RCU-DEC-027 deleted I7), but the spec carried
  it as one for a revision, and the old framing may persist in readers' memory. It is now purely a
  batching tunable.
- **Bump-then-rotate is a silent no-op, and this spec shipped it for a revision.** From RCU-DEC-027
  until 2026-08-01 the §Concurrency Model `retire` pseudocode bumped the open bag's tag to `e` and
  *then* called `maybeRotate(slot, e)`, whose documented trigger is "`e` exceeds the open bag's
  tag" — already falsified by the bump one line earlier. Rotation could never fire, so no bag would
  ever reach `Sealed`, so nothing would ever become claimable, so **the entire reclamation
  machinery would be dead while every individual invariant still held**: I11 is satisfied (the tag
  keeps rising), I6 is satisfied (there is only ever one bag), I2 is vacuously satisfied. There is
  no assertion that fires and no test that fails except an end-to-end one that checks objects are
  actually destroyed. Two properties of the failure make it worth calling out here rather than
  treating it as a one-off typo: it is **invisible to every local check**, and the natural repair
  (keep the spec's order, pass a saved pre-bump staleness flag into `maybeRotate`) is *also*
  correct, so a reviewer can talk themselves into the wrong shape and still be right about safety
  — it just seals every bag one epoch late, forever. The rule to hold onto: **the tag bump is what
  the owner does when it CANNOT rotate.** Found during Phase 1 implementation; see
  `rcu-phase-1.md` P1-DEC-010.

## Verification Targets

| Property | Method |
|---|---|
| I1 — advance blocked while any active slot's snapshot ≠ current epoch | Unit test with injected stalls (Phase 1) |
| I2 — no bag drained before `globalEpoch ≥ tag + 2` | Unit test on the bag algebra (Phase 1) |
| I3 — a stale snapshot delays but never permits advancement | Unit test: stall a thread between epoch load and activation store; assert advancement blocks (Phase 1) |
| I6 — owner-push and drainer-take never collide (HAZARD-1) | Targeted concurrent test with injected stalls at the exact race window (Phase 1) + TSan |
| I13 (as amended) — re-seal exit hands off correctly; `kBagCount ≥ 2` structural floor (rotation needs a second bag; 4 is the tuning default, no longer a correctness floor — I7 retired) | Phase 1 unit tests + `static_assert` |
| No use-after-grace-period | Torture suite where the ASan-runner deleter really calls `free()`, making UAGP a hard ASan trap (Phase 3) |
| Nesting, including interrupt-nested sections on one CPU | Unit test (Phase 1) + in-harness simulation (Phase 3) |
| `synchronize` advances exactly two epochs | Unit test (Phase 1) |
| Memory residue on a quiet system converges to exactly the idle CPU's open-bag contents — all *sealed*-bag retirees reclaimed by other CPUs, no scheduler/IPI input (RCU-DEC-006, ITEM-014) | Dedicated torture phase: a CPU retires then idles permanently; assert sealed-bag reclamation and the exact residue bound (Phase 3; **corrected — "assert full reclamation" contradicted I13 and would fail on a correct build**, final review F4) |
| Fence pairing correctness on weak memory | TSan on ARMv8 (release gate) + herd7 litmus if ITEM-002 resolves that way |
| Stall detector actually fires | Forced-stall injection asserting both the stall *and* the diagnostic (Phase 3) |
| Forbidden-context asserts fire | `EXPECT_ASSERT_FAILURE` negative tests (Phase 2/3) |
| Kernel still boots with the domain registered | `run`, `run_numa`, `run_numa_hmat` (Phase 2) |

## Testing Approach

Per `[[project_armv8_dev_tsan]]`: **ARMv8 M1 TSan is the primary release gate**, with an
ASan/leak runner shipping in parallel. Subagent and CI claims are re-verified by actually running
the suites.

Three layers:

1. **Core engine unit tests** (`tests/core/`) — the engine is a pure algorithm over `Atomic` and a
   caller-supplied slot array, so it tests with plain `std::thread` and no kernel mocks at all.
   Both `CoreTests` and `CoreTestsTSan` source lists must be updated; they are maintained
   separately.
2. **Kernel veneer tests** (`tests/kernel/rcu/`) — the vmsmalloc Phase-8 pattern: `mocks/` first on
   the include path to shadow `<mem/VMSubstrate.h>`, `<mem/NUMA.h>`, `<kassert.h>`; real
   `CpuLocal.h` / `InterruptContextDepths.h` under `CROCOS_TESTING`; `bindThreadToCpu` for
   thread-per-CPU identity. A `timing::monoTimens()` mock must be written — none exists.
3. **Torture suite** (Phase 3, the centerpiece) — rcutorture-shaped. Readers loop sections over a
   writer-mutated structure of versioned cells; writers swap and retire. The ASan runner's deleter
   genuinely frees, making UAGP a hard trap. Phases: stutter, reader/writer ratio sweeps,
   forced-stall injection, and the RCU-DEC-006 quiet-system residue phase.

The in-kernel stress (real vmsmalloc nodes, real `SafePtr` freshness, real interrupt-context
asserts across 8 QEMU CPUs) is deliberately **out of scope for this spec tree** and will be
planned once the framework is proven — mirroring how vmsmalloc Phase 9 followed Phase 8.

## References

- `RCU-proposal.md` (project root, 2026-06-09) — the originating proposal. This spec supersedes
  it. Its §4 design-space catalog is carried into RCU-DEC-001; its Q2, Q6, and Q7 are resolved by
  RCU-DEC-014, RCU-DEC-011, and RCU-DEC-006 respectively.
- `specs/vmsmalloc.md` — DEC-014 (context rules, inherited), DEC-036 (pull-based progress from
  the hot path's slow path), DEC-042 (memory-ordering naming policy), DEC-047 (read-only sentinel
  reclaim, the safety net that turns UAGP into a garbage read rather than a wild fault).
- `libraries/Core/include/core/atomic/TreiberStack.h` — the layering template (Core algorithm +
  consumer-supplied policy structs) and the house style for concurrent Core headers. Its
  "Reclamation: out of scope" note is the hole this subsystem fills.
- K. Fraser, *Practical Lock-Freedom* (2004), ch. 5 — epoch-based reclamation.
- crossbeam-epoch — the modern EBR implementation lineage.
- P. E. McKenney, *Is Parallel Programming Hard?* — RCU semantics, grace-period reasoning, and
  rcutorture's structure.
