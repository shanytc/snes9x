@echo off
REM =========================================================================
REM  build_sdl3.bat - Build SDL3 static libs from the external/SDL3-src
REM                   submodule and drop them into external/SDL3/lib/.
REM
REM  Usage:  build_sdl3.bat                    (x64 + x86, Debug + Release)
REM          build_sdl3.bat x64                (x64 only)
REM          build_sdl3.bat x86                (x86 only)
REM          build_sdl3.bat clean              (wipe build trees first)
REM          build_sdl3.bat syms               (embed /Z7 debug info in the
REM                                             Debug lib so VS can step into
REM                                             SDL source; produces a FAT
REM                                             SDL3-staticd.lib - do NOT
REM                                             commit that artifact. Rerun
REM                                             without "syms" when done to
REM                                             restore the lean release lib)
REM
REM  Flags can be combined: "build_sdl3.bat x64 clean syms" etc.
REM
REM  Requires: Visual Studio 2019 or 2022 with "Desktop C++" workload + CMake.
REM            Run from a normal cmd - this script locates VS automatically
REM            via vswhere.
REM =========================================================================

setlocal EnableDelayedExpansion

set SCRIPT_DIR=%~dp0
set SDL_SRC=%SCRIPT_DIR%..\SDL3-src
set SDL_DST_LIB=%SCRIPT_DIR%lib
set BUILD_ROOT=%SCRIPT_DIR%..\SDL3-src\build

if not exist "%SDL_SRC%\CMakeLists.txt" (
    echo ERROR: SDL source not found at %SDL_SRC%
    echo Did you run: git submodule update --init external/SDL3-src ?
    exit /b 1
)

REM --- Locate Visual Studio via vswhere -----------------------------------
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% set VSWHERE="%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2019 or 2022.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath`) do set VS_DIR=%%i
if "%VS_DIR%"=="" (
    echo ERROR: Visual Studio with Desktop C++ workload not found.
    exit /b 1
)

for /f "tokens=*" %%v in ('%VSWHERE% -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property catalog_productLineVersion') do set VS_YEAR=%%v
if "%VS_YEAR%"=="2022" (
    set CMAKE_GENERATOR=Visual Studio 17 2022
) else if "%VS_YEAR%"=="2019" (
    set CMAKE_GENERATOR=Visual Studio 16 2019
) else (
    echo ERROR: Unsupported Visual Studio version: %VS_YEAR%
    exit /b 1
)
echo Using Visual Studio %VS_YEAR% at: %VS_DIR%
echo CMake generator:    %CMAKE_GENERATOR%

set CMAKE=cmake
where %CMAKE% >nul 2>nul
if errorlevel 1 (
    set CMAKE="%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist !CMAKE! (
        echo ERROR: cmake not on PATH and not found in Visual Studio install.
        exit /b 1
    )
)

REM --- Parse args ----------------------------------------------------------
set DO_X64=1
set DO_X86=1
set DO_CLEAN=0
set DO_SYMS=0

for %%A in (%*) do (
    if /i "%%~A"=="x64"   ( set DO_X64=1 & set DO_X86=0 )
    if /i "%%~A"=="x86"   ( set DO_X64=0 & set DO_X86=1 )
    if /i "%%~A"=="clean" ( set DO_CLEAN=1 )
    if /i "%%~A"=="syms"  ( set DO_SYMS=1 )
)

REM Toggling syms mode changes the compile flags; force a clean reconfigure
REM so the CMake cache doesn't carry stale debug-format settings.
if %DO_SYMS%==1 set DO_CLEAN=1

if %DO_CLEAN%==1 (
    echo Cleaning build trees...
    if exist "%BUILD_ROOT%" rmdir /s /q "%BUILD_ROOT%"
)

if %DO_SYMS%==1 (
    echo *** syms mode: Debug lib will embed /Z7 debug info. DO NOT commit.
) else (
    echo Release mode: Debug lib uses CMake default /Zi (no embedded syms^).
)

