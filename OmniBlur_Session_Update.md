# OmniBlur — Session Update: 16/32-bit Support (16-bit working, 32-bit still broken)

## Starting point
Picked up right after Metal GPU rendering was confirmed fully working end-to-end
(build 15). Next roadmap item: extend rendering across bit depths, since the plugin
was still hardcoded to 8-bit (`PF_Pixel8`) everywhere on the CPU side.

First thing worth establishing up front: **the GPU path needed zero changes for
this.** `SmartRenderGPU` only ever creates `PF_PixelFormat_GPU_BGRA128` worlds —
32-bit float per channel, the only format `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32`
supports — and AE handles converting an 8/16-bit source layer into that F32 buffer
before the kernel runs, and back afterward. So all of this session's work is CPU-path
only: `DoBlur`, `Render()`, and `SmartRender`'s CPU fallback.

## 1. Declaring float awareness
`GlobalSetup` already had `PF_OutFlag_DEEP_COLOR_AWARE` (unlocks 16-bit). Added
`PF_OutFlag2_FLOAT_COLOR_AWARE` to `out_data->out_flags2` alongside the existing
Smart Render / GPU flags, needed to have AE offer 32bpc CPU rendering at all.

## 2. Getting the bitdepth at render time
`SmartRender` (the CPU Smart Render entry point) now reads:
```cpp
A_long bitdepth = extra->input->bitdepth;
```
Confirmed the field name by typing `extra->input->` by hand and checking Xcode's
autocomplete dropdown, rather than trusting it from memory — same rule the whole
project has followed for SDK field names.

## 3. Templating the blur math instead of tripling it
Turned `DoBlur`'s pixel loop into a template, `DoBlurTyped<PixelT>`, so the same
code compiles once each for `PF_Pixel8`, `PF_Pixel16`, and `PF_PixelFloat` instead of
three near-duplicate hand-written copies. Changes inside `DoBlurTyped`:
- Every `PF_Pixel8*` became `PixelT*`
- The four `long ...Sum` accumulators became `double` (needed for `PF_PixelFloat`,
  harmless for the integer types)
- `static_cast<A_u_char>(...)` became `static_cast<decltype(PixelT::alpha)>(...)`
  (and same for red/green/blue) — this is the one line doing real work, since it
  makes the cast target whatever channel type the current instantiation actually uses

Added a new, non-template `DoBlur(in_data, bitdepth, radius, input, output)` that
switches on `bitdepth` and calls the right instantiation, returning its result
directly. `SmartRender` and `Render()` both call this new `DoBlur`, not
`DoBlurTyped` directly. `Render()` (the classic, non-Smart fallback) always passes
`8` — modern AE prefers Smart Render whenever the effect declares support for it, so
this path shouldn't see 16/32-bit in practice; treated as an accepted limitation
rather than chased down further.

**First template mistake, worth remembering:** `template <typename PixelT>` has to
go *above* `static PF_Err`, not between the return type and the function name. Also
`bitdepth` doesn't belong on `DoBlurTyped`'s parameter list at all — `PixelT` already
encodes which depth you're in; `bitdepth` only belongs on the dispatcher.

## 4. The alignment bug (both 16-bit and 32-bit)
First build with the template in place looked clean, but AE showed corrupted output:
16-bit was misaligned every 960px horizontally, 32-bit every 480px. Root cause,
found by inspection rather than trial-and-error: `DoBlurTyped`'s scratch `temp`
world (the horizontal→vertical pass handoff, same role at every bit depth) was
still always being allocated 8-bit-sized via `wsP->new_world(...)`, regardless of
`PixelT`. Writing 16-bit or 32-bit pixels into a buffer sized for 8-bit overflows
into the next row on every write — which explains both symptoms (960 = 2×480,
matching the byte-size ratio between `PF_Pixel16` and `PF_PixelFloat`), and why
toggling Mercury GPU Acceleration on/off made no difference — this was entirely a
CPU-path bug, never touching `SmartRenderGPU`.

