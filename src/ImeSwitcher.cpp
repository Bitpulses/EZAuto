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

// ===================== Robust HIMC Acquisition =====================

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

// ===================== IME-Control Helpers =====================
//
// WM_IME_CONTROL via the default IME window is the MOST RELIABLE way
// to detect and switch IME mode for modern TSF-based apps (Chrome,
// VS Code, Windows Terminal). These apps don't expose HIMC, so IMM
// APIs don't work. But WM_IME_CONTROL is handled by the IME window
// which is managed by TSF.
//
// IMPORTANT: Microsoft Pinyin in Windows 10/11 uses a SINGLE layout
// (e.g., 0x4090409 English) for both CN and EN mode. The CN/EN state
// is tracked by the IME's internal conversion mode (IME_CMODE_NATIVE
// bit), NOT by the keyboard layout. So GetKeyboardLayout is USELESS
// for detecting CN/EN mode with Microsoft Pinyin.

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
        std::cout << "  [IME-Control] GETCONVERSIONMODE=0 but IME is open => EN mode" << std::endl;
        return 0;  // 0 is valid: English mode
    }

    // IME appears closed or query completely failed
    std::cout << "  [IME-Control] Both GETCONVERSIONMODE and GETOPENSTATUS returned 0 (query failed)" << std::endl;
    return -1;  // Use -1 as sentinel for "query failed"
}

// ===================== Mode Detection =====================
//
// Detection priority (CHANGED from previous version):
// 1. WM_IME_CONTROL / IMC_GETCONVERSIONMODE via default IME window
//    - Works for TSF-based apps (Chrome, VS Code, Windows Terminal)
//    - Correctly reads IME internal mode regardless of keyboard layout
// 2. ImmGetConversionStatus via HIMC
//    - Works for traditional Win32 apps
// 3. GetKeyboardLayout (LEAST RELIABLE for Microsoft Pinyin)
//    - Microsoft Pinyin uses same layout for CN and EN mode
//    - Only useful as rough hint
// 4. TSF GetActiveProfile (NOT per-thread, unreliable)

