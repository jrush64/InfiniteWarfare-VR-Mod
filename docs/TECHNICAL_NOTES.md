# Technical notes

How the VR component works, which engine addresses it uses, what was tried and failed, and where
to go next, so whoever continues does not repeat the dead ends.

## Target

- Call of Duty: Infinite Warfare, Steam app 292730, **build 2715288**. `iw7_ship.exe` is
  48,612,264 bytes, PE timestamp 2018-04-18, SHA-256
  `a883ee28ee9f552c63d8ed1e352f60501b2e256d8857ca178c6dd3aae622723b`.
- D3D11 only. Image base `0x140000000`.
- **The code sections are Arxan-encrypted on disk.** Every known function disassembles to garbage
  from the file. iw7-mod maps the decrypted image into `iw7-mod.exe` at `0x140000000`, so **all
  addresses below are runtime VAs inside `iw7-mod.exe`**, and any static analysis needs a memory
  image of a running `iw7-mod.exe` (no game-derived dumps are included in this repository).
- Campaign launch through iw7-mod: the console command `spMode`
  (`Com_GameMode_SetDesiredGameMode(GAME_MODE_SP)`). Launch with `-noupdate` so the updater does not
  replace a local build.

## Architecture

```
iw7-mod.exe (upstream iw7-mod: loader, Arxan handling, components)
  └─ component vr (this fork)
       ├─ vr_probe.cpp   engine hooks (prologue-checked), headset FOV + head pose into the refdef,
       │                 RB_DrawView replay per eye, private code-surface copies for eye 2,
       │                 UI layer, desktop side-by-side composite, diagnostics, fault breadcrumbs
       └─ vr_xr.cpp/.hpp OpenXR: loader, instance, session on the game's D3D11 device, swapchains,
                         pose prediction, frame submit (projection layer + UI quad)
```

It is registered with iw7-mod's `REGISTER_COMPONENT` and picked up by premake's
`src/client/**.cpp` glob; nothing else in iw7-mod is changed. Hooks use iw7-mod's
`utils::hook::detour`. All hooks are installed in `post_unpack` only if the first 8 bytes of every
hooked function match (`sites[]` in `vr_probe.cpp`); otherwise nothing is hooked.

### Per frame (once armed)

The component arms on the first campaign (`GAME_MODE_SP`) call of `R_RenderScene`. Frames are
counted from there ("rel" in the log). D3D11 draw hooks are installed at rel 1400 (diagnostics),
and the per-eye replay runs from **rel 2000** on.

Main thread (frontend), in the `R_RenderScene` hook, before the original runs:

1. `apply_headset_fov`: write the headset's widest symmetric tan half-angles (from
   `xrLocateViews`, both eyes) into `refdef.view.tanHalfFovX/Y`. If the backend reports that the
   rendered vertical tan is a fixed ratio of the horizontal one and falls short, widen the
   horizontal one until the vertical covers the headset.
2. `apply_head`: compose the camera = game aim (yaw + pitch from `refdef.view.axis`) ∘ HMD
   rotation (recentred yaw), and add the HMD position (metres → inches, 1 game unit = 1 inch) in
   the yaw-only body frame. The pose comes from `xrLocateSpace(VIEW in LOCAL)` at the last
   predicted display time + one display period. The written forward axis and the HMD pose are put
   in an 8-entry **pose-tag ring**.

Render thread (backend), in the `RB_DrawView` hook:

1. Read the rendered tan pair from `viewInfo+0x130/+0x134` (this is what the projection is built
   from). When XR runs, overwrite the game's own FOV at `viewInfo+0x160/+0x164` (and the previous
   frame's at `+0x2D0/+0x2D4`) with that pair, for the weapon pass (see open issues).
2. Find the pose tag whose forward axis matches `viewInfo+0x10C` (dot > 0.9999): that is the head
   pose this frame was rendered with.
3. Snapshot all of `viewInfo` (0x5BA0 bytes). Copy every code-surface list (and each surface's
   indices) into a private 32 MB arena.
4. Shift the view origin by +half IPD along the view's left axis (current and previous parms) and
   run the original `RB_DrawView` = **left eye**. While it runs, every render-target release
   (`0x140E2B350`) is held back instead of executed. Copy `Display` into the left eye texture.
