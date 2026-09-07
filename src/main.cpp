#include <windows.h>
#include <dwmapi.h>
#include "dx11.h"
#include "gui.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include <objbase.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


static Dx11Context g_dx;

static bool g_guiInitialized = false;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_NCCALCSIZE:
        // Remove standard Windows titlebar, giving full client canvas while retaining resize borders and aero shadows
        if (wParam) {
            if (IsZoomed(hWnd)) {
                NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lParam;
                HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfo(hMon, &mi)) {
                    p->rgrc[0] = mi.rcWork;
                }
            }
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        if (g_guiInitialized && g_dx.pd3dDevice != nullptr && g_dx.pSwapChain != nullptr) {
            const float clearColor[4] = { 0.067f, 0.071f, 0.082f, 1.00f };
            g_dx.BeginFrame(clearColor);
            RenderGui(hWnd, g_dx);
            g_dx.EndFrame();
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(hMon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        mmi->ptMinTrackSize.x = 750;
        mmi->ptMinTrackSize.y = 480;
        return 0;
    }

    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wParam);
        float newScale = (float)newDpi / 96.0f;
        UpdateDpiScale(newScale);

        RECT* const prcNewWindow = (RECT*)lParam;
        SetWindowPos(hWnd, nullptr,
            prcNewWindow->left, prcNewWindow->top,
            prcNewWindow->right - prcNewWindow->left,
            prcNewWindow->bottom - prcNewWindow->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rc;
        GetWindowRect(hWnd, &rc);
        const int border = (int)(8 * g_dpiScale);

        // Border resize handling for frameless window (only when restored)
        if (!IsZoomed(hWnd)) {
            if (pt.y >= rc.top && pt.y < rc.top + border) {
                if (pt.x >= rc.left && pt.x < rc.left + border) return HTTOPLEFT;
                if (pt.x >= rc.right - border && pt.x < rc.right) return HTTOPRIGHT;
                return HTTOP;
            }
            if (pt.y >= rc.bottom - border && pt.y < rc.bottom) {
                if (pt.x >= rc.left && pt.x < rc.left + border) return HTBOTTOMLEFT;
                if (pt.x >= rc.right - border && pt.x < rc.right) return HTBOTTOMRIGHT;
                return HTBOTTOM;
            }
            if (pt.x >= rc.left && pt.x < rc.left + border) return HTLEFT;
            if (pt.x >= rc.right - border && pt.x < rc.right) return HTRIGHT;
        }

        // Invisible Titlebar Drag Area:
        // Grab band behind navigation in top header (height ~70px scaled with DPI)
        int headerHeightPx = (int)(70.0f * g_dpiScale);
        if (pt.y >= rc.top && pt.y < rc.top + headerHeightPx) {
            if (ImGui::GetCurrentContext() != nullptr) {
                // If cursor is NOT hovering an active ImGui widget (buttons, inputs), treat as caption for instant drag
                if (!ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
                    return HTCAPTION;
                }
            }
        }

        return HTCLIENT;
    }

    case WM_SIZE:
        if (g_dx.pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            g_dx.Resize((UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Initialize COM for SHGetKnownFolderPath and Shell APIs
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // High-DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initial DPI scale based on desktop
    HDC screenDC = GetDC(nullptr);
    int screenDpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    ReleaseDC(nullptr, screenDC);
    float initDpiScale = (float)screenDpi / 96.0f;
    if (initDpiScale < 1.0f) initDpiScale = 1.0f;
    UpdateDpiScale(initDpiScale);

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        hInstance,
        nullptr,
        LoadCursor(nullptr, IDC_ARROW),
        nullptr,
        nullptr,
        L"PoupouP2PStreamClass",
        nullptr
    };
    RegisterClassExW(&wc);

    // Modern frameless window style with standard resize and minimize/maximize support
    DWORD dwStyle = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE;
    
    int windowWidth = (int)(1180 * initDpiScale);
    int windowHeight = (int)(720 * initDpiScale);
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - windowWidth) / 2;
    int posY = (screenHeight - windowHeight) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Poupou P2P Stream",
        dwStyle,
        posX, posY,
        windowWidth, windowHeight,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    if (!hWnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Refresh DPI scale directly from the created window
    UINT winDpi = GetDpiForWindow(hWnd);
    if (winDpi > 0) {
        UpdateDpiScale((float)winDpi / 96.0f);
    }


    // Enable Windows 10/11 dark mode for window frame and shadows
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hWnd, &margins);

    if (!g_dx.Init(hWnd)) {
        g_dx.Cleanup();
        DestroyWindow(hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    InitGui(hWnd, g_dx);
    g_guiInitialized = true;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) break;

        // Throttle when window is minimized
        if (IsIconic(hWnd)) {
            Sleep(16);
            continue;
        }

        // Dark background clear color (#111215)
        const float clearColor[4] = { 0.067f, 0.071f, 0.082f, 1.00f };
        g_dx.BeginFrame(clearColor);

        RenderGui(hWnd, g_dx);

        g_dx.EndFrame();
    }

    g_guiInitialized = false;
    ShutdownGui();
    g_dx.Cleanup();
    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    CoUninitialize();
    return 0;
}
