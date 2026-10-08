---
name: heap-discipline
description: "Memory allocation discipline for the ESP32-C3 (~380KB RAM, no PSRAM, single framebuffer). Use whenever writing or reviewing code that allocates: new / malloc / std::vector / std::string, buffers, caches, or anything held across a loop or a screen lifecycle. Covers makeUniqueNoThrow vs raw new/malloc, fragmentation avoidance, reserve-before-push_back, alloc-once-reuse, stack vs heap sizing, and ScratchHeap."
---

# Heap Discipline (ESP32-C3)

The constraint that makes every call matter: ~380KB RAM, no PSRAM, one
framebuffer. **Fragmentation, not total usage, is what kills this device.**
Free-heap can read fine while the largest free block is too small for the next
allocation. Optimize for not leaving holes, not just for using fewer bytes.

`ScratchHeap` is the one large hole. `main` reserves it before the framebuffer
so the framebuffer does not split the heap. Borrowers write
`ScratchHeap::data()` and must not free it. `ScratchHeap::release()` hands the
hole to the next large allocation. After Wi-Fi, only a restart gets a clean
hole back. Do not malloc a second full-screen buffer. Grayscale passes reuse
the `Gfx` framebuffer (`startGrayscaleBase`, `copyGrayscaleLsbBuffers`,
`cleanupGrayscaleBuffers`).

## Allocation decision procedure

Ask in order; stop at the first yes.

1. **Stack?** Local, bounded, under ~256 bytes total: plain array/struct. No
   heap, no fragmentation. Keep frames lean; the loop task stack is 12 KB
   because EPUB inflate, `FatFile::write`, and the SD SPI wait share it.
2. **Compile-time constant?** `static constexpr` lives in flash, costs zero DRAM.
3. **Allocated once and reused for a screen's lifetime?** Allocate in
   `onEnter`, hold in a member, release in `onExit`. Never per-frame, never
   per-iteration.
4. **Dynamic and fallible?** `makeUniqueNoThrow<T>(...)` /
   `makeUniqueNoThrow<T[]>(n)` from `lib/Memory/Memory.h`. Null-check, `LOG_ERR`
   with the size, return false. It frees on every exit path.
5. **A C/SDK API takes ownership and frees it itself?** Only then raw
   `new (std::nothrow)` / `malloc`, with a comment naming who frees it.

Bare `new` / `new[]` is never correct here: under `-fno-exceptions` it calls
`abort()` on OOM instead of returning null.

## Fragmentation rules

- `std::vector`: `reserve(n)` before any `push_back` loop. Each growth is
  alloc-copy-free (three heap ops) and leaves a hole. Unknown n: estimate high.
  A page index belongs on the SD card. Do not hold one RAM vector per page of
  a long book.
- No repeated `new`/`delete` or growing containers inside a loop or render path.
  Hoist the allocation out of the loop.
- `std::string` / Arduino `String`: acceptable on cold paths (file I/O, the
  file-transfer request, one-shot setup). Banned on hot/render paths. Build
  text with a stack `char[]` + `snprintf`; if a `std::string` is unavoidable,
  `reserve` it first.

## Justify every allocation

When you add a heap allocation, state in one line why stack/static/reuse was
rejected and the worst-case size. If you cannot name the size, you cannot
budget it, and you should not allocate it.

## Self-review before handoff

- [ ] No bare `new`/`new[]`. Every fallible alloc is `makeUniqueNoThrow`, or a
      raw alloc with an explicit owner comment.
- [ ] Every allocation is null-checked with `LOG_ERR` before the error return.
- [ ] No allocation inside a loop or render path that could be hoisted.
- [ ] Every `push_back` loop has a preceding `reserve`.
- [ ] Anything allocated in `onEnter` is released in `onExit`; member `HalFile`
      closed there too.
- [ ] No second full-screen buffer. The picture page and other large scratch
      use `ScratchHeap`.
- [ ] Each new allocation carries a one-line size + why-not-stack/static note.
