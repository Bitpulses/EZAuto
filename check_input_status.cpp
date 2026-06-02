#include <windows.h>
#include <imm.h>
#include <iostream>

// Fix for MinGW missing macro definition
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS 0x0005
#endif

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "user32.lib")

void CheckGlobalIMEState() {
    // 1. Get the handle of the foreground window (the window user is currently using)
    HWND hwndFlipped = GetForegroundWindow();
    if (!hwndFlipped) {
        std::cout << "No foreground window detected." << std::endl;
        return;
    }

    // 2. Get the thread ID of the target window and current console thread
    DWORD targetThreadId = GetWindowThreadProcessId(hwndFlipped, NULL);
    DWORD currentThreadId = GetCurrentThreadId();

    // 3. Attach our thread input to the target window's thread
    BOOL isAttached = FALSE;
    if (targetThreadId != currentThreadId) {
        isAttached = AttachThreadInput(currentThreadId, targetThreadId, TRUE);
    }

    // 4. Get the actual focused control/window handle inside the target application
    HWND hwndFocus = GetFocus();
    if (!hwndFocus) {
        hwndFocus = hwndFlipped; // Fallback to the main window handle
    }

    // 5. Get the Default IME Window for the focused window
    HWND hwndIME = ImmGetDefaultIMEWnd(hwndFocus);
    if (hwndIME) {
        // Send a message to the IME window to query its Open/Close status
        LRESULT isOpen = SendMessageW(hwndIME, WM_IME_CONTROL, IMC_GETOPENSTATUS, 0);
        
        if (isOpen != 0) {
            std::cout << "IME Status: [ CHINESE Mode ]" << std::endl;
        } else {
            std::cout << "IME Status: [ ENGLISH Mode ]" << std::endl;
        }
    } else {
        std::cout << "No IME window associated with the active application." << std::endl;
    }

    // 6. Detach the thread input to clean up
    if (isAttached) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }
}

int main() {
    std::cout << "--- Starting Global IME Monitor ---" << std::endl;
    
    // Loop 5 times, sleeping 2 seconds each time
    for (int i = 0; i < 5; ++i) {
        std::cout << "\n[Check #" << i + 1 << "]" << std::endl;
        CheckGlobalIMEState();
        Sleep(2000);
    }

    std::cout << "\n--- Monitor Finished. Press Enter to exit ---" << std::endl;
    std::cin.get();
    return 0;
}
