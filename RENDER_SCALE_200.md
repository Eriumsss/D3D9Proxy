# Experimental D3D9 200% windowed backbuffer supersampling

**NOT scene-only and NOT retail-gameplay validated.** This opt-in feature is
for the 2009 Conquest x86 D3D9 proxy, not RTX Remix, not OS display scaling,
not a custom desktop mode, and not a change to native Havok/registry pools.

## What 200% means

The request is **two times each physical backbuffer axis**, or **four
times as many pixels**. A window at **1920 × 1080** asks the Direct3D9
driver for a **3840 × 2160** primary swapchain backbuffer. A windowed
`Present` with no source rectangle scales that backbuffer down to the
window's client area. A source RECT expressed in logical window
coordinates is adjusted by the proxy; destination RECT stays in window
client coordinates. Window size, monitor resolution and desktop DPI
are not changed.

This feature is **not a per-scene internal 3D rendering resolution**.
Conquest uses render-to-texture passes, HDR and postprocessing, whose
separate `CreateRenderTarget` / `CreateTexture` buffers are NOT
automatically enlarged. A frame rendered primarily to offscreen
textures may still contain native-resolution 3D and only supersample
the final backbuffer composition. Some fullscreen quads or pretransformed
UI geometry may be mispositioned. This is not a substitute for a
render-graph aware scene-only 200% resolution solution.

For genuine full pipeline supersampling when the game accepts the mode,
consider GPU driver **4.00× DSR** and selecting the doubled
1920×1080 → 3840×2160 resolution in-game. DSR does not require patching
the 2009 engine's render target topology.

## Activation

In the game EXE's existing `proxy.ini`, add:

```ini
[Graphics]
RenderScalePercent=200
```

The auto-generated template uses `RenderScalePercent=100` (OFF).
Only values `100` and `200` are meaningful; anything other than 200
takes the original code path. No changes to other `[StaticMapPools]`
or texture-pool flags are required. This is intentionally independent
of the engine's many experimental registry extensions.

The option is applied only in **windowed** Direct3D9 when the driver
supports 2×width and 2×height within its maximum 2D texture limits,
and the original swapchain is not multisampled. Zero-dimension windowed
backbuffers resolve against the real window's client area.
`CreateDevice` first tries the doubled dimensions; on failure it retries
the original presentation parameters. `Reset` re-evaluates the feature,
catches already-doubled presentation arguments and retries native sizes
if the supersampled swapchain cannot be reset.

The proxy tracks logical viewport/scissor coordinates on the **actual
primary backbuffer only**. Shadowmaps, HDR buffers, depth buffers,
render-to-texture passes and unrelated targets are left alone. It also
scales up to 16 explicit `Clear` rectangles where safe; ordinary
full-surface clears are unchanged. No GPU shader, game EXE, texture
asset, Windows display setting or external D3D9 DLL is patched.

## Log and actual validation

`render_scale_200.log` is created beside the game's EXE only when the
200% option is requested. Statuses:
- `CREATE_ACTIVE`: 2× backbuffer creation returned success.
- `CREATE_FALLBACK`: driver rejected 2× backbuffer and the proxy tried
  the original resolution.
- `CREATE_UNSUPPORTED`: fullscreen, multisampling, unreadable dimensions
  or driver size caps prevented the 2× attempt.
- `RESET_ACTIVE`: 2× backbuffer reset succeeded.
- `RESET_FALLBACK`: 2× reset failed and native was retried.

A successful D3D9 backbuffer creation is **NOT** proof the game correctly
sized its viewport, postprocess quads, HUD or offscreen 3D passes.

Test only in a disposable copy:

1. At `RenderScalePercent=100`, confirm normal 1920×1080 gameplay,
   HUD, mouse, Alt+Enter, level exit and reload.
2. At `RenderScalePercent=200`, inspect the log and the entire screen:
   quarter-screen content, distorted aspect, missing postprocessing,
   incorrect UI hitboxes, offset HUD and excessive VRAM use are failures.
3. Change level, enter/leave several matches, Alt+Enter repeatedly and
   verify the fallback path. If any mismatch occurs, restore `100`
   and restart.

Windows MSVC x86 and Python source-contract CI do NOT run a retail game
or prove this is a real 200% **scene-only** supersampling option.
If only the 3D world should be supersampled while the HUD remains native
resolution, the game's full scene-to-HDR and HUD composition call graph
must first be reconstructed. Do not advertise this proxy-level
backbuffer experiment as accomplishing that narrower goal.

## Direct3D9 specification

Microsoft Direct3D9 documents that windowed swapchain backbuffer
dimensions may differ from the window client size and that `Present`
stretches source pixels to its destination rectangle as needed:
- https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpresent-parameters
- https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dswapchain9-present
