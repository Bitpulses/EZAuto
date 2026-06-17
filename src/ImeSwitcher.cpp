#include <EZAuto/ImeSwitcher.h>

#include <imm.h>
#include <iostream>

#pragma comment(lib, "imm32.lib")

// MinGW imm.h may not define these
#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_SETCONVERSIONMODE
#define IMC_SETCONVERSIONMODE 0x0002
#endif
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS      0x0003
#endif

//  Helpers

static std::string hexStr(DWORD val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", val);
    return buf;
}

static std::string hwndStr(HWND hwnd) {
    if (!hwnd) return "null";
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%llx", reinterpret_cast<unsigned long long>(hwnd));
    return buf;
}

static std::string convModeDesc(DWORD convMode) {
    std::string flags;
    if (convMode & IME_CMODE_NATIVE)     flags += "NATIVE|";
    if (convMode & IME_CMODE_KATAKANA)   flags += "KATAKANA|";
    if (convMode & IME_CMODE_FULLSHAPE)  flags += "FULLSHAPE|";
    if (convMode & IME_CMODE_ROMAN)      flags += "ROMAN|";
    if (convMode & IME_CMODE_CHARCODE)   flags += "CHARCODE|";
    if (convMode & IME_CMODE_SOFTKBD)    flags += "SOFTKBD|";
    if (convMode & IME_CMODE_NOCONVERSION) flags += "NOCONV|";
    if (convMode & IME_CMODE_EUDC)       flags += "EUDC|";
    if (convMode & IME_CMODE_SYMBOL)     flags += "SYMBOL|";
    if (!flags.empty()) flags.pop_back();
    return flags;
}

ImeSwitcher::ImeSwitcher() = default;
ImeSwitcher::~ImeSwitcher() = default;

//  Context Acquisition 

HWND ImeSwitcher::getFocusWindowForThread(DWORD threadId) {
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(GUITHREADINFO);
    if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus) {
        return gti.hwndFocus;
    }
    return nullptr;
}

HIMC ImeSwitcher::acquireImmContext(HWND hwnd, DWORD threadId, HWND& outCtxHwnd) {
    HIMC himc = nullptr;
    outCtxHwnd = nullptr;

    // Strategy 1: Focused control in the target thread
    HWND focusHwnd = getFocusWindowForThread(threadId);
    if (focusHwnd) {
        himc = ImmGetContext(focusHwnd);
        if (himc) { outCtxHwnd = focusHwnd; return himc; }
    }

    // Strategy 2: The passed-in window handle (e.g. foreground/root)
    if (hwnd && hwnd != focusHwnd) {
        himc = ImmGetContext(hwnd);
        if (himc) { outCtxHwnd = hwnd; return himc; }
    }

    // Strategy 3: Default IME window
    HWND targetForIme = focusHwnd ? focusHwnd : hwnd;
    if (targetForIme) {
        HWND imeWnd = ImmGetDefaultIMEWnd(targetForIme);
        if (imeWnd) {
            himc = ImmGetContext(imeWnd);
            if (himc) { outCtxHwnd = imeWnd; return himc; }
        }
    }

    return nullptr;
}

// Mode Detection 

