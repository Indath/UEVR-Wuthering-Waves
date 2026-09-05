# Wuthering Waves LGUI — UI/Scene Separation & UI Cutoff Investigation

**Game:** Wuthering Waves (Unreal Engine, uses **LGUI**, not Slate/UMG)
**Branch:** `uevr-wuthering-waves`
**Problem:** In Native Stereo Fix (NSF) ON, the bottom of the LGUI UI was clipped off. Broader
goal: decouple the UI's layout resolution/aspect from the stereo/eye render resolution.

> **READ THIS FIRST.** This document records every approach tried, what was proven, and what NOT
> to retry. Several "obvious" ideas here are **conclusively disproven with runtime logs** — do not
> waste time re-attempting them. The only working lever is documented in "What Actually Works".

---

## TL;DR — The One Invariant That Matters

**LGUI clips its raster/layout extent to the RENDER-TARGET (UI target) size, NOT to
`FSceneView::ViewRect`, NOT to the per-draw scratch rects, NOT to the game-thread `FViewport`
size.** Presentation-side cropping/stretching can only recover pixels that were *actually
rasterized*. Therefore the **only** way to make the full UI appear is to **size the UEVR-owned UI
target large enough to hold the full canvas**, then crop/stretch it back to 16:9 at presentation
(OpenXR quad).

---

## What Actually Works (Current Shipping Solution)

1. **Size the UEVR-owned UI target independently of the scene RT.**
   - `FFakeStereoRenderingHook::get_ui_target_size()` returns a target large enough to hold the
	 full per-eye canvas (`max(scene RT, per-eye canvas)`), gated by
	 `VR::is_native_stereo_fix_tall_ui_enabled()` so it only applies when NSF is active.
   - The UI target allocation in `pre_texture_hook_callback()` uses this size instead of the shared
	 scene RT size.
2. **Match the UI swapchain to the UI target size** in both `D3D12Component.cpp` and
   `D3D11Component.cpp` (swapchain creation + recreation gates + OpenVR `ui_desc`).
3. **Crop/stretch at presentation** in `OverlayComponent::OpenXR::generate_slate_quad()` — crops the
   quad `imageRect` to `get_ui_draw_extent()` and forces a minimum 16:9 presentation aspect.
4. **NSF-only gating + toggle.** New toggle `m_native_stereo_fix_tall_ui`
   ("Fit UI To Full Canvas (Fix Bottom Cutoff)") in the Native Stereo Fix menu section, with accessor
   `is_native_stereo_fix_tall_ui_enabled()` (true only when the toggle AND NSF are both on). This
   prevents the tall target from distorting AFR / NSF-OFF modes.
5. **Dynamic mode switching.** `D3D12Component.cpp` / `D3D11Component.cpp` watch for
   `is_native_stereo_fix_tall_ui_enabled()` flipping and call `set_should_recreate_textures(true)` so
   the UI target reallocates live (no restart) when the toggle/NSF changes.

**Result:** NSF ON shows the full bottom of the UI and is playable. Toggling NSF/the fix updates live.

---

## Known Remaining Issues (Open, Low Priority)

- **NSF-OFF still needs a game restart** to resize the UI correctly after the tall-target change.
  Deprioritized by the user. The dynamic-recreation watcher covers the toggle flip but a pure
  in-game resolution change in NSF-OFF is still driven by the engine's own reallocation path.
- **NSF ON compressed after an in-game resolution change** until the menu screen forces a resize.
  Timing artifact: LGUI paints one or two empty/old-size frames right after a resolution change
  before repainting at the new size. Low priority ("not a big deal").

---

## DEAD ENDS — DO NOT RETRY (all proven inert or harmful)

### 1. Mutating the shared `FSceneView` / `FSceneViewFamily` ViewRect (PERSISTENT) — **HARMFUL**
Modifying the rect on the view object that the **stereo world render also consumes** corrupted the
stereo projection/raster (right eye pushed up, black bar). The same view object feeds the 3D scene
passes. **This is what caused the earlier "broke stereo rendering" regression.** Never persistently
mutate the shared view rect.

### 2. Game-thread `FSceneView` clone (pristine copy fed to LGUI) — **INERT**
Cloned the view on the game thread hoping LGUI would read the clone. LGUI did not read our copy for
its raster extent. Wrong object, wrong thread.