5. Restore `viewInfo` from the snapshot, point its code-surface descriptors at the private
   copies, shift by −half IPD and run `RB_DrawView` again = **right eye**; releases go through
   and are recorded. Copy `Display` into the right eye texture.
6. Release the targets that only the first run returned (held minus released by run 2), so the
   pool stays balanced.
7. Clear `Display` to transparent black. Everything the game draws afterwards (post-view pass,
   2D, HUD, LUI menus) lands on an otherwise empty UI layer.

Pre-present (`0x140E56330`, before the final copy to the swap chain):

1. Copy the UI layer out of `Display`.
2. Inside a private `ID3DDeviceContextState` (`SwapDeviceContextState`), submit to OpenXR
   (`vr_xr::submit`) and then redraw `Display` as two squeezed full frames side by side, each
   = eye + premultiplied UI. The composite shaders are HLSL compiled at runtime with
   `d3dcompiler_47.dll`.

### OpenXR (`vr_xr.cpp`)

- Loads `openxr_loader.dll` by full path from the exe's folder first (iw7-mod restricts the DLL
  search path, so the bare name is not found), then by bare name. An x86 loader fails with error
  193; it must be the x64 one.
- `XR_KHR_D3D11_enable`, adapter-LUID check against the game's device, session on **the game's
  device** (no second device, no cross-device copies). LOCAL and VIEW reference spaces.
- Swapchains: two eyes + UI, sized like the game's `Display` target, format
  `R8G8B8A8_UNORM_SRGB` when offered. The game writes gamma-encoded values into an 8-bit UNORM
  target, so an sRGB swapchain makes the compositor read them correctly; copies between UNORM and
  UNORM_SRGB of the same family are raw.
- Frame loop runs on the render thread at the pre-present point: `xrWaitFrame` → `xrBeginFrame`
  → `xrLocateViews` (headset FOV and measured IPD are taken from here) → copy eyes + UI →
  `xrEndFrame` with one projection layer and one quad layer.
- Declared FOV = the rendered symmetric tan pair. Declared pose = the tagged head pose ± measured
  half IPD along the head's x axis; if no tag matched, the located views are used.
- UI quad: head-locked (VIEW space), 1.5 m ahead, width `2 * 1.5 * 0.75 * 0.7` m, texture aspect,
  premultiplied alpha.
- Missing loader/runtime: retried every 3000 submits; no headset: retried every 600. Adapter
  mismatch or unusable formats: OpenXR is disabled for the session and the game stays flat.

## Engine map

All VAs for build 2715288. Names in quotes are the job/target names the engine itself uses; other
names are descriptive.

### Hooked functions

| VA | What | Prologue (first 8 bytes) |
|---|---|---|
| `0x140E357E0` | `R_RenderScene(refdef, 1.0f, drawType, &flags, &sceneDef)`, main thread | `48 8B C4 48 89 58 10 48` |
| `0x140E57390` | `RB_Frame(backendData)`, render thread | `48 8B C4 48 89 58 20 57` |
| `0x140E5C9C0` | `RB_DrawView(ctx16*, viewInfo, backendData)` — the replay unit | `4C 8B DC 55 56 41 56 41` |
| `0x140E5C680` | post-view pass `(ctx16*, viewInfo, backendData)` | `48 89 5C 24 18 55 56 57` |
| `0x140E51550` | 2D pass `()` | `48 83 EC 58 48 8B 05 C5` |
| `0x140DE7590` | job 0x3C "draw" worker (unused in practice) | `48 89 5C 24 08 48 89 74` |
| `0x140DE7E00` | view-parms rebuild `(out, viewParms, offsets)` | `40 53 48 83 EC 20 0F 10` |
| `0x140E3E070` | set render targets `(state, count, u16* colourIds, u16 depthId)` | `48 89 5C 24 18 48 89 6C` |
| `0x140E2B350` | return a per-frame render target to the pool `(u16 id)` | `66 89 4C 24 08 48 83 EC` |
| `0x140E5A980` | walk a draw list's code-surface lists `(ctx16*, listDesc*)` | `48 89 5C 24 18 48 89 7C` |
| `0x140E56330` | pre-present: final copy + present `(flags u8, finalTargetId u16, u8, u8)` | `F6 C1 02 74 66 48 89 5C` |

