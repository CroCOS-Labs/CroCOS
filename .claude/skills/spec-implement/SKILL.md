---
name: spec-implement
description: Implementation handoff for a ready leaf spec. Validates readiness and produces a concrete implementation plan.
disable-model-invocation: true
context: fork
agent: Plan
argument-hint: <spec-name>
---

## Specification

!`cat "specs/$ARGUMENTS.md"`

## Instructions

### Step 1 — Validate readiness

Fail fast with a clear explanation if any of the following hold:

- `kind` is `deferred`: the spec kind must be determined before implementation
- `kind` is `subsystem`: subsystems decompose into child specs, they are not implemented directly;
  list the declared components and suggest running `/spec-implement` on each leaf
- Any Question is Open and marked blocking: list each blocker by ITEM-nnn and text

### Step 2 — Produce an implementation plan

Only reached if the spec is a `leaf` with no blocking open questions.

1. Review the Implementation Phases section and use it as the skeleton
2. For each phase, break it down into concrete, ordered tasks
3. Flag provisional Decisions that implementation will stress — these may need revisiting before
   or during the relevant phase
4. Map Hazards to the phases where they are most likely to surface
5. Map Verification Targets to phase boundaries where each should be confirmed
6. Note any Dependency that must be available before a given phase can begin
