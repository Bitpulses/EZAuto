#include <EZAuto/ImeSwitcher.h>

#include <imm.h>
#include <iostream>

#pragma comment(lib, "imm32.lib")

#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_SETCONVERSIONMODE
#define IMC_SETCONVERSIONMODE 0x0002
#endif
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS      0x0003
#endif

static std::string hexStr(DWORD val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", val);
    return buf;
}

static std::string hwndStr(HWND hwnd) {
    if (!hwnd) return "null";
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
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

    HWND focusHwnd = getFocusWindowForThread(threadId);
    if (focusHwnd) {
        himc = ImmGetContext(focusHwnd);
        if (himc) { outCtxHwnd = focusHwnd; return himc; }
    }

    if (hwnd && hwnd != focusHwnd) {
        himc = ImmGetContext(hwnd);
        if (himc) { outCtxHwnd = hwnd; return himc; }
    }

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

ImeMode ImeSwitcher::getCurrentMode(HWND hwnd, bool verbose, std::string* detail) {
    if (!hwnd) {
        if (detail) *detail = "no window → EN";
        return ImeMode::English;
    }

    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);

    // IMM via HIMC
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

    // WM_IME_CONTROL fallback (TSF IMEs without direct HIMC)
    HWND imeWnd = ImmGetDefaultIMEWnd(hwnd);
    if (imeWnd) {
        DWORD_PTR convResultPtr = 0;
        bool convOk = (SendMessageTimeoutA(imeWnd, WM_IME_CONTROL,
                          static_cast<WPARAM>(IMC_GETCONVERSIONMODE), 0,
                          SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000, &convResultPtr) != 0);
        LRESULT convResult = static_cast<LRESULT>(convResultPtr);
        if (convOk && convResult > 0) {
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
        if (convOk && convResult == 0) {
            DWORD_PTR openStatusPtr = 0;
            bool openOk = (SendMessageTimeoutA(imeWnd, WM_IME_CONTROL,
                               static_cast<WPARAM>(IMC_GETOPENSTATUS), 0,
                               SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000, &openStatusPtr) != 0);
            LRESULT openStatus = static_cast<LRESULT>(openStatusPtr);
            if (openOk && openStatus) {
                if (detail) *detail = "IMECtrl NATIVE=0 convMode=0x0 [] → EN";
                if (verbose) {
                    std::cout << logIndent_ << "├─ [Detect] IMECtrl via " << hwndStr(imeWnd)
                              << ": NATIVE=0 convMode=0x0 → EN" << std::endl;
                }
                return ImeMode::English;
            }
            // IME closed, can't reliably determine mode
            if (detail) *detail = "IMECtrl IME closed → unreliable";
            if (verbose) {
                std::cout << logIndent_ << "├─ [Detect] IMECtrl via " << hwndStr(imeWnd)
                          << ": IME closed, falling back to layout" << std::endl;
            }
        }
    }

    // Last resort: keyboard layout (can't distinguish CN/EN within one IME)
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

    DWORD_PTR currentResultPtr = 0;
    LRESULT currentResult;
    if (!SendMessageTimeoutA(imeWnd, WM_IME_CONTROL,
                             static_cast<WPARAM>(IMC_GETCONVERSIONMODE), 0,
                             SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000, &currentResultPtr)) {
        std::cout << logIndent_ << "├─ [ImeCtrl] SendMessageTimeout failed" << std::endl;
        return false;
    }
    currentResult = static_cast<LRESULT>(currentResultPtr);
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

    DWORD_PTR setResultPtr = 0;
    SendMessageTimeoutA(imeWnd, WM_IME_CONTROL,
                        static_cast<WPARAM>(IMC_SETCONVERSIONMODE),
                        static_cast<LPARAM>(targetConvMode),
                        SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000, &setResultPtr);
    LRESULT setResult = static_cast<LRESULT>(setResultPtr);

    std::cout << logIndent_ << "├─ [ImeCtrl] convMode " << hexStr(convMode)
              << " → " << hexStr(targetConvMode)
              << " setResult=" << setResult << std::endl;

    // TSF may revert the change; wait and recheck
    Sleep(200);

    LRESULT recheck;
    {
        DWORD_PTR recheckPtr = 0;
        if (!SendMessageTimeoutA(imeWnd, WM_IME_CONTROL,
                                 static_cast<WPARAM>(IMC_GETCONVERSIONMODE), 0,
                                 SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000, &recheckPtr)) {
            std::cout << logIndent_ << "├─ [ImeCtrl] Recheck SendMessageTimeout failed" << std::endl;
            return false;
        }
        recheck = static_cast<LRESULT>(recheckPtr);
    }
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

    // SendMessageTimeout avoids deadlocks with unresponsive windows
    const UINT timeoutMs = 2000;

    if (method == SwitchMethod::CtrlSpace) {
        WORD scanCtrl = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        WORD scanSpace = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));

        LPARAM lpCtrlDown  = MAKELPARAM(1, scanCtrl);
        LPARAM lpSpaceDown = MAKELPARAM(1, scanSpace);
        LPARAM lpSpaceUp   = MAKELPARAM(1, scanSpace | 0xC00000);
        LPARAM lpCtrlUp    = MAKELPARAM(1, scanCtrl  | 0xC00000);

        DWORD_PTR result = 0;
        SendMessageTimeoutA(targetWnd, WM_KEYDOWN, VK_CONTROL, lpCtrlDown,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG, timeoutMs, &result);
        SendMessageTimeoutA(targetWnd, WM_KEYDOWN, VK_SPACE,  lpSpaceDown,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG, timeoutMs, &result);
        SendMessageTimeoutA(targetWnd, WM_KEYUP,   VK_SPACE,  lpSpaceUp,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG, timeoutMs, &result);
        SendMessageTimeoutA(targetWnd, WM_KEYUP,   VK_CONTROL, lpCtrlUp,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG, timeoutMs, &result);

        std::cout << logIndent_ << "├─ [MsgKey] Ctrl+Space sent to "
                  << hwndStr(targetWnd) << std::endl;
    } else {
        WORD scanShift = static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));

        LPARAM lpShiftDown = MAKELPARAM(1, scanShift);
        LPARAM lpShiftUp   = MAKELPARAM(1, scanShift | 0xC00000);

        DWORD_PTR result = 0;
        SendMessageTimeoutA(targetWnd, WM_KEYDOWN, VK_SHIFT, lpShiftDown,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG, timeoutMs, &result);
        SendMessageTimeoutA(targetWnd, WM_KEYUP,   VK_SHIFT, lpShiftUp,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG, timeoutMs, &result);

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

    Sleep(50);  // let IME process the key

    if (attached) {
        AttachThreadInput(ourThreadId, targetThreadId, FALSE);
    }
}

