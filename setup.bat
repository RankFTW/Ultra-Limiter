@echo off
echo === Ultra Limiter v2.0 — Dependency Setup ===
echo.

if not exist external\minhook (
    echo Cloning MinHook...
    git clone https://github.com/TsudaKageyu/minhook external/minhook
) else (
    echo MinHook already present.
)

if not exist external\reshade (
    echo Cloning ReShade SDK v6.7.3...
    git clone --branch v6.7.3 https://github.com/crosire/reshade external/reshade
) else (
    echo ReShade SDK already present.
)

if not exist external\imgui (
    echo Cloning Dear ImGui v1.92.5-docking...
    git clone --branch v1.92.5-docking https://github.com/ocornut/imgui external/imgui
) else (
    echo Dear ImGui already present.
)

if not exist external\nvapi (
    echo Cloning NVIDIA NVAPI...
    git clone https://github.com/NVIDIA/nvapi external/nvapi
) else (
    echo NVAPI already present.
)

echo.
echo Done. Run build.bat to compile.
