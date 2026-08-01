---
name: spec-review
description: Adversarial review of a spec. Use when asked to stress-test, find gaps, poke holes, or critically review a spec — or after significant refinement.
argument-hint: <spec-name>
allowed-tools: Write(specs/**) Edit(specs/**) Read
---

## Current spec

!`cat "specs/$ARGUMENTS.md"`

## Instructions

Conduct an adversarial review. Examine:

- **Invariant violations** — scenarios where stated invariants could be broken
- **Contract gaps** — behaviours callers might reasonably expect that are unspecified
- **Missing failure modes** — what can go wrong that is not accounted for
- **Concurrency hazards** — races, ordering assumptions, ABA problems, lock inversion
- **Non-goal boundary violations** — does the design accidentally require something ruled out?
- **Provisional decision load-bearing** — does anything rely too heavily on a provisional decision?
- **Blocking question graph** — are there circular dependencies or unreachable resolutions?
- **Hazard gaps** — tricky areas not yet recorded

For each issue found:
- If it is an open question: assign the next available ITEM-nnn, add to Questions, mark blocking if appropriate
- If it is a known risk without a clean resolution path: add to Hazards

Report all issues found, then write the updated spec back to `specs/$ARGUMENTS.md`.
