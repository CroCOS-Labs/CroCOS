---
kind: deferred          # deferred | subsystem | leaf
status: draft           # draft | refining | review | ready | in-progress | complete
parent: ~               # path to parent spec, or ~ if root
components: []          # subsystem only: paths to child specs
---

# <Name>

> <One-sentence goal. e.g. "Slab allocator layer on top of VMSubstrate providing per-size-class object
> caching with NUMA-aware slab pooling.">

## Non-Goals

<!-- What this component explicitly does not handle. Be specific — name things a reader might reasonably
     expect to be in scope. -->

-

## Consumer Contract

<!-- What callers can rely on: usage rules, ownership semantics, memory model guarantees. If concurrency
     semantics are non-trivial, add a Concurrency Model supplementary section and cross-reference it here. -->

## Dependencies

<!-- Upstream components and interfaces this relies on. Flag anything that must be stable before
     implementation can begin. -->

| Dependency | Role | Must be stable first? |
|---|---|---|
| | | |

## Invariants

<!-- Conditions that must hold at all times within this component. State them as falsifiable assertions,
     not aspirations. -->

-

## Failure Modes

<!-- What can go wrong and the defined behavior for each. Is it recoverable? Does it propagate to callers?
     Does it panic? -->

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| | | |

## Questions

<!-- Open questions. Status: Open | Deferred (not urgent, not currently blocking).
     Blocking: whether this question blocks decomposition (subsystem) or implementation (leaf).
     Blocked-by: ITEM-nnn dependencies within this table. -->

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| | Open | | | | |

## Decisions

<!-- Design decisions, both those resolved from Questions and those recorded directly.
     Questions resolved here retain their ITEM-nnn. New decisions take the next available ITEM-nnn.
     Certainty — settled: changing requires significant rework; provisional: best current guess. -->

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| | | | |

## Hazards

<!-- Known tricky spots likely to produce bugs or subtle misbehavior. These focus adversarial review
     and inform where to concentrate testing effort. -->

-

## Verification Targets

<!-- Properties to actively confirm. Specify the intended method: unit test, integration test,
     stress test, formal verification, or manual review. -->

| Property | Method |
|---|---|
| | |

## Testing Approach

<!-- Mock interfaces needed, overall test strategy, stress test story, formal verification scope if any. -->

## Implementation Phases

<!-- Leaf specs only. Delete this section for subsystem specs — decomposition is tracked via components
     in frontmatter. Ordered breakdown of implementation steps. -->

1.

## References

<!-- Relevant papers, prior art, related CroCOS components. -->

-

---
<!-- Supplementary sections: add below as needed. Canonical candidates:

## Concurrency Model
Include when locking strategy, atomic access patterns, or per-CPU/NUMA ownership semantics are
complex enough to warrant separation from the Consumer Contract.

## Performance Envelope
Include when latency bounds, throughput targets, or contention behavior under load are
design constraints rather than implementation details.
-->
