<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011%2064--bit-0078D6?style=for-the-badge&logo=windows&logoColor=white" />
  <img src="https://img.shields.io/badge/CPU-AVX2%20%7C%20Haswell+-FF8C00?style=for-the-badge&logo=intel&logoColor=white" />
  <img src="https://img.shields.io/badge/GPU-DirectX%2011-68217A?style=for-the-badge&logo=directx&logoColor=white" />
  <img src="https://img.shields.io/badge/License-Snes9x-green?style=for-the-badge" />
</p>

<h1 align="center">Snes10x</h1>

<p align="center">
  <b>High-performance Super Nintendo emulator for modern Windows PCs</b><br>
  <i>Fork of <a href="https://github.com/snes9xgit/snes9x">Snes9x</a> &mdash; optimized for speed, stripped for clarity</i>
</p>

<p align="center">
  <a href="https://github.com/ayinedjimi/Snes10x/releases"><img src="https://img.shields.io/github/v/release/ayinedjimi/Snes10x?style=for-the-badge&color=FF8C00&label=Download" /></a>
  <img src="https://img.shields.io/github/repo-size/ayinedjimi/Snes10x?style=for-the-badge&color=555" />
  <img src="https://img.shields.io/badge/Binary-5.4%20MB-blue?style=for-the-badge" />
</p>

---

## Performance vs Snes9x Original

> Benchmarked on Intel i5-8265U @ 1.60GHz (laptop), Parodius (Europe) PAL, 3000 frames unthrottled

| Metric | Snes9x | Snes10x v0.5 | Gain |
|:-------|-------:|-------------:|-----:|
| **Score** | 15,723 | **16,257** | **+3.4%** |
| **Raw FPS** | 943 | **975** | +3.4% |
| **Median Frame** | 886 us | **583 us** | **-34%** |
| **Min Frame** | 577 us | **370 us** | **-36%** |
| **Stability** | &mdash; | **3.7%** | Ultra-stable |
| **Binary Size** | 13.4 MB | **5.4 MB** | **-60%** |

---

## Architecture

```
                        Snes10x Threading Model
  +---------------------------------------------------------+
  |                                                         |
  |  [Thread 1: CPU Emulation]                              |
  |   65C816 dispatch (switch jump table, M0X0 inlined)     |
  |   DMA/HDMA, Memory Map LUT, branch hints                |
  |       |                          |                      |
  |       | frame buffer copy        | scanline sync        |
  |       v                          v                      |
  |  [Thread 2: D3D11 Render]   [Thread 3: SPC700 APU]     |
  |   Filter + GPU color conv    Audio DSP + resampling     |
  |   Texture upload + Present   Lock-free ring buffer      |
  |                                   |                     |
  |                                   v                     |
  |                          [Thread 4: XAudio2]            |
  |                           Low-latency output            |
  +---------------------------------------------------------+
```

---

## Optimizations

### CPU Core
| Technique | Description |
|:----------|:------------|
| **Direct dispatch (switch)** | 256 M0X0 opcodes via `switch(Op)` &mdash; compiler generates jump table with inlined handlers |
| **Flatten inlining** | `__attribute__((flatten))` on main loop + cpuops.cpp in same translation unit (manual LTO) |
| **Batch event checking** | NMI/IRQ/Timer checks merged into single `S9X_UNLIKELY` branch per opcode |
| **Branch prediction hints** | `S9X_LIKELY`/`S9X_UNLIKELY` on 65C816 loop, memory access, SPC700, DMA |
| **Cache-line alignment** | `alignas(64)` on `SCPUState`, `SICPU`, `SRegisters` with hot fields first |
| **ROM prefetch** | `_mm_prefetch` 64 bytes ahead on opcode fetch |

### GPU Rendering
| Technique | Description |
|:----------|:------------|
| **Async render thread** | 1-frame pipeline overlap with back-pressure sync |
| **GPU color conversion** | HLSL pixel shader converts R16_UINT (RGB565) to RGBA on GPU |
| **Non-temporal stores** | `_mm256_stream_si256` for CPU fallback path (bypasses L1/L2 cache) |
| **AVX2 SIMD** | 8-pixel color conversion using 256-bit vector instructions |
| **FLIP_DISCARD** | Modern DXGI swap chain with tearing support |

### Audio
| Technique | Description |
|:----------|:------------|
| **APU threading** | SPC700 on dedicated thread with mutex+condvar sync |
| **Atomic resampler** | `std::atomic` with `memory_order_acquire/release` barriers |
| **Safe buffer management** | `SubmitSourceBuffer` verified before `InterlockedIncrement` |

### Code Cleanup
| Removed | Lines Saved |
|:--------|------------:|
| OpenGL + CG shaders | ~3,000 |
| Vulkan + SPIRV-Cross + glslang | ~73,000 |
| DirectDraw (legacy DX) | ~700 |
| GTK / macOS / Qt platforms | ~7,700 |
| Movie, AVI, Netplay | ~5,000 |
| **Total** | **~85,000+** |

### Compiler
```
clang-cl -O3 -march=haswell -mtune=native -fno-strict-aliasing
```

---

## Download

> **[Latest Release](https://github.com/ayinedjimi/Snes10x/releases)**

### System Requirements

| | Minimum |
|:--|:--------|
| **OS** | Windows 10/11 64-bit |
| **CPU** | Intel Haswell (4th gen, 2013+) or AMD with AVX2 |
| **GPU** | Any DirectX 11 GPU |
| **RAM** | 256 MB free |

---

## Build from Source

Requires **Visual Studio 2022** with the **clang-cl** (LLVM) toolset.

```batch
:: Full rebuild (dependencies + main project)
build.bat

:: Fast incremental build (main project only)
build_fast.bat
```

| Setting | Value |
|:--------|:------|
| Configuration | `Release Unicode \| x64` |
| Compiler | `clang-cl` (LLVM 19) |
| Optimization | `-O3 -march=haswell -mtune=native` |
| Output | `win32\snes9x-x64.exe` |

---

## Acknowledgments

Snes10x stands on the shoulders of the incredible **Snes9x** project and its contributors:

| Period | Contributors |
|:-------|:-------------|
| 1996-2002 | Gary Henderson, Jerremy Koot &mdash; *Original authors* |
| 2002-2004 | Matthew Kendora |
| 2002-2005 | Peter Bortas |
| 2004-2005 | Joel Yliluoma |
| 2001-2006 | John Weidman |
| 2002-2010 | Brad Jorsch, funkyass, Kris Bleakley, Nach, zones |
| 2006-2007 | nitsuja |
| 2009-2023 | BearOso, OV2 |
| Win32 port | Matthew Kendora, funkyass, nitsuja, Nach, blip, OV2 |

Thank you for creating and maintaining one of the best SNES emulators ever made.

**Original project:** [github.com/snes9xgit/snes9x](https://github.com/snes9xgit/snes9x)

---

## License

Licensed under the Snes9x License. See [LICENSE](LICENSE) for details.

---

<p align="center">
  <i>Custom build by <b>Ayi NEDJIMI</b> (2024-2026)</i><br>
  <sub>Built with the assistance of Claude (Anthropic)</sub>
</p>