ImeMode ImeSwitcher::getCurrentMode(HWND hwnd, bool verbose, std::string* detail) {
    if (!hwnd) {
        if (detail) *detail = "no window → EN";
        return ImeMode::English;
    }

    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);

    // Step 1: ImmGetConversionStatus via HIMC (most direct)
    HWND ctxHwnd = nullptr;
    HIMC himc = acquireImmContext(hwnd, threadId, ctxHwnd);
    if (himc) {
        DWORD convMode = 0, sentMode = 0;
        ImmGetConversionStatus(himc, &convMode, &sentMode);
        ImmReleaseContext(ctxHwnd, himc);

        bool isNative = (convMode & IME_CMODE_NATIVE) != 0;
        if (detail) {
            *detail = "IMM NATIVE=" + std::to_string(isNative) +
                      " convMode=" + hexStr(convMode) +
                      " [" + convModeDesc(convMode) + "]" +
                      " → " + (isNative ? "CN" : "EN");
        }
        if (verbose) {
            std::cout << logIndent_ << "├─ [Detect] IMM via " << hwndStr(ctxHwnd)
                      << ": NATIVE=" << isNative
                      << " convMode=" << hexStr(convMode)
                      << " [" << convModeDesc(convMode) << "]"
                      << " → " << (isNative ? "CN" : "EN") << std::endl;
        }
        return isNative ? ImeMode::Chinese : ImeMode::English;
    }

    // Step 2: WM_IME_CONTROL via default IME window (for TSF IMEs without direct HIMC)
    HWND imeWnd = ImmGetDefaultIMEWnd(hwnd);
    if (imeWnd) {
        LRESULT convResult = SendMessage(imeWnd, WM_IME_CONTROL,
                                         static_cast<WPARAM>(IMC_GETCONVERSIONMODE), 0);
        // Disambiguate convResult==0: could be EN mode or query failure
        if (convResult > 0) {
            DWORD convMode = static_cast<DWORD>(convResult);
            bool isNative = (convMode & IME_CMODE_NATIVE) != 0;
            if (detail) {
                *detail = "IMECtrl NATIVE=" + std::to_string(isNative) +
                          " convMode=" + hexStr(convMode) +
                          " [" + convModeDesc(convMode) + "]" +
                          " → " + (isNative ? "CN" : "EN");
            }
            if (verbose) {
                std::cout << logIndent_ << "├─ [Detect] IMECtrl via " << hwndStr(imeWnd)
                          << ": NATIVE=" << isNative
                          << " convMode=" << hexStr(convMode)
                          << " → " << (isNative ? "CN" : "EN") << std::endl;
            }
            return isNative ? ImeMode::Chinese : ImeMode::English;
        }
        if (convResult == 0) {
            LRESULT openStatus = SendMessage(imeWnd, WM_IME_CONTROL,
                                             static_cast<WPARAM>(IMC_GETOPENSTATUS), 0);
            if (openStatus) {
                // IME is open, convMode=0 → genuine EN mode
                if (detail) *detail = "IMECtrl NATIVE=0 convMode=0x0 [] → EN";
                if (verbose) {
                    std::cout << logIndent_ << "├─ [Detect] IMECtrl via " << hwndStr(imeWnd)
                              << ": NATIVE=0 convMode=0x0 → EN" << std::endl;
                }
                return ImeMode::English;
            }
        }
    }

    // Step 3: GetKeyboardLayout (UNRELIABLE — cannot distinguish CN/EN within one IME)
    HKL hkl = GetKeyboardLayout(threadId);
    LANGID langId = LOWORD(reinterpret_cast<UINT_PTR>(hkl));
    bool isChinese = (PRIMARYLANGID(langId) == LANG_CHINESE);

    if (detail) {
        *detail = std::string("Layout=") + (isChinese ? "Chinese" : "English") +
                  " → " + (isChinese ? "CN" : "EN") + " (unreliable)";
    }
    if (verbose) {
        std::cout << logIndent_ << "├─ [Detect] Layout HKL=0x"
                  << std::hex << reinterpret_cast<UINT_PTR>(hkl) << std::dec
                  << " → " << (isChinese ? "CN" : "EN") << " (unreliable)" << std::endl;
    }
    return isChinese ? ImeMode::Chinese : ImeMode::English;
}

//  Switch Methods 

bool ImeSwitcher::switchViaImmSet(HWND hwnd, ImeMode targetMode) {
    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    HWND ctxHwnd = nullptr;
    HIMC himc = acquireImmContext(hwnd, threadId, ctxHwnd);
    if (!himc) {
        std::cout << logIndent_ << "├─ [ImmSet] No HIMC available" << std::endl;
        return false;
    }

    DWORD convMode = 0, sentMode = 0;
    ImmGetConversionStatus(himc, &convMode, &sentMode);

    DWORD targetConvMode = convMode;
    if (targetMode == ImeMode::Chinese) {
        targetConvMode |= IME_CMODE_NATIVE;
    } else {
        targetConvMode &= ~IME_CMODE_NATIVE;
    }

    if (convMode == targetConvMode) {
        ImmReleaseContext(ctxHwnd, himc);
        std::cout << logIndent_ << "├─ [ImmSet] Already in target mode" << std::endl;
        return true;
    }

    BOOL ok = ImmSetConversionStatus(himc, targetConvMode, sentMode);
    ImmReleaseContext(ctxHwnd, himc);

    std::cout << logIndent_ << "├─ [ImmSet] convMode " << hexStr(convMode)
              << " → " << hexStr(targetConvMode)
              << " → " << (ok ? "OK" : "FAILED") << std::endl;
    return ok;
}

