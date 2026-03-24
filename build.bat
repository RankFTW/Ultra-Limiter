@echo off
echo === ReLimiter — Build ===

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not exist build (
    echo Configuring...
    %CMAKE% -B build -G "Visual Studio 18 2026" -A x64
)

echo Building...
%CMAKE% --build build --config Release

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful: build\Release\ultra_limiter.addon64
) else (
    echo.
    echo Build FAILED.
)
