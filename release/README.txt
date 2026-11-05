================================================================================
  CALL OF DUTY: INFINITE WARFARE VR  (experimental, unfinished)
  a fork of iw7-mod (https://github.com/auroramod/iw7-mod)
================================================================================

WHAT YOU NEED

  - Call of Duty: Infinite Warfare (Steam, PC), legally owned. Campaign only.
  - A PC VR headset and an OpenXR runtime (SteamVR, Meta Quest Link,
    Virtual Desktop, etc.). Set the runtime you use as the active OpenXR runtime.
  - The iw7-mod client data. The easiest way to get it is to install the
    normal iw7-mod once, following https://docs.auroramod.dev/iw7-install ,
    and start it once. (It lives in %LOCALAPPDATA%\auroramod\iw7-mod\cdata;
    it can also be copied from this repository's data\cdata folder.)

INSTALLING

  1. Close the game.

  2. Find your Infinite Warfare folder. In Steam, right click the game, then
     Manage, then Browse local files. It contains iw7_ship.exe.

  3. Copy these files into that folder:

         iw7-mod.exe         (from the builds folder; replaces the normal
                              iw7-mod.exe if you have one - back that up)
         openxr_loader.dll   (from this folder)

  4. Start your VR runtime. Then start iw7-mod.exe with the -noupdate option,
     for example from a shortcut or a command prompt in the game folder:

         iw7-mod.exe -noupdate

     Without -noupdate the iw7-mod updater replaces the exe with the official
     one, which has no VR.

  5. Start a campaign mission. Stereo turns on by itself a short while
     (about 2,000 frames) after the first campaign frame is drawn.

USING IT

  Numpad 5   recenters the view.

  The desktop shows both eyes side by side. If no headset or OpenXR runtime
  is found, the game keeps running on the monitor only.

  See the main README for what does not work yet (the gun can disappear,
  no motion controllers, campaign only).

LOG

  iw7-vr-probe.log is written to the game folder. Include it when reporting
  problems.

UNINSTALLING

  Delete iw7-mod.exe (or put the normal one back), openxr_loader.dll and
  iw7-vr-probe.log from the game folder.

THIRD-PARTY

  openxr_loader.dll is the Khronos OpenXR loader (1.1.58, x64),
  Apache License 2.0. See openxr_loader.LICENSE.txt.
