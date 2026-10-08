# crossjp: project skills

On-demand Agent Skills for this repository. Compatible agents load one when the
task matches its `description`; you do not invoke them by hand. They encode how
this firmware wants C/C++ written: the judgment calls and self-review gates that
keep the firmware small, stable, and reviewable.

These are written for capable agents, not beginners. They are principle- and
decision-focused on purpose. They deliberately avoid line-number citations,
which drift; they anchor on durable names (APIs, types, macros, files).

crossjp is the on-device EPUB and plain-text reader for the Xteink X3 and X4.
`src/screens/` is the UI, `src/core/` is the screen stack and settings,
`src/network/` is Wi-Fi and file transfer, `lib/hal/` wraps the board,
`lib/Typesetter/` lays out a book, and `lib/Gfx/` is the portrait framebuffer.

Each skill is a directory containing `SKILL.md` whose `name` matches the directory.

| Skill | Loads when you are... |
|---|---|
| `heap-discipline` | allocating memory: new/malloc/vector/string, buffers, caches |
| `control-flow-clarity` | writing branching logic, state flags, modes, if/else ladders |
| `hal-and-abstractions` | touching storage, input, display, settings, or rendering |
| `scope-discipline` | adding a feature, screen, lib, setting, or dependency |
| `refactor-for-review` | refactoring, cleaning up, or preparing a change for review |

Each skill ends with a self-review checklist the agent runs against its own
diff before handing it back.

## Maintaining these

Edit the `SKILL.md` under each directory. Keep them tight. Do not restate
another skill; add the judgment that skill does not already carry. Trigger
quality lives in the `description` field: it must name the situations that
should pull the skill in, in the words a contributor's task would use.
