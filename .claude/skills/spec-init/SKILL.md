---
name: spec-init
description: Bootstrap a new subsystem spec from sparse input and enter an interactive design dialogue. Use when starting a new component, subsystem, or feature that needs a planning document.
disable-model-invocation: true
argument-hint: <description>
allowed-tools: Write(specs/**) Edit(specs/**) Read
---

## Instructions

You are bootstrapping a new subsystem spec from this input: $ARGUMENTS

### Step 1 — Derive a name

From the input, derive a concise kebab-case filename (e.g. "VMSubstrate slab allocator layer" → `slab-allocator`).
Announce it: "I'll create this spec as `specs/<name>.md` — correct me if you'd prefer a different name."

### Step 2 — Create the initial skeleton

Using the template at the end of this skill, write `specs/<name>.md`:
- Populate only what can be confidently inferred from the input — do not speculate
- Set `kind: deferred`, `status: draft`
- If genuine design questions are already apparent, record them as ITEM-001, ITEM-002… in the Questions table
- Leave all other sections at their placeholder values

Write the file. Confirm it was created and list which sections were populated.

### Step 3 — Design dialogue loop

Enter an interactive design dialogue. On each iteration:

1. Identify the single most important unresolved aspect of the spec — prefer questions that are
   blocking or that would unlock other sections
2. Ask the user about it — one question at a time, clearly phrased. Do not ask multiple questions
   at once.
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

---

## Template

!`cat "${CLAUDE_SKILL_DIR}/SPEC_TEMPLATE.md"`