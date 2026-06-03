#pragma once

#include <windows.h>
#include <UIAutomation.h>
#include <string>
#include <vector>

// ===================== IME Types =====================

enum class ImeMode {
    Chinese,
    English
};

enum class SwitchMethod {
    Shift,       // Simulate Shift key (most compatible)
    CtrlSpace,   // Simulate Ctrl+Space
    TSF          // Use TSF interface (native, no key simulation)
};

// ===================== Focus Info =====================

struct FocusInfo {
    DWORD processId = 0;
    std::string processName;   // lowercase, e.g. "cmd.exe"
    CONTROLTYPEID controlType = 0;
    bool isPassword = false;
    bool isEditable = false;
    HWND hwnd = nullptr;
    std::vector<int> runtimeId;  // UIA RuntimeId for unique element identification
};
