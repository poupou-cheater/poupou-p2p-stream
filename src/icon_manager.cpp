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

#include <windows.h>

std::string IconManager::ResolveSvgPath(const std::string& filename) {
    // 1. Direct path check
    DWORD attr = GetFileAttributesA(filename.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return filename;
    }

    // 2. Relative search locations
    const char* searchPrefixes[] = {
        "icons/",
        "ext/icon/",
        "../icons/",
        "../ext/icon/",
        "../../icons/",
        "../../ext/icon/"
    };
    for (const char* prefix : searchPrefixes) {
        std::string candidate = prefix + filename;
        attr = GetFileAttributesA(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidate;
        }
    }

    // 3. Search relative to executable directory
    char exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        char* lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) lastSlash = strrchr(exePath, '/');
        if (lastSlash) {
            *lastSlash = '\0';
            std::string exeDir = exePath;
            const char* subDirs[] = {
                "/icons/",
                "/ext/icon/",
                "/../../icons/",
                "/../../ext/icon/",
                "/../icons/",
                "/../ext/icon/"
            };
            for (const char* sub : subDirs) {
                std::string cand = exeDir + sub + filename;
                attr = GetFileAttributesA(cand.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    return cand;
                }
            }
        }
    }

    return "icons/" + filename;
}

void IconManager::DrawSvgIcon(ImDrawList* drawList, const std::string& svgFilename, ImVec2 center, float size, ImU32 tintColor) {
    if (!drawList || size <= 1.0f) return;

    std::string resolved = ResolveSvgPath(svgFilename);
    int texDim = 128; // High resolution rasterization for smooth linear scaling
    ID3D11ShaderResourceView* srv = GetSvgTexture(resolved, texDim, texDim);
    if (srv) {
        float half = size * 0.5f;
        ImVec2 pMin(center.x - half, center.y - half);
        ImVec2 pMax(center.x + half, center.y + half);
        drawList->AddImage((ImTextureID)srv, pMin, pMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tintColor);
    }
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
    memset(imgData, 0, imgSize);

    float scaleX = (image->width > 0.0f) ? ((float)width / image->width) : 1.0f;
    float scaleY = (image->height > 0.0f) ? ((float)height / image->height) : 1.0f;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale <= 0.0001f) scale = 1.0f;

    float tx = (width - image->width * scale) * 0.5f;
    float ty = (height - image->height * scale) * 0.5f;

    nsvgRasterize(rast, image, tx, ty, scale, imgData, width, height, width * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // Normalize icon glyph to pure white with antialiased alpha so ImGui tinting works 100% accurately
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        unsigned char a = imgData[i * 4 + 3];
        if (a > 0) {
            imgData[i * 4 + 0] = 255;
            imgData[i * 4 + 1] = 255;
            imgData[i * 4 + 2] = 255;
        }
    }

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

void IconManager::DrawIconScreenPlus(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float sw = size * 0.84f;
    float sh = size * 0.62f;
    ImVec2 smin(center.x - sw * 0.5f, center.y - sh * 0.5f);
    ImVec2 smax(center.x + sw * 0.5f, center.y + sh * 0.5f);
    drawList->AddRect(smin, smax, color, 3.0f * g_dpiScale, 0, 1.8f * g_dpiScale);

    // Plus sign inside screen
    float pLen = size * 0.32f;
    drawList->AddLine(ImVec2(center.x - pLen * 0.5f, center.y), ImVec2(center.x + pLen * 0.5f, center.y), color, 2.0f * g_dpiScale);
    drawList->AddLine(ImVec2(center.x, center.y - pLen * 0.5f), ImVec2(center.x, center.y + pLen * 0.5f), color, 2.0f * g_dpiScale);
}

void IconManager::DrawIconPaintbrush(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float s = size * 0.44f;
    // Slanted handle extending up-right at 45 degrees
    ImVec2 handleStart(center.x + s * 0.15f, center.y - s * 0.15f);
    ImVec2 handleEnd(center.x + s * 0.90f, center.y - s * 0.90f);
    drawList->AddLine(handleStart, handleEnd, color, 3.0f * g_dpiScale);

    // Ferrule (metal collar band)
    ImVec2 f1(center.x + s * 0.28f, center.y - s * 0.04f);
    ImVec2 f2(center.x + s * 0.04f, center.y - s * 0.28f);
    drawList->AddLine(f1, f2, color, 2.0f * g_dpiScale);

    // Bristles pointing down-left
    ImVec2 tip(center.x - s * 0.88f, center.y + s * 0.88f);
    ImVec2 b1(center.x - s * 0.15f, center.y + s * 0.45f);
    ImVec2 b2(center.x - s * 0.45f, center.y + s * 0.15f);
    drawList->AddTriangleFilled(tip, b1, b2, color);
}

