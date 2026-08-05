# OmniBlur — Session Update: 32-bit follow-up, GPU confirmations, CPU blur performance

## Starting point
Picked up right after the prior session ended with 8-bit and 16-bit CPU rendering
confirmed correct, and 32-bit float still showing visible rendering errors despite a
clean build. Open items carried in: confirm the real `PF_PixelFormat` constant used
for the float scratch world, re-check `PF_NewWorld`'s `clear_pixB` argument, confirm
`extra->input->device_index` and `in16f = 0` on `PF_SmartRenderExtra`, delete the dead
Build Rule, and bump `BUILD_VERSION`.

## 1. PF_PixelFormat_ARGB128 confirmed correct
Looked up the real `AE_EffectPixelFormat.h` directly. `PF_PixelFormat_ARGB128` is
built from `MAKE_PIXEL_FORMAT_FOURCC('a', 'e', '3', '2')` — "After Effects-style
ARGB, 32 bits floating point per channel, 1.0 is white." The name already used in
`DoBlurTyped`'s `wsP2->PF_NewWorld(...)` call was correct as written. Not the source
of the earlier corruption.

## 2. 32-bit corruption: not reproducible
Re-tested and could not recreate the visible errors from last session. Two likely
factors: the earlier test had AE's built-in Grid effect (8-bit only) applied
upstream of OmniBlur, and clearing AE's disk cache made the errors disappear.
Treating this as a probable stale-cache/tainted-source artifact rather than a real
bug in the `PF_WorldSuite2::PF_NewWorld` path — **not conclusively confirmed either
way**. `project_OmniBlur.md` updated to reflect this; worth a clean re-test (genuine
32-bit source, cleared cache) before fully trusting 32-bit is solid.

## 3. GPU-path unknowns confirmed
Looked up the real `PF_SmartRenderInput` struct (what `extra->input` points to in
`SmartRenderGPU`):
```cpp
typedef struct {
    PF_RenderRequest output_request;
    short            bitdepth;
    void             *pre_render_data;
    const void       *gpu_data;      // AE 16.0+
    PF_GPU_Framework what_gpu;       // AE 16.0+
    A_u_long         device_index;   // AE 16.0+
} PF_SmartRenderInput;
```
`extra->input->device_index` is confirmed correct as written. `in16f = 0` left as-is
— matches the F32-only GPU support already declared
(`PF_OutFlag2_SUPPORTS_GPU_RENDER_F32`), no change needed.

## 4. Build version bumped
`BUILD_VERSION` 16 → 17. Rebuilt clean, no errors.

## 5. CPU blur performance: sliding-window box sum
Baseline going in: 256ms at radius 10, vs. native AE blurs on the same test — CC
Cross Blur 22ms, Fast Box Blur 28ms, Gaussian Blur 2ms.

Root cause: both passes in `DoBlurTyped` recomputed the entire window sum from
scratch for every output pixel (`for dx = -radius to radius` nested inside
`for x`) — O(radius) work per pixel per pass, not O(1).

**Horizontal pass fix:** one running sum per row. Prime it once for `x = 0`, then as
`x` advances, add the pixel entering the window on the right (`x + radius`) and
subtract the one leaving on the left (`x - radius - 1`), each clamped to
`[0, width)`. O(1) per pixel, and access stays row-major (cache-friendly) since it's
all still one row at a time.

**Vertical pass fix:** same sliding-window idea, but the window slides along `y`
while output still needs to be written row-major for cache-friendliness. Instead of
one scalar running sum, kept one running sum **per column**
(`std::vector<double>`, sized to `width`, one per channel, plus a
`std::vector<A_long>` for count), and stepped all columns forward together as `y`
advances — each row of `temp` gets added to / subtracted from every column's sum
exactly once across the whole pass. Still O(1) amortized per output pixel.
Added `#include <vector>` (`#include <type_traits>` was already present from the
bit-depth work — no change needed there).

Confirmed: build succeeded, render time dropped substantially.

