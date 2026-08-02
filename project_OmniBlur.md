# OmniBlur — After Effects Plugin

## Overview
OmniBlur is an After Effects plugin built with the Adobe AE SDK (ae25.6_61.64bit), developed in Xcode on Apple Silicon. Started as a proof-of-concept box blur; the long-term goal is a full-featured, GPU-accelerated blur toolkit with multiple algorithms and a luma-based blur map.

## Current Status (POC)
- Plugin registers correctly in AE's Effect Manager via `PluginDataEntryFunction2`
- **Separable box blur implemented** — horizontal pass then vertical pass via an intermediate scratch `PF_EffectWorld`, replacing the original O(radius²) nested-loop blur with O(radius) per pixel. Pulled out of `Render()` into a standalone `DoBlur(in_data, radius, input, output)` helper so both the classic and Smart FX render paths can share it.
- **Converted from classic render architecture to Smart FX.** Added `PF_OutFlag2_SUPPORTS_SMART_RENDER` in `GlobalSetup`, plus `PreRender()` (`PF_Cmd_SMART_PRE_RENDER`) and `SmartRender()` (`PF_Cmd_SMART_RENDER`). `PreRender` grows the requested input checkout rect by `radius` in every direction so edge pixels don't blur against out-of-bounds data. Classic `Render()` kept as a fallback for non-Smart hosts, calling the same `DoBlur` helper. **Build succeeded, edge blurring confirmed correct.** This was the necessary prerequisite for GPU rendering — Metal isn't reachable through the classic Render path.
- **Metal GPU rendering: in progress, kernel pipeline now fully wired and building.** Added `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` in `GlobalSetup`, plus `GPUDeviceSetup` (`PF_Cmd_GPU_DEVICE_SETUP`), `GPUDeviceSetdown` (`PF_Cmd_GPU_DEVICE_SETDOWN`), and `SmartRenderGPU` (`PF_Cmd_SMART_RENDER_GPU`), dispatched from `EffectMain`.
  - **Kernel source and build-time translation: DONE.** Kernel logic lives in `OmniBlur_Kernel.cu` (the real `GF_KERNEL_FUNCTION` DSL implementation of `BoxBlurKernel`, adapted from the earlier sketch — wrapped in `#if GF_DEVICE_TARGET_DEVICE`, includes `PrGPU/KernelSupport/KernelCore.h` and `KernelMemory.h`). `OmniBlur_Kernel.cl` is a one-line trigger file (`#include "OmniBlur_Kernel.cu"`) that an Xcode custom Build Rule watches for. The Build Rule runs Adobe's `GPUUtils/CreateCString.py` toolchain (via two `clang -E` preprocess passes, one per `-DGF_DEVICE_TARGET_METAL=1` / `_OPENCL=1`) to generate `OmniBlur_Kernel.metal.h` (symbol `kOmniBlur_Kernel_MetalString`) and `.cl.h` (symbol `kOmniBlur_Kernel_OpenCLString`) at build time. Confirmed working via a successful build log showing the rule firing on `OmniBlur_Kernel.cl`.
  - **Real MTLDevice lookup: DONE.** `GPUDeviceSetup` now calls the real `PF_GPUDeviceSuite1::GetDeviceInfo(effect_ref, device_index, &device_info)` and reads `device_info.devicePV` (bridged to `id<MTLDevice>`) and `device_info.command_queuePV` (bridged to `id<MTLCommandQueue>`, stashed in the `OmniBlurMetalGPUData` struct alongside `blur_pipeline` for later use in `SmartRenderGPU`). `source` in `GPUDeviceSetup` now pulls from `kOmniBlur_Kernel_MetalString` instead of an empty placeholder string.
  - **`GPUDeviceSetup` crash and shader compile error: RESOLVED.** After wiring up the real `MTLDevice` lookup, the plugin started crashing After Effects. Root cause: `GPUDeviceSetup` created the Metal library/function/pipeline unconditionally with no nil/error checks — unlike the rest of the file's `if (!err)` pattern — so a failed device lookup or shader compile fell through to calling `newComputePipelineStateWithFunction` with a nil `MTLFunction`, which is a hard Metal validation crash (not a catchable AE error). Fixed by guarding the whole block behind `!err`/nil checks, checking each Metal API call's result, and logging failures via `os_log_error` with `%{public}@` (plain `NSLog`/`%@` gets redacted to `<private>` by macOS's unified logging for dynamic strings — use `os_log` with the public format specifier to actually see compiler errors in Console.app).
  - Once the crash was fixed, `GPUDeviceSetup` failed cleanly instead (AE warning: "GPU device setup failed; the effect will render on the CPU") with a Metal shader compile error: `MTLLibraryErrorDomain Code=3, "redefinition of 'clamp'"`. Root cause: `KernelCore.h` does `#include <metal_common/metal_compute/metal_integer/metal_texture>` inside its `#if GF_DEVICE_TARGET_METAL` block (confirmed by inspecting the header directly, around line 68). Our Build Rule's `clang -E` preprocessing pass textually inlines those real Apple headers into the generated kernel source — and Metal's compiler *also* implicitly supplies its own copy of the same standard library when compiling that source via `newLibraryWithSource:`. Two copies of `metal_common` in the same compiled unit = redefinition.
  - **Fix:** created four empty stub header files (`metal_common`, `metal_compute`, `metal_integer`, `metal_texture`, 0 bytes each) in a new `MetalStubHeaders/` folder alongside the project. Added a new Xcode Custom Path `METAL_STUB_HEADERS_PATH` pointing at it (Xcode → Settings → Locations → Custom Paths, same pattern as `BOOST_BASE_PATH`/`GPUUTILS_BASE_PATH`). Swapped the Build Rule's `-I` flag from the real Apple `GPUCompiler.framework` metal header path to this stub path, so our own preprocessing pass resolves those `#include` lines to nothing, and Metal's own implicit stdlib inclusion becomes the only real copy. Confirmed fixed by grepping the actual preprocessed `.i` file on disk for `metal_common` before/after.
  - **Gotcha hit while debugging this:** the project has **two separate Xcode Build Rules that both match `*.cl` files** — "Files '.cl' using Script" and "OpenCL source files using Script." Only the second one is actually invoked by Xcode; edits made to the first one silently do nothing, no error, no indication anything is wrong. Wasted several rebuild cycles before catching this by directly grepping the real preprocessed output file on disk (`${DERIVED_FILE_DIR}/64/PreprocessedMetal/${INPUT_FILE_BASE}.i`) instead of trusting that a script edit had taken effect. **"Files '.cl' using Script" should be deleted** (Build Rules tab → trash icon) to prevent this from happening again — not yet done.
  - Also discovered along the way: an `-fdirectives-only` flag added to `clang -E` during debugging turned out to have zero effect on the output (verified byte-identical: 161762 bytes / 5343 lines with or without it) — it does not stop `#include` from being followed and inlined, despite that being the initial assumption. Not used in the final fix.
  - **Still open:** the actual two-pass (horizontal/vertical) `MTLComputeCommandEncoder` dispatch inside `SmartRenderGPU` — still the original commented-out sketch, not real calls yet. This is the last piece before GPU rendering is functionally complete. Everything upstream of it (kernel generation, device lookup, pipeline creation) is now confirmed working end-to-end with no crash and no compile error.
- Radius slider works (`PF_ADD_SLIDER`, read via `params[OMNIBLUR_RADIUS]->u.sd.value`)
- Deep color aware flag set (`PF_OutFlag_DEEP_COLOR_AWARE`)
- Current build: v1.0.3, build 6 (bump before next test build)
- **Git + GitHub connected** (repo: `github.com/kimballdenetso/OmniBlur`, public, `main` branch). Set up via Terminal rather than Xcode's Source Control UI. Notable gotchas from setup, worth remembering if repeating this for another project:
  - The `Mac` subfolder (containing the actual `.xcodeproj`) had accidentally been initialized as its own nested git repo at some point — git treats a folder containing its own `.git` as an embedded repo/gitlink, not as regular tracked files, which silently breaks clones. Fix was `rm -rf Mac/.git` before adding it to the outer repo.
  - `.gitignore` needs `build/` (no leading slash, matches at any depth) to exclude Xcode's build output — the default entries alone (`DerivedData/`, `xcuserdata/`) don't catch it, since this project's build folder is just named `build/`.
  - Standard day-to-day workflow: `git add .` → `git status` (sanity check before every commit) → `git commit -m "..."` → `git push`.
- Build succeeds and links cleanly (see Known Issues Resolved below)

## Known Issues Resolved
- **Scratch buffer allocation**: world creation/disposal is NOT on `in_data->utils` — it lives behind `PF_WorldSuite1`, accessed via `AEGP_SuiteHandler`. Correct call pattern:
  ```cpp
  AEGP_SuiteHandler suites(in_data->pica_basicP);
  PF_WorldSuite1 *wsP = suites.WorldSuite1();   // NOT WorldSuite2 — this SDK only has v1
  ERR(wsP->new_world(in_data->effect_ref, width, height, PF_NewWorldFlag_CLEAR_PIXELS, &temp));
  // ...
  ERR(wsP->dispose_world(in_data->effect_ref, &temp));
  ```
  Xcode's autocomplete (type `suites.` and let it suggest) is the most reliable way to confirm exact suite accessor names — more reliable than reading SDK header text directly, since names/versions can be inconsistent across SDK releases.
- **Linker "undefined symbol" errors for `AEGP_SuiteHandler`**: the header declares the class, but its implementation (`AEGP_SuiteHandler.cpp`) and a helper it calls (`MissingSuiteError.cpp`) are separate files in the SDK's `Examples/Util/` folder that must be explicitly added to the Xcode target — being reachable via Header Search Paths is NOT enough for `.cpp` implementation files, only for `.h` declarations. Both now added to the "Supporting Code" group with target membership checked.
- **`PF_GPUDeviceSuite1` missing from `AEGP_SuiteHandler`**: this SDK's shared `AEGP_SuiteHandler.h` (in `Examples/Util/`, not duplicated inside the OmniBlur repo) predates the GPU device suite and has no accessor for it — confirmed by inspecting the real `Suites` struct and boilerplate macro list, no `GPUDeviceSuite1`/`gpu_device_suite1P` entry anywhere. Fixed by manually adding three lines following the exact pattern already used for `WorldSuite1`: a struct member (`PF_GPUDeviceSuite1 *gpu_device_suite1P;`), a release-boilerplate line inside `ReleaseAllSuites()`, and an access-boilerplate line in the public section (`AEGP_SUITE_ACCESS_BOILERPLATE(GPUDeviceSuite, 1, PF_, gpu_device_suite1P, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);`) — plus `#include "AE_EffectGPUSuites.h"` near the file's other includes. Since this edits the *shared* SDK copy rather than a project-local file, marked with a comment explaining why it's there; the fix will also apply automatically to any other plugin built against this same SDK checkout.
  - Gotcha hit while making this edit: the release-boilerplate line and the access-boilerplate line both get added "near `world_suite1P`," but that name appears in two different sections of the file (private release section vs. public accessor section) — easy to drop both new lines in the same spot by mistake. Doing so puts a full macro-expanded function definition inside `ReleaseAllSuites()`'s body, which fails with a cluster of confusing "undeclared identifier" errors that don't obviously point at "wrong location."
  - Second gotcha: got a duplicate copy of the access-boilerplate line pasted in twice (once during the original edit, again while relocating it to fix the above) — surfaced as "Class member cannot be redeclared." Cmd+F across the file for the new member's name is the fastest way to spot duplicates.
- **Metal kernel build pipeline**: kernel source (`GF_KERNEL_FUNCTION` DSL) must be compiled at build time into a per-platform header via Adobe's `GPUUtils/CreateCString.py` — not written as raw Metal source directly. Requires: a custom Xcode Build Rule matching `*.cl` files, a Custom Path build setting pointing at the SDK's `GPUUtils` folder (`GPUUTILS_BASE_PATH`, following the same pattern as the existing `BOOST_BASE_PATH`), and Header Search Paths including `$(DERIVED_FILE_DIR)/64/PreprocessedMetal` so the generated header can be found by a plain `#include`. Verified the machine's `GPUCompiler.framework` version (`32023`, clang lib path `32023.884`) matches Adobe's reference sample exactly, so the Metal include path in the Build Rule script needed no adjustment.

## SDK-Level Edit Not Covered By Git
**`METAL_STUB_HEADERS_PATH` Custom Path:** like `BOOST_BASE_PATH` and `GPUUTILS_BASE_PATH`, this is an Xcode app-level preference (Xcode → Settings → Locations → Custom Paths), not a project file — so it's **not backed up by git** and won't exist on a fresh machine or after an Xcode reinstall. If the Metal shader ever starts failing again with "redefinition of 'clamp'" (or similar redefinition errors for `metal_compute`/`metal_integer`/`metal_texture`), check that this Custom Path still exists and points at:
```
/Users/kimball/Documents/Dev/AE-SDK/ae25.6_61.64bit.AfterEffectsSDK/Examples/Effect/OmniBlur/MetalStubHeaders
```
That folder itself (4 empty files: `metal_common`, `metal_compute`, `metal_integer`, `metal_texture`) *is* inside the OmniBlur repo path and so *is* tracked by git — only the Xcode-side Custom Path pointer needs manual recreation.

The `PF_GPUDeviceSuite1` fix to `AEGP_SuiteHandler.h` (see Known Issues Resolved above) lives in the shared AE SDK folder, not inside the OmniBlur repo — so it's **not backed up by version control**. If that SDK checkout is ever reinstalled, wiped, or replaced, this fix needs to be redone by hand. Steps to redo:

1. Open `AEGP_SuiteHandler.h` (find it via Cmd+click on `#include "AEGP_SuiteHandler.h"` in `OmniBlur.cpp`, or it's at `Examples/Util/AEGP_SuiteHandler.h` inside the SDK checkout).
2. Add `#include "AE_EffectGPUSuites.h"` near the file's other includes at the top.
3. Inside the private `struct Suites { ... }` block, find `PF_WorldSuite1 *world_suite1P;` and add right after it:
   ```cpp
   PF_GPUDeviceSuite1			*gpu_device_suite1P;
   ```
4. Inside `ReleaseAllSuites()`, find `AEGP_SUITE_RELEASE_BOILERPLATE(world_suite1P, kPFWorldSuite, kPFWorldSuiteVersion1);` and add right after it:
   ```cpp
   AEGP_SUITE_RELEASE_BOILERPLATE(gpu_device_suite1P, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
   ```
5. In the public section, find `AEGP_SUITE_ACCESS_BOILERPLATE(WorldSuite, 1, PF_, world_suite1P, kPFWorldSuite, kPFWorldSuiteVersion1);` and add right after it:
   ```cpp
   AEGP_SUITE_ACCESS_BOILERPLATE(GPUDeviceSuite, 1, PF_, gpu_device_suite1P, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
   ```
6. Double check with Cmd+F for `gpu_device_suite1P` — should be exactly 3 matches (struct member, release line, access line), none of them inside `ReleaseAllSuites()`'s body except the release line itself.

Worth considering longer-term: copying this one header into the OmniBlur repo (alongside the already-copied `AEGP_SuiteHandler.cpp`/`MissingSuiteError.cpp`) so this fix travels with the project instead of living in a shared, unversioned SDK folder.

## Remaining Known Issues
- **Bit depth**: still 8-bit only (`PF_Pixel8`); no 16/32-bit path yet
- **Metal/GPU acceleration**: kernel generation and device lookup are done (see Current Status above). Specifically still open:
  - The real two-pass (horizontal/vertical) `MTLComputeCommandEncoder` dispatch in `SmartRenderGPU` — currently a commented-out sketch, not real calls
  - Verifying GPU rendering actually runs correctly end-to-end in AE (visually confirming the blur, not just confirming the code compiles) — not yet tested, since `SmartRenderGPU`'s dispatch isn't implemented
  - No CUDA host-launch wrapper was written for `OmniBlur_Kernel.cu` (intentionally omitted — Mac-only project, `__NVCC__`-guarded code isn't needed)

## Architecture Notes
- Entry point: `EffectMain`, dispatches on `PF_Cmd` (ABOUT, GLOBAL_SETUP, PARAMS_SETUP, RENDER, and now GPU_DEVICE_SETUP / GPU_DEVICE_SETDOWN / SMART_RENDER_GPU)
- Registration: `PluginDataEntryFunction2` (required — PiPL alone doesn't register the plugin on this SDK setup)
- Header quirk: `entry.h` lives in `Examples/Util/`, not `Examples/Headers/` — added to Xcode Header Search Paths
- `PiPL.h` does not exist in this SDK version
- `DllExport` macro doesn't resolve in this project's include setup — using `__attribute__((visibility("default")))` instead
- Suite access pattern: any AE SDK functionality beyond basic `in_data`/`out_data` fields (e.g. world allocation, pixel iteration, sampling, GPU device info) is reached via a "suite" struct of function pointers, fetched through `AEGP_SuiteHandler`. This pattern held for the GPU device suite too, once the missing accessor was added by hand (see Known Issues Resolved).
- Kernel source split: `OmniBlur_Kernel.cu` (real cross-platform DSL implementation) + `OmniBlur_Kernel.cl` (thin include trigger for the Build Rule) — mirrors Adobe's own `SDK_Invert_ProcAmp` sample structure exactly.

## Roadmap

### 1. Performance: Metal GPU Rendering — **IN PROGRESS**
- Move render path from CPU (`PF_EffectWorld` pixel loops) to a Metal compute shader
- Requires: Metal device/command queue setup, bridging `PF_EffectWorld` data to `MTLTexture`/`MTLBuffer`, compute pipeline state, threadgroup dispatch sized to frame dimensions
- ~~Consider `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` / Smart Render pipeline~~ **DONE**
- ~~Interim CPU win before Metal lands: separable blur~~ **DONE**
- ~~Identify the exact GPU device suite accessor and wire up the real MTLDevice/command queue lookup~~ **DONE**
- ~~Kernel build wiring: get `GF_KERNEL_FUNCTION` DSL translating into a usable Metal source string at build time~~ **DONE**
- **Next:** write the real `MTLComputeCommandEncoder` dispatch in `SmartRenderGPU` (two passes, horizontal then vertical, threadgroups sized to `width`/`height`), then test an actual GPU-rendered blur in AE
- Reference: Adobe's `SDK_Invert_ProcAmp` sample implements this full pipeline (CUDA/OpenCL/Metal) and was cross-checked directly for the kernel Build Rule script and the `PF_GPUDeviceSuite1` call shape

### 2. Bit Depth Support: 16-bit and 32-bit
- Currently 8-bit only (`PF_Pixel8`)
- Add 16-bit path (`PF_Pixel16`) and 32-bit float path (`PF_PixelFloat`)
- Requires checking `PF_WORLD_IS_DEEP` / bit depth flags at render time and branching pixel format handling accordingly (or building this natively into the Metal shader with format-agnostic math)

### 3. Multiple Blur Algorithms
- Add an algorithm selector param (`PF_ADD_POPUP` or similar) to switch between:
  - **Fast Blur** (AE-style approximate/optimized blur)
  - **Box Blur** (current POC baseline)
  - **Lens Blur** (bokeh-style, likely needs a proper circular kernel / possibly depth-aware)
  - (room for more — e.g. Gaussian, directional/motion blur)
- Each algorithm likely wants its own render function, dispatched by the selected popup value, sharing the same param plumbing where possible

### 4. Luma-Based Blur Map (reference layer)
- Add a layer parameter (`PF_ADD_LAYER`) to let users pick a second layer as a blur map input
- Sample the luma of that layer per-pixel to modulate blur radius/intensity spatially (bright = more blur, dark = less, or vice versa — likely worth an invert toggle)
- Needs to handle the reference layer's own resolution/transform relative to the effect layer (checkout point sampling / resizing behavior)

## Suggested Build Order
1. ~~Separable blur (quick CPU perf win, low risk)~~ **DONE**
2. ~~Convert classic render architecture to Smart FX (prerequisite for GPU rendering)~~ **DONE**
3. Metal compute shader for box blur — **IN PROGRESS**: kernel generation, device/command queue lookup all wired and building; only the actual compute dispatch in `SmartRenderGPU` remains
4. 16/32-bit support (extend the now-working Metal path across bit depths)
5. Additional blur algorithms (fast blur, lens blur) built on top of the working GPU pipeline
6. Luma-based blur map (layered on top, since it modulates whichever algorithm is active)

## Tooling
- Xcode (Apple Silicon), AE SDK ae25.6_61.64bit, checkout at `/Users/kimball/Documents/Dev/AE-SDK/ae25.6_61.64bit.AfterEffectsSDK`
- Git + GitHub for version control
- Custom Xcode paths (Locations → Custom Paths): `BOOST_BASE_PATH` (Boost), `GPUUTILS_BASE_PATH` (SDK's `Examples/GPUUtils` folder, used by the kernel Build Rule)
- **Deployment is automated via an Xcode Run Script build phase** — every build copies, unquarantines, and re-signs the plugin directly into AE's plugin folder. No manual copy step needed:
  ```bash
  PLUGIN_DEST="/Applications/Adobe After Effects 2026/Plug-ins/voidy/OmniBlur.plugin"

  rm -rf "$PLUGIN_DEST"
  cp -R "${TARGET_BUILD_DIR}/${WRAPPER_NAME}" "$PLUGIN_DEST"
  xattr -dr com.apple.quarantine "$PLUGIN_DEST" 2>/dev/null

  codesign --force --deep --sign - "$PLUGIN_DEST"

  echo "Copied and signed OmniBlur.plugin to AE Plug-ins folder"
  ```
- Full quit/relaunch of AE is still required after each build to pick up the updated plugin — AE only scans its plugin folder at launch.
- **Bump the version/build number in the `About()` string (`MAJOR_VERSION`/`MINOR_VERSION`/`BUG_VERSION`/`BUILD_VERSION` at the top of `OmniBlur.cpp`) on every build.** AE caches plugin metadata aggressively; without a version bump, it's easy to be looking at a stale build and mistake it for the current one. Check via Effect > About in AE, or the effect's own About entry, to confirm the version matches what you just built.
