@echo off
echo Building EZAuto...

set GCC_PATH=D:\Software\msys64\ucrt64\bin
#set GCC_PATH=D:\MySoftware\MSYS2\ucrt64\bin
set PATH=%GCC_PATH%;%PATH%

g++ -std=c++17 -O2 -o release/EZAuto.exe ^
    src/main.cpp ^
    src/FocusMonitor.cpp ^
    src/ImeSwitcher.cpp ^
    src/ConfigManager.cpp ^
    -Iinclude ^
    -lole32 -loleaut32 -luuid -loleacc -limm32 -lpsapi ^
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00

if %ERRORLEVEL% EQU 0 (
    echo Build successful! Output: EZAuto.exe
) else (
    echo Build failed!
)
