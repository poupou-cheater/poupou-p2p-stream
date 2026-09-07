#include "icon_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma warning(push)
#pragma warning(disable: 4244)
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#pragma warning(pop)


bool IconManager::Init(ID3D11Device* device) {
    m_device = device;
    return m_device != nullptr;
}

void IconManager::Cleanup() {
    for (auto& pair : m_textures) {
        if (pair.second) {
            pair.second->Release();
        }
    }
    m_textures.clear();
    m_device = nullptr;
}

ID3D11ShaderResourceView* IconManager::GetSvgTexture(const std::string& svgPath, int width, int height) {
    if (!m_device) return nullptr;

    std::string key = svgPath + "_" + std::to_string(width) + "x" + std::to_string(height);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        return it->second;
    }

    NSVGimage* image = nsvgParseFromFile(svgPath.c_str(), "px", 96.0f);
    if (!image) {
        return nullptr;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return nullptr;
    }

    size_t imgSize = (size_t)width * (size_t)height * 4;
    unsigned char* imgData = (unsigned char*)malloc(imgSize);
    if (!imgData) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return nullptr;
    }

    float scaleX = (float)width / image->width;
    float scaleY = (float)height / image->height;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;

    nsvgRasterize(rast, image, 0, 0, scale, imgData, width, height, width * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // Create Direct3D 11 Texture2D
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData;
    initData.pSysMem = imgData;
    initData.SysMemPitch = width * 4;
    initData.SysMemSlicePitch = 0;

    ID3D11Texture2D* pTexture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;

    HRESULT hr = m_device->CreateTexture2D(&desc, &initData, &pTexture);
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        hr = m_device->CreateShaderResourceView(pTexture, &srvDesc, &srv);
        pTexture->Release();
    }

    free(imgData);

    if (SUCCEEDED(hr) && srv) {
        m_textures[key] = srv;
        return srv;
    }

    return nullptr;
}

void IconManager::DrawStar(ImDrawList* drawList, ImVec2 center, float radius, bool filled, ImU32 color, float thickness) {
    const float PI = 3.14159265358979323846f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        float angle = -PI * 0.5f + i * (PI / 5.0f);
        float r = (i % 2 == 0) ? radius : (radius * 0.382f);
        pts[i] = ImVec2(center.x + cosf(angle) * r, center.y + sinf(angle) * r);
    }

    if (filled) {
        // Triangulate star from center to avoid concave polygon distortion
        for (int i = 0; i < 10; ++i) {
            drawList->AddTriangleFilled(center, pts[i], pts[(i + 1) % 10], color);
        }
        drawList->AddPolyline(pts, 10, color, ImDrawFlags_Closed, thickness);
    } else {
        drawList->AddPolyline(pts, 10, color, ImDrawFlags_Closed, thickness);
    }
}

void IconManager::DrawStatusIndicator(ImDrawList* drawList, ImVec2 center, float radius, bool online, bool pulse) {
    if (online) {
        if (pulse) {
            float time = (float)ImGui::GetTime();
            float pulseScale = 1.0f + 0.35f * sinf(time * 3.0f);
            ImU32 glowCol = IM_COL32(72, 224, 110, (int)(70 + 40 * sinf(time * 3.0f)));
            drawList->AddCircleFilled(center, radius * pulseScale * 1.5f, glowCol, 20);
        }
        drawList->AddCircleFilled(center, radius, IM_COL32(72, 224, 110, 255), 16);
    } else {
        drawList->AddCircleFilled(center, radius, IM_COL32(255, 85, 85, 255), 16);
    }
}
