#include <EZAuto/FocusMonitor.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

std::vector<int> FocusMonitor::FocusChangedHandler::getRuntimeId(IUIAutomationElement* pElement) {
    std::vector<int> result;
    SAFEARRAY* pSA = nullptr;
    HRESULT hr = pElement->GetRuntimeId(&pSA);
    if (FAILED(hr) || !pSA) return result;

    int* pData = nullptr;
    hr = SafeArrayAccessData(pSA, reinterpret_cast<void**>(&pData));
    if (SUCCEEDED(hr) && pData) {
        long lb = 0, ub = 0;
        SafeArrayGetLBound(pSA, 1, &lb);
        SafeArrayGetUBound(pSA, 1, &ub);
        for (long i = lb; i <= ub; ++i) {
            result.push_back(pData[i]);
        }
        SafeArrayUnaccessData(pSA);
    }
    SafeArrayDestroy(pSA);
    return result;
}

bool FocusMonitor::FocusChangedHandler::compareRuntimeId(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

HRESULT STDMETHODCALLTYPE FocusMonitor::FocusChangedHandler::HandleFocusChangedEvent(
    IUIAutomationElement* pSender)
{
    if (!pSender || !callback_) return S_OK;

    if (running_ && !running_->load(std::memory_order_relaxed)) {
        return S_OK;
    }

    std::vector<int> runtimeId = getRuntimeId(pSender);
    {
        std::lock_guard<std::mutex> lock(lastFocusMutex_);
        if (!runtimeId.empty() && compareRuntimeId(runtimeId, lastRuntimeId_)) {
            return S_OK;
        }
        lastRuntimeId_ = runtimeId;
    }

    FocusInfo info;
    info.runtimeId = runtimeId;

    int procId = 0;
    pSender->get_CurrentProcessId(&procId);
    info.processId = static_cast<DWORD>(procId);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.processId);
    if (hProcess) {
        wchar_t processPath[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
            std::wstring pathStr(processPath, size);
            size_t pos = pathStr.find_last_of(L"\\/");
            std::wstring name = (pos != std::wstring::npos) ? pathStr.substr(pos + 1) : pathStr;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            info.processName.assign(name.begin(), name.end());
        }
        CloseHandle(hProcess);
    }

    pSender->get_CurrentControlType(&info.controlType);

    BOOL isPassword = FALSE;
    pSender->get_CurrentIsPassword(&isPassword);
    info.isPassword = (isPassword == TRUE);

    UIA_HWND uiaHwnd = nullptr;
    pSender->get_CurrentNativeWindowHandle(&uiaHwnd);
    info.hwnd = reinterpret_cast<HWND>(uiaHwnd);

    if (!info.hwnd) {
        info.hwnd = GetForegroundWindow();
    }

    callback_(info);
    return S_OK;
}

FocusMonitor::FocusMonitor() = default;

FocusMonitor::~FocusMonitor() {
    stop();
}

bool FocusMonitor::start(FocusCallback callback) {
    if (running_) return true;

    callback_ = std::move(callback);

    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void**>(&pAutomation_));
    if (FAILED(hr) || !pAutomation_) {
        std::cerr << "FocusMonitor: Failed to create IUIAutomation, hr=0x"
                  << std::hex << hr << std::endl;
        return false;
    }

    pHandler_ = new FocusChangedHandler(callback_);
    pHandler_->setAutomation(pAutomation_);
    pHandler_->setRunningFlag(&running_);

    hr = pAutomation_->AddFocusChangedEventHandler(nullptr, pHandler_);
    if (FAILED(hr)) {
        std::cerr << "FocusMonitor: Failed to register focus handler, hr=0x"
                  << std::hex << hr << std::endl;
        pHandler_->Release();
        pHandler_ = nullptr;
        pAutomation_->Release();
        pAutomation_ = nullptr;
        return false;
    }

    running_ = true;
    std::cout << "FocusMonitor: Started monitoring focus changes" << std::endl;
    return true;
}

void FocusMonitor::stop() {
    if (!running_) return;

    if (pAutomation_ && pHandler_) {
        pAutomation_->RemoveFocusChangedEventHandler(pHandler_);
        pHandler_->Release();
        pHandler_ = nullptr;
    }

    if (pAutomation_) {
        pAutomation_->Release();
        pAutomation_ = nullptr;
    }

    running_ = false;
    std::cout << "FocusMonitor: Stopped" << std::endl;
}
