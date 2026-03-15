@echo off
REM snes9x-v0.5 build (Turbo)
REM Step 1: Build all deps with MSVC
"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" C:\snes\snes9x-v0.5\win32\snes9xw.sln "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Rebuild /m /v:minimal /p:BuildProjectReferences=true > C:\snes\snes9x-v0.5\build_out.txt 2>&1

REM Step 2: Rebuild main project with clang-cl -march=haswell
"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" C:\snes\snes9x-v0.5\win32\snes9xw.vcxproj "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Rebuild /p:CLToolExe=clang-cl.exe "/p:CLToolPath=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin" /v:minimal /p:BuildProjectReferences=false >> C:\snes\snes9x-v0.5\build_out.txt 2>&1
echo EXIT_CODE=%ERRORLEVEL% >> C:\snes\snes9x-v0.5\build_out.txt