ImeMode ImeSwitcher::getCurrentMode(HWND hwnd) {
    if (!hwnd) return ImeMode::English;

    DWORD threadId = getWindowThreadId(hwnd);

    std::cout << "  [Detect] ---- Begin ----" << std::endl;

    // Step 1: WM_IME_CONTROL via default IME window (MOST RELIABLE)
    // This works even for TSF-based modern apps and correctly reads
    // the IME's internal CN/EN mode regardless of keyboard layout.
    HWND imeWnd = getDefaultImeWnd(hwnd);
    if (imeWnd) {
        std::cout << "  [Detect] IME window found: 0x" << std::hex
                  << reinterpret_cast<UINT_PTR>(imeWnd) << std::dec << std::endl;

        LRESULT convResult = imeControlGetConvMode(imeWnd, 3, 30);
        if (convResult >= 0) {
            // Valid result (0 = English mode, >0 = check NATIVE bit)
            DWORD convMode = static_cast<DWORD>(convResult);
            bool isNative = (convMode & IME_CMODE_NATIVE) != 0;
            std::cout << "  [Detect] IME-Control: NATIVE=" << isNative
                      << " convMode=0x" << std::hex << convMode << std::dec
                      << " -> " << (isNative ? "CN" : "EN") << std::endl;
            return isNative ? ImeMode::Chinese : ImeMode::English;
        }
        // convResult == -1 means query failed, fall through
    } else {
        std::cout << "  [Detect] No default IME window" << std::endl;
    }

    // Step 2: Try ImmGetConversionStatus via HIMC (works for Win32 apps)
    HWND ctxHwnd = nullptr;
    HIMC himc = acquireImmContext(hwnd, threadId, ctxHwnd);
    if (himc) {
        DWORD convMode = 0, sentMode = 0;
        ImmGetConversionStatus(himc, &convMode, &sentMode);
        ImmReleaseContext(ctxHwnd, himc);

        bool isNative = (convMode & IME_CMODE_NATIVE) != 0;
        std::cout << "  [Detect] IMM: NATIVE=" << isNative
                  << " convMode=0x" << std::hex << convMode << std::dec
                  << " -> " << (isNative ? "CN" : "EN") << std::endl;
        return isNative ? ImeMode::Chinese : ImeMode::English;
    }

    // Step 3: GetKeyboardLayout (UNRELIABLE for Microsoft Pinyin!)
    // Microsoft Pinyin uses a single layout for both CN and EN mode.
    // An English layout does NOT mean the IME is in English mode.
    HKL hkl = GetKeyboardLayout(threadId);
    LANGID langId = LOWORD(reinterpret_cast<UINT_PTR>(hkl));

    std::cout << "  [Detect] Layout HKL=0x" << std::hex << reinterpret_cast<UINT_PTR>(hkl)
              << " Lang=0x" << langId << std::dec << std::endl;

    if (PRIMARYLANGID(langId) == LANG_CHINESE) {
        // Chinese layout - IME is active, likely CN mode
        std::cout << "  [Detect] -> CN (Chinese layout, likely CN)" << std::endl;
        return ImeMode::Chinese;
    }

    // English layout - we CANNOT determine the IME mode from layout alone.
    // Microsoft Pinyin can be in CN mode with English layout.
    // Without IME-Control or HIMC, we have to guess.
    // Default to the configured default mode as a hint.
    std::cout << "  [Detect] -> EN (English layout, unreliable for Pinyin)" << std::endl;
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
        std::cout << "  [TSF] CreateInstance failed: 0x" << std::hex << hr << std::dec << std::endl;
        return ImeMode::Chinese;
    }

    TF_INPUTPROCESSORPROFILE profile = {};
    hr = pMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile);
    pMgr->Release();

    if (FAILED(hr)) {
        std::cout << "  [TSF] GetActiveProfile failed: 0x" << std::hex << hr << std::dec << std::endl;
        return ImeMode::Chinese;
    }

    std::cout << "  [TSF] Type=" << profile.dwProfileType
              << " HKL=0x" << std::hex << reinterpret_cast<UINT_PTR>(profile.hkl)
              << " Lang=0x" << profile.langid << std::dec << std::endl;

    if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
        std::cout << "  [TSF] -> CN (input processor)" << std::endl;
        return ImeMode::Chinese;
    }

    if (profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT) {
        std::cout << "  [TSF] -> EN (keyboard layout)" << std::endl;
        return ImeMode::English;
    }

    std::cout << "  [TSF] -> CN (unknown profile type)" << std::endl;
    return ImeMode::Chinese;
}

// ===================== Switch Verification =====================

bool ImeSwitcher::verifySwitch(HWND hwnd, ImeMode expectedMode) {
    Sleep(100);  // Longer delay to let IME state settle

    ImeMode actualMode = getCurrentMode(hwnd);
    if (actualMode == expectedMode) {
        std::cout << "  [Verify] OK - mode is "
                  << (expectedMode == ImeMode::Chinese ? "CN" : "EN") << std::endl;
        return true;
    }

    std::cout << "  [Verify] FAILED - expected "
              << (expectedMode == ImeMode::Chinese ? "CN" : "EN")
              << " but got " << (actualMode == ImeMode::Chinese ? "CN" : "EN") << std::endl;
    return false;
}

// ===================== Switching Methods =====================

