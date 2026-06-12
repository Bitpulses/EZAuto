#include <EZAuto/ImeSwitcher.h>

#include <imm.h>
#include <msctf.h>
#include <iostream>
#include <algorithm>

#pragma comment(lib, "imm32.lib")

// MinGW's imm.h may not define these WM_IME_CONTROL sub-messages
#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_SETCONVERSIONMODE
#define IMC_SETCONVERSIONMODE  0x0002
#endif
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS      0x0003
#endif
#ifndef IMC_SETOPENSTATUS
#define IMC_SETOPENSTATUS      0x0004
#endif

// Helper: format DWORD as hex string
static std::string hexStr(DWORD val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", val);
    return buf;
}

// Helper: format HWND as hex string
static std::string hwndStr(HWND hwnd) {
    if (!hwnd) return "null";
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%llx", reinterpret_cast<unsigned long long>(hwnd));
    return buf;
}

// Helper: decode convMode flags into readable string
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
    if (!flags.empty()) flags.pop_back();  // Remove trailing '|'
    return flags;
}

ImeSwitcher::ImeSwitcher() = default;
ImeSwitcher::~ImeSwitcher() = default;

DWORD ImeSwitcher::getWindowThreadId(HWND hwnd) {
    return GetWindowThreadProcessId(hwnd, nullptr);
}

HWND ImeSwitcher::getFocusWindowForThread(DWORD threadId) {
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(GUITHREADINFO);
    if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus) {
        return gti.hwndFocus;
    }
    return nullptr;
}

//  Robust HIMC Acquisition 

HIMC ImeSwitcher::acquireImmContext(HWND hwnd, DWORD threadId, HWND& outCtxHwnd) {
    HIMC himc = nullptr;
    outCtxHwnd = nullptr;

    // Strategy 1: Real focused control
    HWND focusHwnd = getFocusWindowForThread(threadId);
    if (focusHwnd) {
        himc = ImmGetContext(focusHwnd);
        if (himc) {
            outCtxHwnd = focusHwnd;
            return himc;
        }
    }

    // Strategy 2: The passed-in window handle
    if (hwnd && hwnd != focusHwnd) {
        himc = ImmGetContext(hwnd);
        if (himc) {
            outCtxHwnd = hwnd;
            return himc;
        }
    }

    // Strategy 3: Default IME window (most reliable for TSF-based IMEs)
    HWND targetForIme = focusHwnd ? focusHwnd : hwnd;
    if (targetForIme) {
        HWND imeWnd = ImmGetDefaultIMEWnd(targetForIme);
        if (imeWnd) {
            himc = ImmGetContext(imeWnd);
            if (himc) {
                outCtxHwnd = imeWnd;
                return himc;
            }
        }
    }

    return nullptr;
}

//  IME-Control Helpers 

HWND ImeSwitcher::getDefaultImeWnd(HWND hwnd) {
    if (!hwnd) return nullptr;

    // Try the window directly
    HWND imeWnd = ImmGetDefaultIMEWnd(hwnd);
    if (imeWnd) return imeWnd;

    // Try the focused control in that thread
    DWORD threadId = getWindowThreadId(hwnd);
    HWND focusHwnd = getFocusWindowForThread(threadId);
    if (focusHwnd && focusHwnd != hwnd) {
        imeWnd = ImmGetDefaultIMEWnd(focusHwnd);
        if (imeWnd) return imeWnd;
    }

    return nullptr;
}

LRESULT ImeSwitcher::imeControlGetConvMode(HWND imeWnd, int maxRetries, int retryDelayMs) {
    if (!imeWnd) return 0;

    for (int i = 0; i < maxRetries; ++i) {
        LRESULT result = SendMessage(imeWnd, WM_IME_CONTROL,
                                     static_cast<WPARAM>(IMC_GETCONVERSIONMODE), 0);
        if (result != 0) {
            return result;
        }
        // IME window might not be fully initialized yet - retry
        if (i < maxRetries - 1) {
            Sleep(retryDelayMs);
        }
    }

    // Returned 0 on all attempts - ambiguous:
    // Could be English mode (convMode=0) or query failure.
    // Use IMC_GETOPENSTATUS to disambiguate.
    LRESULT openStatus = SendMessage(imeWnd, WM_IME_CONTROL,
                                     static_cast<WPARAM>(IMC_GETOPENSTATUS), 0);
    if (openStatus) {
        // IME is open but conversion mode is 0 => genuine English/alpha mode
        std::cout << logIndent_ << "├─ [IME-Control] " << hwndStr(imeWnd) << " convMode=0, IME open → EN" << std::endl;
        return 0;  // 0 is valid: English mode
    }

    // IME appears closed or query completely failed
    std::cout << logIndent_ << "├─ [IME-Control] " << hwndStr(imeWnd) << " both queries returned 0 (failed)" << std::endl;
    return -1;  // Use -1 as sentinel for "query failed"
}

//  Mode Detection 

