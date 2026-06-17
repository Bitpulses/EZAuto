#pragma once

#include "Types.h"

#include <chrono>
#include <string>

class ImeSwitcher {
public:
    ImeSwitcher();
    ~ImeSwitcher();

    // Detect current IME mode for the given window
    // verbose: if true, print detection details to stdout
    // detail: if not null, filled with summary like "NATIVE=1 convMode=0x1 → CN"
    ImeMode getCurrentMode(HWND hwnd, bool verbose = true, std::string* detail = nullptr);

    // Switch to target mode.
    // currentMode: already-detected mode (avoids re-detection race).
    // Returns true if switch succeeded.
    bool switchTo(ImeMode targetMode, HWND hwnd, ImeMode currentMode,
                  SwitchMethod method = SwitchMethod::Shift);

private:
    // Get focused control within a thread
    HWND getFocusWindowForThread(DWORD threadId);

    // Try multiple strategies to acquire a valid HIMC
    HIMC acquireImmContext(HWND hwnd, DWORD threadId, HWND& outCtxHwnd);

    // Step 1: set conversion mode via ImmSetConversionStatus API (needs HIMC)
    bool switchViaImmSet(HWND hwnd, ImeMode targetMode);

    // Step 2: set conversion mode via WM_IME_CONTROL IMC_SETCONVERSIONMODE (for TSF apps)
    bool switchViaImeControl(HWND hwnd, ImeMode targetMode);

    // Step 3: key simulation via PostMessage WM_KEYDOWN/WM_KEYUP (no injected flag, works for TSF)
    void simulateKeyViaMessage(HWND hwnd, SwitchMethod method);

    // Step 4/5: key simulation via SendInput (last resort, has injected flag)
    void simulateKey(HWND hwnd, SwitchMethod method);

    // Verify switch succeeded (re-read mode after delay)
    bool verifySwitch(HWND hwnd, ImeMode expectedMode);

    // ---- State ----
    std::chrono::steady_clock::time_point lastSwitchTime_;
    ImeMode lastTargetMode_ = ImeMode::Chinese;
    static constexpr int DEBOUNCE_MS = 100;

    // Log indentation for tree-style output
    std::string logIndent_;
};