bool ImeSwitcher::switchViaImeControl(HWND hwnd, ImeMode targetMode) {
    HWND imeWnd = ImmGetDefaultIMEWnd(hwnd);
    if (!imeWnd) {
        std::cout << logIndent_ << "├─ [ImeCtrl] No IME window" << std::endl;
        return false;
    }

    // Get current convMode via WM_IME_CONTROL
    LRESULT currentResult = SendMessage(imeWnd, WM_IME_CONTROL,
                                         static_cast<WPARAM>(IMC_GETCONVERSIONMODE), 0);
    if (currentResult < 0) {
        std::cout << logIndent_ << "├─ [ImeCtrl] Cannot read current mode (result="
                  << currentResult << ")" << std::endl;
        return false;
    }

    DWORD convMode = static_cast<DWORD>(currentResult);
    DWORD targetConvMode = convMode;
    if (targetMode == ImeMode::Chinese) {
        targetConvMode |= IME_CMODE_NATIVE;
    } else {
        targetConvMode &= ~IME_CMODE_NATIVE;
    }

    if (convMode == targetConvMode) {
        std::cout << logIndent_ << "├─ [ImeCtrl] Already in target mode" << std::endl;
        return true;
    }

    // Set conversion mode via WM_IME_CONTROL IMC_SETCONVERSIONMODE
    // Returns previous convMode on success
    LRESULT setResult = SendMessage(imeWnd, WM_IME_CONTROL,
                                     static_cast<WPARAM>(IMC_SETCONVERSIONMODE),
                                     static_cast<LPARAM>(targetConvMode));

    std::cout << logIndent_ << "├─ [ImeCtrl] convMode " << hexStr(convMode)
              << " → " << hexStr(targetConvMode)
              << " setResult=" << setResult << std::endl;

    // Wait for TSF to potentially sync back the real state to the compatibility layer.
    // If TSF didn't accept the change, the compatibility layer value will revert.
    Sleep(200);

    LRESULT recheck = SendMessage(imeWnd, WM_IME_CONTROL,
                                   static_cast<WPARAM>(IMC_GETCONVERSIONMODE), 0);
    if (recheck >= 0) {
        DWORD reconvMode = static_cast<DWORD>(recheck);
        bool nowNative = (reconvMode & IME_CMODE_NATIVE) != 0;
        bool wantNative = (targetMode == ImeMode::Chinese);
        if (nowNative != wantNative) {
            std::cout << logIndent_ << "├─ [ImeCtrl] TSF reverted → convMode="
                      << hexStr(reconvMode) << std::endl;
            return false;
        }
    }

    return true;
}

void ImeSwitcher::simulateKeyViaMessage(HWND hwnd, SwitchMethod method) {
    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    HWND focusHwnd = getFocusWindowForThread(threadId);
    HWND targetWnd = focusHwnd ? focusHwnd : hwnd;

    std::cout << logIndent_ << "├─ [MsgKey] Target=" << hwndStr(targetWnd) << std::endl;

    if (method == SwitchMethod::CtrlSpace) {
        WORD scanCtrl = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        WORD scanSpace = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));

        LPARAM lpCtrlDown  = MAKELPARAM(1, scanCtrl);
        LPARAM lpSpaceDown = MAKELPARAM(1, scanSpace);
        LPARAM lpSpaceUp   = MAKELPARAM(1, scanSpace | 0xC00000);  // previous=1, transition=1
        LPARAM lpCtrlUp    = MAKELPARAM(1, scanCtrl  | 0xC00000);

        // Use SendMessage (synchronous) instead of PostMessage (async).
        // PostMessage queues messages, allowing real user keystrokes to interleave
        // between Ctrl-down and Space-down, causing Space to be typed as a character.
        // SendMessage processes each message atomically before returning, preventing
        // the Ctrl and Space from being split apart.
        SendMessage(targetWnd, WM_KEYDOWN, VK_CONTROL, lpCtrlDown);
        SendMessage(targetWnd, WM_KEYDOWN, VK_SPACE,  lpSpaceDown);
        SendMessage(targetWnd, WM_KEYUP,   VK_SPACE,  lpSpaceUp);
        SendMessage(targetWnd, WM_KEYUP,   VK_CONTROL, lpCtrlUp);

        std::cout << logIndent_ << "├─ [MsgKey] Ctrl+Space sent to "
                  << hwndStr(targetWnd) << std::endl;
    } else {
        WORD scanShift = static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));

        LPARAM lpShiftDown = MAKELPARAM(1, scanShift);
        LPARAM lpShiftUp   = MAKELPARAM(1, scanShift | 0xC00000);

        SendMessage(targetWnd, WM_KEYDOWN, VK_SHIFT, lpShiftDown);
        SendMessage(targetWnd, WM_KEYUP,   VK_SHIFT, lpShiftUp);

        std::cout << logIndent_ << "├─ [MsgKey] Shift sent to "
                  << hwndStr(targetWnd) << std::endl;
    }
}

