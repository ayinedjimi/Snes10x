# Snes10x

**High-performance Super Nintendo emulator for Windows 11 64-bit**

Snes10x is a heavily optimized fork of [Snes9x](https://github.com/snes9xgit/snes9x), built for modern x86-64 processors with AVX2 support. It delivers maximum emulation speed through multithreading, SIMD optimizations, and a streamlined D3D11-only rendering pipeline.

---

## Key Optimizations

### Multithreading (3 dedicated threads)
- **Async Render Thread** &mdash; D3D11 rendering runs on a dedicated thread. The emulation thread copies the framebuffer and continues immediately while the GPU processes the previous frame (1-frame pipeline overlap with back-pressure sync)
- **APU Thread** &mdash; SPC700 audio CPU runs on its own thread with producer-consumer synchronization. Sound processing happens in parallel with main CPU emulation
- **XAudio2 Audio Thread** &mdash; Low-latency audio output with lock-free buffer management

### CPU Optimizations
- **AVX2 SIMD** &mdash; 8-pixel-at-a-time color conversion (RGB565 to BGRA8888) using 256-bit vector instructions
- **Cache-line alignment** &mdash; `alignas(64)` on hot CPU state structures (`SCPUState`, `SICPU`, `SRegisters`) with fields reordered by access frequency
- **Branch prediction hints** &mdash; `S9X_LIKELY`/`S9X_UNLIKELY` on critical paths in the 65C816 CPU loop, memory access, SPC700 interpreter, and DMA engine
- **ROM prefetch** &mdash; Software prefetch (`_mm_prefetch`) on opcode fetch to preload the next cache line
- **Compiler** &mdash; clang-cl with `-O3 -march=haswell -mtune=native` for aggressive auto-vectorization and instruction scheduling

### GPU Pipeline (D3D11)
- **GPU color conversion** &mdash; RGB565 pixels uploaded as `R16_UINT` texture, converted to RGBA by a custom HLSL pixel shader directly on the GPU (CPU fallback via AVX2 if needed)
- **FLIP_DISCARD swap chain** &mdash; Modern DXGI presentation model with tearing support for lowest latency
- **Single renderer** &mdash; D3D11-only build eliminates OpenGL/Vulkan/DirectDraw overhead and code bloat

### Audio Reliability
- **Atomic resampler** &mdash; Ring buffer uses `std::atomic<int>` with proper `memory_order_acquire/release` barriers instead of `volatile`
- **Safe XAudio2 buffer management** &mdash; `SubmitSourceBuffer` verified before incrementing buffer count (no phantom counters)
- **Increased resampler buffer** &mdash; 1.5 video frames of headroom for thread scheduling jitter

### Code Cleanup (-60% binary size)
- Removed: OpenGL, Vulkan, DirectDraw, SPIRV-Cross, glslang (28 compiled files, ~85,000 lines)
- Removed: GTK, macOS, Qt platform code (7.7 MB)
- Removed: Movie record/play, AVI recording, Netplay (menus and hot-path code)
- Result: **5.3 MB** executable vs 13.4 MB original

---

## Performance Comparison

Benchmarked on Intel i5-8265U @ 1.60GHz (laptop), Parodius (Europe) PAL, 3000 frames unthrottled:

| Metric | Snes9x Original | Snes10x v0.5 | Improvement |
|--------|----------------|--------------|-------------|
| **S9X Score** | 15,723 | **16,246** | **+3.3%** |
| **Raw FPS** | 943 | **975** | +3.4% |
| **Avg Frame Time** | 1,060 us | **1,026 us** | -3.2% |
| **Median Frame Time** | 886 us | **595 us** | **-33%** |
| **Min Frame Time** | 577 us | **384 us** | **-33%** |
| **Stability** | N/A | **3.5%** | Ultra-stable |
| **Binary Size** | 13.4 MB | **5.3 MB** | **-60%** |

---

## System Requirements

- **OS:** Windows 10/11 64-bit (optimized for Windows 11)
- **CPU:** Intel Haswell (4th gen, 2013) or newer with AVX2 support
- **GPU:** Any DirectX 11 compatible GPU
- **RAM:** 256 MB free

---

## Build

Requires Visual Studio 2022 with clang-cl (LLVM toolset).

```batch
REM Full build (deps + main)
build.bat

REM Fast incremental build (main project only)
build_fast.bat
```

Configuration: `Release Unicode | x64`
Compiler: `clang-cl -O3 -march=haswell -mtune=native`
Output: `win32\snes9x-x64.exe`

---

## Acknowledgments

Snes10x is built on the incredible work of the **Snes9x team**:

- Gary Henderson and Jerremy Koot &mdash; Original Snes9x authors (1996-2002)
- Matthew Kendora (2002-2004)
- Peter Bortas (2002-2005)
- Joel Yliluoma (2004-2005)
- John Weidman (2001-2006)
- Brad Jorsch, funkyass, Kris Bleakley, Nach, zones (2002-2010)
- nitsuja (2006-2007)
- BearOso, OV2 (2009-2023)
- Windows port: Matthew Kendora, funkyass, nitsuja, Nach, blip, OV2

Thank you for creating and maintaining one of the best SNES emulators ever made.

Visit the original project: [https://github.com/snes9xgit/snes9x](https://github.com/snes9xgit/snes9x)

---

## License

Licensed under the Snes9x License. See [LICENSE](LICENSE) for details.

---

*Custom build by Ayi NEDJIMI (2024-2026)*