**Every hook stub must take and forward at least the four register arguments.** A stub declared
with two arguments for the post-view pass clobbered `r8` (= backend data) and crashed.

### Globals and layouts

| Item | Value |
|---|---|
| Frame counter (incremented by `R_BeginFrame`) | `0x148B9D27C` |
| `cg` array / `refdef_t` | `*0x141FA6C88[localClient]`, `refdef` at `cg+0x4B38`, `refdef.view` at `cg+0x4B48` |
| `GfxScene` | `0x149BD1300`; code-surface counts at `+0x319898` |
| Frontend data / backend data pointers | `0x148F73088` / `*0x14AFCAB20` |
| Backend frame data | two static buffers, `0x148F73100` and `0x149530380`, alternating per frame |
| `viewInfo` | `backendData[0x19C8] + backendData[0x19B0] * 0x5BA0`; double-buffered by frame parity (`0x149AED600` / `0x149B0FBC0`); **one view slot per frame** |
| Render-target table | `0x149B3F238`, entries 0x120 bytes, **id = index + 1** (mask 0x7FFF); `+0x78` name, `+0x80` view, `+0x100` alternate view |

Render-target ids are assigned at runtime (e.g. `Display` was 2 in one memory image and 109 in a
running game). Always resolve them by name (`find_rt_id("Display")`).

`viewInfo` (stride 0x5BA0):

| Offset | Content |
|---|---|
| `+0x000` | current `GfxViewParms` (0x170 bytes) |
| `+0x170` | previous-frame `GfxViewParms` (motion vectors / temporal) |
| `+0x2008` | per-call sequence; `+0x2008…+0x2076` u16 render-target ids of this view (e.g. `+0x2030` depth, `+0x2062` main scene colour) written by `0x140E39660` |
| `+0x2698…+0x3898` | 19 draw-list descriptors, stride 0x100 |

`GfxViewParms` (offsets from the start of the parms): `+0x100` origin (world units = inches),
`+0x10C` forward, `+0x118` left, `+0x124` up (orthonormal), `+0x130/+0x134` tanHalfFovX/Y used for
the projection, `+0x138…` near/far values, `+0x148/+0x14C` **projection offsets** (0 in normal
play; asymmetric frustum), `+0x160/+0x164` the game's own FOV pair (monitor FOV; smaller in ADS).
The view matrix is built from the axes only (camera-relative rendering); the origin reaches the
shaders separately.

Draw-list descriptor (from `0x140E5A810`), as {ptr, count, entry size}: {+0x08,+0x00,4},
{+0x18,+0x10,raw}, {+0x30,+0x28,raw}, {+0x48,+0x40,raw}, {+0x60,+0x58,raw}, {+0x70,+0x78,16},
**{+0x80,+0x88,40 = GfxCodeSurf}**, {+0x90,+0x98,0xE8}, {+0xA0,+0xA8,32}, {+0xB8,+0xC0,4},
{+0xC8,+0xD0,4}. `GfxCodeSurf`: `+0x08` triangle count (u32), `+0x10` indices (u16*).

### Frontend chain (main thread)

```
CG_DrawActiveFrame 0x14026CB50
 └─ 0x14077AF70 (cg-side scene builder)
     └─ tail-jmp 0x1403181D0 (obfuscated wrapper)
         ├─ 0x140E38F70(&sceneDef)           zero-init
         ├─ 0x140E3C080(refdef, 1, &sceneDef) flag bits from dvars
         └─ R_RenderScene 0x140E357E0
             ├─ 0x140E360B0  GfxViewParms from RefdefView (org, axis, tanHalfFov, zNear/zFar)
             ├─ 0x140CE1730 / 0x140CE0E90  view matrix → vp+0x40
             ├─ 0x140E3BF40  projection → vp+0x80 (uses tan +0x30/0x34 and offsets +0x148/0x14C)
             ├─ per-view previous parms table 0x14A145A80
             └─ 0x140E32E00 "add view to scene" → viewInfo, then the scene jobs:
                bsp, models, lights, probes, decals, volumetrics, shadows, … and 0x28 "gen drawsurfs"
```

`0x140AC28E0` is **not** `R_RenderScene`: it is the FX camera setup (`FX_SetupCamera`), which
writes the effects system's own view and queues "fx pass 2" jobs. Particles are culled against
it, so it matters for VR later.