REM --- Build x64 -----------------------------------------------------------
if %DO_X64%==1 (
    call :build_arch x64 x64
    if errorlevel 1 exit /b 1
)

REM --- Build x86 -----------------------------------------------------------
if %DO_X86%==1 (
    call :build_arch Win32 x86
    if errorlevel 1 exit /b 1
)

echo.
echo ============================================================
echo   SDL3 static libs built and installed to %SDL_DST_LIB%
echo ============================================================
endlocal
exit /b 0

REM -------------------------------------------------------------------------
REM :build_arch <cmake-platform> <lib-dir-name>
REM   e.g.  build_arch x64 x64
REM         build_arch Win32 x86
REM -------------------------------------------------------------------------
:build_arch
set CM_PLATFORM=%~1
set LIB_ARCH=%~2
set BUILD_DIR=%BUILD_ROOT%\%LIB_ARCH%

REM Extra CMake args added only in `syms` mode. Overrides CMake's default
REM debug flags "/Zi /Ob0 /Od /RTC1" with /Z7 so debug info is embedded in
REM the .obj (and hence the .lib), not written to a separate .pdb - without
REM this the snes9x debugger can't bind breakpoints inside SDL source. In
REM normal mode we leave CMake defaults alone so the committed libs stay
REM lean.
set EXTRA_CMAKE_ARGS=
if %DO_SYMS%==1 (
    set "EXTRA_CMAKE_ARGS=-DCMAKE_C_FLAGS_DEBUG=/Z7 /Ob0 /Od /RTC1"
    set "EXTRA_CMAKE_ARGS=!EXTRA_CMAKE_ARGS! -DCMAKE_CXX_FLAGS_DEBUG=/Z7 /Ob0 /Od /RTC1"
)

echo.
echo ============================================================
echo   Configuring SDL3 for %LIB_ARCH% (%CM_PLATFORM%)
echo ============================================================
REM snes9xw is built with /MT (Release) and /MTd (Debug) - static CRT.
REM Force SDL to match or the final link fails with __imp_* unresolved
REM externals. CMP0091=NEW is required so CMake honours
REM CMAKE_MSVC_RUNTIME_LIBRARY instead of burning /MD into the flags.
%CMAKE% -S "%SDL_SRC%" -B "%BUILD_DIR%" ^
    -G "%CMAKE_GENERATOR%" -A %CM_PLATFORM% ^
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>" ^
    %EXTRA_CMAKE_ARGS% ^
    -DSDL_STATIC=ON ^
    -DSDL_SHARED=OFF ^
    -DSDL_TEST_LIBRARY=OFF ^
    -DSDL_TESTS=OFF ^
    -DSDL_EXAMPLES=OFF ^
    -DSDL_INSTALL_TESTS=OFF ^
    -DCMAKE_DEBUG_POSTFIX=d
if errorlevel 1 exit /b 1

echo.
echo ============================================================
echo   Building SDL3 %LIB_ARCH% - Release
echo ============================================================
%CMAKE% --build "%BUILD_DIR%" --config Release --target SDL3-static -- /m
if errorlevel 1 exit /b 1

echo.
echo ============================================================
echo   Building SDL3 %LIB_ARCH% - Debug
echo ============================================================
%CMAKE% --build "%BUILD_DIR%" --config Debug --target SDL3-static -- /m
if errorlevel 1 exit /b 1

set DST=%SDL_DST_LIB%\%LIB_ARCH%
if not exist "%DST%" mkdir "%DST%"

echo Copying static libs to %DST% ...
copy /y "%BUILD_DIR%\Release\SDL3-static.lib"  "%DST%\SDL3-static.lib"  >nul || exit /b 1
copy /y "%BUILD_DIR%\Debug\SDL3-staticd.lib"   "%DST%\SDL3-staticd.lib" >nul || exit /b 1

exit /b 0
