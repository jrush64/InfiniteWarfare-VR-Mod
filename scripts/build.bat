@echo off
REM Build iw7-mod with the VR component (Release, x64).
REM Output: builds\iw7-mod.exe
REM Prerequisites: Visual Studio 2022 (Community/Professional/Enterprise or the standalone Build Tools)
REM with the "Desktop development with C++" workload, and git on PATH. See BUILD.md.
setlocal
cd /d "%~dp0.."

REM The dependencies are git submodules (deps\). Fetch them if this checkout does not have them yet.
if not exist "deps\minhook\src\hook.c" (
  echo [build] fetching submodules...
  git submodule update --init --recursive
  if errorlevel 1 ( echo [build] git submodule update failed - is git installed and on PATH? & exit /b 1 )
)

REM Locate any VS2022 install via vswhere instead of assuming one edition or drive letter.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo [build] vswhere.exe not found - is Visual Studio 2022 installed? & exit /b 1 )

set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD ( echo [build] no VS install with the C++ toolset found & exit /b 1 )

REM Generate build\iw7-mod.sln (premake5.exe ships with upstream iw7-mod in tools\).
tools\premake5 vs2022
if errorlevel 1 ( echo [build] premake failed & exit /b 1 )

REM CL_MPCount caps parallel compiler processes per project: on many-core machines an uncapped build can
REM run out of memory for the client's large precompiled header (C1076 / C3859).
"%MSBUILD%" build\iw7-mod.sln /m /v:minimal /nologo /p:Configuration=Release /p:Platform=x64 /p:CL_MPCount=8
if errorlevel 1 ( echo [build] msbuild FAILED & exit /b 1 )

if not exist builds mkdir builds
copy /y "build\bin\x64\Release\iw7-mod.exe" "builds\iw7-mod.exe" >nul
if errorlevel 1 ( echo [build] copy failed & exit /b 1 )

echo [build] OK -^> "%CD%\builds\iw7-mod.exe"
endlocal
