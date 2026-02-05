@echo off
setlocal EnableDelayedExpansion
:: Force script to run from current directory
cd /d "%~dp0"

set CC=aarch64-none-linux-gnu-gcc
set CFLAGS=-std=gnu89 -O2 -mcpu=cortex-a53 -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -lm

echo Scanning for sources...

:: Initialize empty source list
set "SOURCES="

:: 1. Loop through ALL .c files in the folder
:: 2. Pipe into FINDSTR /V (Inverse match) to ignore the specific backends
for /f "delims=" %%f in ('dir /b *.c ^| findstr /v /i "allegro emscripten linuxvt sdl soso win xlib"') do (
    
    :: Logic to ensure we don't accidentally exclude our own uart file if it matched a pattern
    :: (Not strictly necessary with the specific keywords above, but good practice)
    set "skip=0"
    
    :: Add valid file to the list
    if !skip! equ 0 (
        set "SOURCES=!SOURCES! %%f"
    )
)

echo.
echo Compiling Doom for PYNQ (UART Backend)...
echo Ignoring: SDL, Win, LinuxVT, Allegro, etc.
echo.

:: Compile
%CC% %CFLAGS% -o doomgeneric !SOURCES!

if exist doomgeneric (
    echo.
    echo [SUCCESS] doomgeneric binary created.
) else (
    echo.
    echo [FAILURE] Compilation failed.
)

pause