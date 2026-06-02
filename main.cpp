#include <windows.h>
#include <initguid.h> // Required to define GUIDs properly across different compilers (like MinGW)
#include <msctf.h>    // TSF Core Header
#include <iostream>

// Ensure necessary libraries are linked
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

void CheckTSFLanguage() {
    // 1. Initialize COM Library
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::cout << "COM Initialization Failed." << std::endl;
        return;
    }

    ITfInputProcessorProfiles* pProfiles = nullptr;

    // 2. Create an instance of ITfInputProcessorProfiles
    hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles,
        (void**)&pProfiles
    );

    if (SUCCEEDED(hr) && pProfiles != nullptr) {
        LANGID langId = 0;

        // 3. Get the active Language ID for the current thread
        hr = pProfiles->GetCurrentLanguage(&langId);
        if (SUCCEEDED(hr)) {
            std::cout << "Current TSF Language ID: 0x" << std::hex << langId << std::dec << std::endl;

            // 4. Identify the language profile
            // 0x0804 = Chinese (Simplified, China) -> LANG_CHINESE
            // 0x0409 = English (United States)   -> LANG_ENGLISH
            if (PRIMARYLANGID(langId) == LANG_CHINESE) {
                std::cout << "Current State: [ Chinese Input Profile ]" << std::endl;
            } else if (PRIMARYLANGID(langId) == LANG_ENGLISH) {
                std::cout << "Current State: [ English Input Profile ]" << std::endl;
            } else {
                std::cout << "Current State: [ Other Language Profile ]" << std::endl;
            }
        } else {
            std::cout << "Failed to retrieve the current language." << std::endl;
        }

        // Release the interface
        pProfiles->Release();
    } else {
        std::cout << "Failed to create ITfInputProcessorProfiles instance." << std::endl;
    }

    // 5. Uninitialize COM
    CoUninitialize();
}

int main() {
    CheckTSFLanguage();
    return 0;
}
