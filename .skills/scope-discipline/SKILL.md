---
name: scope-discipline
description: "Feature-scope discipline for the crossjp e-reader. Use when adding a feature, a new screen, a new lib, a setting, or a dependency, or when a request would grow the firmware's surface. Covers the product boundary, the RAM-cost vs reading-benefit gate, preferring no-code or existing-mechanism solutions, and how to push back on out-of-scope asks."
---

# Scope Discipline

The mission: typeset an EPUB or a UTF-8 text file on the Xteink X3 or X4.
One firmware binary per panel. The UI is Japanese.

## The gate

Before adding a feature, screen, lib, setting, or dependency, answer in order:

1. **Does it typeset or open a book the device already accepts?** In: `.epub`
   and `.txt` (UTF-8, including light Aozora markup). Out: `.xtch`, `.xtc`,
   OPDS, WebDAV, KOSync, a dictionary, theme packs, more UI languages, a
   second reading orientation, and anything that is not reading (apps, feeds,
   audio). If it is out, say so and stop.
2. **Does it materially improve focused reading?** If the benefit is
   "nice to have" or serves a different use case, it is out.
3. **What does it cost in RAM and in the largest-free-block budget?** A feature
   that adds steady-state RAM or a large transient allocation needs a reading
   benefit that clearly outweighs it. Budget the allocation with
   `heap-discipline`.
4. **Can it be done with no new code?** Prefer an existing screen, an existing
   setting, or a doc over a new code path.

If a request fails the gate, push back with the specific reason and offer the
in-scope alternative. Make the call and say why.

## Surface awareness

Each new screen is permanent RAM, permanent maintenance, and another thing
every future change must not break. Default to extending an existing screen
or setting before adding one. New top-level surface needs a reading
justification.

## Settings are not free

`settings.bin` is a packed struct (`Settings::VERSION`). A new field is a
version bump and must not shift the bytes already on cards. `reservedLanguage`
is the spare byte. Add a setting only when readers genuinely need the choice;
otherwise pick a fixed default.

## Self-review

- [ ] Stays inside EPUB / UTF-8 text on X3 and X4.
- [ ] Stated the concrete reading benefit.
- [ ] Named the RAM cost and why the benefit wins.
- [ ] Checked whether an existing screen or setting already covers it.
- [ ] A new settings field keeps the on-disk layout and is justified by a
      real reader need.
