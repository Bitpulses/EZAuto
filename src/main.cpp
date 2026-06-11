#include <windows.h>
#include <iostream>
#include <string>
#include <csignal>
#include <mutex>

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
    g_mainThreadId = GetCurrentThreadId();

    std::cout << "========================================" << std::endl;
    std::cout << "  EZAuto v0.1.4 - Auto IME Switcher" << std::endl;
    std::cout << "========================================" << std::endl;

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
            std::cout << "[" << getTimestamp() << "] [Focus] ARTIFACT (PID="
                      << info.processId << " name='" << info.processName
                      << "') filtered" << std::endl;
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

        // Same-app detection: compare rootHwnd only.
        // Must NOT compare processName because multi-process apps (VS Code, Chrome,
        // Windows Terminal) have child processes with different names sharing the same
        // top-level window. Using processName would cause same-window events from
        // different child processes to be treated as "different app", triggering
        // unwanted IME switches that override the user's manual IME choice.
        //
        // Safety: PID=0 / empty-processName artifact events (which previously poisoned
        // lastState.rootHwnd) are now filtered by the check above (FIX 1), so a
        // rootHwnd-only comparison is safe.
        bool sameApp = (rootHwnd != nullptr && rootHwnd == lastState.rootHwnd);

        // ---- Same top-level window: DO NOT touch IME ----
        // User may have manually switched IME, or IME state may be in flux
        // during Chinese text composition. Either way, we must not interfere.
        // This also handles multi-process apps (Windows Terminal, Chrome, etc.)
        // where child processes report different PIDs but share the same root window.
        if (sameApp) {
            std::cout << "[" << getTimestamp() << "] [Focus] " << info.processName
                      << " | Ctrl: " << ctrlType
                      << " | PID: " << info.processId
                      << " | [SAME APP, skip]" << std::endl;
            return;
        }

        // ---- Different top-level window: switch IME ----
        ImeMode targetMode = config.getTargetMode(info.processName, info.isPassword);
        HWND targetHwnd = GetForegroundWindow();
        if (!targetHwnd) return;

        ImeMode currentMode = switcher.getCurrentMode(targetHwnd);

        // Update state BEFORE switching (so that re-entrant events from our
        // own switch operation see sameApp=true and get skipped)
        lastState.rootHwnd = rootHwnd;
        lastState.processName = info.processName;

        std::cout << "[" << getTimestamp() << "] [Focus] " << info.processName
                  << " | Ctrl: " << ctrlType
                  << " | PID: " << info.processId
                  << " | RootHwnd: " << rootHwnd
                  << " | Editable: " << (info.isEditable ? "Y" : "N")
                  << " | Pwd: " << (info.isPassword ? "Y" : "N")
                  << " | IME: " << (currentMode == ImeMode::Chinese ? "CN" : "EN")
                  << " -> " << (targetMode == ImeMode::Chinese ? "CN" : "EN");

        if (currentMode != targetMode) {
            // FIX 3: Pass detected currentMode so switchTo doesn't need to re-detect
            // (eliminates race where GetForegroundWindow() after Sleep(50) may return
            // a different window than the one we intended to switch).
            switcher.switchTo(targetMode, targetHwnd, currentMode, config.getSwitchMethod());
            std::cout << " [SWITCHED]";
        } else {
            std::cout << " [OK]";
        }
        std::cout << std::endl;
    })) {
        std::cerr << "Failed to start focus monitor!" << std::endl;
        CoUninitialize();
        return 1;
    }

    std::cout << std::endl;
    std::cout << "EZAuto is running in background. Press Ctrl+C to exit." << std::endl;
    std::cout << "----------------------------------------" << std::endl;

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
