# RCU — overview

> **Reading order.** Start here for the map. Authoritative design decisions live in `rcu.md`
> (the parent spec). Implementation-level detail per phase lives in `rcu-phase-{1,2,3}.md`.
> This document indexes the others — it does NOT duplicate their decisions.

## Subsystem in one paragraph

RCU is CroCOS's safe-memory-reclamation framework: it lets a writer unlink an object from a
concurrent data structure and destroy it later, with a guarantee that no reader still holds a
pointer to it. The engine is epoch-based reclamation — a per-domain 64-bit epoch, one activation
slot per CPU, and epoch-tagged per-CPU limbo bags — chosen because it is the only scheme that
works with **no timer tick, no scheduler, and no IPI**, all three of which CroCOS lacks or
forbids. Readers publish their state explicitly instead of being sampled, so an idle CPU is
invisible to grace periods rather than blocking them. Its intended consumer is a RadixVM-style
radix tree for the virtual memory manager.

## Why this exists

Tick-driven RCU (Linux's model) needs the tick because its readers are *unmarked*: the only way
to learn a CPU is not mid-section is to sample it at a known-safe moment. CroCOS is tickless and
has no scheduler, so every sampling point is gone. Making read-side sections **explicitly marked**
replaces sampling with published state, and the constraint dissolves. See `rcu.md` §Concurrency
Model.

## Phase dependency graph

```
        ┌───────────────────────────┐
        │  Phase 1 — Core engine    │   Core::rcu::EpochDomain
        │  libraries/Core/.../rcu/  │   pure algorithm, zero kernel deps
        └─────────────┬─────────────┘
                      │
                      v
        ┌───────────────────────────┐
        │  Phase 2 — Kernel veneer  │   kernel::rcu::Domain / ReadGuard
        │  kernel/{include/,}rcu/   │   NUMA slots, SafePtr, DEC-014 rules
        └─────────────┬─────────────┘
                      │
                      v
        ┌───────────────────────────┐
        │  Phase 3 — Torture suite  │   RELEASE GATE
        │  tests/kernel/rcu/        │   ASan-as-UAGP-oracle + ARMv8 TSan
        └─────────────┬─────────────┘
                      │
                      v
              (out of scope here)
        in-kernel stress  →  RadixVM
```

Strictly sequential. Phase 1 has unit tests of its own and can be validated before Phase 2 exists;
Phase 3 is the gate every future consumer depends on.

## What each phase delivers

**Phase 1 — Core epoch engine.** `Core::rcu::{EpochDomain, ReaderSlot, RetireHead}` as a
header-only component over `Atomic` and a caller-supplied slot array, with no kernel includes at
all. Contains the whole algorithm: the activation/scan fence pairing, the exact-match advance
rule, and the four-state limbo-bag machine that makes bags stealable. Tested as a pure algorithm
with plain `std::thread` — no mocks needed.

**Phase 2 — Kernel veneer.** `kernel::rcu::Domain` / `ReadGuard` / `protect` / `retire`. Binds
slot index to `getLogicalProcessorID()`, places slots in NUMA-local
`reservePerDomainStaticBuffer` memory, returns `SafePtr<T>` from `protect` so TLB freshness and
RCU protection compose in one call, and inherits vmsmalloc's DEC-014 context rules verbatim.
Registered via `kernel/rcu/rcu.icd` at the `memory_management` phase.

**Phase 3 — Torture suite.** rcutorture-shaped stress in `tests/kernel/rcu/`. The ASan runner's
deleter really calls `free()`, so any use-after-grace-period is a hard trap. ARMv8 TSan is the
release gate. Includes a scenario dedicated to proving memory residue reaches zero on a quiet
system with no scheduler or IPI input.

## Thematic index — where the design lives

| Topic | Where |
|---|---|
| Why EBR and not QSBR / SRCU / hazard pointers / refcounts | `rcu.md` RCU-DEC-001 |
| Core-vs-kernel layering rationale | `rcu.md` RCU-DEC-002; `rcu-phase-1.md` Non-Goals |
| The safety argument (I1/I2/I3 + fence pairing) | `rcu.md` RCU-DEC-004, Invariants |
| **Why memory stays bounded with no scheduler** | `rcu.md` §Concurrency Model, RCU-DEC-006 |
| **HAZARD-1 — the push/steal collision and its fix** | `rcu.md` RCU-DEC-008, Hazards; `rcu-phase-1.md` |
| Why no IPI is needed at all | `rcu.md` RCU-DEC-006, RCU-DEC-007 |
| Memory-ordering constants and the naming policy | `rcu.md` §Memory ordering constants, RCU-DEC-010 |
| Why bag heads need no ABA tag | `rcu.md` §Why no ABA tag, ITEM-003 |
| Relationship to `SafePtr` / TLB freshness | `rcu.md` RCU-DEC-011; `rcu-phase-2.md` P2-DEC-003 |
| Forbidden-context rules | `rcu.md` RCU-DEC-012; `rcu-phase-2.md` P2-ITEM-001 |
| Slot storage placement (and why not `CpuLocal`) | `rcu.md` RCU-DEC-014; `rcu-phase-2.md` P2-DEC-002 |
| Inert-constructor / init-order requirement | `rcu.md` RCU-DEC-015; `rcu-phase-2.md` Hazards |
| Debug-only-checks policy | `rcu.md` RCU-DEC-013; `rcu-phase-2.md` P2-DEC-005 |
| The UAGP oracle and why it needs a real `free` | `rcu-phase-3.md` P3-DEC-001 |

## Resolved from the original proposal

`RCU-proposal.md` (project root, 2026-06-09) is superseded by this spec tree. Its open questions
resolve as:

| Proposal Q | Resolution |
|---|---|
| Q1 — veneer placement | `kernel/include/rcu/` as its own subsystem — `rcu-phase-2.md` P2-DEC-001 |
| Q2 — slot storage shape | Per-domain static buffers, not `CpuLocal` — `rcu.md` RCU-DEC-014 |
| Q3 — active-flag encoding | Bit 0 of the packed state word — `rcu.md` §Slot state word |
| Q4 — `kBagCount` / thresholds | `kBagCount = 4` provisional; tuning deferred — `rcu.md` ITEM-005 |
| Q5 — ordering-proof artifact | Still open — `rcu.md` ITEM-002 |
| Q6 — should `protect<T>` return `SafePtr<T>` | Yes, at the veneer layer only — `rcu.md` RCU-DEC-011 |
| Q7 — where the drain-nudge IPI's hook lives | **Dissolved.** Stealable bags remove the need for the IPI — `rcu.md` RCU-DEC-006, RCU-DEC-007 |

The proposal's §4 design-space catalog (A1–A8) is carried into RCU-DEC-001 and remains the record
of why the seven rejected approaches were rejected.

## Still open before implementation

Two rounds of adversarial review (2026-08-01) resolved three blocking items and found four
further ordering defects. **Three blocking questions remain, all in Phase 1's territory:**

| Item | Issue | Blocks |
|---|---|---|
| ITEM-001 / P1-ITEM-001 | The owner's non-blocking fallback when its index-matching bag is `Claimed` by another CPU. | Phase 1 step 4 |
| ITEM-002 | herd7 litmus proof for the fence pairing, or a manual proof appendix. Must use an **RC11/C++20** model (C11 does not forbid the relay), and must include ITEM-016's negative control. | Phase 1 step 3 |
| ITEM-003 | Is the no-ABA-tag argument for bag heads airtight? Expensive to discover late. | Phase 1 step 4 |

**Resolved by review:**

| Item | Resolution |
|---|---|
| ITEM-007 (unpinned retire) | RCU-DEC-018: a **SEQ_CST fence in `retire`** between the unlink and the epoch load. The original diagnosis ("the RELAXED epoch load is the defect") named the right line and prescribed the wrong edit — an SC *load* does not work, since the operations needing order are the non-SC unlink and the non-SC link load. RCU-DEC-019 keeps pinning mandatory, but for writer-side safety, not reclamation safety. |
| ITEM-008 (interrupt-atomic entry) | RCU-DEC-024: mask across the transition only; forbid NMI/#MC sections. Rests on the transition window being **fault-free by construction**. |
| ITEM-009 (stolen-bag freshness) | RCU-DEC-017: `onPreTouch` hook, making `EpochDomain` a template. |

**Four independent ordering defects found in the same pass** — none related to ITEM-007:
`kStatePublish` had to move RELAXED → RELEASE (RCU-DEC-020, C++20 release sequences exclude
same-thread relaxed stores); `kSweepEpochLoad` was missing entirely and must be ACQUIRE
(RCU-DEC-021); `kProtectedLinkLoad` was missing (RCU-DEC-022); `synchronize` needs the same fence
as `retire` (RCU-DEC-023). RCU-DEC-004 was **rewritten**, not annotated — the original argument
was unsound.

The most dangerous single finding is **I10**: `tryAdvance`'s epoch load must be sequenced *before*
its fence. Reversing them collapses the entire safety argument while changing no ordering
constant, so the DEC-042 naming discipline does not cover it.

Non-blocking but unresolved: ITEM-010 (`synchronize` — grace period elapsed, or callbacks run?),
ITEM-011 (unbounded deleter execution per sweep), ITEM-012 (domain teardown with objects in
limbo), ITEM-013 (effective epoch width), ITEM-014 (quiet-system residue floor), ITEM-015
(tag-at-seal amortization), ITEM-016 (litmus negative control).

**Landed ahead of Phase 2** (2026-08-01): the forbidden-context predicate hoisted into
`InterruptContextDepths.h` with packed `uint8_t` counters (RCU-DEC-026), and a fix to
`arch::InterruptDisabler`, whose constructor never actually disabled interrupts — `ReadGuard`
depends on it, so ITEM-008's fix would have been a silent no-op.

## Out of scope for this tree

In-kernel `smp_bringup` stress; RadixVM itself; the drain-nudge IPI and any IPI-subsystem
build-out; expedited / asymmetric-fence grace periods; preemptible SRCU-shaped domains.
