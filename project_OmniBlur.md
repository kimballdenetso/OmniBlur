# OmniBlur — After Effects Plugin

## Overview
OmniBlur is an After Effects plugin built with the Adobe AE SDK (ae25.6_61.64bit), developed in Xcode on Apple Silicon. Started as a proof-of-concept box blur; the long-term goal is a full-featured, GPU-accelerated blur toolkit with multiple algorithms and a luma-based blur map.

## Current Status (POC)
- Plugin registers correctly in AE's Effect Manager via `PluginDataEntryFunction2`
- **Separable box blur implemented in `Render()`** — horizontal pass then vertical pass via an intermediate scratch `PF_EffectWorld`, replacing the original O(radius²) nested-loop blur with O(radius) per pixel
- Radius slider works (`PF_ADD_SLIDER`, read via `params[OMNIBLUR_RADIUS]->u.sd.value`)
- Deep color aware flag set (`PF_OutFlag_DEEP_COLOR_AWARE`)
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
- **No Metal/GPU acceleration yet** — separable blur is the CPU-side interim win

## Architecture Notes
- Entry point: `EffectMain`, dispatches on `PF_Cmd` (ABOUT, GLOBAL_SETUP, PARAMS_SETUP, RENDER)
- Registration: `PluginDataEntryFunction2` (required — PiPL alone doesn't register the plugin on this SDK setup)
- Header quirk: `entry.h` lives in `Examples/Util/`, not `Examples/Headers/` — added to Xcode Header Search Paths
- `PiPL.h` does not exist in this SDK version
- `DllExport` macro doesn't resolve in this project's include setup — using `__attribute__((visibility("default")))` instead
- Suite access pattern: any AE SDK functionality beyond basic `in_data`/`out_data` fields (e.g. world allocation, pixel iteration, sampling) is reached via a "suite" struct of function pointers, fetched through `AEGP_SuiteHandler`. Expect this pattern to recur for future features (e.g. GPU device suites for Metal work).

## Roadmap

### 1. Performance: Metal GPU Rendering
- Move render path from CPU (`PF_EffectWorld` pixel loops) to a Metal compute shader
- Requires: Metal device/command queue setup, bridging `PF_EffectWorld` data to `MTLTexture`/`MTLBuffer`, compute pipeline state, threadgroup dispatch sized to frame dimensions
- Consider `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` / Smart Render pipeline (may require converting from classic to Smart FX architecture — worth confirming before deep Metal work)
- ~~Interim CPU win before Metal lands: separable blur (horizontal pass then vertical pass) to cut box blur from O(r²) to O(r) per pixel~~ **DONE**

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
2. Metal compute shader for box blur (validates the GPU pipeline before adding algorithm complexity)
3. 16/32-bit support (extend the now-working Metal path across bit depths)
4. Additional blur algorithms (fast blur, lens blur) built on top of the working GPU pipeline
5. Luma-based blur map (layered on top, since it modulates whichever algorithm is active)

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
