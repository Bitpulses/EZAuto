#include <windows.h>
#include <iostream>
#include <string>
#include <csignal>
#include <mutex>
#include <algorithm>

#include <EZAuto/ConfigManager.h>
#include <EZAuto/FocusMonitor.h>
#include <EZAuto/ImeSwitcher.h>
#include <EZAuto/Types.h>

static volatile bool g_running = true;
static DWORD g_mainThreadId = 0;

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        g_running = false;
        PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0);
        return TRUE;
    }
    return FALSE;
}

static std::string getTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

static std::string getExeDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string dir(path);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) {
        dir = dir.substr(0, pos);
    }
    return dir;
}

// Helper: convert wide string to UTF-8
static std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &result[0], len, nullptr, nullptr);
    return result;
}

// Helper: get window title (truncated for logging, UTF-8)
static std::string getWindowTitle(HWND hwnd) {
    if (!hwnd) return "";
    wchar_t buf[256] = {};
    int len = GetWindowTextW(hwnd, buf, 256);
    if (len == 0) return "";
    std::string title = wideToUtf8(std::wstring(buf, len));
    if (title.length() > 60) {
        title = title.substr(0, 57) + "...";
    }
    return title;
}

// Helper: get window class name (UTF-8)
static std::string getWindowClass(HWND hwnd) {
    if (!hwnd) return "";
    wchar_t buf[256] = {};
    int len = GetClassNameW(hwnd, buf, 256);
    if (len == 0) return "";
    return wideToUtf8(std::wstring(buf, len));
}

// Helper: format HWND as hex string
static std::string hwndStr(HWND hwnd) {
    if (!hwnd) return "null";
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%llx", reinterpret_cast<unsigned long long>(hwnd));
    return buf;
}

// ===================== Focus State Tracker =====================
//
// CORE PRINCIPLE: Only switch IME when the user moves to a DIFFERENT
// top-level window (i.e., a different application). Within the same
// top-level window, never touch the IME again after the initial switch.
//
// WHY top-level window instead of process ID?
// - Multi-process apps like Windows Terminal have a main process
//   (windowsterminal.exe) and child processes (openconsole.exe, cmd.exe).
//   UIA focus events may report different PIDs within the same app,
//   causing false "app switch" detection with process-level tracking.
// - Using GetAncestor(GA_ROOTOWNER) gives us the actual top-level owner window,
//   which is stable regardless of which child process has focus.
// - This also correctly handles Chrome, VS Code, and other apps that
//   use utility processes for different UI elements.

struct FocusState {
    HWND rootHwnd = nullptr;       // Top-level window handle (stable identity)
    std::string processName;       // For logging only
};

