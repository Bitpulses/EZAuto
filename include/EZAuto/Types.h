#pragma once

#include <windows.h>
#include <UIAutomation.h>
#include <string>
#include <vector>

enum class ImeMode {
    Chinese,
    English
};

enum class SwitchMethod {
    Shift,
    CtrlSpace,
    TSF
};

struct FocusInfo {
    DWORD processId = 0;
    std::string processName;
    CONTROLTYPEID controlType = 0;
    bool isPassword = false;
    HWND hwnd = nullptr;
    std::vector<int> runtimeId;
};