## 5. Fixing 16-bit
`PF_WorldSuite1::new_world` takes a flags argument, and one of its flags requests a
16-bit ("deep") world. Added, right before the `new_world` call in `DoBlurTyped`:
```cpp
PF_NewWorldFlags flags = PF_NewWorldFlag_CLEAR_PIXELS;
if (std::is_same<PixelT, PF_Pixel16>::value) {
    flags |= PF_NewWorldFlag_DEEP_PIXELS;
}
```
and changed the `new_world` call's flags argument from the literal
`PF_NewWorldFlag_CLEAR_PIXELS` to this new `flags` variable. Confirmed
`PF_NewWorldFlag_DEEP_PIXELS` is the real name via autocomplete. Added
`#include <type_traits>` near the top of the file for `std::is_same`.

**Result: 16-bit confirmed rendering correctly in AE after this fix.** 8-bit
unaffected (still uses the plain `PF_NewWorldFlag_CLEAR_PIXELS` path).

## 6. 32-bit: needed a whole different suite
`PF_WorldSuite1::new_world` has no pixel-format argument at all — it can't create a
float world no matter what flags you pass it. Had to find a suite that could.

Checked `suites.` autocomplete on the existing `AEGP_SuiteHandler suites(...)`
instance and found `WorldSuite1()`, `WorldSuite2()`, `WorldSuite3()`, and
`WorldTransformSuite1()` all listed. Went for the highest version first
(`WorldSuite3`) — wrong move, turned out its type (`PF_WorldSuite3`) doesn't exist;
Cmd+click on it landed in an `AEGP_SUITE_ACCESS_BOILERPLATE(WorldSuite, 3, AEGP_,
...)` line — that's the **AEGP** suite family (opaque `AEGP_WorldH` handles), a
different and unrelated API from the `PF_WorldSuite*` family this project has been
using everywhere else. Backed off and checked `WorldSuite2` the same way — same
`AEGP_` prefix problem, but that one's `PF_` counterpart (`PF_WorldSuite2`) is
actually the right tool here: Cmd+click on `PF_NewWorld` (found via `wsP2->`
autocomplete once a temporary `PF_WorldSuite2*` variable was declared) landed in
`AE_EffectCBSuites.h` and showed a real, pixel-format-aware signature:
```cpp
PF_Err (*PF_NewWorld)(
    PF_ProgPtr effect_ref,
    A_long widthL,
    A_long heightL,
    PF_Boolean clear_pixB,
    PF_PixelFormat pixel_format,
    PF_EffectWorld *worldP);
```
Exactly what was needed — no `_ex` variant required, `WorldSuite2` is enough.