bool ImeSwitcher::switchViaImeControl(HWND hwnd, ImeMode targetMode) {
    if (!hwnd) return false;

    HWND imeWnd = getDefaultImeWnd(hwnd);
    if (!imeWnd) {
        std::cout << "  [IME-Control] No default IME window" << std::endl;
        return false;
    }

    bool wantNative = (targetMode == ImeMode::Chinese);

    // Try to read current conversion mode (with retry)
    LRESULT convResult = imeControlGetConvMode(imeWnd, 3, 20);
    DWORD convMode = 0;

    if (convResult >= 0) {
        // Successfully read current mode
        convMode = static_cast<DWORD>(convResult);
        bool isNative = (convMode & IME_CMODE_NATIVE) != 0;

        std::cout << "  [IME-Control] Current NATIVE=" << isNative
                  << " want=" << wantNative
                  << " convMode=0x" << std::hex << convMode << std::dec << std::endl;

        if (isNative == wantNative) {
            std::cout << "  [IME-Control] Already in target mode" << std::endl;
            return true;
        }

        // Modify NATIVE bit while preserving other flags
        if (wantNative) {
            convMode |= IME_CMODE_NATIVE;
        } else {
            convMode &= ~IME_CMODE_NATIVE;
        }
    } else {
        // Could not read current mode - set a reasonable default
        // For CN: NATIVE bit set; For EN: just alphanumeric (0)
        convMode = wantNative ? IME_CMODE_NATIVE : 0;
        std::cout << "  [IME-Control] Cannot read current mode, setting convMode=0x"
                  << std::hex << convMode << std::dec << std::endl;
    }

    // Set the new conversion mode
    LRESULT setResult = SendMessage(imeWnd, WM_IME_CONTROL,
                                    static_cast<WPARAM>(IMC_SETCONVERSIONMODE), convMode);

    std::cout << "  [IME-Control] SETCONVERSIONMODE(0x" << std::hex << convMode << std::dec
              << ") returned " << setResult << std::endl;

    // The return value should be the previous conversion mode
    // Non-zero usually means success (previous mode was non-zero)
    // Zero could mean: previous mode was 0 (English) OR failure
    // We'll verify later, so just return true to indicate we tried
    return true;
}

bool ImeSwitcher::switchViaImm(HWND hwnd, ImeMode targetMode) {
    if (!hwnd) return false;

    DWORD threadId = getWindowThreadId(hwnd);
    HWND ctxHwnd = nullptr;
    HIMC himc = acquireImmContext(hwnd, threadId, ctxHwnd);
    if (!himc) {
        std::cout << "  [IMM Switch] No HIMC available" << std::endl;
        return false;
    }

    DWORD convMode = 0, sentMode = 0;
    ImmGetConversionStatus(himc, &convMode, &sentMode);

    bool isNative = (convMode & IME_CMODE_NATIVE) != 0;
    bool wantNative = (targetMode == ImeMode::Chinese);

    std::cout << "  [IMM Switch] NATIVE=" << isNative
              << " want=" << wantNative
              << " convMode=0x" << std::hex << convMode << std::dec << std::endl;

    if (isNative == wantNative) {
        ImmReleaseContext(ctxHwnd, himc);
        return true;
    }

    if (wantNative) {
        convMode |= IME_CMODE_NATIVE;
    } else {
        convMode &= ~IME_CMODE_NATIVE;
    }

    BOOL result = ImmSetConversionStatus(himc, convMode, sentMode);
    ImmReleaseContext(ctxHwnd, himc);

    std::cout << "  [IMM Switch] ImmSetConversionStatus -> "
              << (result ? "OK" : "FAILED") << std::endl;

    return (result == TRUE);
}

