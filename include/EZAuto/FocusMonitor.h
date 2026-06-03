#pragma once

#include "Types.h"

#include <UIAutomation.h>
#include <functional>
#include <atomic>
#include <mutex>

class FocusMonitor {
public:
    using FocusCallback = std::function<void(const FocusInfo&)>;

    FocusMonitor();
    ~FocusMonitor();

    bool start(FocusCallback callback);
    void stop();

private:
    // COM event handler implementation
    class FocusChangedHandler : public IUIAutomationFocusChangedEventHandler {
    public:
        FocusChangedHandler(FocusCallback callback)
            : callback_(std::move(callback)), refCount_(1) {}

        // IUnknown
        ULONG STDMETHODCALLTYPE AddRef() override {
            return InterlockedIncrement(&refCount_);
        }
        ULONG STDMETHODCALLTYPE Release() override {
            long val = InterlockedDecrement(&refCount_);
            if (val == 0) delete this;
            return val;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (riid == __uuidof(IUnknown) ||
                riid == __uuidof(IUIAutomationFocusChangedEventHandler)) {
                *ppv = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        // IUIAutomationFocusChangedEventHandler
        HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement* pSender) override;

        void setAutomation(IUIAutomation* pAuto) { pAutomation_ = pAuto; }

    private:
        FocusCallback callback_;
        IUIAutomation* pAutomation_ = nullptr;
        long refCount_;

        // Dedup consecutive same-element events
        std::mutex lastFocusMutex_;
        std::vector<int> lastRuntimeId_;
        static bool compareRuntimeId(const std::vector<int>& a, const std::vector<int>& b);
        static std::vector<int> getRuntimeId(IUIAutomationElement* pElement);
    };

    FocusCallback callback_;
    IUIAutomation* pAutomation_ = nullptr;
    FocusChangedHandler* pHandler_ = nullptr;
    std::atomic<bool> running_{false};
};