### Backend (render thread)

```
render job 0x140E574E0
 └─ RB_Frame 0x140E57390(backendData)
     ├─ 0x140E51AE0 → RB_DrawView 0x140E5C9C0(&ctx16, viewInfo, backendData)
     │     └─ draw lists 0x140E5D960 → 0x140E5EFA0; post-FX 0x140E63BD0 → 0x140E629A0 ('Postfx color')
     │        → 0x140E66C40 (→ 'Display')
     ├─ post-view pass 0x140E5C680
     ├─ 2D 0x140E51550
     ├─ HUD / LUI draws (to 'Display')
     └─ 0x140E56330 pre-present → 0x140E2A960 copy to swap chain → present 0x140E59A40
```

Facts the replay depends on (all measured in a running game):

- **All 3D draws are issued on the immediate context from one render thread.** No deferred
  contexts, no command lists, the "draw" worker job is not used. `RB_DrawView` issues about
  2,500–2,800 draws in 2.0–3.5 ms CPU.
- **The backend never uses the frontend's matrices.** Every pass rebuilds view/projection from
  `viewInfo+0x000` (and `+0x170` for the previous frame) through `0x140DE7E00`, 7 times per frame,
  all inside `RB_DrawView`. So changing a view = changing the parms in `viewInfo`, not patching
  shaders or constant buffers.
- **`RB_DrawView` consumes its view**: it returns per-frame pool targets via `0x140E2B350` (76 call
  sites) and zeroes their ids in `viewInfo`. Hence the snapshot/restore and the release hold-back.
- **The pipeline is SMP**: main-thread `R_RenderScene` for frame N+1 runs during `RB_DrawView` of
  frame N, but writes the other buffer. N+2 starts only after `RB_Frame` exits, so eye 2 is not
  overwritten by the frontend.
- Eye 1 changes only about 20 small ranges (~230 bytes) of the backend frame data, nearly all
  upload/page pointers. No H2-style dynamic-arena cursor problem was found.
- Bind order at the end of the view: Resolved Scene chain → motion blur → tonemap mips → veil →
  `Postfx color` → `Display`. `Postfx color` is the finished image of a view.
- All UI draws after the 3D view render to `Display` and nothing else, which is what makes the
  clear-and-composite UI layer possible.

## Approaches that failed (don't repeat)

- **Calling `R_RenderScene` twice per frame.** First attempt: access violation in an arena
  allocation (`0x1405EF887`, null arena), because a once-per-frame request queue
  (`0x140E239D0`, gated by `sceneDef+0x24` bit 0x4) was drained twice. Clearing that bit for the
  second call let it return, but **both calls share the single `viewInfo` slot and the same
  targets** (`frontEndData+0x19B0` is only reset to 0 by `R_BeginFrameData` `0x140E28290`,
  nothing increments it). The screen went black and the render thread crashed in the render-target
  sub-view lookup (`0x140E2B89F`, id 0). The frontend has one view slot per frame by design.
- **Replaying `RB_DrawView` without restoring `viewInfo`.** The second run bound target id 0 and
  crashed at the same lookup (`0x140E2B89F`): the first run had released the targets and zeroed
  the ids.
- **Replaying with eye 2 reading the same code-surface lists as eye 1.** An emissive wall screen
  (and its light on the floor) was lit in the left eye and dark in the right. Per-draw
  comparison: same draws, same shaders and SRVs, only the dynamic vertex/index ring offsets and
  contents differed. Later, eye 2 crashed at `0x140E4E421` (reading a `GfxCodeSurf` array) right
  after a screen was shot out. Cause: eye 1 consumes/rebases the frame's code-surface lists in
  place. Fix: private copies for eye 2 (about 57 surfaces / 7.5k indices / 17 KB per frame).
- **Declaring the game's FOV (`+0x160/+0x164`) to the compositor.** That pair is the monitor FOV
  (0.7645 × 0.4778, smaller in ADS), not what is rendered. It squashed the image vertically,
  warped it under head motion, and shrank the view in ADS. The rendered pair is `+0x130/+0x134`.
- **Rendering the monitor FOV in the headset** gave a small rectangle. The refdef tan is now the
  headset's.
- **Asking only for the headset's vertical tan** filled the top of the view but not the bottom;
  the engine derived the vertical from the horizontal. The frontend now learns the ratio and widens
  the horizontal tan.
