#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <string>

struct Dx11Context {
    ID3D11Device* pd3dDevice = nullptr;
    ID3D11DeviceContext* pd3dDeviceContext = nullptr;
    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11RenderTargetView* mainRenderTargetView = nullptr;
    bool vsync = true;

    bool Init(HWND hWnd);
    void Cleanup();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void Resize(UINT width, UINT height);
    void BeginFrame(const float clearColor[4]);
    void EndFrame();
    bool SaveScreenshot(const std::wstring& filePath);
};
