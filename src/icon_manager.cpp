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

void IconManager::DrawStatusIndicator(ImDrawList* drawList, ImVec2 center, float radius, PeerStatus status, bool pulse) {
    float time = (float)ImGui::GetTime();
    if (status == PeerStatus::Online) {
        if (pulse) {
            float pulseScale = 1.0f + 0.35f * sinf(time * 3.0f);
            ImU32 glowCol = IM_COL32(72, 224, 110, (int)(70 + 40 * sinf(time * 3.0f)));
            drawList->AddCircleFilled(center, radius * pulseScale * 1.5f, glowCol, 20);
        }
        drawList->AddCircleFilled(center, radius, IM_COL32(72, 224, 110, 255), 16);
    } else if (status == PeerStatus::Waiting) {
        if (pulse) {
            float pulseScale = 1.0f + 0.35f * sinf(time * 4.0f);
            ImU32 glowCol = IM_COL32(255, 204, 0, (int)(80 + 50 * sinf(time * 4.0f)));
            drawList->AddCircleFilled(center, radius * pulseScale * 1.5f, glowCol, 20);
        }
        drawList->AddCircleFilled(center, radius, IM_COL32(255, 204, 0, 255), 16);
    } else {
        drawList->AddCircleFilled(center, radius, IM_COL32(255, 85, 85, 255), 16);
    }
}

void IconManager::DrawIconAdd(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float half = size * 0.45f;
    float thick = 2.2f * g_dpiScale;
    drawList->AddLine(ImVec2(center.x - half, center.y), ImVec2(center.x + half, center.y), color, thick);
    drawList->AddLine(ImVec2(center.x, center.y - half), ImVec2(center.x, center.y + half), color, thick);
}

void IconManager::DrawIconDraw(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float s = size * 0.42f;
    ImVec2 tip(center.x - s * 0.8f, center.y + s * 0.8f);
    ImVec2 back(center.x + s * 0.7f, center.y - s * 0.7f);
    drawList->AddLine(tip, back, color, 2.4f * g_dpiScale);
    drawList->AddTriangleFilled(tip, 
                                ImVec2(tip.x + 3.5f * g_dpiScale, tip.y), 
                                ImVec2(tip.x, tip.y - 3.5f * g_dpiScale), color);
}

void IconManager::DrawIconShare(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float sw = size * 0.75f;
    float sh = size * 0.52f;
    ImVec2 smin(center.x - sw * 0.5f, center.y - sh * 0.65f);
    ImVec2 smax(center.x + sw * 0.5f, center.y + sh * 0.35f);
    drawList->AddRect(smin, smax, color, 2.0f * g_dpiScale, 0, 1.8f * g_dpiScale);
    // Screen stand
    drawList->AddLine(ImVec2(center.x, smax.y), ImVec2(center.x, smax.y + 4.0f * g_dpiScale), color, 1.8f * g_dpiScale);
    drawList->AddLine(ImVec2(center.x - 5.0f * g_dpiScale, smax.y + 4.0f * g_dpiScale),
                      ImVec2(center.x + 5.0f * g_dpiScale, smax.y + 4.0f * g_dpiScale), color, 1.8f * g_dpiScale);
}

void IconManager::DrawIconMic(ImDrawList* drawList, ImVec2 center, float size, bool muted, ImU32 color) {
    float mw = size * 0.28f;
    float mh = size * 0.50f;
    ImVec2 capMin(center.x - mw * 0.5f, center.y - mh * 0.65f);
    ImVec2 capMax(center.x + mw * 0.5f, center.y + mh * 0.15f);
    drawList->AddRectFilled(capMin, capMax, color, mw * 0.5f);
    // Arc cradle
    drawList->PathArcTo(ImVec2(center.x, center.y - 1.0f * g_dpiScale), mw * 0.9f, 0.0f, 3.14159f, 16);
    drawList->PathStroke(color, 0, 1.8f * g_dpiScale);
    // Stem
    drawList->AddLine(ImVec2(center.x, center.y + mw * 0.9f - 1.0f * g_dpiScale),
                      ImVec2(center.x, center.y + mh * 0.65f), color, 1.8f * g_dpiScale);
    if (muted) {
        drawList->AddLine(ImVec2(center.x - size * 0.45f, center.y - size * 0.45f),
                          ImVec2(center.x + size * 0.45f, center.y + size * 0.45f),
                          IM_COL32(255, 75, 75, 255), 2.2f * g_dpiScale);
    }
}

void IconManager::DrawIconAudio(ImDrawList* drawList, ImVec2 center, float size, bool deafened, ImU32 color) {
    float hw = size * 0.65f;
    // Headband arc
    drawList->PathArcTo(ImVec2(center.x, center.y - 1.0f * g_dpiScale), hw * 0.5f, 3.14159f, 6.28318f, 16);
    drawList->PathStroke(color, 0, 1.8f * g_dpiScale);
    // Earcups
    float ew = 3.5f * g_dpiScale;
    float eh = 7.0f * g_dpiScale;
    drawList->AddRectFilled(ImVec2(center.x - hw * 0.5f - ew * 0.5f, center.y - 1.0f * g_dpiScale),
                            ImVec2(center.x - hw * 0.5f + ew * 0.5f, center.y + eh), color, 2.0f * g_dpiScale);
    drawList->AddRectFilled(ImVec2(center.x + hw * 0.5f - ew * 0.5f, center.y - 1.0f * g_dpiScale),
                            ImVec2(center.x + hw * 0.5f + ew * 0.5f, center.y + eh), color, 2.0f * g_dpiScale);
    if (deafened) {
        drawList->AddLine(ImVec2(center.x - size * 0.45f, center.y - size * 0.45f),
                          ImVec2(center.x + size * 0.45f, center.y + size * 0.45f),
                          IM_COL32(255, 75, 75, 255), 2.2f * g_dpiScale);
    }
}

void IconManager::DrawIconEnd(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float ew = size * 0.65f;
    ImVec2 pCenter(center.x, center.y + size * 0.22f);
    drawList->PathArcTo(pCenter, ew * 0.5f, 3.65f, 5.77f, 16);
    drawList->PathStroke(color, 0, 2.4f * g_dpiScale);
    drawList->AddCircleFilled(ImVec2(center.x - ew * 0.42f, center.y + size * 0.12f), 2.4f * g_dpiScale, color);
    drawList->AddCircleFilled(ImVec2(center.x + ew * 0.42f, center.y + size * 0.12f), 2.4f * g_dpiScale, color);
}
