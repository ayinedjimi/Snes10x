@echo off
REM snes9x-v0.5 QUICK build — only compiles changed files in the main project (clang-cl).
REM Use when you only changed .cpp/.h in win32 or core (no deps: zlib, libpng, imgui, etc.).
REM No dependency step: fastest. If you get link errors, run build_fast.bat once.
REM Incremental: only recompiles modified sources, then links.

"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" C:\snes\snes9x-v0.5\win32\snes9xw.vcxproj "/p:Configuration=Release Unicode" /p:Platform=x64 /t:Build /p:CLToolExe=clang-cl.exe "/p:CLToolPath=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin" /v:minimal /p:BuildProjectReferences=false
echo Exit code: %ERRORLEVEL%
