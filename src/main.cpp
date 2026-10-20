#include "panel.hpp"
#include <fcntl.h>
#include <io.h>
#include <iostream>
int wmain(int argc, wchar_t **argv) {
    bool fixture = argc == 3 && std::wstring(argv[1]) == L"--self-test";
    bool transport = argc == 2 && std::wstring(argv[1]) == L"--fixture-protocol";
    bool windowPolicy = argc == 2 && std::wstring(argv[1]) == L"--window-policy-test";
    if (argc != 1 && !fixture && !transport && !windowPolicy)
        return 2;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        return 1;
    int result = 1;
    try {
        activity::Panel panel(fixture || transport || windowPolicy);
        result = windowPolicy ? panel.WindowPolicyTest()
                              : fixture ? panel.SelfTest(argv[2]) : panel.Run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
    Gdiplus::GdiplusShutdown(token);
    if (SUCCEEDED(com))
        CoUninitialize();
    return result;
}