### 3. Render-thread `a2` scratch-rect patching (per-draw FViewInfo rects at 0x3e0/0x410/0x4a0/0x4f8)
— **INERT**
Patching the per-draw scratch rects changed layout numbers but `[LGUI_BOUNDS]` painted region stayed
at the per-eye size. LGUI does not take its clip extent from these.

### 4. Texture-like RDG entry / a3 shadow-copy redirect — **INERT (copy) / HARMFUL (real swap)**
- Passing a shadow copy of the `FRDGTexture` (a3) as the argument: **inert** (LGUI doesn't use the
  argument itself).
- Persistently swapping the **real** a3 RHI pointer: redirected the whole scene composite
  (ViewFamilyTexture is shared) → eyes went blank. **Harmful.**

### 5. Self-field patching (a2 offsets 0x270/0x348/0x3a0 etc.) — **PARTIAL/CRASH**
Redirecting the a3-refs inside a2 could redirect the UI, but redirecting the wrong group
(0x3d0/0x400/0x4e8) crashed when a menu opened (another pass reads those). Not a viable clean lever.

### 6. D3D12 viewport/scissor vtable hook — **INERT (and suspected lag source)**
The viewport was already full-target (3840x2160), so patching it did nothing. Also a global vtable
hook that was a confirmed performance drag; removed.

### 7. Game-thread `FViewport::GetSizeXY` / SizeXY patch (Option B) — **INERT**
`FViewport::GetSizeXY` already read 3840x2160; shrinking it for the UI draw was a no-op for the
painted extent. Proven by `[LGUI_D3D]` (viewport always full-target) + `[LGUI_BOUNDS]`.

### 8. Render-thread-scoped **real `FSceneView::ViewRect` swap** (the "swap during LGUI's pass,
restore immediately after" idea) — **INERT (definitively disproven 2026-09-05)**
This is the most important dead end to record because it is the most intuitively appealing.
- **Design:** Hook LGUI's render pass (`lgui_slot24_hook`), overwrite the REAL `FSceneView::ViewRect`
  reachable from `a3` to the ui_target size for the DURATION of LGUI's draw only, restore
  synchronously the instant it returns. Architecturally safe for stereo (scoped, restored same-call).
- **Result:** The swap fired every draw (`[LGUI_VRPROBE] swapped 1 real view rect field(s)
  a3view+a88 -> 3840x2683`), but `[LGUI_BOUNDS]` painted width **stayed at hmd_w (2268), never
  3840**. LGUI ignored the swapped rect entirely.
- **Conclusion:** LGUI does **not** read `FSceneView::ViewRect` for its raster/layout extent. It
  clips to the render-target/HMD size. Safe, but **inert**. Do not retry.
- Probe code is preserved behind `constexpr bool LGUI_PROBE_VIEWRECT_SWAP = false` in
  `FFakeStereoRenderingHook.cpp` with the result recorded in a comment. Flip to `true` (plus
  `LGUI_BOUNDS_DIAG` in `D3D12Component.cpp`) only to re-measure — but the answer is already known.

---

## How the UI Actually Gets Its Resolution/Scale (Expected Pathway)

Based on all diagnostics, LGUI's screen-space UI pathway is:

1. **Game thread:** LGUI's `ISceneViewExtension` callbacks (slot 2 `SetupView`, slot 13
   `BeginRenderViewFamily`) run. The canvas layout (the rect LGUI paints into) is fixed here, and it
   measures as exactly `hmd_w x hmd_h`.
2. **Render thread:** LGUI's draw runs via `ISceneViewExtension` slot 24 (`lgui_slot24_hook`
   intercepts this). The **raster viewport is set from `Desc.Extent` of the render target**
   (top-left aligned), NOT from any rect in the view/family/scratch state.
3. **Render target resolution:** LGUI fetches its RT via
   `InView.Family->RenderTarget->GetRenderTargetTexture()`, i.e. the `FViewport` vtable slot that
   UEVR's "AHUD UI Compatibility" option (`viewport_get_render_target_texture_hook`) redirects to the
   UEVR-owned `ui_target`.
4. **Clip:** The painted extent is bounded by the UI target's `Desc.Extent` height/width. If the
   canvas is taller than the target, the bottom is clipped (the original bug).
5. **Presentation:** UEVR copies the UI target and presents it as an OpenXR quad layer, cropping to
   the reported draw extent and forcing a minimum 16:9 aspect.

**Key takeaway:** Steps 2–4 mean the *render-target size is the single authoritative clip bound*.
Every layout-side lever (view rect, viewport size, scratch rects) was proven not to move the clip.

---

