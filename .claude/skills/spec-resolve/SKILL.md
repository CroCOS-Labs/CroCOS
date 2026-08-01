---
name: spec-resolve
description: Record the resolution of an open question as a decision. Use when a question has been answered in conversation or a decision has been reached.
argument-hint: <spec-name> <ITEM-nnn>
arguments: [spec, item]
allowed-tools: Write(specs/**) Edit(specs/**) Read
---

## Current spec

!`cat "specs/$spec.md"`

## Instructions

Resolve $item:

1. Locate $item in the Questions table
2. Move it to the Decisions table, retaining its ITEM-nnn
3. Record the decision as stated in the current conversation — do not paraphrase beyond what was said
4. Set certainty:
   - `settled` if the question is firmly closed and revisiting would require significant rework
   - `provisional` if this is the current best guess and may change under implementation pressure
5. Write a concise rationale capturing the reasoning, not just the conclusion
6. If this resolution unblocks other questions via Blocked-by, note which ones are now unblocked

Write the updated spec back to `specs/$spec.md` and confirm what changed.
