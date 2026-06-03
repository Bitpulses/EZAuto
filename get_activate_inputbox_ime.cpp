#include <windows.h>
#include <iostream>
#include <tlhelp32.h>
#include <string>
#include <algorithm>

// Retrieves the process executable name and forces it to lowercase
std::string GetProcessNameLower(DWORD processID) {
    std::string processName = "unknown";
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnapshot, &pe32)) {
            do {
                if (pe32.th32ProcessID == processID) {
                    processName = pe32.szExeFile;
                    // Force the string to lowercase to ensure reliable evaluation
                    std::transform(processName.begin(), processName.end(), processName.begin(), ::tolower);
                    break;
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }
    return processName;
}

void AnalyzeInputMethodPreference() {
    HWND hwndForeground = GetForegroundWindow();
    if (!hwndForeground) return;

    DWORD processId = 0;
    DWORD threadId = GetWindowThreadProcessId(hwndForeground, &processId);
    if (!threadId) return;

    GUITHREADINFO guiInfo;
    guiInfo.cbSize = sizeof(GUITHREADINFO);
    
    if (GetGUIThreadInfo(threadId, &guiInfo)) {
        HWND hwndFocus = guiInfo.hwndFocus ? guiInfo.hwndFocus : hwndForeground;

        char className[256] = {0};
        GetClassNameA(hwndFocus, className, sizeof(className));
        std::string strClass(className);

        // Native Verification: Check if the element handles active text input
        bool isInputArea = false;
        if (strClass == "Edit" || strClass == "RichEdit" || strClass == "RICHEDIT50W") {
            isInputArea = true;
        } else if (strClass == "Chrome_RenderWidgetHostHWND") {
            if (guiInfo.flags & 0x00000010) isInputArea = true; 
            HIMC himc = ImmGetContext(hwndFocus);
            if (himc) { isInputArea = true; ImmReleaseContext(hwndFocus, himc); }
        } else if (strClass == "Intermediate D3D Window" || strClass == "Chrome_WidgetWin_1") {
            if (!(guiInfo.flags & GUI_CARETBLINKING) || (guiInfo.rcCaret.left != 0 || guiInfo.rcCaret.top != 0)) {
                isInputArea = true;
            }
        }

        if (!isInputArea) {
            std::cout << "[Monitor] Current focus is NOT a text input area." << std::endl;
            return;
        }

        std::cout << "==================================================" << std::endl;
        std::cout << "        INPUT FIELD PREFERENCE ANALYSIS           " << std::endl;
        std::cout << "==================================================" << std::endl;
        std::cout << "[Target]   Class Name:   " << className << std::endl;

        // Metric 1: Resolve Lowercase Process Name
        std::string exeNameLower = GetProcessNameLower(processId);
        std::cout << "[Target]   Process Name: " << exeNameLower << std::endl;

        // Metric 2: Analyze Window Style Flags
        LONG_PTR windowStyle = GetWindowLongPtr(hwndFocus, GWL_STYLE);
        bool isPasswordBox = (windowStyle & ES_PASSWORD);

        // Metric 3: Probe Input Method Manager (IMM) Context Status
        DWORD conversion = 0, sentence = 0;
        bool imeEnabled = false;
        HIMC himc = ImmGetContext(hwndFocus);
        if (himc) {
            imeEnabled = ImmGetOpenStatus(himc);
            ImmGetConversionStatus(himc, &conversion, &sentence);
            ImmReleaseContext(hwndFocus, himc);
        }

        std::cout << "[Analysis] IME Open Status: " << (imeEnabled ? "OPEN" : "CLOSED") << std::endl;

        // Smart Intent Decision Tree
        std::string recommendedMode = "CHINESE_MODE"; 

        if (isPasswordBox) {
            recommendedMode = "ENGLISH_MODE -> Reason: Secure password field detected.";
        }
        // Match against forced lowercase strings
        else if (exeNameLower == "code.exe" || exeNameLower == "powershell.exe" || exeNameLower == "cmd.exe" || exeNameLower == "windowsterminal.exe") {
            if (imeEnabled) {
                recommendedMode = "CHINESE_MODE -> Reason: Dev environment, but user explicitly toggled IME ON.";
            } else {
                recommendedMode = "ENGLISH_MODE -> Reason: Dev environment (IDE/Terminal) detected with inactive IME.";
            }
        }
        else {
            if (himc) {
                if (conversion & IME_CMODE_ALPHANUMERIC) {
                    recommendedMode = "ENGLISH_MODE -> Reason: Sub-level IME flagged for Alphanumeric tracking.";
                } else if (conversion & IME_CMODE_CHINESE) {
                    recommendedMode = "CHINESE_MODE -> Reason: Sub-level IME flagged for Chinese characters.";
                }
            }
        }

        std::cout << "[RESULT]   RECOMMENDED:  " << recommendedMode << std::endl;
        std::cout << "==================================================\n" << std::endl;
    }
}

int main() {
    std::cout << "--- Starting Smart Input Method Intent Analyzer ---" << std::endl;
    
    for (int i = 0; i < 10; ++i) {
        std::cout << "[Loop Check #" << i + 1 << "]" << std::endl;
        AnalyzeInputMethodPreference();
        Sleep(2500);
    }
    return 0;
}