- **Camera = game yaw only + head rotation.** Stick/mouse pitch still moved the aim and the
  viewmodel hanging off it, so the gun drifted out of view and use prompts could not be targeted.
  Replaced by game aim (yaw + pitch) ∘ head (untested, see below).
- **x86 `openxr_loader.dll`** fails to load (error 193). Use the x64 loader.

## Log (`iw7-vr-probe.log`, working folder)

| Prefix | Meaning |
|---|---|
| `[probe]` | startup: prologue check per hook (`ok` / `MISMATCH`), "armed at engine frame N" |
| `[replay]` | first replayed frame: held/released target ids, private copy stats |
| `[heartbeat]` | every 600 replayed frames: device removed reason, private arena use |
| `[head]` | recenter, axis orientation, game aim pitch and head offset (every 600 frames), rendered tan, tag found/MISSING + match, half IPD |
| `[fov]` | refdef tan asked vs rendered, vertical-fit ratio |
| `[xr]` | loader path, runtime name/version, session state, swapchain format/size, headset FOV per eye, status every 600 frames |
| `[ui]` | compositor setup; in detail frames, where post-3D draws went and UI layer coverage |
| `[rb]`, `[bind]`, `[vp]`, `[ctx]`, `[drawdiff]`, `[content]`, `[list]`, `[diff]`, `[scene]` | detail frames only (rel 1500–1502 and 2000–2002): backend stages and timing, target binds, view-parms rebuilds, draw counts per context, eye-1 vs eye-2 per-draw comparison, draw-list descriptors |
| `[fault]` | first-chance access violations (first 8; the last one before a crash is usually the crash) |

The log is capped at 60,000 lines. Healthy signs: all prologues `ok`, `tag found (match 1.000000)`,
rendered tan == declared tan, heartbeats with `device=0x00000000`.

Keys: **Numpad 5** recenters (yaw and position). There are no other keys and no config file.

## Where to go next

1. **Verify the gun** in a headset with this build: weapon FOV pair overwrite at
   `viewInfo+0x160/+0x164` and the camera carrying the game's aim pitch. If the gun still vanishes,
   find the viewmodel (depth-hack) projection builder: search for a second `0x140E3BF40`-style
   projection fed from `cg->viewModelAxis` (iw7-mod's `structs.hpp` has `cg_s::viewModelAxis`).
2. **Asymmetric per-eye frustum**: use the projection offsets `viewParms +0x148/+0x14C` per eye and
   declare each eye's real FOV instead of the widest symmetric one.
3. **Render resolution**: the eyes are the size of `Display`. Render toward the headset's
   recommended size (e.g. 3072 × 3264 on the test headset), for example through iw7-mod's renderer
   resolution handling or a larger scene size, and size the swapchains from that.
4. **Culling and history**: the frontend culls for the centre camera only; widen it to cover both
   eye offsets. Give each eye its own temporal history (SMAA temporal targets, Resolved Scene chain,
   motion blur, the previous parms at `+0x170`).
5. **Effects camera**: `0x140AC28E0` (FX setup) sorts and culls particles for the centre view.
6. **Motion controllers**: aim from a controller pose (viewmodel placement `cg->viewModelAxis`,
   bullet origin/direction not yet located).
7. **Clean-up**: separate the shipping path from the diagnostics in `vr_probe.cpp` (the D3D draw
   hooks, per-draw comparison, backend diff and list logging are research tools), add settings
   (IPD scale, UI distance/size, start delay) and an in-game toggle.
8. One NVIDIA driver crash (`nvwgf2umx.dll+0x612F73`, render thread) happened during an early
   long desktop-stereo session and did not reproduce; the fault breadcrumbs exist to catch it.

## Prior art

- [h2-mod-vr](https://github.com/Aeka0/h2-mod-vr): VR for MW2 Campaign Remastered on H2-Mod (the
  same client family as iw7-mod). Its docs describe the per-eye "owner transaction" model and the
  list of engine hooks a full dual-pass renderer needs; useful for steps 3–5 above. Its OpenXR path
  uses a private device; this mod deliberately uses the game's own device.
- vorpX's geometry 3D works on this campaign, which showed early on that geometric stereo is
  feasible in this renderer.
