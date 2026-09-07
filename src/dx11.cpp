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

#include <wincodec.h>
#include <vector>
#pragma comment(lib, "windowscodecs.lib")

bool Dx11Context::SaveScreenshot(const std::wstring& filePath) {
    if (!pSwapChain || !pd3dDevice || !pd3dDeviceContext) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    if (FAILED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) return false;

    D3D11_TEXTURE2D_DESC desc;
    pBackBuffer->GetDesc(&desc);

    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.MiscFlags = 0;

    ID3D11Texture2D* pStaging = nullptr;
    if (FAILED(pd3dDevice->CreateTexture2D(&desc, nullptr, &pStaging))) {
        pBackBuffer->Release();
        return false;
    }

    // Unbind render targets from Output Merger and flush queued draw calls before reading
    ID3D11RenderTargetView* nullRTV = nullptr;
    pd3dDeviceContext->OMSetRenderTargets(1, &nullRTV, nullptr);
    pd3dDeviceContext->Flush();

    pd3dDeviceContext->CopyResource(pStaging, pBackBuffer);
    pBackBuffer->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(pd3dDeviceContext->Map(pStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
        pStaging->Release();
        return false;
    }

    int nonZeroCount = 0;
    const BYTE* testPtr = (const BYTE*)mapped.pData;
    for (UINT y = 0; y < desc.Height; ++y) {
        const BYTE* row = testPtr + y * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width * 4; ++x) {
            if (row[x] != 0) nonZeroCount++;
        }
    }
    FILE* dbgF = nullptr;
    fopen_s(&dbgF, "screenshot_debug.txt", "w");
    if (dbgF) {
        fprintf(dbgF, "SaveScreenshot: %ux%u pitch=%u nonZeroBytes=%d\n", desc.Width, desc.Height, mapped.RowPitch, nonZeroCount);
        fclose(dbgF);
    }

    IWICImagingFactory* pFactory = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (!pFactory) {
        pd3dDeviceContext->Unmap(pStaging, 0);
        pStaging->Release();
        return false;
    }

    IWICBitmapEncoder* pEncoder = nullptr;
    pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder);
    IWICStream* pStream = nullptr;
    pFactory->CreateStream(&pStream);
    pStream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
    pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* pFrame = nullptr;
    pEncoder->CreateNewFrame(&pFrame, nullptr);
    pFrame->Initialize(nullptr);
    pFrame->SetSize(desc.Width, desc.Height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    pFrame->SetPixelFormat(&format);

    // Convert RGBA to BGRA
    std::vector<BYTE> buffer(desc.Width * desc.Height * 4);
    const BYTE* srcRow = (const BYTE*)mapped.pData;
    BYTE* dstRow = buffer.data();
    for (UINT y = 0; y < desc.Height; ++y) {
        for (UINT x = 0; x < desc.Width; ++x) {
            BYTE r = srcRow[x * 4 + 0];
            BYTE g = srcRow[x * 4 + 1];
            BYTE b = srcRow[x * 4 + 2];
            BYTE a = 255; // Force solid alpha for window screenshot
            dstRow[x * 4 + 0] = b;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = r;
            dstRow[x * 4 + 3] = a;
        }
        srcRow += mapped.RowPitch;
        dstRow += desc.Width * 4;
    }

    pFrame->WritePixels(desc.Height, desc.Width * 4, (UINT)buffer.size(), buffer.data());
    pFrame->Commit();
    pEncoder->Commit();

    pFrame->Release();
    pStream->Release();
    pEncoder->Release();
    pFactory->Release();

    pd3dDeviceContext->Unmap(pStaging, 0);
    pStaging->Release();
    return true;
}

