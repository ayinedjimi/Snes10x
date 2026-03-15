@echo off
REM snes9x-v0.5 PGO (Profile-Guided Optimization) build
REM 3-phase process:
REM   Phase 1: Build with instrumentation (-fprofile-instr-generate)
REM   Phase 2: Run emulator to collect profile data (user plays for ~30s)
REM   Phase 3: Merge profiles and rebuild with -fprofile-instr-use
REM
REM Usage: build_pgo.bat [ROM_PATH]

setlocal
set ROM=%~1
if "%ROM%"=="" set "ROM=C:\snes\Parodius (Europe).sfc"
set PROFDIR=C:\snes\snes9x-v0.5\pgo_data
set LLVM_BIN=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin
set PROF_RT="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\lib\clang\19\lib\windows\clang_rt.profile-x86_64.lib"
set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"

echo ============================================================
echo   snes9x-v0.5 PGO Build
echo ============================================================

REM ---- Phase 1: Build with instrumentation ----
echo.
echo [Phase 1/3] Building with profiling instrumentation...

REM Clean old profile data
if exist "%PROFDIR%" rmdir /s /q "%PROFDIR%"
mkdir "%PROFDIR%"

REM Build deps incrementally (MSVC, no PGO needed)
%MSBUILD% C:\snes\snes9x-v0.5\win32\snes9xw.sln "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Build /m /v:minimal /p:BuildProjectReferences=true > C:\snes\snes9x-v0.5\build_out.txt 2>&1

REM Rebuild main project with clang-cl + frontend instrumentation
REM Use -fprofile-instr-generate (frontend, compatible with MSVC linker)
REM Link profiling runtime explicitly
%MSBUILD% C:\snes\snes9x-v0.5\win32\snes9xw.vcxproj "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Rebuild /p:CLToolExe=clang-cl.exe "/p:CLToolPath=%LLVM_BIN%" "/p:AdditionalOptions=-fprofile-instr-generate=%PROFDIR%/snes9x.profraw" "/p:AdditionalDependencies=%PROF_RT%;%%(AdditionalDependencies)" /v:minimal /p:BuildProjectReferences=false >> C:\snes\snes9x-v0.5\build_out.txt 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Phase 1 build failed! Check build_out.txt
    type C:\snes\snes9x-v0.5\build_out.txt | findstr /i "error"
    exit /b 1
)
echo [Phase 1/3] Instrumented build complete.

REM ---- Phase 2: Collect profile data ----
echo.
echo [Phase 2/3] Collecting profile data...
echo   ROM: %ROM%
echo.
echo   ========================================
echo   PLAY THE GAME FOR 30-60 SECONDS
echo   Then close the emulator window
echo   ========================================
echo.

"C:\snes\snes9x-v0.5\win32\snes9x-x64.exe" "%ROM%"

REM Check if profile data was generated
if not exist "%PROFDIR%\snes9x.profraw" (
    echo [ERROR] No profile data generated!
    dir "%PROFDIR%" 2>nul
    exit /b 2
)

REM Merge/index profile data
echo   Merging profile data...
"%LLVM_BIN%\llvm-profdata.exe" merge -output="%PROFDIR%\merged.profdata" "%PROFDIR%\snes9x.profraw"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Profile merge failed!
    exit /b 3
)
echo [Phase 2/3] Profile data collected and merged.

REM ---- Phase 3: Optimized rebuild ----
echo.
echo [Phase 3/3] Rebuilding with profile-guided optimization...

%MSBUILD% C:\snes\snes9x-v0.5\win32\snes9xw.vcxproj "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Rebuild /p:CLToolExe=clang-cl.exe "/p:CLToolPath=%LLVM_BIN%" "/p:AdditionalOptions=-fprofile-instr-use=%PROFDIR%/merged.profdata -Wno-profile-instr-unprofiled" /v:minimal /p:BuildProjectReferences=false >> C:\snes\snes9x-v0.5\build_out.txt 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Phase 3 build failed! Check build_out.txt
    type C:\snes\snes9x-v0.5\build_out.txt | findstr /i "error"
    exit /b 4
)

echo.
echo ============================================================
echo   PGO BUILD COMPLETE
echo   Output: C:\snes\snes9x-v0.5\win32\snes9x-x64.exe
echo ============================================================
echo EXIT_CODE=0 >> C:\snes\snes9x-v0.5\build_out.txt