int main() {
    // Set console to UTF-8 mode so Unicode box-drawing characters display correctly
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_mainThreadId = GetCurrentThreadId();

    std::cout << "===================================================" << std::endl;
    std::cout << "        EZAuto v0.2.0 - Auto IME Switcher" << std::endl;
    std::cout << "===================================================" << std::endl;

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    ConfigManager config;
    std::string configPath = getExeDirectory() + "\\config.json";
    if (config.load(configPath)) {
        std::cout << "Config loaded from: " << configPath << std::endl;
    } else {
        std::cout << "Using default configuration (config.json not found)" << std::endl;
    }
    std::cout << "Default mode: " << (config.getDefaultMode() == ImeMode::Chinese ? "Chinese" : "English") << std::endl;
    std::cout << "Switch method: " << (config.getSwitchMethod() == SwitchMethod::Shift ? "Shift" :
                              config.getSwitchMethod() == SwitchMethod::CtrlSpace ? "Ctrl+Space" : "TSF") << std::endl;
    std::cout << "Rules count: " << config.getRules().size() << std::endl;

    ImeSwitcher switcher;

    FocusState lastState;
    std::mutex stateMutex;

    FocusMonitor monitor;
    if (!monitor.start([&config, &switcher, &lastState, &stateMutex](const FocusInfo& info) {
        std::lock_guard<std::mutex> lock(stateMutex);

        // FIX 1: Filter UIA intermediate/artifact events.
        // UIA sometimes fires transient focus events with PID=0 or empty processName
        // (e.g., Type_50025 CustomControl) that share the same rootHwnd as the real
        // target app. If we let these through, they update lastState and cause the
        // subsequent real event to be skipped as "[SAME APP, skip]".
        if (info.processId == 0 || info.processName.empty()) {
            std::cout << "[" << getTimestamp() << "] Focus ── ARTIFACT (PID="
                      << info.processId << ") → filtered" << std::endl;
            return;
        }

        // Build control type name for logging
        std::string ctrlType;
        switch (info.controlType) {
            case UIA_EditControlTypeId:    ctrlType = "Edit"; break;
            case UIA_DocumentControlTypeId: ctrlType = "Document"; break;
            case UIA_TextControlTypeId:    ctrlType = "Text"; break;
            case UIA_ButtonControlTypeId:  ctrlType = "Button"; break;
            case UIA_ListControlTypeId:    ctrlType = "List"; break;
            case UIA_TreeControlTypeId:    ctrlType = "Tree"; break;
            case UIA_MenuControlTypeId:    ctrlType = "Menu"; break;
            case UIA_TabControlTypeId:     ctrlType = "Tab"; break;
            case UIA_PaneControlTypeId:    ctrlType = "Pane"; break;
            case UIA_WindowControlTypeId:  ctrlType = "Window"; break;
            case UIA_DataItemControlTypeId: ctrlType = "DataItem"; break;
            case UIA_TableControlTypeId:   ctrlType = "Table"; break;
            default: ctrlType = "Type_" + std::to_string(info.controlType); break;
        }

        // Get the top-level window (root owner) for stable app identity
        HWND rootHwnd = nullptr;
        if (info.hwnd) {
            rootHwnd = GetAncestor(info.hwnd, GA_ROOTOWNER);
        }
        if (!rootHwnd) {
            rootHwnd = GetForegroundWindow();
        }

        // Filter transient/system windows that should not trigger IME switching:
        // - #32768: popup menus (right-click menus)
        // - tooltips_class32: tooltips
        // - XamlExplorerHostIslandWindow: Alt+Tab task switcher (title "任务切换")
        // - ForegroundStaging: explorer transient staging window
        std::string rootClass = getWindowClass(rootHwnd);
        if (rootClass == "#32768" || rootClass == "tooltips_class32" ||
            rootClass == "XamlExplorerHostIslandWindow" || rootClass == "ForegroundStaging") {
            std::cout << "[" << getTimestamp() << "] Focus ── " << info.processName
                      << " (PID:" << info.processId << ", " << ctrlType << ")"
                      << " → system UI filtered" << std::endl;
            return;
        }

        // Same-app detection: compare rootHwnd only.
        bool sameApp = (rootHwnd != nullptr && rootHwnd == lastState.rootHwnd);

        if (sameApp) {
            std::cout << "[" << getTimestamp() << "] Focus ── " << info.processName
                      << " (PID:" << info.processId << ", " << ctrlType << ")" << std::endl;
            std::cout << " ├─ Hwnd: " << hwndStr(info.hwnd) << " | Root: " << hwndStr(rootHwnd) << std::endl;
            std::cout << " └─ Same app" << std::endl;
            return;
        }

        // Different top-level window: switch IME
        ImeMode targetMode = config.getTargetMode(info.processName, info.isPassword);
        HWND targetHwnd = GetForegroundWindow();
        if (!targetHwnd) return;

        // Determine rule source for logging
        std::string ruleSource;
        if (info.isPassword) {
            ruleSource = "password → EN";
        } else {
            std::string lowerName = info.processName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto& rules = config.getRules();
            auto it = rules.find(lowerName);
            if (it != rules.end()) {
                ruleSource = "rule:" + lowerName + " → " + (it->second == ImeMode::Chinese ? "CN" : "EN");
            } else {
                ruleSource = "default → " + std::string(config.getDefaultMode() == ImeMode::Chinese ? "CN" : "EN");
            }
        }

        DWORD targetTid = GetWindowThreadProcessId(targetHwnd, nullptr);

        ImeMode currentMode = switcher.getCurrentMode(targetHwnd, false);

        // Update state BEFORE switching
        lastState.rootHwnd = rootHwnd;
        lastState.processName = info.processName;

        std::string currentStr = (currentMode == ImeMode::Chinese ? "CN" : "EN");
        std::string targetStr = (targetMode == ImeMode::Chinese ? "CN" : "EN");

        // Tree-style output with detailed info
        std::cout << "\n[" << getTimestamp() << "] Focus ── " << info.processName
                  << " (PID:" << info.processId << ", " << ctrlType << ")" << std::endl;
        std::cout << " ├─ Hwnd: " << hwndStr(info.hwnd) << " | Root: " << hwndStr(rootHwnd)
                  << " | TID:" << targetTid << std::endl;
        std::cout << " ├─ Class: " << getWindowClass(rootHwnd)
                  << " | Title: \"" << getWindowTitle(rootHwnd) << "\"" << std::endl;
        std::cout << " ├─ Rule: " << ruleSource
                  << " | IME: " << currentStr << " → " << targetStr << std::endl;

        if (currentMode != targetMode) {
            std::cout << " ├─ Switch: " << currentStr << " → " << targetStr << std::endl;
            // FIX 3: Pass detected currentMode so switchTo doesn't need to re-detect
            // (eliminates race where GetForegroundWindow() after Sleep(50) may return
            // a different window than the one we intended to switch).
            bool switched = switcher.switchTo(targetMode, targetHwnd, currentMode, config.getSwitchMethod());
            if (switched) {
                std::cout << " └─ ✓ SWITCHED" << std::endl;
            } else {
                std::cout << " └─ ✗ FAILED" << std::endl;
            }
        } else {
            std::cout << " └─ OK" << std::endl;
        }
    })) {
        std::cerr << "Failed to start focus monitor!" << std::endl;
        CoUninitialize();
        return 1;
    }

    std::cout << std::endl;
    std::cout << "EZAuto is running in background. Press Ctrl+C to exit." << std::endl;
    std::cout << "---------------------------------------------------" << std::endl;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    monitor.stop();
    CoUninitialize();

    std::cout << "EZAuto stopped." << std::endl;
    return 0;
}