void IconManager::DrawIconCursorScreen(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float sq = size * 0.82f;
    ImVec2 sqMin(center.x - sq * 0.5f, center.y - sq * 0.5f);
    ImVec2 sqMax(center.x + sq * 0.5f, center.y + sq * 0.5f);
    drawList->AddRect(sqMin, sqMax, color, 4.5f * g_dpiScale, 0, 1.8f * g_dpiScale);

    // Arrow pointer / cursor pointing up-left inside the rounded square
    float a = size * 0.38f;
    ImVec2 tip(center.x - a * 0.45f, center.y - a * 0.45f);
    ImVec2 rPt(tip.x + a * 0.90f, tip.y + a * 0.35f);
    ImVec2 mPt(tip.x + a * 0.45f, tip.y + a * 0.45f);
    ImVec2 bPt(tip.x + a * 0.35f, tip.y + a * 0.90f);
    ImVec2 tail(tip.x + a * 0.85f, tip.y + a * 0.85f);

    drawList->AddTriangleFilled(tip, rPt, mPt, color);
    drawList->AddTriangleFilled(tip, mPt, bPt, color);
    drawList->AddLine(mPt, tail, color, 2.0f * g_dpiScale);
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

    // Stem and base
    drawList->AddLine(ImVec2(center.x, center.y + mw * 0.9f - 1.0f * g_dpiScale),
                      ImVec2(center.x, center.y + mh * 0.60f), color, 1.8f * g_dpiScale);
    drawList->AddLine(ImVec2(center.x - 4.5f * g_dpiScale, center.y + mh * 0.60f),
                      ImVec2(center.x + 4.5f * g_dpiScale, center.y + mh * 0.60f), color, 1.8f * g_dpiScale);

    if (muted) {
        drawList->AddLine(ImVec2(center.x - size * 0.45f, center.y - size * 0.45f),
                          ImVec2(center.x + size * 0.45f, center.y + size * 0.45f),
                          IM_COL32(255, 75, 75, 255), 2.2f * g_dpiScale);
    }
}

void IconManager::DrawIconAudio(ImDrawList* drawList, ImVec2 center, float size, bool deafened, ImU32 color) {
    float hw = size * 0.68f;
    // Headband arc
    drawList->PathArcTo(ImVec2(center.x, center.y - 1.0f * g_dpiScale), hw * 0.5f, 3.14159f, 6.28318f, 16);
    drawList->PathStroke(color, 0, 1.8f * g_dpiScale);

    // Earcups
    float ew = 3.5f * g_dpiScale;
    float eh = 7.5f * g_dpiScale;
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

void IconManager::DrawIconPhoneHangup(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float ew = size * 0.64f;
    // Handset arc curving downwards
    ImVec2 pCenter(center.x - size * 0.08f, center.y + size * 0.20f);
    drawList->PathArcTo(pCenter, ew * 0.48f, 3.65f, 5.77f, 16);
    drawList->PathStroke(color, 0, 2.6f * g_dpiScale);
    drawList->AddCircleFilled(ImVec2(pCenter.x - ew * 0.40f, pCenter.y - size * 0.08f), 2.6f * g_dpiScale, color);
    drawList->AddCircleFilled(ImVec2(pCenter.x + ew * 0.40f, pCenter.y - size * 0.08f), 2.6f * g_dpiScale, color);

    // Small 'x' cross next to the phone on the top right (per sketch!)
    float xCenterX = center.x + size * 0.32f;
    float xCenterY = center.y - size * 0.22f;
    float xHalf = 3.5f * g_dpiScale;
    drawList->AddLine(ImVec2(xCenterX - xHalf, xCenterY - xHalf), ImVec2(xCenterX + xHalf, xCenterY + xHalf), color, 1.8f * g_dpiScale);
    drawList->AddLine(ImVec2(xCenterX + xHalf, xCenterY - xHalf), ImVec2(xCenterX - xHalf, xCenterY + xHalf), color, 1.8f * g_dpiScale);
}

void IconManager::DrawIconVolume(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float s = size * 0.50f;
    // Speaker cone
    ImVec2 p0(center.x - s * 0.7f, center.y - s * 0.35f);
    ImVec2 p1(center.x - s * 0.3f, center.y - s * 0.35f);
    ImVec2 p2(center.x + s * 0.2f, center.y - s * 0.75f);
    ImVec2 p3(center.x + s * 0.2f, center.y + s * 0.75f);
    ImVec2 p4(center.x - s * 0.3f, center.y + s * 0.35f);
    ImVec2 p5(center.x - s * 0.7f, center.y + s * 0.35f);

    ImVec2 conePts[6] = { p0, p1, p2, p3, p4, p5 };
    drawList->AddConvexPolyFilled(conePts, 6, color);

    // Sound waves
    drawList->PathArcTo(ImVec2(center.x + s * 0.25f, center.y), s * 0.45f, -0.7f, 0.7f, 8);
    drawList->PathStroke(color, 0, 1.6f * g_dpiScale);
    drawList->PathArcTo(ImVec2(center.x + s * 0.25f, center.y), s * 0.85f, -0.7f, 0.7f, 8);
    drawList->PathStroke(color, 0, 1.6f * g_dpiScale);
}

void IconManager::DrawIconFullscreen(ImDrawList* drawList, ImVec2 center, float size, ImU32 color) {
    float s = size * 0.40f;
    float len = s * 0.55f;
    float thick = 1.8f * g_dpiScale;

    // Top-left
    drawList->AddLine(ImVec2(center.x - s, center.y - s), ImVec2(center.x - s + len, center.y - s), color, thick);
    drawList->AddLine(ImVec2(center.x - s, center.y - s), ImVec2(center.x - s, center.y - s + len), color, thick);

    // Top-right
    drawList->AddLine(ImVec2(center.x + s, center.y - s), ImVec2(center.x + s - len, center.y - s), color, thick);
    drawList->AddLine(ImVec2(center.x + s, center.y - s), ImVec2(center.x + s, center.y - s + len), color, thick);

    // Bottom-left
    drawList->AddLine(ImVec2(center.x - s, center.y + s), ImVec2(center.x - s + len, center.y + s), color, thick);
    drawList->AddLine(ImVec2(center.x - s, center.y + s), ImVec2(center.x - s, center.y + s - len), color, thick);

    // Bottom-right
    drawList->AddLine(ImVec2(center.x + s, center.y + s), ImVec2(center.x + s - len, center.y + s), color, thick);
    drawList->AddLine(ImVec2(center.x + s, center.y + s), ImVec2(center.x + s, center.y + s - len), color, thick);
}
