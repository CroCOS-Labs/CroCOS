# specs

Subsystem planning documents for CroCOS.

Each file is a spec in the format defined by `.claude/skills/spec-init/SPEC_TEMPLATE.md`.

## Workflow

| Skill | When |
|---|---|
| `/spec-init <name>` | Start a new spec from sparse input |
| `/spec-refine <name>` | Deepen a spec over one or more sessions |
| `/spec-status <name>` | Check current state and blockers |
| `/spec-review <name>` | Adversarial pass — find gaps and hazards |
| `/spec-resolve <name> <ITEM-nnn>` | Record a decision reached in conversation |
| `/spec-implement <name>` | Implementation handoff (leaf specs only) |

Skills that can also be invoked by Claude automatically: `spec-status`, `spec-review`, `spec-resolve`.