void ImeSwitcher::simulateCtrlSpace(HWND targetHwnd) {
    DWORD targetThreadId = GetWindowThreadProcessId(targetHwnd, nullptr);
    DWORD ourThreadId = GetCurrentThreadId();

    bool attached = false;
    if (targetThreadId != ourThreadId) {
        attached = AttachThreadInput(ourThreadId, targetThreadId, TRUE);
        std::cout << "  [KeySim] AttachThreadInput -> "
                  << (attached ? "OK" : "FAILED") << std::endl;
    }

    if (attached) Sleep(20);

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
    std::cout << "  [KeySim] Ctrl+Space sent=" << sent << "/4" << std::endl;

    Sleep(80);

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
        std::cout << "  [KeySim] AttachThreadInput -> "
                  << (attached ? "OK" : "FAILED") << std::endl;
    }

    if (attached) Sleep(20);

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_SHIFT;
    inputs[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_SHIFT;
    inputs[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    UINT sent = SendInput(2, inputs, sizeof(INPUT));
    std::cout << "  [KeySim] Shift sent=" << sent << "/2" << std::endl;

    Sleep(80);

    if (attached) {
        AttachThreadInput(ourThreadId, targetThreadId, FALSE);
    }
}

// ===================== Switching Logic =====================
//
// Strategy: Stay on the current keyboard layout and toggle the IME's
// internal CN/EN mode via the conversion mode (IME_CMODE_NATIVE bit).
//
// Order of methods:
// 1. WM_IME_CONTROL via default IME window - works for TSF apps (Chrome, VS Code, Terminal)
// 2. IMM ImmSetConversionStatus - works for Win32 apps with HIMC
// 3. AttachThreadInput + SendInput (Ctrl+Space/Shift) - keyboard simulation
// 4. Raw SendInput (no attach) - last resort

void ImeSwitcher::switchTo(ImeMode targetMode, HWND hwnd, ImeMode currentMode,
                           SwitchMethod method) {
    if (!hwnd) return;

    // Debounce: avoid switching too rapidly
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSwitchTime_).count();
    if (elapsed < DEBOUNCE_MS && lastTargetMode_ == targetMode) {
        return;
    }

    // Delay to let focus and IME window settle
    Sleep(50);

    // FIX 3: Use the passed-in hwnd directly instead of calling GetForegroundWindow().
    // This eliminates a race condition where GetForegroundWindow() may return a
    // different window than the one we intended to switch after the 50ms delay.
    // Also use the pre-detected currentMode instead of re-querying.

    if (currentMode == targetMode) {
        std::cout << "  [Switch] Already in target mode, skip" << std::endl;
        return;
    }

    std::cout << "  [Switch] " << (currentMode == ImeMode::Chinese ? "CN" : "EN")
              << " -> " << (targetMode == ImeMode::Chinese ? "CN" : "EN") << std::endl;

    // Step 1: Try WM_IME_CONTROL (most universal, works for TSF apps)
    std::cout << "  [Switch] Trying IME-Control" << std::endl;
    if (switchViaImeControl(hwnd, targetMode)) {
        if (verifySwitch(hwnd, targetMode)) {
            lastSwitchTime_ = std::chrono::steady_clock::now();
            lastTargetMode_ = targetMode;
            return;
        }
        std::cout << "  [Switch] IME-Control set but verification failed" << std::endl;
    }

    // Step 2: Try IMM API (works for Win32 apps with HIMC)
    std::cout << "  [Switch] Trying IMM" << std::endl;
    if (switchViaImm(hwnd, targetMode)) {
        if (verifySwitch(hwnd, targetMode)) {
            lastSwitchTime_ = std::chrono::steady_clock::now();
            lastTargetMode_ = targetMode;
            return;
        }
        std::cout << "  [Switch] IMM succeeded but verification failed" << std::endl;
    }

    // Step 3: Keyboard simulation with AttachThreadInput
    std::cout << "  [Switch] Trying keyboard simulation with AttachThreadInput" << std::endl;
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
        lastSwitchTime_ = std::chrono::steady_clock::now();
        lastTargetMode_ = targetMode;
        return;
    }

    // Step 4: Last resort - keyboard simulation WITHOUT AttachThreadInput
    std::cout << "  [Switch] Trying keyboard simulation without attach" << std::endl;
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

    verifySwitch(hwnd, targetMode);

    lastSwitchTime_ = std::chrono::steady_clock::now();
    lastTargetMode_ = targetMode;
}

// ===================== Raw keyboard simulation (no AttachThreadInput) =====================

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
    std::cout << "  [KeySim-Raw] Ctrl+Space sent=" << sent << "/4" << std::endl;
}

void ImeSwitcher::simulateShiftKeyRaw() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_SHIFT;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_SHIFT;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(2, inputs, sizeof(INPUT));
    std::cout << "  [KeySim-Raw] Shift sent=" << sent << "/2" << std::endl;
}
