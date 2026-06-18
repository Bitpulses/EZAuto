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

static std::atomic<bool> g_running{true};
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

static std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &result[0], len, nullptr, nullptr);
    return result;
}

static std::string getExeDirectory() {
    wchar_t wpath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, wpath, MAX_PATH);
    std::wstring dir(wpath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dir = dir.substr(0, pos);
    }
    return wideToUtf8(dir);
}

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

static std::string getWindowClass(HWND hwnd) {
    if (!hwnd) return "";
    wchar_t buf[256] = {};
    int len = GetClassNameW(hwnd, buf, 256);
    if (len == 0) return "";
    return wideToUtf8(std::wstring(buf, len));
}

static std::string hwndStr(HWND hwnd) {
    if (!hwnd) return "null";
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
    return buf;
}

// Use root window handle for stable app identity.
// Process ID is unreliable: multi-process apps like Windows Terminal
// report different PIDs for the same visual window.

struct FocusState {
    HWND rootHwnd = nullptr;
    std::string processName;
};

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_mainThreadId = GetCurrentThreadId();

    std::cout << "EZAuto v0.2.2" << std::endl;

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
        // UIA sometimes fires events with PID=0 or empty processName
        // (e.g. Type_50025 CustomControl) that share the same rootHwnd
        if (info.processId == 0 || info.processName.empty()) {
            std::cout << "[" << getTimestamp() << "] Focus ── ARTIFACT (PID="
                      << info.processId << ") → filtered" << std::endl;
            return;
        }

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

        HWND rootHwnd = nullptr;
        if (info.hwnd) {
            rootHwnd = GetAncestor(info.hwnd, GA_ROOTOWNER);
        }
        if (!rootHwnd) {
            rootHwnd = GetForegroundWindow();
        }

        // Filter system windows that shouldn't trigger switching
        // #32768=popup menu, tooltips, XamlExplorerHostIslandWindow=Alt+Tab, ForegroundStaging=explorer
        std::string rootClass = getWindowClass(rootHwnd);
        if (rootClass == "#32768" || rootClass == "tooltips_class32" ||
            rootClass == "XamlExplorerHostIslandWindow" || rootClass == "ForegroundStaging") {
            std::cout << "[" << getTimestamp() << "] Focus ── " << info.processName
                      << " (PID:" << info.processId << ", " << ctrlType << ")"
                      << " → system UI filtered" << std::endl;
            return;
        }

        bool sameApp;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            sameApp = (rootHwnd != nullptr && rootHwnd == lastState.rootHwnd);
        }

        if (sameApp) {
            std::cout << "[" << getTimestamp() << "] Focus ── " << info.processName
                      << " (PID:" << info.processId << ", " << ctrlType << ")" << std::endl;
            std::cout << " ├─ Hwnd: " << hwndStr(info.hwnd) << " | Root: " << hwndStr(rootHwnd) << std::endl;
            std::cout << " └─ Same app" << std::endl;
            return;
        }

        ImeMode targetMode = config.getTargetMode(info.processName, info.isPassword);
        HWND targetHwnd = GetForegroundWindow();
        if (!targetHwnd) return;

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

        // Win11 restores per-app IME state asynchronously after focus change.
        // Without this delay we may read stale state.
        Sleep(200);

        std::string detectDetail;
        ImeMode currentMode = switcher.getCurrentMode(targetHwnd, false, &detectDetail);
        bool detectionReliable = (detectDetail.find("unreliable") == std::string::npos);

        std::string currentStr = (currentMode == ImeMode::Chinese ? "CN" : "EN");
        std::string targetStr = (targetMode == ImeMode::Chinese ? "CN" : "EN");

        std::cout << "\n[" << getTimestamp() << "] Focus ── " << info.processName
                  << " (PID:" << info.processId << ", " << ctrlType << ")" << std::endl;
        std::cout << " ├─ Hwnd: " << hwndStr(info.hwnd) << " | Root: " << hwndStr(rootHwnd)
                  << " | TID:" << targetTid << std::endl;
        std::cout << " ├─ Class: " << getWindowClass(rootHwnd)
                  << " | Title: \"" << getWindowTitle(rootHwnd) << "\"" << std::endl;
        std::cout << " ├─ Rule: " << ruleSource
                  << " | IME: " << currentStr << " → " << targetStr
                  << " | " << detectDetail << std::endl;

        bool switchSucceeded = false;
        if (currentMode != targetMode) {
            if (!detectionReliable) {
                std::cout << " └─ ⊘ SKIPPED" << std::endl;
            } else {
                bool switched = switcher.switchTo(targetMode, targetHwnd, currentMode, config.getSwitchMethod());
                if (switched) {
                    std::cout << " └─ ✓ SWITCHED" << std::endl;
                } else {
                    std::cout << " └─ ✗ FAILED" << std::endl;
                }
                switchSucceeded = switched;
            }
        } else {
            std::cout << " └─ OK" << std::endl;
            switchSucceeded = true;
        }

        if (switchSucceeded) {
            std::lock_guard<std::mutex> lock(stateMutex);
            lastState.rootHwnd = rootHwnd;
            lastState.processName = info.processName;
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
