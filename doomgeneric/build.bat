@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set PATH=C:\arm-toolchain\bin;%PATH%

set CC=aarch64-none-linux-gnu-gcc

:: CFLAGS UPDATES:
:: 1. Removed "-shared -fPIC" (Builds an executable, not a library)
:: 2. Changed to gnu99 standard to support snprintf and other C99 features
:: 3. Added "-D_POSIX_C_SOURCE=200809L" for POSIX functions like strdup
set CFLAGS=-std=gnu99 -O3 -mcpu=cortex-a53 -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -D_POSIX_C_SOURCE=200809L

echo Scanning for sources...
if exist doom_stream del doom_stream
set "SOURCES="

:: EXCLUSION LIST UPDATES:
:: Removed "pynq" so your new platform file is included, regardless of its name.
for /f "delims=" %%f in ('dir /b *.c ^| findstr /v /i "allegro emscripten linuxvt sdl soso win xlib"') do (
    set "SOURCES=!SOURCES! %%f"
)

echo.
echo Compiling doomgeneric UDP Stream executable...
echo %CC% %CFLAGS% -o doom_stream !SOURCES! -lm
%CC% %CFLAGS% -o doom_stream !SOURCES! -lm

if exist doom_stream (
    echo.
    echo [SUCCESS] doom_stream created.
) else (
    echo.
    echo [FAILURE] Compilation failed.
)
pause