bool ImeSwitcher::verifySwitch(HWND hwnd, ImeMode expectedMode) {
    Sleep(50);
    std::string detail;
    ImeMode actualMode = getCurrentMode(hwnd, false, &detail);
    std::cout << logIndent_ << "├─ Verify: " << detail << std::endl;
    return actualMode == expectedMode;
}

bool ImeSwitcher::switchTo(ImeMode targetMode, HWND hwnd, ImeMode currentMode,
                           SwitchMethod method) {
    if (!hwnd) return false;

    std::string savedIndent = logIndent_;
    logIndent_ = " │  ";

    std::lock_guard<std::mutex> lock(switchMutex_);

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

    std::cout << logIndent_ << "ImmSetConversionStatus" << std::endl;
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

    std::cout << logIndent_ << "ImeCtrl (WM_IME_CONTROL)" << std::endl;
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

    std::string step3Name = (method == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "MsgKey " << step3Name << std::endl;
    simulateKeyViaMessage(hwnd, method);
    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }
    std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;

    std::string step4Name = (method == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "KeySim " << step4Name << std::endl;
    simulateKey(hwnd, method);
    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }
    std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;

    SwitchMethod altMethod = (method == SwitchMethod::CtrlSpace) ? SwitchMethod::Shift : SwitchMethod::CtrlSpace;
    std::string step5Name = (altMethod == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "MsgKey " << step5Name << std::endl;
    simulateKeyViaMessage(hwnd, altMethod);
    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }
    std::cout << logIndent_ << "├─ Verify FAILED" << std::endl;

    std::string step6Name = (altMethod == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "Shift";
    std::cout << logIndent_ << "KeySim " << step6Name << std::endl;
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