## 6. Removed unneeded full-buffer clear
`temp`'s allocation was requesting a full clear every render — `PF_NewWorldFlag_CLEAR_PIXELS`
on the CPU path (`PF_Pixel8`/`PF_Pixel16`), `clear_pixB = TRUE` on the float path
(`PF_WorldSuite2::PF_NewWorld`). Unnecessary: with the sliding window, every pixel
of `temp` always has `count >= 1` (the window always includes the pixel itself) and
gets overwritten by Pass 1 regardless, so the pre-clear was a wasted full-frame
memset every render.
- `flags` initializer changed from `PF_NewWorldFlag_CLEAR_PIXELS` to `0`
  (`PF_NewWorldFlag_DEEP_PIXELS` still OR'd in for `PF_Pixel16` right after)
- `clear_pixB` argument to `wsP2->PF_NewWorld(...)` changed from `TRUE` to `FALSE`

Build succeeded.

## 7. Remaining performance gap diagnosed
After the above, radius no longer drives render time much — single-digit radii were
over 100ms, radius 10-11 (fastest) around 90ms. Roughly flat across radius points at
a fixed per-frame cost rather than the blur math, which is already O(1)/pixel.
Prime suspect: `temp` is allocated fresh and disposed on every single render call.
Fast Box Blur almost certainly reuses a scratch buffer across frames instead.

## 8. Drafted plan: cache the scratch buffer via SequenceSetup/SequenceSetdown
No code written yet — plan only, recorded in `project_OmniBlur.md` roadmap section 1a.
Summary:
- New `OmniBlurSequenceData` struct: cached `PF_EffectWorld`, its cached
  `width`/`height`/`bitdepth`, and a `has_temp` flag
- `PF_Cmd_SEQUENCE_SETUP` (and `PF_Cmd_SEQUENCE_RESETUP` — need to confirm whether
  these share one `switch` case): allocate a handle sized to the struct, zero it,
  mark `has_temp = FALSE`, store as `out_data->sequence_data`
- `DoBlurTyped`: reuse the cached world when width/height/bitdepth match; only
  reallocate on first render or when the comp/bit depth changes; stop disposing
  `temp` at the end of every call — its lifetime becomes "the life of the sequence"
- `PF_Cmd_SEQUENCE_SETDOWN`: dispose the cached world (if any) and the handle itself
- **Thread-safety note, checked and confirmed safe as planned:** OmniBlur does not
  set `PF_OutFlag2_SUPPORTS_THREADED_RENDERING`, so AE does not run this effect's
  render selectors concurrently — mutating `sequence_data` inside `SmartRender` is
  safe. If Multi-Frame Rendering support is ever added later, this caching scheme
  would need to be redone in favor of something like `PF_EffectSequenceDataSuite1`
  — flagged for a deliberate future decision, not blocking now.
- Explicitly out of scope: the GPU path's own `CreateGPUWorld`/`DisposeGPUWorld`
  scratch buffer in `SmartRenderGPU` — a separate, already-necessary GPU-resident
  allocation, untouched by this plan.
- Open items to VERIFY before building: exact handle allocation API for
  `out_data->sequence_data` (likely `PF_NEW_HANDLE`/`PF_LOCK_HANDLE`/
  `PF_UNLOCK_HANDLE`/`PF_DISPOSE_HANDLE`, not yet confirmed via autocomplete for this
  use); whether `SEQUENCE_SETUP`/`SEQUENCE_RESETUP` need distinct handling; whether
  `in_data->sequence_data` is already valid/locked to dereference directly inside
  `SmartRender`/`Render` or needs an explicit lock call first.
- Test plan once built: resize the comp, change bit depth mid-session, scrub the
  timeline — confirm the cache invalidates/reallocates correctly with no leaks or
  stale/wrong-sized buffers handed back.

## Where it stands
- Build succeeds clean, `BUILD_VERSION` 17.
- 8-bit and 16-bit CPU rendering: correct. 32-bit float: corruption from last
  session not reproducible; not conclusively resolved — re-test with a clean cache
  and genuine 32-bit source before trusting it.
- GPU path: all previously-open assumptions (`device_index`, `in16f`) now confirmed.
- Sliding-window blur math and unneeded-clear removal: done, build succeeds, render
  time substantially improved.
- Cached scratch buffer (the next big performance win): planned, not yet built.

## Loose ends / next session starting point
- Build the SequenceSetup/SequenceSetdown scratch-buffer caching plan from section 8
  above (resolve the VERIFY items via Xcode autocomplete first)
- Re-test 32-bit with a genuine float source and a clean AE cache to conclusively
  confirm or rule out the earlier corruption
- Once the cache lands, re-measure render time against the native blurs (target:
  closer to Fast Box Blur's ~20-28ms)
- Still not done: delete the dead "Files '.cl' using Script" Build Rule
- After performance work lands: full 8/16/32-bit × GPU on/off regression pass before
  starting on additional blur algorithms or the luma-based blur map (deferred by
  design this session)

## Files changed
- `OmniBlur.cpp` — `BUILD_VERSION` bumped 16→17; `DoBlurTyped`'s horizontal and
  vertical passes rewritten as O(1)-per-pixel sliding windows; scratch-world
  `flags`/`clear_pixB` no longer request a full clear; `#include <vector>` added
- `project_OmniBlur.md` — bit-depth status updated (ARGB128 confirmed, 32-bit
  corruption marked unreproduced), GPU device_index/in16f marked confirmed, new
  Roadmap section 1a (CPU Render Path performance) added with the
  SequenceSetup/Setdown plan, build order and version-bump notes updated
- `OmniBlur_Session_Update.md` — this file

---

## Pushing to GitHub

Same established workflow for this repo:

```bash
cd /path/to/OmniBlur          # the repo root (one level above the Mac/ subfolder)
git add .
git status                    # sanity check -- should show OmniBlur.cpp and
                               # project_OmniBlur.md as modified, plus this new
                               # OmniBlur_Session_Update.md as untracked/added
git commit -m "Sliding-window blur math, drop unneeded scratch-world clear, confirm GPU device_index/in16f and PF_PixelFormat_ARGB128, draft SequenceSetup/Setdown caching plan"
git push
```

If `git status` shows anything unexpected (extra files, or nothing staged), stop and
paste it here before pushing.