ImeMode ImeSwitcher::getCurrentMode(HWND hwnd, bool verbose, std::string* detail) {
    if (!hwnd) {
        if (detail) *detail = "no window → EN";
        return ImeMode::English;
    }

    DWORD threadId = getWindowThreadId(hwnd);

    // Step 1: WM_IME_CONTROL via default IME window (MOST RELIABLE)
    HWND imeWnd = getDefaultImeWnd(hwnd);
    if (imeWnd) {
        if (verbose) {
            std::cout << logIndent_ << "├─ [Detect] IME wnd: " << hwndStr(imeWnd)
                      << " | TID:" << threadId << std::endl;
        }

        LRESULT convResult = imeControlGetConvMode(imeWnd, 3, 30);
        if (convResult >= 0) {
            DWORD convMode = static_cast<DWORD>(convResult);
            bool isNative = (convMode & IME_CMODE_NATIVE) != 0;
            if (detail) {
                *detail = "NATIVE=" + std::to_string(isNative) +
                          " convMode=" + hexStr(convMode) +
                          " [" + convModeDesc(convMode) + "]" +
                          " → " + (isNative ? "CN" : "EN");
            }
            if (verbose) {
                std::cout << logIndent_ << "├─ [Detect] NATIVE=" << isNative
                          << " convMode=" << hexStr(convMode)
                          << " [" << convModeDesc(convMode) << "]"
                          << " → " << (isNative ? "CN" : "EN") << std::endl;
            }
            return isNative ? ImeMode::Chinese : ImeMode::English;
        }
        // convResult == -1 means query failed, fall through
    } else {
        if (verbose) {
            std::cout << logIndent_ << "├─ [Detect] No default IME window | TID:" << threadId << std::endl;
        }
    }

    // Step 2: Try ImmGetConversionStatus via HIMC (works for Win32 apps)
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
                      " sentMode=" + hexStr(sentMode) +
                      " → " + (isNative ? "CN" : "EN");
        }
        if (verbose) {
            std::cout << logIndent_ << "├─ [Detect] IMM via " << hwndStr(ctxHwnd)
                      << ": NATIVE=" << isNative
                      << " convMode=" << hexStr(convMode)
                      << " [" << convModeDesc(convMode) << "]"
                      << " sentMode=" << hexStr(sentMode)
                      << " → " << (isNative ? "CN" : "EN") << std::endl;
        }
        return isNative ? ImeMode::Chinese : ImeMode::English;
    }

    // Step 3: GetKeyboardLayout (UNRELIABLE for Microsoft Pinyin!)
    HKL hkl = GetKeyboardLayout(threadId);
    LANGID langId = LOWORD(reinterpret_cast<UINT_PTR>(hkl));

    if (verbose) {
        std::cout << logIndent_ << "├─ [Detect] Layout HKL=0x" << std::hex << reinterpret_cast<UINT_PTR>(hkl)
                  << " Lang=0x" << langId << std::dec << std::endl;
    }

    if (PRIMARYLANGID(langId) == LANG_CHINESE) {
        if (detail) {
            *detail = "Layout=Chinese → CN (unreliable)";
        }
        if (verbose) {
            std::cout << logIndent_ << "├─ [Detect] → CN (Chinese layout)" << std::endl;
        }
        return ImeMode::Chinese;
    }

    // English layout - unreliable for Pinyin
    if (detail) {
        *detail = "Layout=English → EN (unreliable)";
    }
    if (verbose) {
        std::cout << logIndent_ << "├─ [Detect] → EN (English layout, unreliable)" << std::endl;
    }
    return ImeMode::English;
}

ImeMode ImeSwitcher::getCurrentModeViaTsf() {
    ITfInputProcessorProfileMgr* pMgr = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr,
        reinterpret_cast<void**>(&pMgr));

    if (FAILED(hr) || !pMgr) {
        std::cout << logIndent_ << "├─ [TSF] CreateInstance failed: 0x" << std::hex << hr << std::dec << std::endl;
        return ImeMode::Chinese;
    }

    TF_INPUTPROCESSORPROFILE profile = {};
    hr = pMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile);
    pMgr->Release();

    if (FAILED(hr)) {
        std::cout << logIndent_ << "├─ [TSF] GetActiveProfile failed: 0x" << std::hex << hr << std::dec << std::endl;
        return ImeMode::Chinese;
    }

    std::cout << logIndent_ << "├─ [TSF] Type=" << profile.dwProfileType
              << " HKL=0x" << std::hex << reinterpret_cast<UINT_PTR>(profile.hkl)
              << " Lang=0x" << profile.langid << std::dec << std::endl;

    if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
        std::cout << logIndent_ << "├─ [TSF] → CN (input processor)" << std::endl;
        return ImeMode::Chinese;
    }

    if (profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT) {
        std::cout << logIndent_ << "├─ [TSF] → EN (keyboard layout)" << std::endl;
        return ImeMode::English;
    }

    std::cout << logIndent_ << "├─ [TSF] → CN (unknown profile type)" << std::endl;
    return ImeMode::Chinese;
}

//  Switch Verification 
//
// Prints Detect line using logIndent_, but does NOT print Verify line.
// The caller (switchTo) prints Verify with └─ (success) or ├─ (failure).

