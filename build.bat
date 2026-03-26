@echo off
echo === ReLimiter — Build ===

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not exist build (
    echo Configuring x64...
    %CMAKE% -B build -G "Visual Studio 18 2026" -A x64
)

if not exist build32 (
    echo Configuring x86...
    %CMAKE% -B build32 -G "Visual Studio 18 2026" -A Win32
)

echo Building x64...
%CMAKE% --build build --config Release
if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED x64.
    exit /b 1
)

echo Building x86...
%CMAKE% --build build32 --config Release
if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED x86.
    exit /b 1
)

echo.
echo Build successful:
echo   x64: build\Release\relimiter.addon64
echo   x86: build32\Release\relimiter.addon32