### The accessor collision
`AEGP_SuiteHandler.h` already has a method literally named `WorldSuite2()` — for the
AEGP suite family, as above. Its `AEGP_SUITE_ACCESS_BOILERPLATE` macro turned out
to build *both* the generated method's name *and* its return type from the same
`SUITE_NAME`+`VERSION_NUMBER` arguments (confirmed by reading the macro's actual
`#define` body):
```cpp
#define AEGP_SUITE_ACCESS_BOILERPLATE(SUITE_NAME, VERSION_NUMBER, SUITE_PREFIX, MEMBER_NAME, kSUITE_NAME, kVERSION_NAME) \
SUITE_PREFIX##SUITE_NAME##VERSION_NUMBER *SUITE_NAME##VERSION_NUMBER() const \
{ ... }
```
First attempt tried invoking the macro with a different first argument
(`PFWorldSuite` instead of `WorldSuite`) to dodge the name collision — that broke
instead, since it also changed the *type* the macro tries to use
(`PF_PFWorldSuite2`, which doesn't exist). The macro can't have its method name and
type name decoupled by changing its arguments.

**Fix:** skip the macro for this one accessor, write it by hand instead, keeping
the correct type but giving it a non-colliding method name:
```cpp
PF_WorldSuite2 *PFWorldSuite2() const
{
    if (i_suites.world_suite2P == NULL) {
        i_suites.world_suite2P = (PF_WorldSuite2*)LoadSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
    }
    return i_suites.world_suite2P;
}
```
The struct member (`PF_WorldSuite2 *world_suite2P;`) and the release-boilerplate
line inside `ReleaseAllSuites()` didn't have this collision problem and were added
normally via the existing macros, same pattern as the `PF_GPUDeviceSuite1` fix
earlier in the project.

**Gotcha hit while pasting this in:** the hand-written method landed *after* the
class's closing `};` instead of inside the class body, which cascaded into a wall of
unrelated-looking errors (`Expected ';' after class`, `Use of undeclared identifier
'i_suites'`, `Use of undeclared identifier 'LoadSuite'`) across multiple files
(`MissingSuiteError.cpp`, `AEGP_SuiteHandler.cpp` both showed "too many errors
emitted, stopping now"). Also hit a duplicate stray `}` left over from an earlier
paste. Both cleared once the method was correctly relocated inside the class and the
extra brace removed. **Lesson reinforced again**: when a change to a shared header
produces a large, scattered error list, check placement/braces in the actually-edited
file first before assuming each error is independent.

### Wiring it into `DoBlurTyped`
```cpp
if (std::is_same<PixelT, PF_PixelFloat>::value) {
    ERR(wsP2->PF_NewWorld(
        in_data->effect_ref,
        width,
        height,
        TRUE,
        PF_PixelFormat_ARGB128, // placeholder -- confirm real name via autocomplete
        &temp));
} else {
    ERR(wsP->new_world(
        in_data->effect_ref,
        width,
        height,
        flags,
        &temp));
}
```
`PF_PixelFormat_ARGB128` was a guess based on the naming pattern of the already-
confirmed `PF_PixelFormat_GPU_BGRA128` (used in `SmartRenderGPU`) — that one's
GPU-specific though, and this is a CPU world, so the real constant name needed
independent confirmation via autocomplete against `AE_EffectPixelFormat.h`
(already in scope from the earlier `PF_PixelFormat_GPU_BGRA128` lookup).

## Where it stands
**Build succeeds clean.** In AE:
- 8-bit: correct
- 16-bit: correct (confirmed after the scratch-world fix in step 5)
- 32-bit: **still shows visible rendering errors**, not resolved this session

## Loose ends / next session starting point
- Confirm the real `PF_PixelFormat` constant name that ended up in the `PF_NewWorld`
  call (was mid-confirmation via autocomplete at session's end) and get it recorded
  in `project_OmniBlur.md` — if it's not actually the right float format, that alone
  could explain the remaining 32-bit corruption
- Re-check `PF_NewWorld`'s argument order/values against the confirmed signature,
  particularly the `clear_pixB` (`TRUE`) argument — worth testing `FALSE` too in case
  clearing a float-format world behaves differently than the 8/16-bit path
- Once 32-bit is confirmed visually correct, do a full regression pass: 8/16/32-bit,
  GPU on and off, for all three
- Bump `BUILD_VERSION` — not confirmed done this session, check `OmniBlur.cpp`'s
  `About()` block before the next test build
- Still-open items from the GPU session, unrelated to this one: delete the dead
  "Files '.cl' using Script" Build Rule; independently confirm `in16f = 0` and
  `extra->input->device_index` on `PF_SmartRenderExtra`

## Files changed
- `OmniBlur.cpp` — `DoBlur`/`DoBlurTyped` split and templated, `SmartRender` reads
  `bitdepth`, `Render()` updated call site, `GlobalSetup` float-aware flag,
  scratch-world creation branches by pixel type (16-bit via flags, 32-bit via
  `PF_WorldSuite2`), `#include <type_traits>` added
- `AEGP_SuiteHandler.h` (shared SDK file, **not tracked by git** — see
  `project_OmniBlur.md`'s "SDK-Level Edit Not Covered By Git" section) —
  `world_suite2P` struct member, release-boilerplate line, and a hand-written
  `PFWorldSuite2()` accessor method
- `project_OmniBlur.md` — status/roadmap sections updated to reflect this session

---

## Pushing to GitHub

Same established workflow for this repo:

```bash
cd /path/to/OmniBlur          # the repo root (one level above the Mac/ subfolder)
git add .
git status                    # sanity check — should show OmniBlur.cpp and
                               # project_OmniBlur.md as modified (AEGP_SuiteHandler.h
                               # is OUTSIDE the repo and won't show up here -- that's
                               # expected, it's the SDK-level edit noted above)
git commit -m "Add 16/32-bit CPU render support: templated DoBlurTyped, float-aware world creation (16-bit working, 32-bit still broken)"
git push
```

If `git status` shows anything unexpected (extra files, or nothing staged), stop and
paste it here before pushing.
