# OmniBlur — After Effects Plugin

## Overview
OmniBlur is an After Effects plugin built with the Adobe AE SDK (ae25.6_61.64bit), developed in Xcode on Apple Silicon. Started as a proof-of-concept box blur; the long-term goal is a full-featured, GPU-accelerated blur toolkit with multiple algorithms and a luma-based blur map.

## Current Status (POC)
- Plugin registers correctly in AE's Effect Manager via `PluginDataEntryFunction2`
- **Separable box blur implemented** — horizontal pass then vertical pass via an intermediate scratch `PF_EffectWorld`, replacing the original O(radius²) nested-loop blur with O(radius) per pixel. Pulled out of `Render()` into a standalone `DoBlur(in_data, radius, input, output)` helper so both the classic and Smart FX render paths can share it.
- **Converted from classic render architecture to Smart FX.** Added `PF_OutFlag2_SUPPORTS_SMART_RENDER` in `GlobalSetup`, plus `PreRender()` (`PF_Cmd_SMART_PRE_RENDER`) and `SmartRender()` (`PF_Cmd_SMART_RENDER`). `PreRender` grows the requested input checkout rect by `radius` in every direction so edge pixels don't blur against out-of-bounds data. Classic `Render()` kept as a fallback for non-Smart hosts, calling the same `DoBlur` helper. **Build succeeded, edge blurring confirmed correct.** This was the necessary prerequisite for GPU rendering — Metal isn't reachable through the classic Render path.
- **Metal GPU rendering: in progress.** Added `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` in `GlobalSetup`, plus scaffolding for `GPUDeviceSetup` (`PF_Cmd_GPU_DEVICE_SETUP`), `GPUDeviceSetdown` (`PF_Cmd_GPU_DEVICE_SETDOWN`), and `SmartRenderGPU` (`PF_Cmd_SMART_RENDER_GPU`), dispatched from `EffectMain`. Confirmed via Xcode autocomplete that `PF_GPUDeviceSetupInput` only exposes `device_index` (`A_u_long`) and `what_gpu` — no direct device pointer field. Still need: the actual suite call (off `AEGP_SuiteHandler`, same pattern as `PF_WorldSuite1`) that turns `device_index` into a real `MTLDevice`; proper build-time wiring for the kernel source (Adobe's SDK uses a `GF_KERNEL_FUNCTION`-based DSL translated into CUDA/OpenCL/Metal at build time, not raw inline Metal source); and the actual two-pass (horizontal/vertical) kernel dispatch in `SmartRenderGPU`, currently commented out as a sketch.
- Radius slider works (`PF_ADD_SLIDER`, read via `params[OMNIBLUR_RADIUS]->u.sd.value`)
- Deep color aware flag set (`PF_OutFlag_DEEP_COLOR_AWARE`)
- Current build: v1.0.3, build 6
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

## Remaining Known Issues
- **Bit depth**: still 8-bit only (`PF_Pixel8`); no 16/32-bit path yet
- **Metal/GPU acceleration**: scaffolding in place (see above), not yet functional. Specifically still open:
  - The suite call to convert `device_index` into a real `MTLDevice` (confirmed it's not a struct field on `PF_GPUDeviceSetupInput`)
  - Where the `GF_KERNEL_FUNCTION`-based kernel source actually needs to live in this project's build setup for the CUDA/OpenCL/Metal translation step to pick it up
  - The real two-pass (horizontal/vertical) `MTLComputeCommandEncoder` dispatch in `SmartRenderGPU` — currently a commented-out sketch, not real calls
  - How `gpu_data` (the struct holding the compiled Metal pipeline state) is actually stashed between `GPUDeviceSetup` and retrieved in `SmartRenderGPU`/`GPUDeviceSetdown`

## Architecture Notes
- Entry point: `EffectMain`, dispatches on `PF_Cmd` (ABOUT, GLOBAL_SETUP, PARAMS_SETUP, RENDER)
- Registration: `PluginDataEntryFunction2` (required — PiPL alone doesn't register the plugin on this SDK setup)
- Header quirk: `entry.h` lives in `Examples/Util/`, not `Examples/Headers/` — added to Xcode Header Search Paths
- `PiPL.h` does not exist in this SDK version
- `DllExport` macro doesn't resolve in this project's include setup — using `__attribute__((visibility("default")))` instead
- Suite access pattern: any AE SDK functionality beyond basic `in_data`/`out_data` fields (e.g. world allocation, pixel iteration, sampling) is reached via a "suite" struct of function pointers, fetched through `AEGP_SuiteHandler`. Expect this pattern to recur for future features (e.g. GPU device suites for Metal work).

## Roadmap

### 1. Performance: Metal GPU Rendering — **IN PROGRESS**
- Move render path from CPU (`PF_EffectWorld` pixel loops) to a Metal compute shader
- Requires: Metal device/command queue setup, bridging `PF_EffectWorld` data to `MTLTexture`/`MTLBuffer`, compute pipeline state, threadgroup dispatch sized to frame dimensions
- ~~Consider `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` / Smart Render pipeline (may require converting from classic to Smart FX architecture — worth confirming before deep Metal work)~~ **DONE** — Smart FX conversion complete and verified working; `SUPPORTS_GPU_RENDER_F32` flag added
- ~~Interim CPU win before Metal lands: separable blur (horizontal pass then vertical pass) to cut box blur from O(r²) to O(r) per pixel~~ **DONE**
- Confirmed: the `MTLDevice` is not a direct field on `PF_GPUDeviceSetupInput` (only `device_index`/`what_gpu` are) — reaching it requires a suite call, same "fetch via `AEGP_SuiteHandler`, not `in_data->utils`" pattern documented under Known Issues Resolved below. Next: identify the exact GPU device suite accessor via Xcode autocomplete on `suites.`
- Reference: Adobe's `SDK_Invert_ProcAmp` sample implements this full pipeline (CUDA/OpenCL/Metal) and is the intended template to cross-check field/method names against, rather than guessing from SDK docs alone
- Kernel logic is written once using Adobe's `GF_KERNEL_FUNCTION` macro DSL and auto-translated into CUDA/OpenCL/Metal at build time (via the same Boost toolchain already in use for this project) — not three hand-written kernels

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
2. ~~Convert classic render architecture to Smart FX (prerequisite for GPU rendering)~~ **DONE** — build succeeded, edge blurring confirmed correct
3. Metal compute shader for box blur (validates the GPU pipeline before adding algorithm complexity) — **IN PROGRESS**: `GPUDeviceSetup`/`GPUDeviceSetdown`/`SmartRenderGPU` scaffolded and wired into `EffectMain`; still need the device suite call, kernel build wiring, and the actual two-pass dispatch
4. 16/32-bit support (extend the now-working Metal path across bit depths)
5. Additional blur algorithms (fast blur, lens blur) built on top of the working GPU pipeline
6. Luma-based blur map (layered on top, since it modulates whichever algorithm is active)

## Tooling
- Xcode (Apple Silicon), AE SDK ae25.6_61.64bit
- Git + GitHub for version control
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
