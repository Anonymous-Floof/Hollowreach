@echo off
rem Hollowreach native build.
rem
rem Neither CMake nor Ninja nor the MSVC toolchain is normally on PATH, so this
rem finds all three through the Visual Studio installation and sets up the
rem environment itself. A clean checkout builds with just "build.bat".
rem
rem   build.bat                 configure + build (RelWithDebInfo)
rem   build.bat debug           configure + build Debug
rem   build.bat release         configure + build Release
rem   build.bat run             build then launch
rem   build.bat clean           delete the build directory
rem   build.bat package         build then produce the release zip in dist\
rem
rem NOTE: this file must keep CRLF line endings. cmd.exe reading an LF-only
rem batch file drops the first character of every line, so `rem` runs as `m`.
rem See .gitattributes at the repository root.

setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "CONFIG=RelWithDebInfo"
set "ACTION=build"
set "PASSTHROUGH="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="debug" (
  set "CONFIG=Debug"
) else if /i "%~1"=="release" (
  set "CONFIG=Release"
) else if /i "%~1"=="reldeb" (
  set "CONFIG=RelWithDebInfo"
) else if /i "%~1"=="run" (
  set "ACTION=run"
) else if /i "%~1"=="clean" (
  set "ACTION=clean"
) else if /i "%~1"=="package" (
  set "ACTION=package"
) else if /i "%~1"=="--" (
  rem Everything after -- is forwarded to the game when running. A label cannot
  rem live inside a parenthesised block, so the collect loop sits below.
  shift
  goto collect
) else (
  echo Unknown argument: %~1
  echo Usage: build.bat [debug^|release^|reldeb] [run^|clean^|package] [-- game args]
  exit /b 1
)
shift
goto parse

:collect
if "%~1"=="" goto parsed
set "PASSTHROUGH=!PASSTHROUGH! %1"
shift
goto collect

:parsed

set "BUILD_DIR=build\%CONFIG%"

if /i "%ACTION%"=="clean" (
  if exist "build" rmdir /s /q "build"
  echo Removed build directory.
  exit /b 0
)

rem --- locate Visual Studio -------------------------------------------------
rem The installer path contains "(x86)". Parentheses inside a for /f "in (...)"
rem clause terminate the clause early even when quoted, so vswhere's output goes
rem through a temp file instead.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found. Install Visual Studio 2019 or newer with the
  echo        "Desktop development with C++" workload.
  exit /b 1
)

set "VSPATH="
set "VSPATH_FILE=%TEMP%\hollowreach_vspath.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSPATH_FILE%" 2>nul
if exist "%VSPATH_FILE%" (
  set /p VSPATH=<"%VSPATH_FILE%"
  del "%VSPATH_FILE%" >nul 2>nul
)
if not defined VSPATH (
  echo ERROR: no Visual Studio installation with the C++ toolset was found.
  echo        Install the "Desktop development with C++" workload.
  exit /b 1
)
echo Visual Studio: !VSPATH!

rem --- toolchain ------------------------------------------------------------
set "VCVARS=!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" (
  echo ERROR: !VCVARS! is missing.
  exit /b 1
)

set "CMAKE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "!CMAKE!" (
  set "CMAKE="
  set "WHICH_FILE=%TEMP%\hollowreach_which.txt"
  where cmake > "!WHICH_FILE!" 2>nul
  if exist "!WHICH_FILE!" (
    set /p CMAKE=<"!WHICH_FILE!"
    del "!WHICH_FILE!" >nul 2>nul
  )
)
if not defined CMAKE (
  echo ERROR: cmake.exe not found in Visual Studio or on PATH.
  exit /b 1
)
if not exist "!CMAKE!" (
  echo ERROR: cmake.exe not found in Visual Studio or on PATH.
  exit /b 1
)

set "GENERATOR=Ninja"
if not exist "!NINJA!" (
  echo Ninja not found; falling back to the Visual Studio generator.
  set "GENERATOR=Visual Studio 17 2022"
  set "NINJA="
)

rem Only set up the MSVC environment once per shell — vcvars is slow and
rem re-running it duplicates PATH entries.
if not defined VSCMD_VER (
  echo === Setting up MSVC environment ===
  rem stderr is suppressed as well: vcvars probes for vswhere on PATH and prints
  rem a "not recognized" line before falling back to its own copy, which looks
  rem like a failure but is not. Real failures still set errorlevel.
  call "!VCVARS!" >nul 2>nul
  if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    exit /b 1
  )
)

rem --- configure ------------------------------------------------------------
if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo === Configuring %CONFIG% ===
  if defined NINJA (
    "!CMAKE!" -S . -B "%BUILD_DIR%" -G "!GENERATOR!" -DCMAKE_MAKE_PROGRAM="!NINJA!" -DCMAKE_BUILD_TYPE=%CONFIG%
  ) else (
    "!CMAKE!" -S . -B "%BUILD_DIR%" -G "!GENERATOR!" -A x64 -DCMAKE_BUILD_TYPE=%CONFIG%
  )
  if errorlevel 1 (
    echo ERROR: configure failed.
    exit /b 1
  )
)

rem --- build ----------------------------------------------------------------
echo === Building %CONFIG% ===
"!CMAKE!" --build "%BUILD_DIR%" --config %CONFIG% --parallel
if errorlevel 1 (
  echo ERROR: build failed.
  exit /b 1
)

set "EXE=%BUILD_DIR%\bin\Hollowreach.exe"
if not exist "%EXE%" (
  echo ERROR: expected %EXE% but it was not produced.
  exit /b 1
)
echo === Built %EXE% ===

if /i "%ACTION%"=="package" (
  echo === Packaging ===
  "!CMAKE!" --build "%BUILD_DIR%" --config %CONFIG% --target package
  if errorlevel 1 exit /b 1
  exit /b 0
)

if /i "%ACTION%"=="run" (
  echo === Running ===
  "%EXE%"!PASSTHROUGH!
  exit /b !errorlevel!
)

exit /b 0
