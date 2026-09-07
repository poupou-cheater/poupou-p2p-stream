#include "dx11.h"

bool Dx11Context::Init(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    // Prefer modern DXGI Flip model for zero-copy presentation and locked 60 FPS
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &pSwapChain,
        &pd3dDevice,
        &featureLevel,
        &pd3dDeviceContext
    );

    // Fallback without debug layer if it failed (e.g. Graphics Tools not installed)
    if (FAILED(res) && (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        createDeviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &pSwapChain,
            &pd3dDevice,
            &featureLevel,
            &pd3dDeviceContext
        );
    }

    // Fallback to legacy discard if flip model is not supported on older GPU/OS
    if (FAILED(res)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &pSwapChain,
            &pd3dDevice,
            &featureLevel,
            &pd3dDeviceContext
        );
    }

    if (FAILED(res)) {
        return false;
    }

    CreateRenderTarget();
    return true;
}

void Dx11Context::Cleanup() {
    CleanupRenderTarget();
    if (pSwapChain) {
        pSwapChain->Release();
        pSwapChain = nullptr;
    }
    if (pd3dDeviceContext) {
        pd3dDeviceContext->Release();
        pd3dDeviceContext = nullptr;
    }
    if (pd3dDevice) {
        pd3dDevice->Release();
        pd3dDevice = nullptr;
    }
}

void Dx11Context::CreateRenderTarget() {
    if (!pSwapChain || !pd3dDevice) return;
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
        pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void Dx11Context::CleanupRenderTarget() {
    if (mainRenderTargetView) {
        mainRenderTargetView->Release();
        mainRenderTargetView = nullptr;
    }
}

void Dx11Context::Resize(UINT width, UINT height) {
    if (!pSwapChain) return;
    CleanupRenderTarget();
    pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

void Dx11Context::BeginFrame(const float clearColor[4]) {
    if (!pd3dDeviceContext || !mainRenderTargetView) return;
    pd3dDeviceContext->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);
    pd3dDeviceContext->ClearRenderTargetView(mainRenderTargetView, clearColor);
}

void Dx11Context::EndFrame() {
    if (pSwapChain) {
        pSwapChain->Present(vsync ? 1 : 0, 0);
    }
}
