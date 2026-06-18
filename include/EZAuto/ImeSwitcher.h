#pragma once

#include "Types.h"

#include <chrono>
#include <string>
#include <mutex>

class ImeSwitcher {
public:
    ImeSwitcher();
    ~ImeSwitcher();

    ImeMode getCurrentMode(HWND hwnd, bool verbose = true, std::string* detail = nullptr);
    bool switchTo(ImeMode targetMode, HWND hwnd, ImeMode currentMode,
                  SwitchMethod method = SwitchMethod::Shift);

private:
    HWND getFocusWindowForThread(DWORD threadId);
    HIMC acquireImmContext(HWND hwnd, DWORD threadId, HWND& outCtxHwnd);
    bool switchViaImmSet(HWND hwnd, ImeMode targetMode);
    bool switchViaImeControl(HWND hwnd, ImeMode targetMode);
    void simulateKeyViaMessage(HWND hwnd, SwitchMethod method);
    void simulateKey(HWND hwnd, SwitchMethod method);
    bool verifySwitch(HWND hwnd, ImeMode expectedMode);

    std::mutex switchMutex_;
    std::chrono::steady_clock::time_point lastSwitchTime_;
    ImeMode lastTargetMode_ = ImeMode::Chinese;
    static constexpr int DEBOUNCE_MS = 100;
    std::string logIndent_;
};
