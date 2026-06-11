#pragma once

#include "Types.h"

#include <chrono>
#include <string>
#include <vector>

class ImeSwitcher {
public:
    ImeSwitcher();
    ~ImeSwitcher();

    // Detect current IME mode for the given window
    // verbose: if true, print detection details to stdout
    // detail: if not null, filled with summary like "NATIVE=1 convMode=0x1 → CN"
    ImeMode getCurrentMode(HWND hwnd, bool verbose = true, std::string* detail = nullptr);

    // Switch to target mode if needed.
    // currentMode: the already-detected mode (caller should pass this to avoid
    //              re-detection race after Sleep delay).
    // Returns true if switch succeeded (or was unnecessary).
    bool switchTo(ImeMode targetMode, HWND hwnd, ImeMode currentMode,
                  SwitchMethod method = SwitchMethod::Shift);

private:
    // ---- Detection Methods ----

    // TSF API hint (NOT per-thread - returns calling thread's profile)
    // Only used as fallback when layout is Chinese and no HIMC/IME-Control available
    ImeMode getCurrentModeViaTsf();

    // ---- Switching Methods ----

    // Method 1: IMM API - toggle IME_CMODE_NATIVE bit (works for Win32 apps with HIMC)
    bool switchViaImm(HWND hwnd, ImeMode targetMode);

    // Method 2: WM_IME_CONTROL through default IME window (may work for TSF apps)
    bool switchViaImeControl(HWND hwnd, ImeMode targetMode);

    // Method 3: Keyboard simulation with AttachThreadInput
    void simulateCtrlSpace(HWND targetHwnd);
    void simulateShiftKey(HWND targetHwnd);

    // Method 4: Raw keyboard simulation (no AttachThreadInput, last resort)
    void simulateCtrlSpaceRaw();
    void simulateShiftKeyRaw();

    // ---- Helpers ----
    DWORD getWindowThreadId(HWND hwnd);
    HWND getFocusWindowForThread(DWORD threadId);
    HIMC acquireImmContext(HWND hwnd, DWORD threadId, HWND& outCtxHwnd);

    // Get the default IME window for the target window's thread
    HWND getDefaultImeWnd(HWND hwnd);

    // Query conversion mode via WM_IME_CONTROL with retry
    // Returns: >= 0 = valid conversion mode, -1 = query failed
    LRESULT imeControlGetConvMode(HWND imeWnd, int maxRetries = 3, int retryDelayMs = 30);

    // Verify switch succeeded (re-read mode after delay)
    // Prints Detect line with logIndent_, does NOT print Verify line.
    bool verifySwitch(HWND hwnd, ImeMode expectedMode);

    // ---- State ----
    std::chrono::steady_clock::time_point lastSwitchTime_;
    ImeMode lastTargetMode_ = ImeMode::Chinese;
    static constexpr int DEBOUNCE_MS = 100;

    // Log indentation for tree-style output.
    // Set by switchTo() before calling sub-functions.
    // "" = standalone, " │  " = inside switch tree
    std::string logIndent_;
};
