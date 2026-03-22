# Ultra Limiter v2.0 (Clean-Room Rewrite)

FG-aware frame rate limiter for PC games, implemented as a ReShade addon.

## Features

- FPS limiting via NVIDIA Reflex Sleep or QPC timing fallback
- Frame Generation aware pacing (DLSS-G / FSR FG detection)
- Multiple pacing presets (native, marker-based, Streamline proxy)
- On-screen display: FPS, frametime, native FPS, GPU latency, FG mode, resolution
- Frametime graph
- Monitor switching and fullscreen/borderless override
- INI-based configuration with ReShade overlay UI

## Building

Requires: CMake 3.15+, Visual Studio 2022+ (or 2026), 64-bit target.

```
git clone https://github.com/TsudaKageyu/minhook external/minhook
git clone --branch v6.7.3 https://github.com/crosire/reshade external/reshade
git clone --branch v1.92.5-docking https://github.com/ocornut/imgui external/imgui
git clone https://github.com/NVIDIA/nvapi external/nvapi

cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Output: `build/Release/ultra_limiter.addon64`

## Installation

Copy `ultra_limiter.addon64` next to your game's ReShade installation.

## Credits & Licenses

- **Ultra Limiter** — MIT License (see LICENSE)
- **MinHook** by Tsuda Kageyu — BSD 2-Clause
- **ReShade SDK** by Patrick Mours — BSD 3-Clause
- **Dear ImGui** by Omar Cornut — MIT
- **NVIDIA NVAPI** by NVIDIA Corporation — MIT

## Code Origins

This is a clean-room rewrite. All code was written from public API
documentation only. No code from Display Commander, Special K, or any
other frame limiter project was used. See ORIGINS.txt for full details.
