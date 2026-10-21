#include <windows.h>
#include <objbase.h>

#include "daemon/app.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\win-assist-daemon");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(wa::kMainWindowClass, nullptr)) {
            PostMessageW(existing, wa::kMsgAnotherInstance, 0, 0);
        }
        return 0;
    }

    SetDllDirectoryW(L"");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int code;
    {
        wa::App app;
        code = app.run(instance);
    }

    if (SUCCEEDED(com)) CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}
