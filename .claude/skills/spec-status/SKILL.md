---
name: spec-status
description: Summarise the current state of a spec. Use when asked about the status, progress, readiness, or blockers of a subsystem spec.
argument-hint: <spec-name>
allowed-tools: Read Write(specs/**) Edit(specs/**)
---

## Current spec

!`cat "specs/$ARGUMENTS.md"`

## Instructions

Report concisely:

1. **Kind and status** — what phase the spec is in
2. **Section completion** — which sections are substantive vs placeholder
3. **Blocking questions** — enumerate all open ITEM-nnn marked blocking with their text
4. **Non-blocking open questions** — count only, unless there are two or fewer
5. **Provisional decisions** — list any; these may need revisiting under implementation pressure
6. **Readiness verdict**
   - Leaf: are all blocking questions resolved?
   - Subsystem: are all components defined in frontmatter?
   - Deferred: what needs to be determined before progress can be made?

This is a status report, not a content summary. Be brief.
