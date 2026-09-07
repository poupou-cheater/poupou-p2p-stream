#pragma once

#include <d3d11.h>
#include <string>
#include <unordered_map>
#include "imgui.h"

class IconManager {
public:
    static IconManager& Get() {
        static IconManager instance;
        return instance;
    }

    bool Init(ID3D11Device* device);
    void Cleanup();

    // Load or retrieve an SVG rasterized to DX11 texture
    ID3D11ShaderResourceView* GetSvgTexture(const std::string& svgPath, int width, int height);

    // Vector drawing helpers for pixel-perfect Dear ImGui rendering
    static void DrawStar(ImDrawList* drawList, ImVec2 center, float radius, bool filled, ImU32 color, float thickness = 1.4f);
    static void DrawStatusIndicator(ImDrawList* drawList, ImVec2 center, float radius, bool online, bool pulse = true);

private:
    IconManager() = default;
    ~IconManager() { Cleanup(); }

    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, ID3D11ShaderResourceView*> m_textures;
};
