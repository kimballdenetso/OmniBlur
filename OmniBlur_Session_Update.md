# OmniBlur — Session Update: GPU Dispatch Complete

## What got done this session

Picked up where the last session left off — Metal kernel generation, device lookup,
and pipeline creation were already working, but `SmartRenderGPU` had no real dispatch
code. Two things stood between that and a working GPU blur:

### 1. Intermediate GPU-resident scratch buffer
The horizontal blur pass needs somewhere to write its output before the vertical pass
reads it — same role as `DoBlur()`'s CPU-only scratch world, but GPU-side. Added via
`PF_GPUDeviceSuite1::CreateGPUWorld` / `DisposeGPUWorld`:

- `CreateGPUWorld` takes 9 arguments (confirmed via Xcode jump-to-definition after an
  initial guess was short by 5): `effect_ref`, `device_index`, `width`, `height`,
  `pixel_aspect_ratio`, `field_type`, `pixel_format`, `clear_pixB`, and an out-param
  for the new world.
- Used `PF_PixelFormat_GPU_BGRA128` (confirmed: GPU, BGRA, 32-bit float per channel —
  matches the F32-only GPU support already declared), `PF_Field_FRAME` (no
  interlacing to preserve in a scratch buffer), and skipped the pixel clear since the
  blur pass overwrites every pixel anyway.
- Confirmed `AcquireExclusiveDeviceAccess`/`ReleaseExclusiveDeviceAccess` aren't
  needed here — that's only for plugins sharing one render path between CPU and GPU;
  OmniBlur has a dedicated `SmartRenderGPU` entry point, so exclusive access is
  already held automatically.
- `DisposeGPUWorld` is disposed on every exit path (guarded so a mid-render failure
  can't leak the GPU buffer), using a manual error-preserving pattern rather than an
  assumed `ERR2` macro that isn't confirmed to exist in this SDK's `Param_Utils.h`.

### 2. The actual Metal dispatch
Read the real generated kernel signature by temporarily dumping the compiled Metal
source to disk (since guessing at GPU buffer bindings risks silent wrong output
rather than a build error):

```
kernel void BoxBlurKernel(
    device const float4* inSrc      [[buffer(0)]],
    device float4*       outDst     [[buffer(1)]],
    device BoxBlurKernelValues *inValues [[buffer(2)]],
    uint2 inXY [[thread_position_in_grid]])
```

`BoxBlurKernelValues` packs `inSrcPitch, inDstPitch, in16f, inWidth, inHeight,
inRadius, inHorizontal` — notably including an `in16f` field the earlier placeholder
sketch didn't have.

Wrote `SmartRenderGPU` to match: `GetGPUWorldData` pulls each world's real GPU
buffer (bridged to `id<MTLBuffer>`, same `__bridge` pattern already confirmed for
`devicePV`/`command_queuePV`), then two `dispatchThreads` passes on one command
buffer/encoder — horizontal (input → intermediate), then vertical (intermediate →
output) — with a `memoryBarrierWithScope:MTLBarrierScopeBuffers` between them, since
Metal doesn't auto-sync successive dispatches within a single encoder. Pitch
converted from `rowbytes` (bytes) to a float4-element stride via `/ 16`.

**Result: clean build (build 15), and the blur renders correctly on GPU in After
Effects.** Metal GPU rendering is now functionally complete end-to-end.

## Loose ends (not blocking, worth revisiting)
- Delete the dead "Files '.cl' using Script" Build Rule in Xcode — confirmed unused,
  just hasn't been cleaned up yet.
- A few assumptions worked on the first real test but aren't independently
  confirmed: `in16f = 0` as the correct "not half-float" value, and
  `extra->input->device_index` existing on `PF_SmartRenderExtra` the same way it
  does on `GPUDeviceSetupExtra`.

## Files changed
- `OmniBlur.cpp` — `SmartRenderGPU` dispatch, `BoxBlurKernelValues` struct, version
  bumped to build 15
- `project_OmniBlur.md` — roadmap and status sections updated to reflect GPU
  rendering as complete

---

## Pushing to GitHub

Your established workflow for this repo (from `git add .` → `git status` → `commit`
→ `push`):

```bash
cd /path/to/OmniBlur          # the repo root (one level above the Mac/ subfolder)
git add .
git status                    # sanity check — should show OmniBlur.cpp and
                               # project_OmniBlur.md as modified
git commit -m "Wire up Metal GPU dispatch: intermediate buffer + two-pass compute encoder (build 15)"
git push
```

If `git status` shows anything unexpected (extra files, or nothing staged), stop and
paste it here before pushing — easier to sort out before it's on GitHub than after.
