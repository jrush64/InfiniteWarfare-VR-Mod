# Call of Duty: Infinite Warfare VR (iw7-mod fork)

An unfinished VR mod for the **Call of Duty: Infinite Warfare campaign** (Steam, x64, D3D11, IW7
engine). It is a **fork of [auroramod/iw7-mod](https://github.com/auroramod/iw7-mod)** (GPL-3.0):
the VR code is one extra iw7-mod component, `src/client/component/vr/`, compiled into
`iw7-mod.exe`. iw7-mod is the client that loads the game: `iw7-mod.exe` sits in the game folder,
maps `iw7_ship.exe` into its own process, deals with the Arxan protection and installs hooks. The VR
component hooks the renderer from inside that process and submits both eyes to any OpenXR runtime
(SteamVR, Meta Quest Link, Virtual Desktop, etc.).

⚠️ Experimental, **unfinished** community mod. Not affiliated with Activision, Infinity Ward or the
iw7-mod / auroramod team. Use at your own risk. Development has stopped, and the source is
published so that others can pick it up. See [What doesn't work yet](#what-doesnt-work-yet) and
[`docs/TECHNICAL_NOTES.md`](docs/TECHNICAL_NOTES.md).

You must legally own Call of Duty: Infinite Warfare on Steam. Licensed under [GPL-3.0](LICENSE),
like iw7-mod itself.

## What it does

Everything below was seen working, on the monitor or in a headset (tested with Virtual Desktop's
OpenXR runtime), unless it says otherwise.

- **Same-frame stereo by backend replay.** The engine builds one view per frame. The mod runs the
  render thread's view draw (`RB_DrawView`) twice per frame, once per eye, with the view origin
  shifted by half the IPD each way. Before the first run it snapshots the view's state and holds
  back the render targets the first run returns to the pool, so the second run starts from the same
  state. It is not alternate-eye rendering and not reprojection. The second eye costs about
  1.5–1.9 ms of render-thread CPU.
- **Flat side-by-side desktop view** of both eyes (parallel free-viewing), seen running at 120 fps.
- **Eye 2 matches eye 1.** Dynamic "code surfaces" (emissive screens, effects) are consumed by the
  first run. Eye 2 reads private copies taken before eye 1. This fixed both a dark screen in the
  right eye and a crash after shooting out a screen.
- **UI in both eyes.** After eye 2 the game's UI (HUD, menus, subtitles) is drawn onto its own
  transparent layer, which is composited over each eye. Confirmed on the monitor.
- **OpenXR output.** Session on the game's own D3D11 device, sRGB swapchains, both eyes as one
  projection layer and the UI as a head-locked quad 1.5 m ahead. Confirmed in a headset.
- **Headset field of view.** The game renders the headset's (widest, symmetric) FOV instead of the
  monitor's, and the FOV declared to the compositor is read back from what was actually rendered.
  The log confirmed declared == rendered.
- **Head tracking.** The predicted HMD pose (rotation and position) goes into the game camera before
  the frame is built. Each frame is tagged with the pose it was rendered with, and that pose is
  submitted, so the image and the declared pose agree. The eye separation uses the headset's
  measured IPD. Used in the headset; see the open issues below for the weapon.
- **Fails safe.** Every hooked engine function has a prologue byte check. On a different game build
  no hooks are installed. If the OpenXR loader, runtime or headset is missing, the game keeps
  running on the monitor (side by side).

## What doesn't work yet

- **The gun.** In headset testing the weapon was at first not visible at all. After the first fix
  below it was visible at the start, then vanished, and a "press X" use prompt could not be
  targeted. Two fixes are in this build and **neither is verified in a headset**:
  - the weapon (depth-hack) pass is given the scene's FOV instead of the game's own (it was believed
    to project the gun magnified off the bottom of the view). Not confirmed that the gun now shows
    at normal size;
  - the camera now follows the game's aim pitch, with the head on top (before, only the game's yaw
    was used, so looking up/down with the stick moved the aim and the gun out of view). Untested.
- **Symmetric frustum.** Both eyes render the widest FOV of either eye. The engine has projection
  offsets for an asymmetric per-eye frustum (`viewParms +0x148/+0x14C`), but they are not used yet.
- **Render resolution.** Eyes are rendered at the game's display resolution, not the headset's
  recommended size.
- **Shared history and culling.** Culling is done once for the centre camera, and temporal history
  (SMAA temporal, motion blur, previous-frame view) is shared by both eyes. No artefacts from this
  have been isolated yet, but nothing handles it.
- **No motion controllers.** Play with a gamepad or keyboard and mouse.
- **Campaign only.** Stereo arms on the first campaign scene render. Multiplayer and Zombies are not
  supported.
- **No settings or menu.** Everything (IPD fallback, UI quad size, start delay) is a constant in the
  source. The only key is **Numpad 5** (recenter).
- **Still a research build.** The component carries its diagnostic hooks and logging
  (`iw7-vr-probe.log`), and stereo starts about 2,000 frames after the first campaign scene render.
- **One unexplained crash.** An NVIDIA driver access violation once happened during a desktop
  stereo session. It did not reproduce.

## Building

See [BUILD.md](BUILD.md). In short: install Visual Studio 2022 with the C++ workload and git, clone
with `--recurse-submodules`, then run `scripts\build.bat`.

## Installing

See [`release/README.txt`](release/README.txt).

## Layout

| Path | What's there |
|---|---|
| `src/client/component/vr/` | **The VR component**: `vr_probe.cpp` (engine hooks, per-eye replay, head tracking, UI layer, desktop composite, diagnostics), `vr_xr.cpp` / `vr_xr.hpp` (OpenXR) |
| `src/client/component/vr/third_party/openxr/` | Khronos OpenXR headers (Apache-2.0, license beside them) |
| `src/`, `data/`, `tools/`, `premake5.lua`, … | Upstream iw7-mod at commit `c0a1c6d` (see below) |
| `deps/` | Upstream's dependencies as git submodules, pinned to the same commits as upstream |
| `scripts/` | `build.bat` |
| `docs/` | `TECHNICAL_NOTES.md`: engine addresses, how the replay works, approaches that failed and why |
| `release/` | End-user install notes, OpenXR loader, its license |
| `builds/` | A prebuilt `iw7-mod.exe` with the VR component |

## Changes from upstream iw7-mod

The base is iw7-mod at commit `c0a1c6d` ("Merge pull request #704 from Divity/renderer"). Changes:

- added `src/client/component/vr/` (picked up by iw7-mod's existing source globbing and component
  registration; no other source file is touched);
- `premake5.lua`: a few lines that keep the local checkout path out of the built binary
  (`/d1trimfile`, `/PDBALTPATH`);
- `.github/workflows/build.yml`: the deploy job (which uploads to auroramod's servers) is removed; the
  build job is unchanged;
- `.gitignore`: a few extra patterns;
- this README, `BUILD.md`, `docs/`, `scripts/`, `release/` and `builds/` are new.

## License

GPL-3.0 - see [LICENSE](LICENSE) (iw7-mod's own license file, unchanged). The VR component is
Copyright (C) 2026 Halcyon, released under the same license. iw7-mod and everything else from
upstream remain the work of their authors.

The OpenXR headers and the shipped `openxr_loader.dll` are from the Khronos OpenXR SDK
(Apache-2.0). The submodules in `deps/` keep their own licenses.

The VR design was informed by [h2-mod-vr](https://github.com/Aeka0/h2-mod-vr) (MW2 Campaign
Remastered VR on the related H2-Mod client), whose rendering notes were read during research.

## Upstream: IW7-Mod

IW7-Mod is a client for Call of Duty®: Infinite Warfare that adds dedicated servers, hands-on modding
utilities, and custom mods that feature content like weapons, models, sounds, and more. You must
legally own [Call of Duty®: Infinite Warfare](https://store.steampowered.com/app/292730/Call_of_Duty_Infinite_Warfare/)
to run it. Unlicensed or cracked versions of the game are **not** supported.

- Upstream repository: https://github.com/auroramod/iw7-mod
- Installing upstream iw7-mod: https://docs.auroramod.dev/iw7-install

<p align="center">
  <img src="assets/github/banner.png?raw=true" width="300" height="300" />
</p>

### Upstream credits

- [s1x-client](https://git.alterware.dev/alterware/s1-mod) *(now **s1-mod**)* - codebase and research
- [h1-mod](https://github.com/auroramod/h1-mod) - extended work and research
- [h2-mod](https://github.com/alicelys/h2-mod) - research
- [momo5502](https://github.com/momo5502) - Arxan & Steam research, former lead developer of [XLabsProject](https://github.com/XLabsProject)

### Upstream disclaimer

This software has been created purely for the purposes of academic research. It is not intended to be
used to attack other systems. Project maintainers are not responsible or liable for misuse of the
software. Use responsibly.