## Diagnostic Infrastructure (for future testers)

All gated behind compile-time flags; **leave FALSE for normal play** (they cause render-thread lag):
- `LGUI_BOUNDS_DIAG` (`D3D12Component.cpp`) — blocking GPU readback of the UI target's actual painted
  bbox. The ground-truth "what did LGUI paint" measurement. Also gates the `[LGUI_OUT]` post-blit
  readback.
- `LGUI_PROBE_VIEWRECT_SWAP` (`FFakeStereoRenderingHook.cpp`) — the disproven real-ViewRect swap
  probe. Emits `[LGUI_VRPROBE]`.
- `LGUI_DIAG_VERBOSE` (`FFakeStereoRenderingHook.cpp`) — verbose per-draw a2 residue scan + `[LGUI_REFS]`.
- `diag_scan_lgui_render_anchors()` — one-shot module scan for LGUI renderer string anchors
  (`[LGUI_ANCHOR]`).

**Interpretation rule:** Compare `[LGUI_VRPROBE]`/patch logs (what we changed) against
`[LGUI_BOUNDS]` (what LGUI actually painted). If the painted bbox does not follow your change, the
lever is inert.

---

## Future Directions (Theoretical, Unexplored)

These are genuinely untried and *could* separate UI resolution from scene resolution. Ordered by
estimated promise:

1. **Hook the RT-provider, not the view.** Since LGUI resolves its RT via
   `Family->RenderTarget->GetRenderTargetTexture()`, the clean separation point is to give LGUI a
   *dedicated* render target sized purely for the UI aspect (e.g. always 3840x2160 16:9) while the
   stereo scene keeps its own per-eye targets. The current fix already sizes the shared `ui_target`;
   the next step would be a **fully independent UI RT** that never tracks the eye resolution at all,
   so no mode (AFR/NSF-OFF/NSF-ON) ever compresses it. This is the most promising path.
2. **Intercept LGUI's ortho/projection matrix construction.** LGUI builds its canvas ortho projection
   somewhere on the game thread from the viewport/canvas size. If that construction site can be found
   (via `[LGUI_ANCHOR]` string scan → the function that builds the screen-space ortho), overriding
   the projection there would decouple UI aspect from render resolution at the true source, rather
   than fighting it downstream.
3. **Force LGUI's `Desc.Extent` at RT-allocation to a fixed 16:9 canvas.** Instead of
   `max(scene, canvas)`, always allocate the UI target at a fixed 16:9 resolution (e.g. 3840x2160)
   regardless of HMD/eye size, and let presentation scale it to the headset. Removes the timing/
   compression artifacts on resolution change because the UI RT never changes size.
4. **Two-phase canvas: render UI at native, composite separately.** Give LGUI its own
   `FSceneViewFamily` with a UI-only view sized to the canvas, rendered to the dedicated UI RT,
   entirely outside the stereo family. Highest effort, cleanest theoretical separation, but requires
   constructing/registering a parallel view family — significant engine-internals work.

**Do NOT pursue:** anything in the "DEAD ENDS" list above. In particular, any approach that relies on
LGUI reading `FSceneView::ViewRect`, the per-draw scratch rects, or the game-thread `FViewport` size
for its clip extent is proven inert.

---

## File Map (where the logic lives)

| File | Role |
|---|---|
| `src/mods/vr/FFakeStereoRenderingHook.hpp` | `get_ui_target_size()`, `get/set_ui_draw_extent()` declarations |
| `src/mods/vr/FFakeStereoRenderingHook.cpp` | UI target allocation, `lgui_slot24_hook`, all probes, NSF gating |
| `src/mods/vr/D3D12Component.cpp` | D3D12 UI copy, `LGUI_BOUNDS`/`LGUI_OUT` diagnostics, swapchain sizing/recreation, tall-UI mode watcher |
| `src/mods/vr/D3D11Component.cpp` | D3D11 parity for UI target sizing/recreation + mode watcher |
| `src/mods/vr/OverlayComponent.cpp` | OpenXR quad presentation crop/stretch to draw extent + 16:9 min aspect |
| `src/mods/VR.hpp` | `m_native_stereo_fix_tall_ui` toggle + `is_native_stereo_fix_tall_ui_enabled()` |
| `src/mods/VR.cpp` | Native Stereo Fix menu entry for the tall-UI toggle |
| `src/Framework.hpp` / `src/Framework.cpp` | Shared RT size accessors (fallback when tall-UI disabled) |
