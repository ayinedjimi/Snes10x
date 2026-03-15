@echo off
REM snes9x-v0.5 FAST build — only rebuilds main project with clang-cl
REM Use this when you only changed snes9x source files (not deps like zlib/libpng/imgui)
REM For full rebuild (deps changed): use build.bat

REM Step 1: Build deps incrementally (only compiles changed files)
"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" C:\snes\snes9x-v0.5\win32\snes9xw.sln "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Build /m /v:minimal /p:BuildProjectReferences=true > C:\snes\snes9x-v0.5\build_out.txt 2>&1

REM Step 2: Rebuild main project with clang-cl -march=haswell
"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" C:\snes\snes9x-v0.5\win32\snes9xw.vcxproj "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Rebuild /p:CLToolExe=clang-cl.exe "/p:CLToolPath=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin" /v:minimal /p:BuildProjectReferences=false >> C:\snes\snes9x-v0.5\build_out.txt 2>&1
echo EXIT_CODE=%ERRORLEVEL% >> C:\snes\snes9x-v0.5\build_out.txt
