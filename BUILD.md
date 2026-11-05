# Building Call of Duty: Infinite Warfare VR

This is iw7-mod with one extra component, so it builds exactly like upstream iw7-mod.

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools) with the
  **"Desktop development with C++"** workload (v143 toolset, a Windows 10/11 SDK).
  `scripts\build.bat` finds it automatically via `vswhere.exe`.
- **git** on `PATH`. The dependencies are git submodules, and the build runs git to stamp version
  information into the exe. **Do not download the repository as a ZIP**: it has no submodules and no
  git metadata, and it will not build.
- Premake is not needed separately: upstream ships `tools\premake5.exe` in the repository.

## Get the source

```
git clone --recurse-submodules https://github.com/jrush64/InfiniteWarfare-VR-Mod.git
```

If you already cloned without `--recurse-submodules`, run `git submodule update --init --recursive`
(`scripts\build.bat` also does this when `deps\` is empty).

## Build

```
scripts\build.bat
```

It runs `tools\premake5 vs2022` (writes `build\iw7-mod.sln`), builds **Release | x64** with MSBuild,
and copies the result to `builds\iw7-mod.exe`. A clean build takes a few minutes; most of it is the
dependencies.

Or by hand, the upstream way: `generate.bat`, then open `build\iw7-mod.sln` in Visual Studio (or
`msbuild build\iw7-mod.sln /p:Configuration=Release /p:Platform=x64`). The exe is at
`build\bin\x64\Release\iw7-mod.exe`.

Release builds treat compiler warnings as errors (upstream's setting).

## Deploying

See [`release/README.txt`](release/README.txt). In short: copy `iw7-mod.exe` and
`release\openxr_loader.dll` into the Infinite Warfare folder (next to `iw7_ship.exe`) and start
`iw7-mod.exe -noupdate`. Without `-noupdate` iw7-mod's updater replaces the exe with the official
iw7-mod release, which has no VR.

The VR component writes `iw7-vr-probe.log` to the working folder (normally the game folder). The
log lines are described in `docs/TECHNICAL_NOTES.md`.

## Third-party components

| Component | Location | License |
|---|---|---|
| iw7-mod (base of this fork) | everything outside `src/client/component/vr/` and the new docs/scripts | GPL-3.0 (`LICENSE`) |
| OpenXR headers (SDK 1.1.60) | `src/client/component/vr/third_party/openxr` | Apache-2.0 OR MIT (`LICENSE` there: Apache-2.0) |
| OpenXR loader 1.1.58, x64 | `release/openxr_loader.dll` (prebuilt Khronos binary, not built here) | Apache-2.0 (`release/openxr_loader.LICENSE.txt`) |
| GSL, asmjit, curl, discord-rpc, DirectXTex, gsc-tool, imgui, libtomcrypt, libtommath, lua, minhook, rapidjson, sol2, stb, udis86, zlib, zstd | `deps/` (git submodules, same commits as upstream iw7-mod) | each submodule's own license file |
| premake5, fxc, protoc | `tools/` (binaries shipped by upstream iw7-mod) | their own licenses (premake: BSD-3-Clause) |