void ImeSwitcher::simulateKey(HWND hwnd, SwitchMethod method) {
    DWORD targetThreadId = GetWindowThreadProcessId(hwnd, nullptr);
    DWORD ourThreadId = GetCurrentThreadId();

    bool attached = false;
    if (targetThreadId != ourThreadId) {
        attached = AttachThreadInput(ourThreadId, targetThreadId, TRUE);
        std::cout << logIndent_ << "├─ [KeySim] AttachThreadInput → "
                  << (attached ? "OK" : "FAILED") << std::endl;
    }
    if (attached) Sleep(5);

    if (method == SwitchMethod::CtrlSpace) {
        // Ctrl+Space key simulation
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_CONTROL;
        inputs[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_SPACE;
        inputs[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wVk = VK_SPACE;
        inputs[2].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
        inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wVk = VK_CONTROL;
        inputs[3].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

        UINT sent = SendInput(4, inputs, sizeof(INPUT));
        std::cout << logIndent_ << "├─ [KeySim] Ctrl+Space sent=" << sent << "/4" << std::endl;
    } else {
        // Shift key simulation (Microsoft Pinyin default toggle)
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_SHIFT;
        inputs[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_SHIFT;
        inputs[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

        UINT sent = SendInput(2, inputs, sizeof(INPUT));
        std::cout << logIndent_ << "├─ [KeySim] Shift sent=" << sent << "/2" << std::endl;
    }

    // Wait for IME to process the key before detaching
    Sleep(50);

    if (attached) {
        AttachThreadInput(ourThreadId, targetThreadId, FALSE);
    }
}

//  Verification 

bool ImeSwitcher::verifySwitch(HWND hwnd, ImeMode expectedMode) {
    Sleep(50);  // Let IME process the change
    std::string detail;
    ImeMode actualMode = getCurrentMode(hwnd, false, &detail);
    std::cout << logIndent_ << "├─ Verify: " << detail << std::endl;
    return actualMode == expectedMode;
}

// Main Switch Logic

bool ImeSwitcher::switchTo(ImeMode targetMode, HWND hwnd, ImeMode currentMode,
                           SwitchMethod method) {
    if (!hwnd) return false;

    std::string savedIndent = logIndent_;
    logIndent_ = " │  ";

    // Debounce
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSwitchTime_).count();
    if (elapsed < DEBOUNCE_MS && lastTargetMode_ == targetMode) {
        std::cout << logIndent_ << "└─ Debounced" << std::endl;
        logIndent_ = savedIndent;
        return true;
    }

    if (currentMode == targetMode) {
        std::cout << logIndent_ << "└─ Already in target mode" << std::endl;
        logIndent_ = savedIndent;
        return true;
    }

    std::string methodStr = (method == SwitchMethod::Shift) ? "Shift" :
                            (method == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "Target: " << hwndStr(hwnd)
              << " | Method: " << methodStr << std::endl;

    // Step 1: ImmSetConversionStatus (direct API, most reliable for IMM apps)
    std::cout << logIndent_ << "Step 1: ImmSetConversionStatus" << std::endl;
    if (switchViaImmSet(hwnd, targetMode)) {
        if (verifySwitch(hwnd, targetMode)) {
            std::cout << logIndent_ << "└─ OK" << std::endl;
            logIndent_ = savedIndent;
            lastSwitchTime_ = std::chrono::steady_clock::now();
            lastTargetMode_ = targetMode;
            return true;
        }
        std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;
    }

    // Step 2: WM_IME_CONTROL IMC_SETCONVERSIONMODE (for TSF apps without HIMC)
    std::cout << logIndent_ << "Step 2: ImeCtrl (WM_IME_CONTROL)" << std::endl;
    if (switchViaImeControl(hwnd, targetMode)) {
        if (verifySwitch(hwnd, targetMode)) {
            std::cout << logIndent_ << "└─ OK" << std::endl;
            logIndent_ = savedIndent;
            lastSwitchTime_ = std::chrono::steady_clock::now();
            lastTargetMode_ = targetMode;
            return true;
        }
        std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;
    }

    // Step 3: Key simulation via PostMessage (no injected flag, works for Win11 TSF IMEs)
    std::string step3Name = (method == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "Step 3: MsgKey " << step3Name << std::endl;
    simulateKeyViaMessage(hwnd, method);
    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }
    std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;

    // Step 4: Key simulation via SendInput (has injected flag, last resort for primary hotkey)
    std::string step4Name = (method == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "Step 4: KeySim " << step4Name << std::endl;
    simulateKey(hwnd, method);
    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }
    std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;

    // Step 5: Try alternative hotkey via PostMessage
    SwitchMethod altMethod = (method == SwitchMethod::CtrlSpace) ? SwitchMethod::Shift : SwitchMethod::CtrlSpace;
    std::string step5Name = (altMethod == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "Step 5: MsgKey " << step5Name << std::endl;
    simulateKeyViaMessage(hwnd, altMethod);
    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }
    std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;

    // Step 6: Try alternative hotkey via SendInput
    std::string step6Name = (altMethod == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "Step 6: KeySim " << step6Name << std::endl;
    simulateKey(hwnd, altMethod);
    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }

    std::cout << logIndent_ << "└─ FAILED" << std::endl;
    logIndent_ = savedIndent;
    lastSwitchTime_ = std::chrono::steady_clock::now();
    lastTargetMode_ = targetMode;
    return false;
}