bool ImeSwitcher::verifySwitch(HWND hwnd, ImeMode expectedMode) {
    Sleep(30);  // Let IME process the key simulation

    std::string detail;
    ImeMode actualMode = getCurrentMode(hwnd, false, &detail);
    std::cout << logIndent_ << "├─ Detect: " << detail << std::endl;
    return actualMode == expectedMode;
}

//  Switching Methods 

void ImeSwitcher::simulateCtrlSpace(HWND targetHwnd) {
    DWORD targetThreadId = GetWindowThreadProcessId(targetHwnd, nullptr);
    DWORD ourThreadId = GetCurrentThreadId();

    bool attached = false;
    if (targetThreadId != ourThreadId) {
        attached = AttachThreadInput(ourThreadId, targetThreadId, TRUE);
        std::cout << logIndent_ << "├─ [KeySim] AttachThreadInput → "
                  << (attached ? "OK" : "FAILED") << std::endl;
    }

    if (attached) Sleep(5);

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

    if (attached) {
        AttachThreadInput(ourThreadId, targetThreadId, FALSE);
    }
}

void ImeSwitcher::simulateShiftKey(HWND targetHwnd) {
    DWORD targetThreadId = GetWindowThreadProcessId(targetHwnd, nullptr);
    DWORD ourThreadId = GetCurrentThreadId();

    bool attached = false;
    if (targetThreadId != ourThreadId) {
        attached = AttachThreadInput(ourThreadId, targetThreadId, TRUE);
        std::cout << logIndent_ << "├─ [KeySim] AttachThreadInput → "
                  << (attached ? "OK" : "FAILED") << std::endl;
    }

    if (attached) Sleep(5);

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

    if (attached) {
        AttachThreadInput(ourThreadId, targetThreadId, FALSE);
    }
}

//  Switching Logic 

bool ImeSwitcher::switchTo(ImeMode targetMode, HWND hwnd, ImeMode currentMode,
                           SwitchMethod method) {
    if (!hwnd) return false;

    // Debounce: avoid switching too rapidly
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSwitchTime_).count();
    if (elapsed < DEBOUNCE_MS && lastTargetMode_ == targetMode) {
        std::cout << logIndent_ << "└─ Debounced" << std::endl;
        return true;
    }

    if (currentMode == targetMode) {
        std::cout << logIndent_ << "└─ Already in target mode" << std::endl;
        return true;
    }

    // Set indent for sub-tree output
    std::string savedIndent = logIndent_;
    logIndent_ = " │  ";

    std::string methodStr = (method == SwitchMethod::Shift) ? "Shift" :
                            (method == SwitchMethod::CtrlSpace) ? "Ctrl+Space" : "TSF";
    std::cout << logIndent_ << "Target: " << hwndStr(hwnd)
              << " | Method: " << methodStr << std::endl;

    // Step 1: Keyboard simulation with AttachThreadInput
    std::cout << logIndent_ << "Step 1: KeySim (AttachThreadInput)" << std::endl;
    switch (method) {
        case SwitchMethod::Shift:
            simulateShiftKey(hwnd);
            break;
        case SwitchMethod::CtrlSpace:
            simulateCtrlSpace(hwnd);
            break;
        case SwitchMethod::TSF:
            simulateShiftKey(hwnd);
            break;
    }

    if (verifySwitch(hwnd, targetMode)) {
        std::cout << logIndent_ << "└─ Verify: OK" << std::endl;
        logIndent_ = savedIndent;
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return true;
    }
    std::cout << logIndent_ << "├─ Verify: FAILED" << std::endl;

    // Step 2: Fallback - keyboard simulation WITHOUT AttachThreadInput
    std::cout << logIndent_ << "Step 2: KeySim (raw)" << std::endl;
    switch (method) {
        case SwitchMethod::Shift:
            simulateShiftKeyRaw();
            break;
        case SwitchMethod::CtrlSpace:
            simulateCtrlSpaceRaw();
            break;
        case SwitchMethod::TSF:
            simulateShiftKeyRaw();
            break;
    }

    bool ok = verifySwitch(hwnd, targetMode);
    if (ok) {
        std::cout << logIndent_ << "└─ Verify: OK" << std::endl;
    } else {
        std::cout << logIndent_ << "└─ Verify: FAILED" << std::endl;
    }

    logIndent_ = savedIndent;
    lastSwitchTime_ = std::chrono::steady_clock::now();
    lastTargetMode_ = targetMode;
    return ok;
}

//  Raw keyboard simulation (no AttachThreadInput) 

void ImeSwitcher::simulateCtrlSpaceRaw() {
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_SPACE;
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = VK_SPACE;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(4, inputs, sizeof(INPUT));
    std::cout << logIndent_ << "├─ [KeySim] Ctrl+Space sent=" << sent << "/4" << std::endl;
}

void ImeSwitcher::simulateShiftKeyRaw() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_SHIFT;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_SHIFT;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(2, inputs, sizeof(INPUT));
    std::cout << logIndent_ << "├─ [KeySim] Shift sent=" << sent << "/2" << std::endl;
}
