#pragma once

#include <d3d11.h>
#include <string>
#include <unordered_map>
#include "imgui.h"
#include "gui.h"

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

    // Resolve an SVG filename to an existing physical path in icons/ or ext/icon/
    std::string ResolveSvgPath(const std::string& filename);

    // Draw an SVG icon directly loaded from disk
    void DrawSvgIcon(ImDrawList* drawList, const std::string& svgFilename, ImVec2 center, float size, ImU32 tintColor = IM_COL32(255, 255, 255, 255));

    // Vector drawing helpers for pixel-perfect Dear ImGui rendering
    static void DrawStar(ImDrawList* drawList, ImVec2 center, float radius, bool filled, ImU32 color, float thickness = 1.4f);
    static void DrawStatusIndicator(ImDrawList* drawList, ImVec2 center, float radius, PeerStatus status, bool pulse = true);

    // Vector dock controls matching the sketch
    static void DrawIconScreenPlus(ImDrawList* drawList, ImVec2 center, float size, ImU32 color);
    static void DrawIconPaintbrush(ImDrawList* drawList, ImVec2 center, float size, ImU32 color);
    static void DrawIconCursorScreen(ImDrawList* drawList, ImVec2 center, float size, ImU32 color);
    static void DrawIconMic(ImDrawList* drawList, ImVec2 center, float size, bool muted, ImU32 color);
    static void DrawIconAudio(ImDrawList* drawList, ImVec2 center, float size, bool deafened, ImU32 color);
    static void DrawIconPhoneHangup(ImDrawList* drawList, ImVec2 center, float size, ImU32 color);
    static void DrawIconVolume(ImDrawList* drawList, ImVec2 center, float size, ImU32 color);
    static void DrawIconFullscreen(ImDrawList* drawList, ImVec2 center, float size, ImU32 color);

private:
    IconManager() = default;
    ~IconManager() { Cleanup(); }

    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, ID3D11ShaderResourceView*> m_textures;
};
