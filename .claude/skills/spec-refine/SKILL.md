---
name: spec-refine
description: Continue refining an existing spec. Picks up the design dialogue where the last session left off.
disable-model-invocation: true
argument-hint: <spec-name>
allowed-tools: Read Edit(specs/**) Write(specs/**)
---

## Current spec

!`cat "specs/$ARGUMENTS.md"`

## Instructions

Review the spec above and enter the design dialogue loop:

1. Identify the single most important unresolved aspect — prefer blocking questions and those
   that would unlock other sections
2. Ask the user about it — one question at a time, clearly phrased
3. After the user responds, update the spec accordingly:
    - Answer resolves an open question → move to Decisions with certainty and rationale
    - Answer surfaces a new design question → add to Questions with the next available ITEM-nnn
    - Answer fills a section → update that section directly
4. Write the updated spec file after every response, before asking the next question
5. Proceed to the next most important unresolved aspect

Stop when:
- The user signals they want to pause or end the session, or
- All blocking questions are resolved and the core sections are substantively filled

At the end of the session, summarise: what was resolved, what remains open, and the spec's current status.