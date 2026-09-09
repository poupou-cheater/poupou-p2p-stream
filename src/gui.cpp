#include "gui.h"
#include "icon_manager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <shlobj.h>
#include <knownfolders.h>

#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

// =========================================================================
// Unified Floating Capsule Design System Colors (Requirements 1 & 2)
// Strict harmonization of background #1E1F22 across ALL floating capsules:
// Top tabs capsule, system capsule, bottom dock, right audio dock, split capsule
// =========================================================================
constexpr ImU32 COLOR_CAPSULE_BG       = IM_COL32(30, 31, 34, 255); // #1E1F22 (100% opaque)
constexpr ImU32 COLOR_CAPSULE_BORDER   = IM_COL32(60, 64, 76, 220); // #3C404C (crisp subtle border)
constexpr ImVec4 COLOR_CAPSULE_BG_VEC4 = ImVec4(30.0f/255.0f, 31.0f/255.0f, 34.0f/255.0f, 1.0f);

static GuiState g_state;

#if defined(_DEBUG) || !defined(NDEBUG)
DebugState g_debug;
#endif


// Helper: Draw avatar as a circular disc using custom/fallback texture (default profile picture.png)
static void DrawCircularAvatar(ImDrawList* drawList, ID3D11ShaderResourceView* tex, ImVec2 center, float radius, const std::string& initials = "", ImU32 fallbackBgCol = IM_COL32(24, 26, 34, 255)) {
    if (tex) {
        ImVec2 pMin(center.x - radius, center.y - radius);
        ImVec2 pMax(center.x + radius, center.y + radius);
        drawList->AddImageRounded((ImTextureID)tex, pMin, pMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE, radius);
    } else {
        drawList->AddCircleFilled(center, radius, fallbackBgCol, 32);
        if (!initials.empty()) {
            ImVec2 tSize = ImGui::CalcTextSize(initials.c_str());
            drawList->AddText(ImVec2(center.x - tSize.x * 0.5f, center.y - tSize.y * 0.5f), IM_COL32(235, 235, 245, 255), initials.c_str());
        }
    }
}

// Requirement 2: AppData vs Portable Mode configuration & peer persistence
static fs::path GetStorageDirectory() {
    wchar_t exePathBuf[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    fs::path exeDir = fs::path(exePathBuf).parent_path();

    std::error_code ec;
    // Portable mode check: portable.txt in exe directory or current working directory
    if (fs::exists(exeDir / "portable.txt", ec) || fs::exists("portable.txt", ec)) {
        return exeDir;
    }

    fs::path appDataDir;

    // 1. Try %APPDATA% environment variable
    wchar_t* envAppData = nullptr;
    size_t envLen = 0;
    if (_wdupenv_s(&envAppData, &envLen, L"APPDATA") == 0 && envAppData != nullptr) {
        appDataDir = fs::path(envAppData) / "poupou_p2p_stream";
        free(envAppData);
    }

    // 2. Try SHGetKnownFolderPath (FOLDERID_RoamingAppData)
    if (appDataDir.empty()) {
        PWSTR appDataPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath) {
            appDataDir = fs::path(appDataPath) / "poupou_p2p_stream";
            CoTaskMemFree(appDataPath);
        }
    }

    // 3. Try SHGetFolderPathW (CSIDL_APPDATA)
    if (appDataDir.empty()) {
        wchar_t szPath[MAX_PATH] = { 0 };
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, szPath))) {
            appDataDir = fs::path(szPath) / "poupou_p2p_stream";
        }
    }

    if (!appDataDir.empty()) {
        fs::create_directories(appDataDir, ec);
        return appDataDir;
    }

    return exeDir;
}

static fs::path GetIniFilePath() {
    return GetStorageDirectory() / "poupou_peers.ini";
}

static fs::path GetJsonFilePath() {
    return GetStorageDirectory() / "peers.json";
}

void LoadPeers(GuiState& state) {
    state.peers.clear();
    fs::path storageDir = GetStorageDirectory();
    std::error_code ec;
    fs::create_directories(storageDir, ec);

    fs::path iniPath = storageDir / "poupou_peers.ini";
    fs::path jsonPath = storageDir / "peers.json";

    // Auto-migration: if AppData file doesn't exist yet, but local poupou_peers.ini exists, copy it
    if (!fs::exists(iniPath, ec)) {
        if (fs::exists("poupou_peers.ini", ec)) {
            fs::copy_file("poupou_peers.ini", iniPath, fs::copy_options::overwrite_existing, ec);
        } else {
            wchar_t exePathBuf[MAX_PATH] = { 0 };
            GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
            fs::path localIni = fs::path(exePathBuf).parent_path() / "poupou_peers.ini";
            if (fs::exists(localIni, ec)) {
                fs::copy_file(localIni, iniPath, fs::copy_options::overwrite_existing, ec);
            }
        }
    }

    std::ifstream file(iniPath);
    if (!file.is_open()) {
        // Default peer from sketch
        Peer defaultPeer;
        defaultPeer.ip = "100.113.254.69";
        defaultPeer.name = "pc poupou";
        defaultPeer.status = PeerStatus::Offline;
        defaultPeer.isFavorite = false;
        defaultPeer.latencyMs = 24;
        state.peers.push_back(defaultPeer);

        // Additional friendly peer to demonstrate favorites and online status
        Peer peer2;
        peer2.ip = "100.84.112.50";
        peer2.name = "poupou-stream-node";
        peer2.status = PeerStatus::Online;
        peer2.isFavorite = true;
        peer2.latencyMs = 12;
        state.peers.push_back(peer2);

        SavePeers(state);
        return;
    }

    std::string line;
    Peer currentPeer;
    bool inPeer = false;

    while (std::getline(file, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line == "[peer]") {
            if (inPeer && !currentPeer.ip.empty()) {
                state.peers.push_back(currentPeer);
            }
            currentPeer = Peer();
            inPeer = true;
        } else if (inPeer) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                if (key == "ip") currentPeer.ip = val;
                else if (key == "name") {
                    // Aggressively sanitize any leading colons or spaces (e.g. ":: WaitingPeer_3")
                    size_t s = 0;
                    while (s < val.size() && (val[s] == ':' || val[s] == ' ' || val[s] == '\t')) s++;
                    currentPeer.name = (s < val.size()) ? val.substr(s) : val;
                }
                else if (key == "favorite") currentPeer.isFavorite = (val == "1");
                else if (key == "status") {
                    if (val == "online") currentPeer.status = PeerStatus::Online;
                    else if (val == "waiting" || val == "connecting") currentPeer.status = PeerStatus::Waiting;
                    else currentPeer.status = PeerStatus::Offline;
                } else if (key == "latency") {
                    currentPeer.latencyMs = std::atoi(val.c_str());
                }
            }
        }
    }
    if (inPeer && !currentPeer.ip.empty()) {
        state.peers.push_back(currentPeer);
    }

    if (state.peers.empty()) {
        Peer defaultPeer;
        defaultPeer.ip = "100.113.254.69";
        defaultPeer.name = "pc poupou";
        defaultPeer.status = PeerStatus::Offline;
        defaultPeer.isFavorite = false;
        defaultPeer.latencyMs = 24;
        state.peers.push_back(defaultPeer);
        SavePeers(state);
    } else {
        SavePeers(state);
    }
}

void SavePeers(const GuiState& state) {
    fs::path storageDir = GetStorageDirectory();
    std::error_code ec;
    fs::create_directories(storageDir, ec);

    fs::path iniPath = storageDir / "poupou_peers.ini";
    fs::path jsonPath = storageDir / "peers.json";

    // 1. Save INI file (poupou_peers.ini)
    std::ofstream iniFile(iniPath, std::ios::trunc);
    if (iniFile.is_open()) {
        for (const auto& peer : state.peers) {
            iniFile << "[peer]\n";
            iniFile << "ip=" << peer.ip << "\n";
            iniFile << "name=" << peer.name << "\n";
            iniFile << "favorite=" << (peer.isFavorite ? "1" : "0") << "\n";
            iniFile << "status=" << (peer.status == PeerStatus::Online ? "online" : (peer.status == PeerStatus::Waiting ? "waiting" : "offline")) << "\n";
            iniFile << "latency=" << peer.latencyMs << "\n\n";
        }
    }

    // 2. Save JSON file (peers.json)
    std::ofstream jsonFile(jsonPath, std::ios::trunc);
    if (jsonFile.is_open()) {
        jsonFile << "[\n";
        for (size_t i = 0; i < state.peers.size(); ++i) {
            const auto& peer = state.peers[i];
            jsonFile << "  {\n";
            jsonFile << "    \"ip\": \"" << peer.ip << "\",\n";
            jsonFile << "    \"name\": \"" << peer.name << "\",\n";
            jsonFile << "    \"favorite\": " << (peer.isFavorite ? "true" : "false") << ",\n";
            jsonFile << "    \"status\": \"" << (peer.status == PeerStatus::Online ? "online" : (peer.status == PeerStatus::Waiting ? "waiting" : "offline")) << "\",\n";
            jsonFile << "    \"latency\": " << peer.latencyMs << "\n";
            jsonFile << "  }" << (i + 1 < state.peers.size() ? ",\n" : "\n");
        }
        jsonFile << "]\n";
    }
}

float g_dpiScale = 1.0f;

static void ApplySketchTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Exact sketch roundings scaled with DPI
    style.WindowRounding = 12.0f * g_dpiScale;
    style.ChildRounding = 10.0f * g_dpiScale;
    style.FrameRounding = 8.0f * g_dpiScale;
    style.PopupRounding = 10.0f * g_dpiScale;
    style.ScrollbarRounding = 8.0f * g_dpiScale;
    style.GrabRounding = 6.0f * g_dpiScale;
    style.TabRounding = 8.0f * g_dpiScale;

    style.WindowPadding = ImVec2(16.0f * g_dpiScale, 14.0f * g_dpiScale);
    style.FramePadding = ImVec2(12.0f * g_dpiScale, 7.0f * g_dpiScale);
    style.ItemSpacing = ImVec2(10.0f * g_dpiScale, 10.0f * g_dpiScale);
    style.ItemInnerSpacing = ImVec2(8.0f * g_dpiScale, 6.0f * g_dpiScale);
    style.ScrollbarSize = 12.0f * g_dpiScale;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    // Charcoal and dark borders
    colors[ImGuiCol_WindowBg]             = ImVec4(0.067f, 0.071f, 0.082f, 1.00f); // #111215
    colors[ImGuiCol_ChildBg]              = ImVec4(0.078f, 0.082f, 0.098f, 1.00f); // #14151a
    colors[ImGuiCol_PopupBg]              = ImVec4(0.086f, 0.090f, 0.114f, 0.98f);
    colors[ImGuiCol_Border]               = ImVec4(0.227f, 0.231f, 0.271f, 1.00f); // #3a3b45
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    
    colors[ImGuiCol_FrameBg]              = ImVec4(0.094f, 0.098f, 0.118f, 1.00f); // #18191e
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.145f, 0.153f, 0.188f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.188f, 0.196f, 0.243f, 1.00f);
    
    colors[ImGuiCol_TitleBg]              = ImVec4(0.067f, 0.071f, 0.082f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.067f, 0.071f, 0.082f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.067f, 0.071f, 0.082f, 1.00f);
    
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.094f, 0.098f, 0.118f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.078f, 0.082f, 0.098f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.200f, 0.210f, 0.250f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.260f, 0.270f, 0.320f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.330f, 0.340f, 0.400f, 1.00f);

    colors[ImGuiCol_CheckMark]            = ImVec4(0.314f, 0.820f, 0.478f, 1.00f); // Lime green
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.450f, 0.200f, 0.250f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.600f, 0.250f, 0.320f, 1.00f);
    
    colors[ImGuiCol_Button]               = ImVec4(0.118f, 0.125f, 0.153f, 1.00f); // #1e2027
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.165f, 0.176f, 0.216f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.220f, 0.231f, 0.282f, 1.00f);

    colors[ImGuiCol_Header]               = ImVec4(0.150f, 0.160f, 0.200f, 1.00f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.220f, 0.230f, 0.280f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.360f, 0.141f, 0.180f, 1.00f); // Wine accent

    colors[ImGuiCol_Separator]            = ImVec4(0.227f, 0.231f, 0.271f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.360f, 0.141f, 0.180f, 1.00f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(0.500f, 0.200f, 0.250f, 1.00f);

    colors[ImGuiCol_Text]                 = ImVec4(0.941f, 0.941f, 0.961f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.480f, 0.490f, 0.550f, 1.00f);
    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.000f, 0.000f, 0.000f, 0.65f);
}

static float g_baseFontScaleFactor = 2.1818f;

void UpdateDpiScale(float newScale) {
    if (newScale < 0.75f) newScale = 0.75f;
    if (newScale > 4.0f) newScale = 4.0f;
    bool scaleChanged = (fabsf(g_dpiScale - newScale) > 0.003f);
    g_dpiScale = newScale;
    if (ImGui::GetCurrentContext() != nullptr) {
        if (scaleChanged) {
            ApplySketchTheme();
        }
        ImGuiIO& io = ImGui::GetIO();
        if (g_baseFontScaleFactor > 0.01f) {
            io.FontGlobalScale = g_dpiScale / g_baseFontScaleFactor;
        }
    }
}

void InitGui(HWND hWnd, Dx11Context& dx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load Segoe UI font at high resolution (36px) so scaling up on 2K/4K stays pin-sharp
    const float BASE_FONT_TEX_SIZE = 36.0f;
    const float BASE_NOMINAL_PX = 16.5f;
    g_baseFontScaleFactor = BASE_FONT_TEX_SIZE / BASE_NOMINAL_PX;

    ImFont* font = nullptr;
    static const ImWchar glyph_ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x2000, 0x206F, // General Punctuation
        0x2500, 0x257F, // Box Drawing
        0x25A0, 0x25FF, // Geometric Shapes
        0x2600, 0x26FF, // Misc Symbols (Stars, etc.)
        0x2700, 0x27BF, // Dingbats
        0
    };

    if (std::ifstream("C:\\Windows\\Fonts\\segoeui.ttf").good()) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", BASE_FONT_TEX_SIZE, nullptr, glyph_ranges);
    }
    if (!font) {
        io.Fonts->AddFontDefault();
    }

    ApplySketchTheme();

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(dx.pd3dDevice, dx.pd3dDeviceContext);

    IconManager::Get().Init(dx.pd3dDevice);
    LoadPeers(g_state);

    // Support command-line switches for testing & verification
    const wchar_t* cmd = GetCommandLineW();
    if (cmd) {
        if (wcsstr(cmd, L"--tab=conn")) {
            g_state.currentTab = AppTab::CurrentConnection;
        }
        if (wcsstr(cmd, L"--tab=home")) {
            g_state.currentTab = AppTab::Home;
        }
        if (wcsstr(cmd, L"--tab=setting")) {
            g_state.currentTab = AppTab::Setting;
        }
        const wchar_t* sStreams = wcsstr(cmd, L"--streams=");
        if (sStreams) {
            g_state.currentTab = AppTab::CurrentConnection;
            int val = _wtoi(sStreams + 10);
            if (val >= 0 && val <= 8) {
#if defined(_DEBUG) || !defined(NDEBUG)
                g_debug.simulatedStreamCount = val;
#endif
            }
        }
        const wchar_t* sSplit = wcsstr(cmd, L"--split=");
        if (sSplit) {
            g_state.gridLayoutMode = _wtoi(sSplit + 8);
        }
        if (wcsstr(cmd, L"--focus")) {
            g_state.currentTab = AppTab::CurrentConnection;
            g_state.focusedStreamIndex = 0;
        }
        if (wcsstr(cmd, L"--mute")) {
            g_state.isMicMuted = true;
            for (auto& p : g_state.peers) p.isMuted = true;
        }
        if (wcsstr(cmd, L"--deafen")) {
            g_state.isAudioDeafened = true;
            for (auto& p : g_state.peers) p.isDeafened = true;
        }
        if (wcsstr(cmd, L"--speak")) {
            g_state.isMicSpeaking = true;
            for (auto& p : g_state.peers) p.isSpeaking = true;
        }
#if defined(_DEBUG) || !defined(NDEBUG)
        if (wcsstr(cmd, L"--show-debug") || wcsstr(cmd, L"--debug")) {
            g_debug.showDebugButton = true;
            g_debug.showDebugWindow = true;
        }
        if (wcsstr(cmd, L"--hide-debug")) {
            g_debug.showDebugButton = false;
            g_debug.showDebugWindow = false;
        }
#endif
    }
}

void ShutdownGui() {
    IconManager::Get().Cleanup();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

// Draw custom circular avatar (simple circle by default, glowing voice activity ring only when speaking)
static void RenderAvatar(const char* id, const char* label, bool isSpeaking, float radius, bool isMe = false) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 center = ImVec2(pos.x + radius, pos.y + radius);

    // Requirement 1: On "me" avatar, disable right-click, context menu, and tooltips. Nothing happens on click.
    if (isMe) {
        ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, radius * 2.0f + 4.0f));
    } else {
        ImGui::InvisibleButton(id, ImVec2(radius * 2.0f + 4.0f, radius * 2.0f + 4.0f));
    }
    bool hovered = !isMe && ImGui::IsItemHovered();

    // Requirement 2: Simple neutral circle without any permanent colored contour
    drawList->AddCircle(center, radius, IM_COL32(70, 74, 88, 170), 32, 1.5f * g_dpiScale);

    // Glowing speech ring ONLY when actively speaking (Voice Activity Detection)
    if (isSpeaking) {
        float ringRadius = radius + 2.5f * g_dpiScale;
        drawList->AddCircle(center, ringRadius, IM_COL32(72, 224, 110, 255), 32, 2.0f * g_dpiScale);
        float time = (float)ImGui::GetTime();
        float glowR = ringRadius + (2.0f + sinf(time * 6.0f) * 1.5f) * g_dpiScale;
        drawList->AddCircle(center, glowR, IM_COL32(72, 224, 110, 120), 32, 1.4f * g_dpiScale);
    }

    // Label inside transparent circle
    if (label && label[0] != '\0') {
        ImVec2 textSize = ImGui::CalcTextSize(label);
        ImVec2 textPos = ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);
        ImU32 textCol = IM_COL32(235, 238, 245, 255);
        drawList->AddText(textPos, textCol, label);
    }

    // Tooltip & Context Menu: ONLY for other peers, strictly disabled for "me"
    if (!isMe) {
        if (hovered) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", label && label[0] ? label : "Peer");
            ImGui::TextColored(isSpeaking ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.8f, 1.0f),
                               isSpeaking ? "Speaking" : "Idle");
            ImGui::TextDisabled("Right-click for audio & poke options");
            ImGui::EndTooltip();
        }

        if (ImGui::BeginPopupContextItem(id)) {
            ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.98f, 1.0f), "%s Options", label && label[0] ? label : "Peer");
            ImGui::Separator();
            ImGui::Spacing();

            Peer* p = nullptr;
            for (auto& peer : g_state.peers) {
                if (label && peer.name == label) {
                    p = &peer;
                    break;
                }
            }
            if (!p && !g_state.peers.empty()) p = &g_state.peers[0];

            if (p) {
                ImGui::Text("Volume: %.0f%%", p->volume * 100.0f);
                if (ImGui::SliderFloat("##av_vol", &p->volume, 0.0f, 1.5f, "%.0f%%")) {
                    SavePeers(g_state);
                }
                ImGui::Spacing();
                if (ImGui::MenuItem("Mute Audio", nullptr, &p->isMuted)) {
                    SavePeers(g_state);
                }
                if (ImGui::MenuItem("Deafen Audio", nullptr, &p->isDeafened)) {
                    SavePeers(g_state);
                }
                if (ImGui::MenuItem("Poke Peer (Ping)")) {
                    p->pokeTimer = 2.0f;
                }
            }
            ImGui::EndPopup();
        }
    }
}


// Helper: Draw Mute/Deafen red badge on avatar (Item 5)
static void DrawAvatarAudioBadge(ImDrawList* drawList, ImVec2 center, float radius, bool isMuted, bool isDeafened) {
    if (!isMuted && !isDeafened) return;

    float badgeRadius = 10.0f * g_dpiScale;
    // Bottom-right quadrant of circular avatar
    ImVec2 bCenter(center.x + radius * 0.707f, center.y + radius * 0.707f);

    // Solid red background with dark border
    drawList->AddCircleFilled(bCenter, badgeRadius, IM_COL32(235, 48, 58, 255), 16);
    drawList->AddCircle(bCenter, badgeRadius, IM_COL32(24, 25, 30, 255), 16, 1.5f * g_dpiScale);

    if (isDeafened) {
        IconManager::Get().DrawSvgIcon(drawList, "IcBaselineHeadsetOff.svg", bCenter, 12.0f * g_dpiScale, IM_COL32(255, 255, 255, 255));
    } else if (isMuted) {
        IconManager::Get().DrawSvgIcon(drawList, "MdiMicrophoneOff.svg", bCenter, 12.0f * g_dpiScale, IM_COL32(255, 255, 255, 255));
    }
}

static int GetActiveStreamCount() {
    int count = 0;
#if defined(_DEBUG) || !defined(NDEBUG)
    if (g_debug.simulatedStreamCount >= 0) {
        return g_debug.simulatedStreamCount;
    }
#endif
    if (g_state.isStreaming) count++;
    for (auto& peer : g_state.peers) {
        if (peer.status == PeerStatus::Online && !peer.isStreamHidden) {
            count++;
        }
    }
    return count;
}

// Titlebar & Window Header matching the sketch
static void RenderHeader(HWND hWnd, float windowWidth) {
    float headerHeight = 84.0f * g_dpiScale;

    // Requirement 3 & 4: Isolate TopBar in its own dedicated ImGui window at highest z-order
    // to guarantee 100% click priority over any video or stream tiles beneath
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, headerHeight));
    ImGuiWindowFlags headerFlags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##TopBarOverlay", nullptr, headerFlags);
    ImGui::PopStyleVar(2);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();

    // Requirement 2: TopBar background is 100% transparent. No giant black/grey bar across the screen.

    // Load default profile picture for avatar fallback
    ID3D11ShaderResourceView* defaultAvatarTex = IconManager::Get().GetImageTexture("ext/img/default profile picture.png");

    float topAvatarRadius = 23.0f * g_dpiScale;
    float topAvatarSpacing = 10.0f * g_dpiScale;
    float avatarDiam = topAvatarRadius * 2.0f;
    float topAvatarY = (headerHeight - avatarDiam) * 0.5f;

    // Center tab buttons calculations (Structured rounded rectangle style harmonized with dock)
    float navBtnHeight = 44.0f * g_dpiScale;
    float paddingX = 24.0f * g_dpiScale;
    float spacing = 8.0f * g_dpiScale;

    ImGui::SetWindowFontScale(1.15f);
    float wHome = ImGui::CalcTextSize("home").x + paddingX * 2.0f;
    float wConn = ImGui::CalcTextSize("current connection").x + paddingX * 2.0f;
    float wSet  = ImGui::CalcTextSize("setting").x + paddingX * 2.0f;

    float centerX = windowWidth * 0.5f;
    float xConn = centerX - (wConn * 0.5f);
    float homeX = xConn - spacing - wHome;
    float xSet  = xConn + wConn + spacing;

    float meMarginToHome = 40.0f * g_dpiScale;
    float meX = homeX - meMarginToHome - avatarDiam;

    // Calculate Right-side System Controls Pill geometry first
    float btnSize = 44.0f * g_dpiScale;
    float btnGap = 4.0f * g_dpiScale;
    float capPadX = 7.0f * g_dpiScale;
    float capPadY = 6.0f * g_dpiScale;
    float capH = btnSize + capPadY * 2.0f;
    float rightMargin = 14.0f * g_dpiScale;

    float sysControlsW = btnSize * 3.0f + btnGap * 2.0f; // [ — ] [ ▢ ] [ ✕ ]

    bool isDebugVisible = false;
#if defined(_DEBUG) || !defined(NDEBUG)
    float dbgBtnW = 86.0f * g_dpiScale;
    float dbgBtnH = 38.0f * g_dpiScale;
    float dbgGap = 6.0f * g_dpiScale;
    isDebugVisible = g_debug.showDebugButton;
#endif

    float visibleButtonsW = sysControlsW;
#if defined(_DEBUG) || !defined(NDEBUG)
    if (isDebugVisible) {
        visibleButtonsW += dbgBtnW + dbgGap;
    }
#endif

    float capW = visibleButtonsW + capPadX * 2.0f;
    float capX = windowWidth - rightMargin - capW;
    float sysY = (headerHeight - btnSize) * 0.5f;

    // =========================================================================
    // Item 1: TopBar Avatars Placement (Strictly Top-Left, Top-Right stays clear)
    // - On 'home' / 'setting' or when stream is active on 'current connection':
    //   Full Top-Left avatar row (Waiting [yellow ring] -> Silent -> Speaking -> Me)
    // - On 'current connection' when NO stream is active (voice-only view):
    //   Connected members are in the central voice bubbles.
    //   Waiting members (Waiting [yellow ring]) are placed in Top-Left.
    // - Top-Right corner next to Debug is ALWAYS empty/clear.
    // =========================================================================
    bool isVoiceOnlyView = (g_state.currentTab == AppTab::CurrentConnection && GetActiveStreamCount() == 0);

    // Categorize remote peers
    std::vector<Peer*> waitingPeers;
    std::vector<Peer*> silentPeers;
    std::vector<Peer*> speakingPeers;

    for (auto& peer : g_state.peers) {
        if (peer.status == PeerStatus::Waiting) {
            waitingPeers.push_back(&peer);
        } else if (peer.status == PeerStatus::Online) {
            if (peer.isSpeaking) {
                speakingPeers.push_back(&peer);
            } else {
                silentPeers.push_back(&peer);
            }
        }
    }

    float leftLimit = 20.0f * g_dpiScale;
    float availW = (meX - topAvatarSpacing) - leftLimit;
    int maxFit = (availW > 0.0f) ? (int)((availW + topAvatarSpacing) / (avatarDiam + topAvatarSpacing)) : 0;

    std::vector<Peer*> peersToDisplay;

    if (isVoiceOnlyView) {
        // In voice-only view, only waiting peers are in the top-left row (connected peers are in central view)
        int waitCount = (int)waitingPeers.size();
        for (int k = 0; k < waitCount && (int)peersToDisplay.size() < maxFit; ++k) {
            peersToDisplay.push_back(waitingPeers[k]);
        }
    } else {
        // Stream active or Home / Setting tab: Full Top-Left avatar row (Waiting -> Silent -> Speaking -> Me)
        int totalPeers = (int)(waitingPeers.size() + silentPeers.size() + speakingPeers.size());
        if (totalPeers <= maxFit) {
            peersToDisplay.insert(peersToDisplay.end(), waitingPeers.begin(), waitingPeers.end());
            peersToDisplay.insert(peersToDisplay.end(), silentPeers.begin(), silentPeers.end());
            peersToDisplay.insert(peersToDisplay.end(), speakingPeers.begin(), speakingPeers.end());
        } else {
            int slotsLeft = maxFit;
            std::vector<Peer*> pickedSpeaking;
            std::vector<Peer*> pickedWaiting;
            std::vector<Peer*> pickedSilent;

            for (auto* p : speakingPeers) {
                if (slotsLeft > 0) { pickedSpeaking.push_back(p); slotsLeft--; }
            }
            for (auto* p : waitingPeers) {
                if (slotsLeft > 0) { pickedWaiting.push_back(p); slotsLeft--; }
            }
            for (auto* p : silentPeers) {
                if (slotsLeft > 0) { pickedSilent.push_back(p); slotsLeft--; }
            }

            peersToDisplay.insert(peersToDisplay.end(), pickedWaiting.begin(), pickedWaiting.end());
            peersToDisplay.insert(peersToDisplay.end(), pickedSilent.begin(), pickedSilent.end());
            peersToDisplay.insert(peersToDisplay.end(), pickedSpeaking.begin(), pickedSpeaking.end());
        }
    }

    // Render remote peers (ending right before meX if 'me' is displayed, or aligned to meX in voice-only)
    int peerCount = (int)peersToDisplay.size();
    float anchorRight = isVoiceOnlyView ? (meX + avatarDiam) : meX;
    for (int k = 0; k < peerCount; ++k) {
        Peer* peer = peersToDisplay[k];
        float peerX = anchorRight - (peerCount - k) * (avatarDiam + topAvatarSpacing);
        ImGui::SetCursorPos(ImVec2(peerX, topAvatarY));
        ImVec2 screenMin = ImGui::GetCursorScreenPos();
        ImVec2 center(screenMin.x + topAvatarRadius, screenMin.y + topAvatarRadius);

        std::string btnId = "##top_peer_" + std::to_string(k) + "_" + peer->name;
        ImGui::InvisibleButton(btnId.c_str(), ImVec2(avatarDiam, avatarDiam));
        bool isHovered = ImGui::IsItemHovered();

        DrawCircularAvatar(drawList, defaultAvatarTex, center, topAvatarRadius, peer->name);

        // Status rings
        if (peer->status == PeerStatus::Waiting) {
            float pulse = 1.0f + 0.08f * sinf((float)ImGui::GetTime() * 4.0f);
            drawList->AddCircle(center, topAvatarRadius * pulse, IM_COL32(255, 204, 0, 230), 32, 2.2f * g_dpiScale);
        } else if (peer->status == PeerStatus::Online && peer->isSpeaking) {
            drawList->AddCircle(center, topAvatarRadius + 2.0f * g_dpiScale, IM_COL32(72, 224, 110, 255), 32, 2.5f * g_dpiScale);
            float glow = 2.0f + 1.5f * sinf((float)ImGui::GetTime() * 6.0f);
            drawList->AddCircle(center, topAvatarRadius + (2.0f + glow) * g_dpiScale, IM_COL32(72, 224, 110, 120), 32, 1.8f * g_dpiScale);
        }

        // Visual Mute / Deafen badge on peer avatar
        DrawAvatarAudioBadge(drawList, center, topAvatarRadius, peer->isMuted, peer->isDeafened);

        if (isHovered) {
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.12f, 0.13f, 0.16f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.27f, 0.33f, 1.00f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * g_dpiScale, 8.0f * g_dpiScale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * g_dpiScale);
            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.98f, 1.0f), "%s", peer->name.c_str());
            if (peer->status == PeerStatus::Waiting) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.1f, 1.0f), "Status: Waiting for connection...");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Status: Connected (Online)");
                ImGui::TextDisabled(peer->isSpeaking ? "Speaking" : "Idle");
            }
            if (peer->isDeafened) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Audio: Deafened (Casque coupé)");
            } else if (peer->isMuted) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Audio: Muted (Micro coupé)");
            }
            if (peer->isStreamHidden) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Stream: Masqué");
            }
            ImGui::TextDisabled("Right-click for options");
            ImGui::EndTooltip();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }

        if (ImGui::BeginPopupContextItem(btnId.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
            ImGui::Text("%s Options", peer->name.c_str());
            ImGui::Separator();
            if (peer->isStreamHidden) {
                if (ImGui::MenuItem("Afficher le stream (Show stream)")) {
                    peer->isStreamHidden = false;
                }
            } else {
                if (ImGui::MenuItem("Masquer le stream (Hide stream)")) {
                    peer->isStreamHidden = true;
                }
            }
            ImGui::Separator();
            ImGui::Text("Volume: %.0f%%", peer->volume * 100.0f);
            if (ImGui::SliderFloat("##top_peer_vol", &peer->volume, 0.0f, 1.5f, "%.0f%%")) {
                SavePeers(g_state);
            }
            ImGui::MenuItem("Mute Audio", nullptr, &peer->isMuted);
            ImGui::MenuItem("Deafen Audio", nullptr, &peer->isDeafened);
            if (ImGui::MenuItem("Poke Peer (Ping)")) {
                peer->pokeTimer = 2.0f;
            }
            if (peer->status == PeerStatus::Waiting) {
                ImGui::Separator();
                if (ImGui::MenuItem("Cancel / Disconnect")) {
                    peer->status = PeerStatus::Offline;
                    if (g_state.activeConnectedIp == peer->ip) {
                        g_state.activeConnectedIp.clear();
                    }
                    SavePeers(g_state);
                }
            }
            ImGui::EndPopup();
        }
    }

    // Render "me" avatar (strictly anchored at meX, only when not in voice-only view)
    if (!isVoiceOnlyView) {
        ImGui::SetCursorPos(ImVec2(meX, topAvatarY));
        ImVec2 screenMin = ImGui::GetCursorScreenPos();
        ImVec2 center(screenMin.x + topAvatarRadius, screenMin.y + topAvatarRadius);

        ImGui::InvisibleButton("##top_me_avatar", ImVec2(avatarDiam, avatarDiam));
        bool meHovered = ImGui::IsItemHovered();

        DrawCircularAvatar(drawList, defaultAvatarTex, center, topAvatarRadius, "Me");

        // Speech glowing green ring ONLY when speaking
        if (g_state.isMicSpeaking) {
            drawList->AddCircle(center, topAvatarRadius + 2.0f * g_dpiScale, IM_COL32(72, 224, 110, 255), 32, 2.5f * g_dpiScale);
            float glow = 2.0f + 1.5f * sinf((float)ImGui::GetTime() * 6.0f);
            drawList->AddCircle(center, topAvatarRadius + (2.0f + glow) * g_dpiScale, IM_COL32(72, 224, 110, 120), 32, 1.8f * g_dpiScale);
        }

        // Visual Mute / Deafen badge on 'me' avatar
        DrawAvatarAudioBadge(drawList, center, topAvatarRadius, g_state.isMicMuted, g_state.isAudioDeafened);

        if (meHovered) {
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.12f, 0.13f, 0.16f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.27f, 0.33f, 1.00f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * g_dpiScale, 8.0f * g_dpiScale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * g_dpiScale);
            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.98f, 1.0f), "Host: Me (Local User)");
            ImGui::TextDisabled(g_state.isMicSpeaking ? "Status: Speaking" : "Status: Connected (Silent)");
            if (g_state.isAudioDeafened) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Audio: Deafened (Casque coupé)");
            } else if (g_state.isMicMuted) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Microphone: Muted (Micro coupé)");
            }
            ImGui::TextDisabled("Right-click for audio / mic options");
            ImGui::EndTooltip();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }

        if (ImGui::BeginPopupContextItem("##top_me_avatar", ImGuiPopupFlags_MouseButtonRight)) {
            ImGui::Text("Host Audio Options");
            ImGui::Separator();
            ImGui::MenuItem("Mute Microphone", nullptr, &g_state.isMicMuted);
            ImGui::MenuItem("Deafen Audio", nullptr, &g_state.isAudioDeafened);
            ImGui::MenuItem("RNNoise Noise Suppression", nullptr, &g_state.rnnoiseNoiseSuppression);
            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // Item 1 & 3: Middle Tab Buttons (home, current connection, setting)
    // 100% Solid opaque rounded rectangle with AddRectFilled(#2B2D31) on window drawList
    // Single AddText call eliminates text doubling/superposition
    // =========================================================================
    float navY = (headerHeight - navBtnHeight) * 0.5f;

    float tabCapPadX = 7.0f * g_dpiScale;
    float tabCapPadY = 6.0f * g_dpiScale;
    ImVec2 tabCapMin(winPos.x + homeX - tabCapPadX, winPos.y + navY - tabCapPadY);
    ImVec2 tabCapMax(winPos.x + xSet + wSet + tabCapPadX, winPos.y + navY + navBtnHeight + tabCapPadY);
    float tabCapRounding = 16.0f * g_dpiScale;

    drawList->AddRectFilled(tabCapMin, tabCapMax, COLOR_CAPSULE_BG, tabCapRounding);
    drawList->AddRect(tabCapMin, tabCapMax, COLOR_CAPSULE_BORDER, tabCapRounding, 0, 1.2f * g_dpiScale);

    auto DrawSolidTabButton = [&](const char* label, float x, float w, AppTab tab, const char* btnId) {
        ImGui::SetCursorPos(ImVec2(x, navY));
        ImVec2 bMin = ImGui::GetCursorScreenPos();
        ImVec2 bMax(bMin.x + w, bMin.y + navBtnHeight);
        bool isSelected = (g_state.currentTab == tab);

        ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool isHover = (mousePos.x >= bMin.x && mousePos.x <= bMax.x && mousePos.y >= bMin.y && mousePos.y <= bMax.y);
        bool isDown = ImGui::GetIO().MouseDown[0];

        if (isSelected) {
            ImU32 selCol = isHover ? IM_COL32(110, 40, 54, 255) : IM_COL32(92, 36, 46, 255);
            drawList->AddRectFilled(bMin, bMax, selCol, 10.0f * g_dpiScale);
            drawList->AddRect(bMin, bMax, IM_COL32(138, 54, 69, 255), 10.0f * g_dpiScale, 0, 1.0f * g_dpiScale);
        } else if (isHover) {
            ImU32 hovCol = isDown ? IM_COL32(24, 25, 28, 255) : IM_COL32(48, 50, 58, 255);
            drawList->AddRectFilled(bMin, bMax, hovCol, 10.0f * g_dpiScale);
            drawList->AddRect(bMin, bMax, IM_COL32(75, 80, 95, 200), 10.0f * g_dpiScale, 0, 1.0f * g_dpiScale);
        }

        ImGui::SetCursorScreenPos(bMin);
        if (ImGui::InvisibleButton(btnId, ImVec2(w, navBtnHeight))) {
            g_state.currentTab = tab;
        }

        // Draw text ONCE strictly on drawList (eliminates double text superposition)
        ImU32 textCol = isSelected ? IM_COL32(255, 255, 255, 255) : (isHover ? IM_COL32(245, 245, 250, 255) : IM_COL32(219, 222, 225, 255));
        ImVec2 tSize = ImGui::CalcTextSize(label);
        ImVec2 tPos(bMin.x + (w - tSize.x) * 0.5f, bMin.y + (navBtnHeight - tSize.y) * 0.5f);
        drawList->AddText(tPos, textCol, label);
    };

    DrawSolidTabButton("home", homeX, wHome, AppTab::Home, "##tab_home");
    DrawSolidTabButton("current connection", xConn, wConn, AppTab::CurrentConnection, "##tab_conn");
    DrawSolidTabButton("setting", xSet, wSet, AppTab::Setting, "##tab_setting");

    ImGui::SetWindowFontScale(1.0f); // Reset font scale back to nominal

    // Dynamic System controls pill on TopBar window drawList
    // Positioned using topY to guarantee it stays in the TopBar, englobing [ Debug ] [ — ] [ ▢ ] [ ✕ ]
    float topY = winPos.y + sysY;
    float pillScreenX = winPos.x + capX;
    float pillScreenY = topY - capPadY;
    ImVec2 rectMin(pillScreenX, pillScreenY);
    ImVec2 rectMax(rectMin.x + capW, rectMin.y + capH);

    // Dark pill background (#1E1F22 opaque) harmonized strictly with top tabs capsule
    drawList->AddRectFilled(rectMin, rectMax, COLOR_CAPSULE_BG, 16.0f * g_dpiScale);
    drawList->AddRect(rectMin, rectMax, COLOR_CAPSULE_BORDER, 16.0f * g_dpiScale, 0, 1.2f * g_dpiScale);

    float curRightX = capX + capPadX;

#if defined(_DEBUG) || !defined(NDEBUG)
    if (isDebugVisible) {
        // Debug toggle button
        float dbgY = (headerHeight - dbgBtnH) * 0.5f;
        ImGui::SetCursorPos(ImVec2(curRightX, dbgY));
        ImVec2 dbgScreenMin = ImGui::GetCursorScreenPos();
        ImVec2 dbgScreenMax = ImVec2(dbgScreenMin.x + dbgBtnW, dbgScreenMin.y + dbgBtnH);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        if (ImGui::Button("##top_dbg_btn", ImVec2(dbgBtnW, dbgBtnH))) {
            g_debug.showDebugWindow = !g_debug.showDebugWindow;
        }
        bool dbgHovered = ImGui::IsItemHovered();
        ImGui::PopStyleColor(3);

        ImU32 dbgBg = g_debug.showDebugWindow ? IM_COL32(122, 51, 64, 255) : (dbgHovered ? IM_COL32(48, 52, 68, 255) : COLOR_CAPSULE_BG);
        drawList->AddRectFilled(dbgScreenMin, dbgScreenMax, dbgBg, 10.0f * g_dpiScale);
        drawList->AddRect(dbgScreenMin, dbgScreenMax, COLOR_CAPSULE_BORDER, 10.0f * g_dpiScale, 0, 1.0f);
        ImVec2 dbgTextSize = ImGui::CalcTextSize("Debug");
        drawList->AddText(ImVec2(dbgScreenMin.x + (dbgBtnW - dbgTextSize.x) * 0.5f, dbgScreenMin.y + (dbgBtnH - dbgTextSize.y) * 0.5f), IM_COL32(240, 242, 250, 255), "Debug");

        if (dbgHovered) ImGui::SetTooltip("Toggle Debug & Simulation Tools (F12)");

        curRightX += dbgBtnW + dbgGap;
    }
#endif

    // Minimize (─)
    ImGui::SetCursorPos(ImVec2(curRightX, sysY));
    ImVec2 minBtnPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("##btn_min", ImVec2(btnSize, btnSize))) {
        ShowWindow(hWnd, SW_MINIMIZE);
    }
    bool minHovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);

    if (minHovered) {
        drawList->AddRectFilled(minBtnPos, ImVec2(minBtnPos.x + btnSize, minBtnPos.y + btnSize), IM_COL32(51, 56, 71, 255), 9.0f * g_dpiScale);
    }
    drawList->AddLine(ImVec2(minBtnPos.x + 12.0f * g_dpiScale, minBtnPos.y + btnSize * 0.5f),
                      ImVec2(minBtnPos.x + btnSize - 12.0f * g_dpiScale, minBtnPos.y + btnSize * 0.5f),
                      IM_COL32(220, 220, 230, 255), 1.8f * g_dpiScale);
    curRightX += btnSize + btnGap;

    // Maximize / Restore (□)
    ImGui::SetCursorPos(ImVec2(curRightX, sysY));
    ImVec2 maxBtnPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("##btn_max", ImVec2(btnSize, btnSize))) {
        if (IsZoomed(hWnd)) {
            ShowWindow(hWnd, SW_RESTORE);
        } else {
            ShowWindow(hWnd, SW_MAXIMIZE);
        }
    }
    bool maxHovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);

    if (maxHovered) {
        drawList->AddRectFilled(maxBtnPos, ImVec2(maxBtnPos.x + btnSize, maxBtnPos.y + btnSize), IM_COL32(51, 56, 71, 255), 9.0f * g_dpiScale);
    }
    float boxPad = 12.0f * g_dpiScale;
    drawList->AddRect(ImVec2(maxBtnPos.x + boxPad, maxBtnPos.y + boxPad),
                      ImVec2(maxBtnPos.x + btnSize - boxPad, maxBtnPos.y + btnSize - boxPad),
                      IM_COL32(220, 220, 230, 255), 1.0f, 0, 1.8f * g_dpiScale);
    curRightX += btnSize + btnGap;

    // Close (✕)
    ImGui::SetCursorPos(ImVec2(curRightX, sysY));
    ImVec2 closeBtnPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("##btn_close", ImVec2(btnSize, btnSize))) {
        PostMessage(hWnd, WM_CLOSE, 0, 0);
    }
    bool closeHovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);

    if (closeHovered) {
        drawList->AddRectFilled(closeBtnPos, ImVec2(closeBtnPos.x + btnSize, closeBtnPos.y + btnSize), IM_COL32(217, 38, 46, 255), 9.0f * g_dpiScale);
    }
    float crossPad = 13.0f * g_dpiScale;
    drawList->AddLine(ImVec2(closeBtnPos.x + crossPad, closeBtnPos.y + crossPad),
                      ImVec2(closeBtnPos.x + btnSize - crossPad, closeBtnPos.y + btnSize - crossPad),
                      IM_COL32(220, 220, 230, 255), 1.8f * g_dpiScale);
    drawList->AddLine(ImVec2(closeBtnPos.x + btnSize - crossPad, closeBtnPos.y + crossPad),
                      ImVec2(closeBtnPos.x + crossPad, closeBtnPos.y + btnSize - crossPad),
                      IM_COL32(220, 220, 230, 255), 1.8f * g_dpiScale);

    ImGui::SetCursorPos(ImVec2(16.0f * g_dpiScale, headerHeight + 10.0f * g_dpiScale));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::End();
}



// Render Peer Card matching user sketch (with reactive cardWidth and unified vertical alignment)
static void RenderPeerCard(Peer& peer, int index, float cardWidth) {
    ImGui::PushID(index);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 cardPos = ImGui::GetCursorScreenPos();
    float cardHeight = 58.0f * g_dpiScale;
    ImVec2 cardEnd = ImVec2(cardPos.x + cardWidth, cardPos.y + cardHeight);

    // Card background with rounded outline
    bool cardHovered = ImGui::IsMouseHoveringRect(cardPos, cardEnd);
    ImU32 cardBg = cardHovered ? IM_COL32(28, 30, 38, 255) : IM_COL32(24, 25, 30, 255);
    ImU32 cardBorder = cardHovered ? IM_COL32(75, 78, 98, 255) : IM_COL32(58, 59, 69, 255);

    drawList->AddRectFilled(cardPos, cardEnd, cardBg, 10.0f * g_dpiScale);
    drawList->AddRect(cardPos, cardEnd, cardBorder, 10.0f * g_dpiScale, 0, 1.2f);

    // Requirement 1: Perfect vertical centering across ALL elements (badges, star, buttons)
    float elemHeight = 34.0f * g_dpiScale;
    float elemY = cardPos.y + (cardHeight - elemHeight) * 0.5f;

    // Inside elements
    // Badge 1: [ 100.113.254.69 | name : pc poupou ]
    std::string badgeText = peer.ip + " | name : " + peer.name;
    ImVec2 textSize = ImGui::CalcTextSize(badgeText.c_str());
    float badgePadX = 14.0f * g_dpiScale;
    float badgeWidth = textSize.x + badgePadX * 2.0f;

    ImVec2 badge1Pos = ImVec2(cardPos.x + 12.0f * g_dpiScale, elemY);
    ImVec2 badge1End = ImVec2(badge1Pos.x + badgeWidth, elemY + elemHeight);

    drawList->AddRectFilled(badge1Pos, badge1End, IM_COL32(18, 19, 23, 255), 8.0f * g_dpiScale);
    drawList->AddRect(badge1Pos, badge1End, IM_COL32(50, 52, 62, 255), 8.0f * g_dpiScale, 0, 1.0f);

    // Draw text inside badge 1 perfectly centered vertically
    float textY = elemY + (elemHeight - textSize.y) * 0.5f;
    drawList->AddText(ImVec2(badge1Pos.x + badgePadX, textY), IM_COL32(240, 240, 248, 255), badgeText.c_str());

    // Badge 2: [ status : offline ] / [ status : waiting ] / [ status : online ]
    const char* statusStr = "offline";
    ImU32 statusCol = IM_COL32(255, 85, 85, 255); // Red

    if (peer.status == PeerStatus::Online) {
        statusStr = "online";
        statusCol = IM_COL32(80, 250, 123, 255); // Green
    } else if (peer.status == PeerStatus::Waiting) {
        statusStr = "waiting";
        statusCol = IM_COL32(255, 204, 0, 255); // Yellow
    }
    
    std::string statusBadgeText = std::string("status : ") + statusStr;
    ImVec2 statusTextSize = ImGui::CalcTextSize(statusBadgeText.c_str());
    float statusBadgeWidth = statusTextSize.x + 32.0f * g_dpiScale;

    ImVec2 statusBadgePos = ImVec2(badge1End.x + 10.0f * g_dpiScale, elemY);
    ImVec2 statusBadgeEnd = ImVec2(statusBadgePos.x + statusBadgeWidth, elemY + elemHeight);

    drawList->AddRectFilled(statusBadgePos, statusBadgeEnd, IM_COL32(18, 19, 23, 255), 8.0f * g_dpiScale);
    drawList->AddRect(statusBadgePos, statusBadgeEnd, IM_COL32(50, 52, 62, 255), 8.0f * g_dpiScale, 0, 1.0f);

    // Status indicator dot centered vertically at elemY + elemHeight * 0.5f
    ImVec2 dotCenter = ImVec2(statusBadgePos.x + 13.0f * g_dpiScale, elemY + elemHeight * 0.5f);
    IconManager::DrawStatusIndicator(drawList, dotCenter, 4.0f * g_dpiScale, peer.status, true);

    // Status text colored and centered vertically
    float statusTextY = elemY + (elemHeight - statusTextSize.y) * 0.5f;
    drawList->AddText(ImVec2(statusBadgePos.x + 23.0f * g_dpiScale, statusTextY), statusCol, statusBadgeText.c_str());

    // Clickable status badge to simulate / toggle handshake when testing
    ImGui::SetCursorScreenPos(statusBadgePos);
    std::string badgeBtnId = "##status_btn_" + std::to_string(index);
    if (ImGui::InvisibleButton(badgeBtnId.c_str(), ImVec2(statusBadgeWidth, elemHeight))) {
        if (peer.status == PeerStatus::Waiting) {
            peer.status = PeerStatus::Online;
            g_state.isStreaming = true;
            SavePeers(g_state);
        } else if (peer.status == PeerStatus::Online) {
            peer.status = PeerStatus::Offline;
            if (g_state.activeConnectedIp == peer.ip) {
                g_state.activeConnectedIp.clear();
                g_state.isStreaming = false;
            }
            SavePeers(g_state);
        }
    }
    if (ImGui::IsItemHovered()) {
        if (peer.status == PeerStatus::Waiting) {
            ImGui::SetTooltip("Waiting for P2P handshake (click to simulate accepted handshake)");
        } else if (peer.status == PeerStatus::Online) {
            ImGui::SetTooltip("P2P stream online & active (click to disconnect)");
        } else {
            ImGui::SetTooltip("Peer is currently offline / unreachable");
        }
    }

    // Favorite Star Button [ ★ ] / [ ☆ ]
    float starBtnSize = elemHeight;
    ImVec2 starBtnPos = ImVec2(statusBadgeEnd.x + 10.0f * g_dpiScale, elemY);
    ImGui::SetCursorScreenPos(starBtnPos);
    
    std::string starBtnId = "##star_" + std::to_string(index);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(18, 19, 23, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(35, 37, 46, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(50, 52, 62, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);

    if (ImGui::Button(starBtnId.c_str(), ImVec2(starBtnSize, starBtnSize))) {
        peer.isFavorite = !peer.isFavorite;
        SavePeers(g_state);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    // Draw crisp star icon centered over the button using physical SVG
    ImVec2 starCenter = ImVec2(starBtnPos.x + starBtnSize * 0.5f, elemY + elemHeight * 0.5f);
    ImU32 starColor = peer.isFavorite ? IM_COL32(255, 204, 0, 255) : IM_COL32(130, 134, 150, 255);
    const char* starSvg = peer.isFavorite ? "MaterialSymbolsLightStarRate_full.svg" : "MaterialSymbolsLightStarOutlineRounded_empty.svg";
    IconManager::Get().DrawSvgIcon(drawList, starSvg, starCenter, 20.0f * g_dpiScale, starColor);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(peer.isFavorite ? "Remove from favorites" : "Add to favorites (moves to top)");
    }

    // Action buttons on the right side: [ connect / disconnect ] [ rename ] [ ✕ ]
    bool isConnected = (g_state.isStreaming && g_state.activeConnectedIp == peer.ip && peer.status == PeerStatus::Online);
    bool isWaiting = (peer.status == PeerStatus::Waiting);

    float btnConnWidth = 96.0f * g_dpiScale;
    float btnRenameWidth = 76.0f * g_dpiScale;
    float btnOptWidth = elemHeight;
    float btnDelWidth = elemHeight;
    float spacing = 8.0f * g_dpiScale;
    float actionsWidth = btnConnWidth + spacing + btnRenameWidth + spacing + btnOptWidth + spacing + btnDelWidth;
    float rightEdge = cardPos.x + cardWidth - 12.0f * g_dpiScale;

    // Requirement 1 & 2: [ connect ] / [ disconnect ] Button
    ImGui::SetCursorScreenPos(ImVec2(rightEdge - actionsWidth, elemY));
    std::string connBtnId = "conn_btn##" + std::to_string(index);

    ImVec4 connCol = isConnected ? ImVec4(0.48f, 0.16f, 0.20f, 1.0f) : (isWaiting ? ImVec4(0.28f, 0.22f, 0.08f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    ImVec4 connColHover = isConnected ? ImVec4(0.62f, 0.20f, 0.25f, 1.0f) : (isWaiting ? ImVec4(0.38f, 0.30f, 0.10f, 1.0f) : ImVec4(0.36f, 0.14f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, connCol);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, connColHover);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_dpiScale, 6.0f * g_dpiScale));

    const char* connText = (isConnected || isWaiting) ? "disconnect" : "connect";
    std::string connLabel = std::string(connText) + "##conn_btn_" + std::to_string(index);
    if (ImGui::Button(connLabel.c_str(), ImVec2(btnConnWidth, elemHeight))) {
        if (isConnected || isWaiting) {
            if (g_state.activeConnectedIp == peer.ip) {
                g_state.activeConnectedIp.clear();
                g_state.isStreaming = false;
            }
            peer.status = PeerStatus::Offline;
            SavePeers(g_state);
        } else {
            // Requirement 1: Do NOT change tab to CurrentConnection! Stay on Home!
            peer.status = PeerStatus::Waiting;
            g_state.activeConnectedIp = peer.ip;
            g_state.isStreaming = false;
            SavePeers(g_state);
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // [ rename ] Button
    ImGui::SetCursorScreenPos(ImVec2(rightEdge - actionsWidth + btnConnWidth + spacing, elemY));
    std::string renameBtnId = "rename##peer_ren_" + std::to_string(index);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * g_dpiScale, 6.0f * g_dpiScale));
    if (ImGui::Button(renameBtnId.c_str(), ImVec2(btnRenameWidth, elemHeight))) {
        g_state.showRenameModal = true;
        g_state.renamePeerIndex = index;
        strncpy_s(g_state.renameBuffer, peer.name.c_str(), sizeof(g_state.renameBuffer) - 1);
    }
    ImGui::PopStyleVar(2);

    // [ SolarMenuDotsSquareBold ] Settings & Hotkeys Button (Requirement 2)
    float optBtnX = rightEdge - btnDelWidth - spacing - btnOptWidth;
    ImGui::SetCursorScreenPos(ImVec2(optBtnX, elemY));
    std::string optBtnId = "##peer_opts_btn_" + std::to_string(index);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(18, 19, 23, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 44, 56, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(50, 52, 62, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);

    if (ImGui::Button(optBtnId.c_str(), ImVec2(btnOptWidth, elemHeight))) {
        ImGui::OpenPopup(("##peer_opts_popup_" + std::to_string(index)).c_str());
    }
    bool isOptHovered = ImGui::IsItemHovered();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImVec2 optCenter(optBtnX + btnOptWidth * 0.5f, elemY + elemHeight * 0.5f);
    ImU32 optColor = isOptHovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(180, 185, 205, 255);
    IconManager::Get().DrawSvgIcon(drawList, "SolarMenuDotsSquareBold.svg", optCenter, 20.0f * g_dpiScale, optColor);

    if (isOptHovered) {
        ImGui::SetTooltip("Paramètres individuels & raccourcis (Settings)");
    }

    // Popup: Options individuelles et raccourcis personnalisés
    if (ImGui::BeginPopup(("##peer_opts_popup_" + std::to_string(index)).c_str())) {
        ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "Options de %s", peer.name.c_str());
        ImGui::TextDisabled("IP: %s", peer.ip.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "Raccourcis personnalisés (Hotkeys):");
        ImGui::SetNextItemWidth(140.0f * g_dpiScale);
        if (ImGui::InputText("Focus direct", peer.focusHotkey, sizeof(peer.focusHotkey))) {
            SavePeers(g_state);
        }
        ImGui::SetNextItemWidth(140.0f * g_dpiScale);
        if (ImGui::InputText("Mute rapide", peer.muteHotkey, sizeof(peer.muteHotkey))) {
            SavePeers(g_state);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "Réglages Audio & Flux:");
        ImGui::SetNextItemWidth(140.0f * g_dpiScale);
        if (ImGui::SliderFloat("Volume stream", &peer.volume, 0.0f, 1.5f, "%.0f%%")) {
            SavePeers(g_state);
        }

        if (ImGui::Checkbox("Couper le son (Mute)", &peer.isMuted)) {
            SavePeers(g_state);
        }
        if (ImGui::Checkbox("Rendre sourd (Deafen)", &peer.isDeafened)) {
            SavePeers(g_state);
        }
        if (ImGui::Checkbox("Masquer le flux vidéo", &peer.isStreamHidden)) {
            SavePeers(g_state);
        }
        if (ImGui::Checkbox("Priorité audio (Ducking)", &peer.audioPriority)) {
            SavePeers(g_state);
        }

        ImGui::Spacing();
        const char* qItems[] = { "1080p60 (Full HD)", "720p60 (Fluide)", "1080p30 (Éco)" };
        ImGui::SetNextItemWidth(150.0f * g_dpiScale);
        if (ImGui::Combo("Qualité cible", &peer.streamQuality, qItems, IM_ARRAYSIZE(qItems))) {
            SavePeers(g_state);
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::MenuItem(peer.isFavorite ? "Retirer des favoris" : "Ajouter aux favoris")) {
            peer.isFavorite = !peer.isFavorite;
            SavePeers(g_state);
        }

        ImGui::EndPopup();
    }

    // [ ✕ ] Delete Button
    ImGui::SetCursorScreenPos(ImVec2(rightEdge - btnDelWidth, elemY));
    std::string delBtnId = "X##del_btn_" + std::to_string(index);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.18f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);
    if (ImGui::Button(delBtnId.c_str(), ImVec2(btnDelWidth, elemHeight))) {
        g_state.showDeleteModal = true;
        g_state.deletePeerIndex = index;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    // Advance cursor past the card and register space with ImGui
    ImGui::SetCursorScreenPos(ImVec2(cardPos.x, cardPos.y + cardHeight + 10.0f * g_dpiScale));
    ImGui::Dummy(ImVec2(cardWidth, 0.0f));

    ImGui::PopID();
}

// Home View strictly following the user's sketch (Reactive dimensions & perfectly centered!)
static void RenderHomeView(float contentWidth, float contentHeight) {
    float padTop = 18.0f * g_dpiScale;
    float addBoxHeight = 52.0f * g_dpiScale;
    float gapY = 16.0f * g_dpiScale;
    float padBottom = 22.0f * g_dpiScale;

    // 1. Proportions and horizontal centering
    float maxContainerWidth = 920.0f * g_dpiScale;
    float containerWidth = (contentWidth < maxContainerWidth + 48.0f * g_dpiScale) ? (contentWidth - 48.0f * g_dpiScale) : maxContainerWidth;
    float containerStartX = (contentWidth - containerWidth) * 0.5f;

    float addBarWidth = 760.0f * g_dpiScale;
    if (addBarWidth > containerWidth) addBarWidth = containerWidth;
    float addBarStartX = (contentWidth - addBarWidth) * 0.5f;

    // Position of the top add bar
    float startY = 84.0f * g_dpiScale + padTop;
    ImGui::SetCursorPos(ImVec2(addBarStartX, startY));

    // Container box around the add bar
    ImVec2 addBoxPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(addBoxPos, ImVec2(addBoxPos.x + addBarWidth, addBoxPos.y + addBoxHeight),
                            IM_COL32(20, 21, 26, 255), 10.0f * g_dpiScale);
    drawList->AddRect(addBoxPos, ImVec2(addBoxPos.x + addBarWidth, addBoxPos.y + addBoxHeight),
                      IM_COL32(58, 59, 69, 255), 10.0f * g_dpiScale, 0, 1.0f);

    float inputElemHeight = 34.0f * g_dpiScale;
    float inputElemY = addBoxPos.y + (addBoxHeight - inputElemHeight) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(addBoxPos.x + 12.0f * g_dpiScale, inputElemY));

    // Calculate proportions
    float addBtnWidth = 84.0f * g_dpiScale;
    float spaceBetween = 8.0f * g_dpiScale;
    float innerTotalWidth = addBarWidth - 24.0f * g_dpiScale;
    float inputsTotalWidth = innerTotalWidth - addBtnWidth - spaceBetween * 2.0f;

    // IP Tailscale input (54% of inputs space), Name input (46% of inputs space)
    float ipInputWidth = inputsTotalWidth * 0.54f;
    float nameInputWidth = inputsTotalWidth * 0.46f;

    ImGui::PushItemWidth(ipInputWidth);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_dpiScale, 7.0f * g_dpiScale));
    ImGui::InputTextWithHint("##iptailscale", "ip tailscale (e.g. 100.113.254.69)", g_state.inputIp, sizeof(g_state.inputIp));
    ImGui::PopItemWidth();

    // [ name ] input
    ImGui::SameLine(0, spaceBetween);
    ImGui::PushItemWidth(nameInputWidth);
    ImGui::InputTextWithHint("##peername", "name (e.g. pc poupou)", g_state.inputName, sizeof(g_state.inputName));
    ImGui::PopItemWidth();

    // [ add ] button
    ImGui::SameLine(0, spaceBetween);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.14f, 0.18f, 1.0f)); // Wine hover
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.36f, 0.42f, 1.0f));

    if (ImGui::Button("add", ImVec2(addBtnWidth, inputElemHeight)) || 
        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) 
    {
        if (strlen(g_state.inputIp) > 0) {
            Peer newPeer;
            newPeer.ip = g_state.inputIp;
            newPeer.name = (strlen(g_state.inputName) > 0) ? g_state.inputName : "peer";
            newPeer.status = PeerStatus::Offline;
            newPeer.isFavorite = false;
            newPeer.latencyMs = 20;

            g_state.peers.push_back(newPeer);
            SavePeers(g_state);

            g_state.inputIp[0] = '\0';
            g_state.inputName[0] = '\0';
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    // 2. Large Rounded Container for Saved Connections (Centered & Harmonious!)
    float containerY = startY + addBoxHeight + gapY;
    float totalWindowHeight = contentHeight + 84.0f * g_dpiScale;
    float containerHeight = totalWindowHeight - containerY - padBottom;
    if (containerHeight < 160.0f * g_dpiScale) containerHeight = 160.0f * g_dpiScale;

    ImGui::SetCursorPos(ImVec2(containerStartX, containerY));
    ImVec2 containerScreenMin = ImGui::GetCursorScreenPos();
    ImVec2 containerScreenMax = ImVec2(containerScreenMin.x + containerWidth, containerScreenMin.y + containerHeight);

    drawList->AddRectFilled(containerScreenMin, containerScreenMax, IM_COL32(19, 20, 24, 255), 14.0f * g_dpiScale);
    drawList->AddRect(containerScreenMin, containerScreenMax, IM_COL32(58, 59, 69, 255), 14.0f * g_dpiScale, 0, 1.2f);

    // Inside child scroll area
    float innerPadX = 14.0f * g_dpiScale;
    float innerPadY = 14.0f * g_dpiScale;
    ImGui::SetCursorPos(ImVec2(containerStartX + innerPadX, containerY + innerPadY));

    ImVec2 childSize = ImVec2(containerWidth - innerPadX * 2.0f, containerHeight - innerPadY * 2.0f);
    ImGui::BeginChild("##peer_cards_scroll", childSize, false, 0);

    // Sort peers: favorites first
    static std::vector<int> sortedIndices;
    if (sortedIndices.size() != g_state.peers.size()) {
        sortedIndices.resize(g_state.peers.size());
    }
    for (size_t i = 0; i < g_state.peers.size(); ++i) sortedIndices[i] = (int)i;

    std::stable_sort(sortedIndices.begin(), sortedIndices.end(), [](int a, int b) {
        if (g_state.peers[a].isFavorite != g_state.peers[b].isFavorite) {
            return g_state.peers[a].isFavorite > g_state.peers[b].isFavorite;
        }
        return false;
    });

    for (int idx : sortedIndices) {
        float cardWidth = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(idx);
        RenderPeerCard(g_state.peers[idx], idx, cardWidth);
        ImGui::PopID();
    }

    if (g_state.peers.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 30.0f * g_dpiScale);
        ImGui::TextDisabled("No Tailscale peers registered yet. Add one with [ ip tailscale ] above.");
    }

    ImGui::EndChild();
}

// Requirement 1: Fill / Cover video helper ensuring 100% cell surface coverage in Focus Mode
static void DrawVideoCover(ImDrawList* drawList, ImTextureID texture, ImVec2 boxMin, ImVec2 boxMax, float srcAspect = 16.0f / 9.0f) {
    float boxW = boxMax.x - boxMin.x;
    float boxH = boxMax.y - boxMin.y;
    if (boxW <= 0.0f || boxH <= 0.0f) return;

    if (!texture) {
        drawList->AddRectFilled(boxMin, boxMax, IM_COL32(11, 12, 16, 255));
        return;
    }

    // Cover: fill 100% of [boxMin, boxMax]
    float boxAspect = boxW / boxH;
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);

    if (boxAspect > srcAspect) {
        // Box is wider than 16:9 => crop top and bottom
        float visibleH = srcAspect / boxAspect;
        float cropY = (1.0f - visibleH) * 0.5f;
        uv0.y = cropY;
        uv1.y = 1.0f - cropY;
    } else {
        // Box is taller than 16:9 => crop left and right
        float visibleW = boxAspect / srcAspect;
        float cropX = (1.0f - visibleW) * 0.5f;
        uv0.x = cropX;
        uv1.x = 1.0f - cropX;
    }

    drawList->AddImage(texture, boxMin, boxMax, uv0, uv1, IM_COL32_WHITE);
}

// Helper: Aggressively sanitize any stream / peer name to eliminate any residual "::", ":::", or leading colons/spaces
static std::string CleanStreamTitle(const std::string& rawName) {
    size_t start = 0;
    while (start < rawName.size() && (rawName[start] == ':' || rawName[start] == ' ' || rawName[start] == '\t')) {
        start++;
    }
    return (start < rawName.size()) ? rawName.substr(start) : rawName;
}


// Requirement 2: Multi-stream grid cell video helper:
// Maximizes tile space without deforming ratio (no stretch) and without excessive zoom that cuts content
static void DrawVideoAspectFit(ImDrawList* drawList, ImTextureID texture, ImVec2 boxMin, ImVec2 boxMax, float srcAspect = 16.0f / 9.0f) {
    float boxW = boxMax.x - boxMin.x;
    float boxH = boxMax.y - boxMin.y;
    if (boxW <= 0.0f || boxH <= 0.0f) return;

    // Sleek dark tile background (#12141A)
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 20, 26, 255));

    if (!texture) return;

    // Aspect fit: preserves 16:9 ratio, fills maximum available cell space along the limiting axis,
    // centered in cell, zero stretch, zero cut off (100% of video content visible)
    float boxAspect = boxW / boxH;
    float renderW, renderH;

    if (boxAspect > srcAspect) {
        renderH = boxH;
        renderW = boxH * srcAspect;
    } else {
        renderW = boxW;
        renderH = boxW / srcAspect;
    }

    float posX = boxMin.x + (boxW - renderW) * 0.5f;
    float posY = boxMin.y + (boxH - renderH) * 0.5f;

    ImVec2 imgMin(posX, posY);
    ImVec2 imgMax(posX + renderW, posY + renderH);

    drawList->AddImage(texture, imgMin, imgMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE);
}

// Secondary View: Current Connection (Live Screen, Adaptive Grid Layout, Focus Mode, Resizable Splitters, and Collaborative Dock)
// Edge-to-edge full bleed rendering strictly following user sketch
static void RenderCurrentConnectionView(float windowWidth, float windowHeight) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImDrawList* fgDrawList = ImGui::GetForegroundDrawList();
    ImGuiIO& io = ImGui::GetIO();

    // Zone boundaries strictly adhering to Requirement 2:
    // Video canvas covers 100% of available window (0, 0) to (windowWidth, windowHeight)
    // without black borders or letterbox padding (ImGuiStyleVar_WindowPadding = 0, WindowBorderSize = 0)
    float headerHeight = 84.0f * g_dpiScale;
    float bottomBarHeight = 80.0f * g_dpiScale;
    float centralYMin = 0.0f;
    float centralYMax = windowHeight;
    float centralH = windowHeight;
    float centralW = windowWidth;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    // 1. Central Stream canvas covering strictly (0, 0) to (windowWidth, windowHeight)
    ImVec2 streamMin = ImVec2(0.0f, 0.0f);
    ImVec2 streamMax = ImVec2(windowWidth, windowHeight);
    drawList->AddRectFilled(streamMin, streamMax, IM_COL32(11, 12, 16, 255));

    // Dockbar dimensions for overlay & boundary calculations
    float btnW = 46.0f * g_dpiScale;
    float btnH = 40.0f * g_dpiScale;
    float btnSpacing = 8.0f * g_dpiScale;
    float btnEndW = 52.0f * g_dpiScale;
    float dockInnerW = (btnW * 5.0f + btnEndW + btnSpacing * 5.0f);
    float dockPadX = 14.0f * g_dpiScale;
    float dockWidth = dockInnerW + dockPadX * 2.0f;
    float dockHeight = 56.0f * g_dpiScale;
    ImVec2 dockMin = ImVec2((windowWidth - dockWidth) * 0.5f, windowHeight - dockHeight - 24.0f * g_dpiScale);
    ImVec2 dockMax = ImVec2(dockMin.x + dockWidth, dockMin.y + dockHeight);

    bool overDock = (io.MousePos.y >= dockMin.y - 12.0f * g_dpiScale && 
                     io.MousePos.y <= dockMax.y + 12.0f * g_dpiScale &&
                     io.MousePos.x >= dockMin.x - 12.0f * g_dpiScale && 
                     io.MousePos.x <= dockMax.x + 12.0f * g_dpiScale);
    bool overHeader = (io.MousePos.y <= headerHeight);

    // Gather active session participants (for Voice Only mode)
    struct ParticipantBubble {
        std::string name;
        std::string role;
        PeerStatus status;
        bool isSpeaking;
        bool isMe;
        Peer* peerPtr = nullptr;
    };
    std::vector<ParticipantBubble> participants;
    participants.push_back({ "me", "local host", PeerStatus::Online, g_state.isMicSpeaking, true, nullptr });
    for (auto& peer : g_state.peers) {
        if (peer.status == PeerStatus::Online || (!g_state.activeConnectedIp.empty() && g_state.activeConnectedIp == peer.ip)) {
            participants.push_back({ peer.name, "live stream", peer.status, peer.isSpeaking, false, &peer });
        }
    }

    // =========================================================================
    // Active Streams Collection (Excluding hidden streams)
    // =========================================================================
    struct ActiveStream {
        std::string name;
        std::string role;
        bool isMe;
        Peer* peerPtr = nullptr;
        bool isSpeaking = false;
    };
    std::vector<ActiveStream> streamList;

    if (g_state.isStreaming) {
        streamList.push_back({ "me", "local host", true, nullptr, g_state.isMicSpeaking });
    }
    for (auto& peer : g_state.peers) {
        if (peer.status == PeerStatus::Online && !peer.isStreamHidden) {
            streamList.push_back({ CleanStreamTitle(peer.name), "remote screen", false, &peer, peer.isSpeaking });
        }
    }

#if defined(_DEBUG) || !defined(NDEBUG)
    if (g_debug.simulatedStreamCount >= 0) {
        int simNeeded = g_debug.simulatedStreamCount;
        streamList.clear();
        for (auto& peer : g_state.peers) {
            if (peer.status == PeerStatus::Online && !peer.isStreamHidden) {
                if ((int)streamList.size() < simNeeded) {
                    streamList.push_back({ CleanStreamTitle(peer.name), "remote screen", false, &peer, peer.isSpeaking });
                }
            }
        }
        int fakeIdx = 1;
        while ((int)streamList.size() < simNeeded) {
            std::string sName = (fakeIdx == 1) ? "Helldivers Stream" : ("Squad Stream #" + std::to_string(fakeIdx));
            streamList.push_back({ CleanStreamTitle(sName), "simulated 1080p", false, nullptr, false });
            fakeIdx++;
        }
    }
#endif

    ID3D11ShaderResourceView* defaultAvatarTex = IconManager::Get().GetImageTexture("ext/img/default profile picture.png");
    int streamTexW = 0, streamTexH = 0;
    ID3D11ShaderResourceView* streamTex = IconManager::Get().GetImageTexture("ext/img/helldivers-2-1_33b62d4e81ea4ef68c12cba0363065df-4243320100.jpg", &streamTexW, &streamTexH);
    float streamAspect = (streamTexW > 0 && streamTexH > 0) ? ((float)streamTexW / (float)streamTexH) : (16.0f / 9.0f);

    int streamCount = (int)streamList.size();

    // Auto-unfocus if transitioning from 1 stream to multiple streams
    static int s_lastStreamCount = 1;
    if (s_lastStreamCount == 1 && streamCount > 1) {
        g_state.focusedStreamIndex = -1;
    }
    s_lastStreamCount = streamCount;

    // Helper: Unified layout reorganization for Split buttons and Context Menu (Dynamic for 2, 3, 4+ streams)
    auto ApplySplitLayout = [&](int mode) {
        g_state.focusedStreamIndex = -1; // Déverrouillage immédiat du mode Focus
        g_state.gridLayoutMode = mode;
        int cols = 1, rows = 1;
        if (mode == 2) {
            g_state.colSplitRow[0] = 0.5f; // Split 50% horizontal (lignes empilées)
            if (streamCount <= 2) { cols = 1; rows = 2; }
            else if (streamCount == 3) { cols = 1; rows = 3; }
            else if (streamCount <= 4) { cols = 2; rows = 2; }
            else if (streamCount <= 6) { cols = 2; rows = 3; }
            else { cols = 2; rows = (streamCount + 1) / 2; }
        } else if (mode == 1 || mode == 0) {
            g_state.rowSplitCol[0] = 0.5f; // Split 50% vertical (colonnes / grille)
            if (streamCount <= 2) { cols = 2; rows = 1; }
            else if (streamCount <= 4) { cols = 2; rows = 2; }
            else if (streamCount <= 6) { cols = 3; rows = 2; }
            else { cols = 4; rows = (streamCount + 3) / 4; }
        }
        int newSlots = cols * rows;
        std::vector<int> newOrder(newSlots, -1);
        for (int s = 0; s < streamCount && s < newSlots; ++s) {
            newOrder[s] = s;
        }
        g_state.streamOrder = newOrder;
    };

    // Requirement 3: Split capsule [ ⊟ ] [ ◫ ] in top-right of video canvas, positioned strictly above volume control
    float sBtnW = 32.0f * g_dpiScale;
    float sBtnH = 28.0f * g_dpiScale;
    float sGap  = 4.0f * g_dpiScale;
    float sPadX = 6.0f * g_dpiScale;
    float sPadY = 5.0f * g_dpiScale;
    float sPillW = sBtnW * 2.0f + sGap + sPadX * 2.0f;
    float sPillH = sBtnH + sPadY * 2.0f;
    float sPillX = windowWidth - 14.0f * g_dpiScale - sPillW;
    float sPillY = headerHeight + 10.0f * g_dpiScale;
    float volBtnSize = 28.0f * g_dpiScale;
    bool overSplitArea = (streamCount > 1 &&
                          io.MousePos.x >= sPillX - 10.0f * g_dpiScale && io.MousePos.x <= windowWidth &&
                          io.MousePos.y >= sPillY - 6.0f * g_dpiScale && io.MousePos.y <= sPillY + sPillH + volBtnSize + 18.0f * g_dpiScale);

    // =========================================================================
    // Dynamic Grid Engine & Focus Mode
    // =========================================================================
    if (streamCount > 0) {
        // Requirement 2: Edge-to-edge video canvas without restrictive black borders
        drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(windowWidth, windowHeight), false);

        // Determine grid dimensions dynamically for 2, 3, 4+ streams
        int cols = 1, rows = 1;
        if (streamCount == 1) {
            // Requirement 3: If 1 stream, automatically pass layout to 1x1 mode and force Focus Mode
            cols = 1;
            rows = 1;
            g_state.gridLayoutMode = 0;
            g_state.focusedStreamIndex = 0;
            g_state.streamOrder = { 0 };
        } else if (g_state.gridLayoutMode == 2) {
            // Horizontal split / Row priority [ ⊟ ] (2, 3, 4+ streams)
            if (streamCount <= 2) { cols = 1; rows = 2; }
            else if (streamCount == 3) { cols = 1; rows = 3; }
            else if (streamCount <= 4) { cols = 2; rows = 2; }
            else if (streamCount <= 6) { cols = 2; rows = 3; }
            else { cols = 2; rows = (streamCount + 1) / 2; }
        } else if (g_state.gridLayoutMode == 1) {
            // Vertical split / Col priority [ ◫ ] (2, 3, 4+ streams)
            if (streamCount <= 2) { cols = 2; rows = 1; }
            else if (streamCount <= 4) { cols = 2; rows = 2; }
            else if (streamCount <= 6) { cols = 3; rows = 2; }
            else { cols = 4; rows = (streamCount + 3) / 4; }
        } else if (g_state.gridLayoutMode == 3) {
            cols = 2; rows = 2; // 2x2 grid (4 slots)
        } else if (g_state.gridLayoutMode == 4) {
            cols = 3; rows = 2; // 3x2 grid (6 slots)
        } else {
            // Auto grid layout based on streamCount
            if (streamCount <= 1) {
                cols = 1; rows = 1;
            } else if (streamCount == 2) {
                cols = 2; rows = 1; // 2 colonnes côte à côte
            } else if (streamCount <= 4) {
                cols = 2; rows = 2;
            } else if (streamCount <= 6) {
                cols = 3; rows = 2;
            } else {
                cols = 4; rows = 2;
            }
        }
        int totalSlots = cols * rows;

        // Synchronize streamOrder with totalSlots
        if ((int)g_state.streamOrder.size() != totalSlots) {
            std::vector<bool> streamAssigned(streamCount, false);
            std::vector<int> newOrder(totalSlots, -1);
            int copyCount = (int)fminf((float)g_state.streamOrder.size(), (float)totalSlots);
            for (int i = 0; i < copyCount; ++i) {
                int idx = g_state.streamOrder[i];
                if (idx >= 0 && idx < streamCount && !streamAssigned[idx]) {
                    newOrder[i] = idx;
                    streamAssigned[idx] = true;
                }
            }
            for (int s = 0; s < streamCount; ++s) {
                if (!streamAssigned[s]) {
                    for (int k = 0; k < totalSlots; ++k) {
                        if (newOrder[k] == -1) {
                            newOrder[k] = s;
                            streamAssigned[s] = true;
                            break;
                        }
                    }
                }
            }
            g_state.streamOrder = newOrder;
        } else {
            for (int i = 0; i < totalSlots; ++i) {
                if (g_state.streamOrder[i] >= streamCount) {
                    g_state.streamOrder[i] = -1;
                }
            }
        }

        // Clamp focused index (Enforce focus on stream 0 if only 1 stream)
        if (streamCount == 1) {
            g_state.focusedStreamIndex = 0;
            if (g_state.streamOrder.empty() || g_state.streamOrder[0] < 0) {
                g_state.streamOrder = { 0 };
            }
        } else if (g_state.focusedStreamIndex >= totalSlots) {
            g_state.focusedStreamIndex = -1;
        } else if (g_state.focusedStreamIndex >= 0 && g_state.streamOrder[g_state.focusedStreamIndex] < 0) {
            g_state.focusedStreamIndex = -1;
        }

        if (g_state.focusedStreamIndex >= 0) {
            // =====================================================================
            // FOCUS MODE: Single stream occupying 100% of the screen (Edge-to-Edge)
            // =====================================================================
            int focusSlot = g_state.focusedStreamIndex;
            int streamIdx = (focusSlot < (int)g_state.streamOrder.size()) ? g_state.streamOrder[focusSlot] : 0;
            if (streamIdx < 0 || streamIdx >= streamCount) streamIdx = 0;
            const auto& stream = streamList[streamIdx];

            // Requirement 1 & 2: Dynamic 16:9 video aspect ratio without stretch or freeze on resize
            DrawVideoAspectFit(drawList, (ImTextureID)streamTex, ImVec2(0.0f, 0.0f), ImVec2(windowWidth, windowHeight), streamAspect);

            // Requirement 4: Fluid exit from Focus Mode (Click anywhere on video or press Escape)
            if (streamCount > 1) {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    g_state.focusedStreamIndex = -1; // Escape key exits Focus Mode!
                }

                ImGui::SetCursorScreenPos(ImVec2(0.0f, headerHeight));
                if (ImGui::InvisibleButton("##focus_stream_hitbox", ImVec2(centralW, centralH - headerHeight - bottomBarHeight))) {
                    if (!overDock && !overSplitArea) {
                        g_state.focusedStreamIndex = -1; // Click on video exits Focus Mode!
                    }
                }
            }

            float focusHeaderY = headerHeight + 12.0f * g_dpiScale;
            float btnH28 = 28.0f * g_dpiScale;
            float leftBadgeX = 24.0f * g_dpiScale;

            // Stream Name Badge (Clean, without grip handle or dots)
            float titlePadX = 10.0f * g_dpiScale;
            std::string fTitle = CleanStreamTitle(stream.name);
            ImVec2 fTextSize = ImGui::CalcTextSize(fTitle.c_str());
            bool isFocusDeaf = (stream.peerPtr && stream.peerPtr->isDeafened) || (stream.isMe && g_state.isAudioDeafened);
            bool isFocusMute = (stream.peerPtr && stream.peerPtr->isMuted) || (stream.isMe && g_state.isMicMuted);
            float fBadgeExtra = (isFocusDeaf || isFocusMute) ? 24.0f * g_dpiScale : 0.0f;

            float fTagW = titlePadX * 2.0f + fTextSize.x + fBadgeExtra;
            ImVec2 fTagMin(leftBadgeX, focusHeaderY);
            ImVec2 fTagMax(fTagMin.x + fTagW, focusHeaderY + btnH28);

            drawList->AddRectFilled(fTagMin, fTagMax, COLOR_CAPSULE_BG, 6.0f * g_dpiScale);
            drawList->AddRect(fTagMin, fTagMax, COLOR_CAPSULE_BORDER, 6.0f * g_dpiScale, 0, 1.0f);

            float textDrawX = fTagMin.x + titlePadX;
            drawList->AddText(ImVec2(textDrawX, fTagMin.y + (btnH28 - fTextSize.y) * 0.5f), IM_COL32(230, 235, 245, 255), fTitle.c_str());

            if (isFocusDeaf || isFocusMute) {
                float bSize = 18.0f * g_dpiScale;
                ImVec2 bMin(textDrawX + fTextSize.x + 6.0f * g_dpiScale, fTagMin.y + (btnH28 - bSize) * 0.5f);
                drawList->AddRectFilled(bMin, ImVec2(bMin.x + bSize, bMin.y + bSize), IM_COL32(235, 48, 58, 255), 4.0f * g_dpiScale);
                const char* bIcon = isFocusDeaf ? "IcBaselineHeadsetOff.svg" : "MdiMicrophoneOff.svg";
                IconManager::Get().DrawSvgIcon(drawList, bIcon, ImVec2(bMin.x + bSize * 0.5f, bMin.y + bSize * 0.5f), 12.0f * g_dpiScale, IM_COL32(255, 255, 255, 255));
            }
            // Requirement 2: Per-stream volume button deleted - only global top-right master volume is kept
        } else {
            // =====================================================================
            // GRID MODE: Adaptive NxM Layout with Free VS Code Docking & Empty Slots
            // =====================================================================
            struct SlotRect {
                ImVec2 min;
                ImVec2 max;
                float w;
                float h;
            };
            std::vector<SlotRect> slots(totalSlots);

            float hitW = 14.0f * g_dpiScale;
            float hitH = 14.0f * g_dpiScale;

            if (cols == 1 && rows == 1) {
                slots[0] = { ImVec2(0.0f, centralYMin), ImVec2(windowWidth, centralYMax), windowWidth, centralH };
            } else if (cols == 2 && rows == 1) {
                float splitX = windowWidth * g_state.rowSplitCol[0];
                slots[0] = { ImVec2(0.0f, centralYMin), ImVec2(splitX, centralYMax), splitX, centralH };
                slots[1] = { ImVec2(splitX, centralYMin), ImVec2(windowWidth, centralYMax), windowWidth - splitX, centralH };

                ImGui::SetCursorScreenPos(ImVec2(splitX - hitW * 0.5f, centralYMin));
                ImGui::InvisibleButton("##grid_splitter_col_0", ImVec2(hitW, centralH));
                bool isHover = ImGui::IsItemHovered();
                bool isActive = ImGui::IsItemActive();
                if (isHover || isActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (isActive && io.MouseDown[0]) {
                    g_state.rowSplitCol[0] = ImClamp(io.MousePos.x / windowWidth, 0.15f, 0.85f);
                }
                if (isHover && io.MouseDoubleClicked[0]) {
                    g_state.rowSplitCol[0] = 0.5f;
                }
                if (isActive || isHover) {
                    ImU32 colLine = isActive ? IM_COL32(110, 170, 255, 255) : IM_COL32(80, 140, 255, 220);
                    drawList->AddLine(ImVec2(splitX, centralYMin), ImVec2(splitX, centralYMax), colLine, (isActive ? 3.0f : 2.0f) * g_dpiScale);
                }
            } else if (cols == 1 && rows == 2) {
                float splitY = centralYMin + centralH * g_state.colSplitRow[0];
                slots[0] = { ImVec2(0.0f, centralYMin), ImVec2(windowWidth, splitY), windowWidth, splitY - centralYMin };
                slots[1] = { ImVec2(0.0f, splitY), ImVec2(windowWidth, centralYMax), windowWidth, centralYMax - splitY };

                ImGui::SetCursorScreenPos(ImVec2(0.0f, splitY - hitH * 0.5f));
                ImGui::InvisibleButton("##grid_splitter_row_0", ImVec2(windowWidth, hitH));
                bool isHover = ImGui::IsItemHovered();
                bool isActive = ImGui::IsItemActive();
                if (isHover || isActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (isActive && io.MouseDown[0]) {
                    g_state.colSplitRow[0] = ImClamp((io.MousePos.y - centralYMin) / centralH, 0.15f, 0.85f);
                }
                if (isHover && io.MouseDoubleClicked[0]) {
                    g_state.colSplitRow[0] = 0.5f;
                }
                if (isActive || isHover) {
                    ImU32 colLine = isActive ? IM_COL32(110, 170, 255, 255) : IM_COL32(80, 140, 255, 220);
                    drawList->AddLine(ImVec2(0.0f, splitY), ImVec2(windowWidth, splitY), colLine, (isActive ? 3.0f : 2.0f) * g_dpiScale);
                }
            } else if (cols == 2 && rows == 2) {
                float splitX0 = windowWidth * g_state.rowSplitCol[0];
                float splitX1 = windowWidth * g_state.rowSplitCol[1];
                float splitY0 = centralYMin + centralH * g_state.colSplitRow[0];
                float splitY1 = centralYMin + centralH * g_state.colSplitRow[1];

                slots[0] = { ImVec2(0.0f, centralYMin), ImVec2(splitX0, splitY0), splitX0, splitY0 - centralYMin };
                slots[1] = { ImVec2(splitX0, centralYMin), ImVec2(windowWidth, splitY1), windowWidth - splitX0, splitY1 - centralYMin };
                slots[2] = { ImVec2(0.0f, splitY0), ImVec2(splitX1, centralYMax), splitX1, centralYMax - splitY0 };
                slots[3] = { ImVec2(splitX1, splitY1), ImVec2(windowWidth, centralYMax), windowWidth - splitX1, centralYMax - splitY1 };

                // 1. Vertical Splitter Row 0 (between slot 0 and slot 1)
                ImGui::SetCursorScreenPos(ImVec2(splitX0 - hitW * 0.5f, centralYMin));
                ImGui::InvisibleButton("##grid_splitter_v_r0", ImVec2(hitW, fmaxf(10.0f, splitY0 - centralYMin)));
                bool isHoverV0 = ImGui::IsItemHovered();
                bool isActiveV0 = ImGui::IsItemActive();
                if (isHoverV0 || isActiveV0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (isActiveV0 && io.MouseDown[0]) {
                    g_state.rowSplitCol[0] = ImClamp(io.MousePos.x / windowWidth, 0.15f, 0.85f);
                }
                if (isHoverV0 && io.MouseDoubleClicked[0]) {
                    g_state.rowSplitCol[0] = 0.5f;
                }
                if (isActiveV0 || isHoverV0) {
                    ImU32 colLineV0 = isActiveV0 ? IM_COL32(110, 170, 255, 255) : IM_COL32(80, 140, 255, 220);
                    drawList->AddLine(ImVec2(splitX0, centralYMin), ImVec2(splitX0, splitY0), colLineV0, (isActiveV0 ? 3.0f : 2.0f) * g_dpiScale);
                }

                // 2. Vertical Splitter Row 1 (between slot 2 and slot 3)
                ImGui::SetCursorScreenPos(ImVec2(splitX1 - hitW * 0.5f, splitY0));
                ImGui::InvisibleButton("##grid_splitter_v_r1", ImVec2(hitW, fmaxf(10.0f, centralYMax - splitY0)));
                bool isHoverV1 = ImGui::IsItemHovered();
                bool isActiveV1 = ImGui::IsItemActive();
                if (isHoverV1 || isActiveV1) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (isActiveV1 && io.MouseDown[0]) {
                    g_state.rowSplitCol[1] = ImClamp(io.MousePos.x / windowWidth, 0.15f, 0.85f);
                }
                if (isHoverV1 && io.MouseDoubleClicked[0]) {
                    g_state.rowSplitCol[1] = 0.5f;
                }
                if (isActiveV1 || isHoverV1) {
                    ImU32 colLineV1 = isActiveV1 ? IM_COL32(110, 170, 255, 255) : IM_COL32(80, 140, 255, 220);
                    drawList->AddLine(ImVec2(splitX1, splitY0), ImVec2(splitX1, centralYMax), colLineV1, (isActiveV1 ? 3.0f : 2.0f) * g_dpiScale);
                }

                // 3. Horizontal Splitter Col 0 (between slot 0 and slot 2)
                ImGui::SetCursorScreenPos(ImVec2(0.0f, splitY0 - hitH * 0.5f));
                ImGui::InvisibleButton("##grid_splitter_h_c0", ImVec2(fmaxf(10.0f, splitX0), hitH));
                bool isHoverH0 = ImGui::IsItemHovered();
                bool isActiveH0 = ImGui::IsItemActive();
                if (isHoverH0 || isActiveH0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (isActiveH0 && io.MouseDown[0]) {
                    g_state.colSplitRow[0] = ImClamp((io.MousePos.y - centralYMin) / centralH, 0.15f, 0.85f);
                }
                if (isHoverH0 && io.MouseDoubleClicked[0]) {
                    g_state.colSplitRow[0] = 0.5f;
                }
                if (isActiveH0 || isHoverH0) {
                    ImU32 colLineH0 = isActiveH0 ? IM_COL32(110, 170, 255, 255) : IM_COL32(80, 140, 255, 220);
                    drawList->AddLine(ImVec2(0.0f, splitY0), ImVec2(splitX0, splitY0), colLineH0, (isActiveH0 ? 3.0f : 2.0f) * g_dpiScale);
                }

                // 4. Horizontal Splitter Col 1 (between slot 1 and slot 3)
                ImGui::SetCursorScreenPos(ImVec2(splitX0, splitY1 - hitH * 0.5f));
                ImGui::InvisibleButton("##grid_splitter_h_c1", ImVec2(fmaxf(10.0f, windowWidth - splitX0), hitH));
                bool isHoverH1 = ImGui::IsItemHovered();
                bool isActiveH1 = ImGui::IsItemActive();
                if (isHoverH1 || isActiveH1) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (isActiveH1 && io.MouseDown[0]) {
                    g_state.colSplitRow[1] = ImClamp((io.MousePos.y - centralYMin) / centralH, 0.15f, 0.85f);
                }
                if (isHoverH1 && io.MouseDoubleClicked[0]) {
                    g_state.colSplitRow[1] = 0.5f;
                }
                if (isActiveH1 || isHoverH1) {
                    ImU32 colLineH1 = isActiveH1 ? IM_COL32(110, 170, 255, 255) : IM_COL32(80, 140, 255, 220);
                    drawList->AddLine(ImVec2(splitX0, splitY1), ImVec2(windowWidth, splitY1), colLineH1, (isActiveH1 ? 3.0f : 2.0f) * g_dpiScale);
                }
            } else {
                for (int slot = 0; slot < totalSlots; ++slot) {
                    int c = slot % cols;
                    int r = slot / cols;
                    float cellW = windowWidth / (float)cols;
                    float cellH = centralH / (float)rows;
                    slots[slot] = { ImVec2(c * cellW, centralYMin + r * cellH), ImVec2((c + 1) * cellW, centralYMin + (r + 1) * cellH), cellW, cellH };
                }
            }

            // Check if any slot is currently empty or if layout cannot be split further
            bool hasEmptySlot = false;
            for (int sIdx = 0; sIdx < totalSlots; ++sIdx) {
                int stIdx = (sIdx < (int)g_state.streamOrder.size()) ? g_state.streamOrder[sIdx] : -1;
                if (stIdx < 0 || stIdx >= streamCount) {
                    hasEmptySlot = true;
                    break;
                }
            }
            bool canSplitH = !hasEmptySlot && (cols < 2) && (totalSlots < 4);
            bool canSplitV = !hasEmptySlot && (rows < 2) && (totalSlots < 4);

            // Render each slot (Occupied or Empty container)
            for (int slot = 0; slot < totalSlots; ++slot) {
                ImGui::PushID(slot);
                const auto& sr = slots[slot];
                int streamIdx = (slot < (int)g_state.streamOrder.size()) ? g_state.streamOrder[slot] : -1;

                if (streamIdx >= 0 && streamIdx < streamCount) {
                    // OCCUPIED STREAM TILE
                    const auto& stream = streamList[streamIdx];

                    // 1 & 2. Aspect-fit video within slot preserving 16:9 ratio with letterbox/pillarbox
                    DrawVideoAspectFit(drawList, (ImTextureID)streamTex, sr.min, sr.max, streamAspect);

                    // 3. Header bar overlay on stream tile
                    float titleH = 26.0f * g_dpiScale;
                    float titlePadX = 10.0f * g_dpiScale;
                    float headerY = (sr.min.y < 5.0f) ? (headerHeight + 10.0f * g_dpiScale) : (sr.min.y + 10.0f * g_dpiScale);
                    ImVec2 tMin(sr.min.x + 12.0f * g_dpiScale, headerY);
                    std::string titleStr = CleanStreamTitle(stream.name);
                    ImVec2 tTextSize = ImGui::CalcTextSize(titleStr.c_str());

                    bool isTileDeaf = (stream.peerPtr && stream.peerPtr->isDeafened) || (stream.isMe && g_state.isAudioDeafened);
                    bool isTileMute = (stream.peerPtr && stream.peerPtr->isMuted) || (stream.isMe && g_state.isMicMuted);
                    float audioBadgeW = (isTileDeaf || isTileMute) ? 24.0f * g_dpiScale : 0.0f;

                    float titleW = titlePadX * 2.0f + tTextSize.x + audioBadgeW;
                    ImVec2 tMax(tMin.x + titleW, tMin.y + titleH);

                    drawList->AddRectFilled(tMin, tMax, COLOR_CAPSULE_BG, 6.0f * g_dpiScale);
                    ImU32 borderCol = stream.isSpeaking ? IM_COL32(72, 224, 110, 255) : COLOR_CAPSULE_BORDER;
                    drawList->AddRect(tMin, tMax, borderCol, 6.0f * g_dpiScale, 0, stream.isSpeaking ? 1.5f * g_dpiScale : 1.0f);

                    float textDrawX = tMin.x + titlePadX;
                    drawList->AddText(ImVec2(textDrawX, tMin.y + (titleH - tTextSize.y) * 0.5f), IM_COL32(230, 235, 245, 255), titleStr.c_str());

                    if (isTileDeaf || isTileMute) {
                        float bSize = 18.0f * g_dpiScale;
                        ImVec2 bMin(textDrawX + tTextSize.x + 6.0f * g_dpiScale, tMin.y + (titleH - bSize) * 0.5f);
                        drawList->AddRectFilled(bMin, ImVec2(bMin.x + bSize, bMin.y + bSize), IM_COL32(235, 48, 58, 255), 4.0f * g_dpiScale);
                        const char* bIcon = isTileDeaf ? "IcBaselineHeadsetOff.svg" : "MdiMicrophoneOff.svg";
                        IconManager::Get().DrawSvgIcon(drawList, bIcon, ImVec2(bMin.x + bSize * 0.5f, bMin.y + bSize * 0.5f), 12.0f * g_dpiScale, IM_COL32(255, 255, 255, 255));
                    }

                    // Requirement 2: Per-stream volume icon buttons deleted - keeping only the top-right global master volume

                    // Requirement 4: Interactive Hitbox - Direct Left Click activates Focus Mode on this stream!
                    // Restrain hitbox strictly below y = headerHeight (74px) and above bottom dock, avoiding split/vol buttons
                    float hitY = fmaxf(sr.min.y, headerHeight);
                    float hitMaxY = sr.max.y;
                    if (sr.max.y >= windowHeight - 10.0f) {
                        hitMaxY -= bottomBarHeight;
                    }
                    float hitH = fmaxf(10.0f, hitMaxY - hitY);
                    ImVec2 hitPos(sr.min.x, hitY);
                    ImGui::SetCursorScreenPos(hitPos);
                    std::string cellBtnId = "##grid_cell_hitbox_" + std::to_string(slot);
                    if (ImGui::InvisibleButton(cellBtnId.c_str(), ImVec2(sr.w, hitH))) {
                        if (!overHeader && !overDock && !overSplitArea) {
                            g_state.focusedStreamIndex = slot; // Direct single left click activates Focus!
                        }
                    }
                    if (ImGui::IsItemHovered() && !overHeader && !overDock && !overSplitArea) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }

                    // Native VS Code style Drag & Drop Source
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        ImGui::SetDragDropPayload("DND_STREAM_SLOT", &slot, sizeof(int));
                        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.09f, 0.12f, 0.90f));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * g_dpiScale, 8.0f * g_dpiScale));
                        std::string dndName = CleanStreamTitle(stream.name);
                        ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "%s", dndName.c_str());
                        if (streamTex) {
                            float prevW = 160.0f * g_dpiScale;
                            float prevH = 90.0f * g_dpiScale;
                            ImGui::Image((ImTextureID)streamTex, ImVec2(prevW, prevH));
                        }
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor();
                        ImGui::EndDragDropSource();
                    }

                    // Native VS Code style Drag & Drop Target
                    if (ImGui::BeginDragDropTarget()) {
                        drawList->AddRectFilled(sr.min, sr.max, IM_COL32(90, 150, 255, 45));
                        drawList->AddRect(sr.min, sr.max, IM_COL32(90, 150, 255, 220), 4.0f * g_dpiScale, 0, 2.0f * g_dpiScale);

                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_STREAM_SLOT")) {
                            int srcSlot = *(const int*)payload->Data;
                            if (srcSlot >= 0 && srcSlot < totalSlots && srcSlot != slot) {
                                std::swap(g_state.streamOrder[srcSlot], g_state.streamOrder[slot]);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // Right-click context menu on stream
                    if (ImGui::BeginPopupContextItem(cellBtnId.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
                        ImGui::Text("%s Options", CleanStreamTitle(stream.name).c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Mode Focus (Plein écran)")) {
                            g_state.focusedStreamIndex = slot;
                        }
                        if (canSplitH) {
                            if (ImGui::MenuItem("Splitter en 2 colonnes [ ◫ ] (Côte à côte)")) {
                                ApplySplitLayout(1);
                            }
                        }
                        if (canSplitV) {
                            if (ImGui::MenuItem("Splitter en 2 lignes [ ⊟ ] (Haut / Bas)")) {
                                ApplySplitLayout(2);
                            }
                        }
                        if (g_state.gridLayoutMode != 0) {
                            if (ImGui::MenuItem("Disposition automatique (Auto-fit)")) {
                                g_state.gridLayoutMode = 0;
                            }
                        }
                        if (stream.peerPtr) {
                            ImGui::Separator();
                            if (stream.peerPtr->isStreamHidden) {
                                if (ImGui::MenuItem("Afficher le stream")) {
                                    stream.peerPtr->isStreamHidden = false;
                                }
                            } else {
                                if (ImGui::MenuItem("Masquer le stream")) {
                                    stream.peerPtr->isStreamHidden = true;
                                }
                            }
                            ImGui::Text("Volume: %.0f%%", stream.peerPtr->volume * 100.0f);
                            if (ImGui::SliderFloat("##stream_vol", &stream.peerPtr->volume, 0.0f, 1.5f, "%.0f%%")) {
                                SavePeers(g_state);
                            }
                            ImGui::MenuItem("Mute Audio", nullptr, &stream.peerPtr->isMuted);
                            ImGui::MenuItem("Deafen Audio", nullptr, &stream.peerPtr->isDeafened);
                        }
                        ImGui::EndPopup();
                    }
                } else {
                    // =========================================================
                    // EMPTY SLOT CONTAINER: Minimalist & Clean
                    // No visible outer borders, no redundant badges, no split invitations
                    // =========================================================
                    // 1. Dark container background without outer border
                    drawList->AddRectFilled(sr.min, sr.max, IM_COL32(12, 13, 17, 255));

                    // 2. Centered visual target card (Discreet & minimal)
                    ImVec2 center((sr.min.x + sr.max.x) * 0.5f, (sr.min.y + sr.max.y) * 0.5f);
                    float cardW = fminf(260.0f * g_dpiScale, sr.w - 24.0f * g_dpiScale);
                    float cardH = fminf(116.0f * g_dpiScale, sr.h - 24.0f * g_dpiScale);
                    ImVec2 cardMin(center.x - cardW * 0.5f, center.y - cardH * 0.5f);
                    ImVec2 cardMax(center.x + cardW * 0.5f, center.y + cardH * 0.5f);

                    drawList->AddRectFilled(cardMin, cardMax, IM_COL32(18, 20, 26, 180), 8.0f * g_dpiScale);

                    // Drop Icon
                    IconManager::Get().DrawSvgIcon(drawList, "MaterialSymbolsAddPhotoAlternate.svg", 
                                                   ImVec2(center.x, cardMin.y + 30.0f * g_dpiScale), 
                                                   24.0f * g_dpiScale, IM_COL32(100, 140, 220, 220));

                    // Text: "Déposer un stream ici"
                    const char* emptyTxt = "Déposer un stream ici";
                    ImVec2 etSize = ImGui::CalcTextSize(emptyTxt);
                    drawList->AddText(ImVec2(center.x - etSize.x * 0.5f, cardMin.y + 54.0f * g_dpiScale), 
                                      IM_COL32(205, 212, 230, 240), emptyTxt);

                    // Subtitle: "Glissez un flux ou clic droit pour placer"
                    const char* subTxt = "Glissez un flux ou clic droit pour placer";
                    ImVec2 stSize = ImGui::CalcTextSize(subTxt);
                    drawList->AddText(ImVec2(center.x - stSize.x * 0.5f, cardMin.y + 78.0f * g_dpiScale), 
                                      IM_COL32(120, 126, 145, 190), subTxt);

                    // Close slot button (ONLY shown if manual split was activated)
                    if (g_state.gridLayoutMode != 0) {
                        float btnActionSize = 26.0f * g_dpiScale;
                        float headerY = sr.min.y + 10.0f * g_dpiScale;
                        float curRightBtnX = sr.max.x - btnActionSize - 12.0f * g_dpiScale;
                        ImVec2 clsMin(curRightBtnX, headerY);
                        ImVec2 clsMax(curRightBtnX + btnActionSize, headerY + btnActionSize);
                        drawList->AddRectFilled(clsMin, clsMax, IM_COL32(16, 18, 24, 210), 6.0f * g_dpiScale);
                        const char* xText = "✕";
                        ImVec2 xSize = ImGui::CalcTextSize(xText);
                        drawList->AddText(ImVec2(clsMin.x + (btnActionSize - xSize.x) * 0.5f, clsMin.y + (btnActionSize - xSize.y) * 0.5f), IM_COL32(220, 100, 100, 255), xText);
                        ImGui::SetCursorScreenPos(clsMin);
                        if (ImGui::InvisibleButton("##empty_close_slot_btn", ImVec2(btnActionSize, btnActionSize))) {
                            g_state.gridLayoutMode = 0; // Reset to auto-fit
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fermer cette case (Revenir à l'auto-fit)");
                    }

                    // 3. Interactive Hitbox restricted below headerHeight (74px)
                    float eHitY = fmaxf(sr.min.y, headerHeight);
                    float eHitMaxY = sr.max.y;
                    if (sr.max.y >= windowHeight - 10.0f) {
                        eHitMaxY -= bottomBarHeight;
                    }
                    float eHitH = fmaxf(10.0f, eHitMaxY - eHitY);
                    ImGui::SetCursorScreenPos(ImVec2(sr.min.x, eHitY));
                    std::string emptyHitId = "##empty_cell_hitbox_" + std::to_string(slot);
                    ImGui::InvisibleButton(emptyHitId.c_str(), ImVec2(sr.w, eHitH));
                    bool isSlotHovered = ImGui::IsItemHovered();
                    if (isSlotHovered && !overHeader && !overDock && !overSplitArea) {
                        drawList->AddRect(cardMin, cardMax, IM_COL32(90, 150, 255, 230), 8.0f * g_dpiScale, 0, 1.5f * g_dpiScale);
                    }

                    // 4. Native Drag & Drop Target to accept streams!
                    if (ImGui::BeginDragDropTarget()) {
                        drawList->AddRectFilled(sr.min, sr.max, IM_COL32(70, 130, 240, 50));
                        drawList->AddRect(sr.min, sr.max, IM_COL32(90, 160, 255, 240), 4.0f * g_dpiScale, 0, 2.0f * g_dpiScale);
                        drawList->AddRect(cardMin, cardMax, IM_COL32(110, 170, 255, 255), 8.0f * g_dpiScale, 0, 2.0f * g_dpiScale);

                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_STREAM_SLOT")) {
                            int srcSlot = *(const int*)payload->Data;
                            if (srcSlot >= 0 && srcSlot < totalSlots && srcSlot != slot) {
                                std::swap(g_state.streamOrder[srcSlot], g_state.streamOrder[slot]);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // 5. Right-click context menu to place stream or auto-fit (NO split invitations)
                    if (!overSplitArea && ImGui::BeginPopupContextItem(emptyHitId.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
                        ImGui::Text("Emplacement Libre");
                        ImGui::Separator();
                        for (int s = 0; s < streamCount; ++s) {
                            std::string placeItem = "Placer " + CleanStreamTitle(streamList[s].name) + " ici";
                            if (ImGui::MenuItem(placeItem.c_str())) {
                                for (int k = 0; k < totalSlots; ++k) {
                                    if (g_state.streamOrder[k] == s) {
                                        std::swap(g_state.streamOrder[k], g_state.streamOrder[slot]);
                                        break;
                                    }
                                }
                            }
                        }
                        if (g_state.gridLayoutMode != 0) {
                            ImGui::Separator();
                            if (ImGui::MenuItem("Fermer cette case (Auto-fit)")) {
                                g_state.gridLayoutMode = 0;
                            }
                        }
                        ImGui::EndPopup();
                    }
                }

                ImGui::PopID();
            }

        }

        drawList->PopClipRect();
    } else {
        // =========================================================================
        // Requirement 3 & 4: Dedicated "Voice Only" View (When streamCount == 0)
        // Generous circular bubbles centered in the window with pseudo underneath
        // and audio indicators (speaking glowing ring, mute/deafen badges, status dot)
        // NO gray card/box behind avatars.
        // =========================================================================
        int N = (int)participants.size();
        if (N > 0) {
            // Central canvas area for avatars: Maximize size between TopBar (headerHeight) and bottom dock (dockMin.y)
            float topLimit = headerHeight + 10.0f * g_dpiScale;
            float bottomLimit = dockMin.y - 10.0f * g_dpiScale;
            float availableH = fmaxf(100.0f * g_dpiScale, bottomLimit - topLimit);

            float badgeH = 28.0f * g_dpiScale;
            float badgeGap = 10.0f * g_dpiScale;
            float hintH = (N == 1) ? 24.0f * g_dpiScale : 0.0f;
            float badgeTotalH = badgeH + badgeGap + hintH;

            float spacing = (N <= 1) ? 0.0f : ((N == 2) ? 48.0f : ((N <= 4) ? 36.0f : 24.0f)) * g_dpiScale;
            float availWidth = windowWidth - 48.0f * g_dpiScale;
            float maxRadiusX = ((availWidth - (N - 1) * spacing) / (float)N) * 0.5f;
            float maxRadiusY = (availableH - badgeTotalH - 12.0f * g_dpiScale) * 0.5f;

            // Maximized bubble radius filling available central viewport
            float baseRadius = fmaxf(44.0f * g_dpiScale, fminf(maxRadiusY, maxRadiusX));

            float totalItemH = 2.0f * baseRadius + badgeTotalH;
            float centerY = topLimit + (availableH - totalItemH) * 0.5f + baseRadius;

            float totalWidth = N * (2.0f * baseRadius) + (N - 1) * spacing;
            float startX = (windowWidth - totalWidth) * 0.5f + baseRadius;

            for (int i = 0; i < N; ++i) {
                auto& p = participants[i];
                ImVec2 center(startX + i * (2.0f * baseRadius + spacing), centerY);

                // Decrement poke visual timer
                if (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) {
                    p.peerPtr->pokeTimer -= ImGui::GetIO().DeltaTime;
                    if (p.peerPtr->pokeTimer < 0.0f) p.peerPtr->pokeTimer = 0.0f;
                }

                // Interactive Hitbox for Right-Click Context Menu & Hover
                ImVec2 bHitMin(center.x - baseRadius, center.y - baseRadius);
                ImGui::SetCursorScreenPos(bHitMin);
                std::string bubbleHitboxId = "##bubble_hitbox_" + std::to_string(i) + "_" + p.name;
                ImGui::InvisibleButton(bubbleHitboxId.c_str(), ImVec2(baseRadius * 2.0f, baseRadius * 2.0f));
                bool isHovered = ImGui::IsItemHovered();

                if (ImGui::BeginPopupContextItem(bubbleHitboxId.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
                    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.98f, 1.0f), "%s Options", p.name.c_str());
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (p.isMe) {
                        if (ImGui::MenuItem("Mute Microphone", nullptr, &g_state.isMicMuted)) {}
                        if (ImGui::MenuItem("Deafen Audio", nullptr, &g_state.isAudioDeafened)) {}
                        if (ImGui::MenuItem("Screen Share", nullptr, &g_state.isStreaming)) {}
                        if (ImGui::MenuItem("Show Cursor on Other Screen", nullptr, &g_state.showCursorOnOtherScreen)) {}
                        if (ImGui::MenuItem("RNNoise Noise Suppression", nullptr, &g_state.rnnoiseNoiseSuppression)) {}
                    } else if (p.peerPtr) {
                        ImGui::Text("Peer Volume: %.0f%%", p.peerPtr->volume * 100.0f);
                        if (ImGui::SliderFloat("##peer_vol", &p.peerPtr->volume, 0.0f, 1.5f, "%.0f%%")) {
                            SavePeers(g_state);
                        }
                        ImGui::Spacing();
                        if (ImGui::MenuItem("Mute Peer Audio", nullptr, &p.peerPtr->isMuted)) {
                            SavePeers(g_state);
                        }
                        if (ImGui::MenuItem("Deafen Peer", nullptr, &p.peerPtr->isDeafened)) {
                            SavePeers(g_state);
                        }
                        if (ImGui::MenuItem("Poke Peer (Ping)")) {
                            p.peerPtr->pokeTimer = 2.0f;
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Disconnect Peer")) {
                            p.peerPtr->status = PeerStatus::Offline;
                            if (g_state.activeConnectedIp == p.peerPtr->ip) {
                                g_state.activeConnectedIp.clear();
                                g_state.isStreaming = false;
                            }
                            SavePeers(g_state);
                        }
                    }
                    ImGui::EndPopup();
                }

                // Poke ripple visual feedback
                if (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) {
                    float pFrac = (2.0f - p.peerPtr->pokeTimer) / 2.0f;
                    float rippleR = baseRadius + pFrac * 36.0f * g_dpiScale;
                    int alpha = (int)((1.0f - pFrac) * 220);
                    drawList->AddCircle(center, rippleR, IM_COL32(255, 204, 0, alpha), 64, 2.5f * g_dpiScale);
                }

                // Draw circular avatar disc (Standalone circular bubble, NO background card)
                std::string initials = p.isMe ? "Me" : p.name;
                DrawCircularAvatar(drawList, defaultAvatarTex, center, baseRadius, initials);

                // Subtle dark border on avatar (No blue highlight on hover)
                drawList->AddCircle(center, baseRadius, IM_COL32(50, 55, 70, 200), 48, 1.5f * g_dpiScale);

                // Voice Activity Detection: Glowing green ring ONLY when speaking
                if (p.isSpeaking) {
                    float ringRadius = baseRadius + 4.0f * g_dpiScale;
                    drawList->AddCircle(center, ringRadius, IM_COL32(72, 224, 110, 255), 64, 3.0f * g_dpiScale);
                    float time = (float)ImGui::GetTime();
                    float glowR = ringRadius + (3.5f + sinf(time * 6.0f) * 2.0f) * g_dpiScale;
                    drawList->AddCircle(center, glowR, IM_COL32(72, 224, 110, 120), 64, 2.0f * g_dpiScale);
                }

                // Micro & Deafen audio badge in bottom-right corner of avatar
                bool isMuted = p.isMe ? g_state.isMicMuted : (p.peerPtr ? p.peerPtr->isMuted : false);
                bool isDeafened = p.isMe ? g_state.isAudioDeafened : (p.peerPtr ? p.peerPtr->isDeafened : false);
                if (isMuted || isDeafened) {
                    float badgeRadius = fminf(28.0f * g_dpiScale, baseRadius * 0.22f);
                    badgeRadius = fmaxf(14.0f * g_dpiScale, badgeRadius);
                    float badgeIconSize = badgeRadius * 1.15f;
                    ImVec2 mCenter(center.x + baseRadius * 0.707f, center.y + baseRadius * 0.707f);
                    drawList->AddCircleFilled(mCenter, badgeRadius, IM_COL32(235, 48, 58, 255), 24);
                    drawList->AddCircle(mCenter, badgeRadius, IM_COL32(24, 25, 30, 255), 24, 1.5f * g_dpiScale);
                    const char* bIcon = isDeafened ? "IcBaselineHeadsetOff.svg" : "MdiMicrophoneOff.svg";
                    IconManager::Get().DrawSvgIcon(drawList, bIcon, mCenter, badgeIconSize, IM_COL32(255, 255, 255, 255));
                }

                // Pill badge below avatar (Pure pseudonym ONLY: e.g. "me", "WaitingPeer_3")
                std::string badgeLabel = p.isMe ? "me" : p.name;
                ImVec2 bTextSize = ImGui::CalcTextSize(badgeLabel.c_str());
                float badgePadX = 14.0f * g_dpiScale;
                float dotRadius = 4.0f * g_dpiScale;
                float dotGap = 6.0f * g_dpiScale;
                float dotAreaW = dotRadius * 2.0f + dotGap;
                float badgeW = bTextSize.x + badgePadX * 2.0f + dotAreaW;
                ImVec2 bMin(center.x - badgeW * 0.5f, center.y + baseRadius + badgeGap);
                ImVec2 bMax(bMin.x + badgeW, bMin.y + badgeH);

                ImU32 pillBgCol = isHovered ? IM_COL32(24, 26, 34, 230) : IM_COL32(18, 19, 25, 220);
                ImU32 pillBdrCol = (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) ? IM_COL32(255, 204, 0, 220) :
                                   (p.isSpeaking ? IM_COL32(72, 224, 110, 200) : IM_COL32(50, 52, 65, 180));

                drawList->AddRectFilled(bMin, bMax, pillBgCol, 14.0f * g_dpiScale);
                drawList->AddRect(bMin, bMax, pillBdrCol, 14.0f * g_dpiScale, 0, 1.0f);

                // Status dot
                ImVec2 dotC(bMin.x + badgePadX + dotRadius, bMin.y + badgeH * 0.5f);
                IconManager::DrawStatusIndicator(drawList, dotC, dotRadius, p.status, true);

                // Pure pseudonym text
                ImU32 textCol = p.isSpeaking ? IM_COL32(120, 255, 150, 255) : IM_COL32(230, 232, 240, 255);
                drawList->AddText(ImVec2(dotC.x + dotRadius + dotGap, bMin.y + (badgeH - bTextSize.y) * 0.5f),
                                  textCol, badgeLabel.c_str());
            }

            if (N == 1) {
                const char* hintMsg = "Salon vocal actif • En attente d'autres participants ou d'un partage d'écran";
                ImVec2 hSize = ImGui::CalcTextSize(hintMsg);
                drawList->AddText(ImVec2((windowWidth - hSize.x) * 0.5f, centerY + baseRadius + badgeH + badgeGap + 8.0f * g_dpiScale),
                                  IM_COL32(130, 135, 155, 200), hintMsg);
            }
        } else {
            const char* msg = "No active peer stream. Select a peer on the 'Home' tab and click [ connect ].";
            ImVec2 ms = ImGui::CalcTextSize(msg);
            drawList->AddText(ImVec2((windowWidth - ms.x) * 0.5f, windowHeight * 0.46f),
                              IM_COL32(150, 154, 170, 255), msg);
        }
    }

    // =========================================================================
    // Floating Split Capsule [ ⊟ ] [ ◫ ] & Master Volume Control (Requirement 3)
    // Placed strictly in top-right of video canvas, above volume control
    // Rendered on fgDrawList with 100% click priority over video canvas
    // =========================================================================
    if (streamCount > 1) {
        ImVec2 spMin(sPillX, sPillY);
        ImVec2 spMax(sPillX + sPillW, sPillY + sPillH);
        fgDrawList->AddRectFilled(spMin, spMax, COLOR_CAPSULE_BG, 8.0f * g_dpiScale);
        fgDrawList->AddRect(spMin, spMax, COLOR_CAPSULE_BORDER, 8.0f * g_dpiScale, 0, 1.0f * g_dpiScale);

        float curBtnX = sPillX + sPadX;
        float btnY = sPillY + sPadY;

        // Button 1: Split Horizontal [ ⊟ ] (Lignes empilées)
        {
            ImVec2 bMin(curBtnX, btnY);
            ImVec2 bMax(bMin.x + sBtnW, bMin.y + sBtnH);
            bool isActiveMode = (g_state.gridLayoutMode == 2) && (g_state.focusedStreamIndex < 0);

            ImGui::SetCursorScreenPos(bMin);
            bool isClickedBtn = ImGui::InvisibleButton("##canvas_split_h", ImVec2(sBtnW, sBtnH));
            bool isMouseInBtn = (io.MousePos.x >= bMin.x && io.MousePos.x <= bMax.x &&
                                 io.MousePos.y >= bMin.y && io.MousePos.y <= bMax.y);
            bool isHov = isMouseInBtn || ImGui::IsItemHovered();
            bool isClicked = isClickedBtn || (isMouseInBtn && ImGui::IsMouseClicked(0));

            if (isHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            if (isClicked) {
                ApplySplitLayout(2); // Disposition horizontale (lignes empilées)
            }

            ImU32 bgCol = isActiveMode ? IM_COL32(110, 40, 54, 255) : (isHov ? IM_COL32(48, 52, 64, 255) : COLOR_CAPSULE_BG);
            fgDrawList->AddRectFilled(bMin, bMax, bgCol, 5.0f * g_dpiScale);
            if (isActiveMode) {
                fgDrawList->AddRect(bMin, bMax, IM_COL32(160, 60, 80, 255), 5.0f * g_dpiScale, 0, 1.0f);
            }

            float iconPad = 5.0f * g_dpiScale;
            ImVec2 rMin(bMin.x + iconPad, bMin.y + iconPad);
            ImVec2 rMax(bMax.x - iconPad, bMax.y - iconPad);
            ImU32 iCol = (isActiveMode || isHov) ? IM_COL32(255, 255, 255, 255) : IM_COL32(210, 215, 225, 255);
            fgDrawList->AddRect(rMin, rMax, iCol, 2.0f * g_dpiScale, 0, 1.2f * g_dpiScale);
            float midY = (rMin.y + rMax.y) * 0.5f;
            fgDrawList->AddLine(ImVec2(rMin.x, midY), ImVec2(rMax.x, midY), iCol, 1.2f * g_dpiScale);

            if (isHov) ImGui::SetTooltip(isActiveMode ? "Disposition Horizontale active (Lignes empilées)" : "Disposition Horizontale [ ⊟ ] (Lignes empilées)");
        }

        curBtnX += sBtnW + sGap;

        // Button 2: Split Vertical / Grille [ ◫ ] (Colonnes / Grille)
        {
            ImVec2 bMin(curBtnX, btnY);
            ImVec2 bMax(bMin.x + sBtnW, bMin.y + sBtnH);
            bool isActiveMode = ((g_state.gridLayoutMode == 1) || (g_state.gridLayoutMode == 0)) && (g_state.focusedStreamIndex < 0);

            ImGui::SetCursorScreenPos(bMin);
            bool isClickedBtn = ImGui::InvisibleButton("##canvas_split_v", ImVec2(sBtnW, sBtnH));
            bool isMouseInBtn = (io.MousePos.x >= bMin.x && io.MousePos.x <= bMax.x &&
                                 io.MousePos.y >= bMin.y && io.MousePos.y <= bMax.y);
            bool isHov = isMouseInBtn || ImGui::IsItemHovered();
            bool isClicked = isClickedBtn || (isMouseInBtn && ImGui::IsMouseClicked(0));

            if (isHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            if (isClicked) {
                ApplySplitLayout(1); // Disposition verticale / grille
            }

            ImU32 bgCol = isActiveMode ? IM_COL32(110, 40, 54, 255) : (isHov ? IM_COL32(48, 52, 64, 255) : COLOR_CAPSULE_BG);
            fgDrawList->AddRectFilled(bMin, bMax, bgCol, 5.0f * g_dpiScale);
            if (isActiveMode) {
                fgDrawList->AddRect(bMin, bMax, IM_COL32(160, 60, 80, 255), 5.0f * g_dpiScale, 0, 1.0f);
            }

            float iconPad = 5.0f * g_dpiScale;
            ImVec2 rMin(bMin.x + iconPad, bMin.y + iconPad);
            ImVec2 rMax(bMax.x - iconPad, bMax.y - iconPad);
            ImU32 iCol = (isActiveMode || isHov) ? IM_COL32(255, 255, 255, 255) : IM_COL32(210, 215, 225, 255);
            fgDrawList->AddRect(rMin, rMax, iCol, 2.0f * g_dpiScale, 0, 1.2f * g_dpiScale);
            float midX = (rMin.x + rMax.x) * 0.5f;
            fgDrawList->AddLine(ImVec2(midX, rMin.y), ImVec2(midX, rMax.y), iCol, 1.2f * g_dpiScale);

            if (isHov) ImGui::SetTooltip(isActiveMode ? "Disposition Grille / Verticale active (Colonnes / Grille)" : "Disposition Grille / Verticale [ ◫ ] (Colonnes / Grille)");
        }
    }

    // =========================================================================
    // Requirement 2: Global Master Volume Control Button (Top-Right of Screen)
    // Conserve UNIQUEMENT le contrôle du volume situé en haut à droite de l'écran global
    // =========================================================================
    if (streamCount > 0) {
        float volBtnX = windowWidth - 14.0f * g_dpiScale - volBtnSize;
        float volBtnY = (streamCount > 1) ? (sPillY + sPillH + 8.0f * g_dpiScale) : (headerHeight + 10.0f * g_dpiScale);
        ImVec2 volMin(volBtnX, volBtnY);
        ImVec2 volMax(volBtnX + volBtnSize, volBtnY + volBtnSize);

        ImGui::SetCursorScreenPos(volMin);
        bool isVolClickedBtn = ImGui::InvisibleButton("##canvas_topright_master_vol_btn", ImVec2(volBtnSize, volBtnSize));
        bool isMouseInVol = (io.MousePos.x >= volMin.x && io.MousePos.x <= volMax.x &&
                             io.MousePos.y >= volMin.y && io.MousePos.y <= volMax.y);
        bool isVolHov = isMouseInVol || ImGui::IsItemHovered();
        bool isVolClicked = isVolClickedBtn || (isMouseInVol && ImGui::IsMouseClicked(0));

        if (isVolHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        ImU32 volBg = isVolHov ? IM_COL32(48, 52, 64, 255) : COLOR_CAPSULE_BG;
        fgDrawList->AddRectFilled(volMin, volMax, volBg, 6.0f * g_dpiScale);
        fgDrawList->AddRect(volMin, volMax, COLOR_CAPSULE_BORDER, 6.0f * g_dpiScale, 0, 1.0f);

        const char* svIcon = (g_state.streamVolume <= 0.01f) ? "MaterialSymbolsNoSound.svg" : "MaterialSymbolsVolumeDown.svg";
        IconManager::Get().DrawSvgIcon(fgDrawList, svIcon, ImVec2(volMin.x + volBtnSize * 0.5f, volMin.y + volBtnSize * 0.5f), 16.0f * g_dpiScale, IM_COL32(230, 235, 245, 255));

        if (isVolClicked) {
            ImGui::OpenPopup("##canvas_master_vol_pop");
        }
        if (isVolHov) {
            ImGui::SetTooltip("Volume principal (%.0f%%)", g_state.streamVolume * 100.0f);
        }
        if (ImGui::BeginPopup("##canvas_master_vol_pop")) {
            ImGui::Text("Volume principal");
            ImGui::Separator();
            ImGui::SliderFloat("##stream_vol_slider_topright", &g_state.streamVolume, 0.0f, 1.0f, "%.0f%%");
            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // 3. Floating Collaborative Bottom Dock (Center) in Overlay (Requirement 2)
    // =========================================================================
    // Restored fully opaque rounded dark capsule with unified #1E1F22 color and crisp border
    drawList->AddRectFilled(dockMin, dockMax, COLOR_CAPSULE_BG, 16.0f * g_dpiScale);
    drawList->AddRect(dockMin, dockMax, COLOR_CAPSULE_BORDER, 16.0f * g_dpiScale, 0, 1.2f * g_dpiScale);

    ImGui::SetCursorScreenPos(ImVec2(dockMin.x + dockPadX, dockMin.y + (dockHeight - btnH) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    // Base dock button colors with subtle contrast inside the #1E1F22 capsule
    ImVec4 dockBtnBg    = ImVec4(42.0f/255.0f, 44.0f/255.0f, 50.0f/255.0f, 1.0f); // #2A2C32
    ImVec4 dockBtnHover = ImVec4(56.0f/255.0f, 60.0f/255.0f, 72.0f/255.0f, 1.0f); // #383C48

    // Control 1: [+] Screen / Window with Plus inside (MaterialSymbolsAddPhotoAlternate.svg per sketch #1)
    ImVec2 bPos1 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, dockBtnBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dockBtnHover);
    if (ImGui::Button("##dock_screen_plus", ImVec2(btnW, btnH))) {}
    bool hovered1 = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    ImU32 col1 = hovered1 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255);
    IconManager::Get().DrawSvgIcon(drawList, "MaterialSymbolsAddPhotoAlternate.svg", ImVec2(bPos1.x + btnW * 0.5f, bPos1.y + btnH * 0.5f), 24.0f * g_dpiScale, col1);
    if (hovered1) ImGui::SetTooltip("Add stream source or webcam window");

    // Control 2: Slanted Paintbrush / Annotation (BoxiconsBrushFilled.svg per sketch #2)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos2 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isDrawMode ? ImVec4(0.361f, 0.141f, 0.180f, 1.0f) : dockBtnBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_state.isDrawMode ? ImVec4(0.431f, 0.157f, 0.212f, 1.0f) : dockBtnHover);
    if (ImGui::Button("##dock_paintbrush", ImVec2(btnW, btnH))) {
        g_state.isDrawMode = !g_state.isDrawMode;
    }
    bool hovered2 = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    ImU32 col2 = g_state.isDrawMode ? IM_COL32(255, 120, 140, 255) : (hovered2 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255));
    IconManager::Get().DrawSvgIcon(drawList, "BoxiconsBrushFilled.svg", ImVec2(bPos2.x + btnW * 0.5f, bPos2.y + btnH * 0.5f), 24.0f * g_dpiScale, col2);
    if (hovered2) ImGui::SetTooltip("Toggle Collaborative Real-time Screen Annotation (Paintbrush)");

    // Control 3: Arrow cursor in rounded square "show cursor on the other screen" (TablerPointer2.svg per sketch #3)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos3 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.showCursorOnOtherScreen ? ImVec4(0.18f, 0.28f, 0.40f, 1.0f) : dockBtnBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_state.showCursorOnOtherScreen ? ImVec4(0.24f, 0.35f, 0.48f, 1.0f) : dockBtnHover);
    if (ImGui::Button("##dock_cursor_screen", ImVec2(btnW, btnH))) {
        g_state.showCursorOnOtherScreen = !g_state.showCursorOnOtherScreen;
    }
    bool hovered3 = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    ImU32 col3 = g_state.showCursorOnOtherScreen ? IM_COL32(100, 180, 255, 255) : (hovered3 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255));
    IconManager::Get().DrawSvgIcon(drawList, "TablerPointer2.svg", ImVec2(bPos3.x + btnW * 0.5f, bPos3.y + btnH * 0.5f), 24.0f * g_dpiScale, col3);
    if (hovered3) ImGui::SetTooltip(g_state.showCursorOnOtherScreen ? "Hide cursor on the other screen" : "Show cursor on the other screen");

    // Control 4: Classic Microphone (MdiMicrophone.svg / MdiMicrophoneOff.svg per sketch #4)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos4 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isMicMuted ? ImVec4(0.40f, 0.15f, 0.18f, 1.0f) : dockBtnBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_state.isMicMuted ? ImVec4(0.50f, 0.18f, 0.22f, 1.0f) : dockBtnHover);
    if (ImGui::Button("##dock_mic", ImVec2(btnW, btnH))) {
        g_state.isMicMuted = !g_state.isMicMuted;
    }
    bool hovered4 = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    const char* micSvg = g_state.isMicMuted ? "MdiMicrophoneOff.svg" : "MdiMicrophone.svg";
    ImU32 col4 = g_state.isMicMuted ? IM_COL32(255, 80, 80, 255) : (hovered4 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255));
    IconManager::Get().DrawSvgIcon(drawList, micSvg, ImVec2(bPos4.x + btnW * 0.5f, bPos4.y + btnH * 0.5f), 24.0f * g_dpiScale, col4);
    if (hovered4) ImGui::SetTooltip(g_state.isMicMuted ? "Unmute Microphone" : "Mute Microphone");

    // Control 5: Audio Headphones (IcBaselineHeadset.svg / IcBaselineHeadsetOff.svg per sketch #5)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos5 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isAudioDeafened ? ImVec4(0.40f, 0.15f, 0.18f, 1.0f) : dockBtnBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_state.isAudioDeafened ? ImVec4(0.50f, 0.18f, 0.22f, 1.0f) : dockBtnHover);
    if (ImGui::Button("##dock_audio", ImVec2(btnW, btnH))) {
        g_state.isAudioDeafened = !g_state.isAudioDeafened;
    }
    bool hovered5 = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    const char* audioSvg = g_state.isAudioDeafened ? "IcBaselineHeadsetOff.svg" : "IcBaselineHeadset.svg";
    ImU32 col5 = g_state.isAudioDeafened ? IM_COL32(255, 80, 80, 255) : (hovered5 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255));
    IconManager::Get().DrawSvgIcon(drawList, audioSvg, ImVec2(bPos5.x + btnW * 0.5f, bPos5.y + btnH * 0.5f), 24.0f * g_dpiScale, col5);
    if (hovered5) ImGui::SetTooltip(g_state.isAudioDeafened ? "Undeafen Audio" : "Deafen Audio");

    // Control 6: Phone Receiver with 'x' (MaterialSymbolsPhoneCancelSharp.svg per sketch #6)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos6 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.48f, 0.16f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.18f, 1.0f));
    if (ImGui::Button("##dock_phone_hangup", ImVec2(btnEndW, btnH))) {
        g_state.activeConnectedIp.clear();
        g_state.isStreaming = false;
        for (auto& peer : g_state.peers) {
            if (peer.status == PeerStatus::Online || peer.status == PeerStatus::Waiting) {
                peer.status = PeerStatus::Offline;
            }
        }
        SavePeers(g_state);
        g_state.currentTab = AppTab::Home;
    }
    bool hovered6 = ImGui::IsItemHovered();
    ImGui::PopStyleColor(2);
    ImU32 col6 = hovered6 ? IM_COL32(255, 255, 255, 255) : IM_COL32(250, 240, 245, 255);
    IconManager::Get().DrawSvgIcon(drawList, "MaterialSymbolsPhoneCancelSharp.svg", ImVec2(bPos6.x + btnEndW * 0.5f, bPos6.y + btnH * 0.5f), 24.0f * g_dpiScale, col6);
    if (hovered6) ImGui::SetTooltip("Disconnect / Leave Session");

    ImGui::PopStyleColor(); // ImGuiCol_Border
    ImGui::PopStyleVar(3);

    ImGui::PopStyleVar(2); // WindowPadding & WindowBorderSize
}

// Secondary View: Settings (Audio, Themes, Hotkeys, Network Stats)
static void RenderSettingsView(float contentWidth, float contentHeight) {
    float sidebarWidth = 180.0f * g_dpiScale;
    ImGui::BeginChild("##settings_sidebar", ImVec2(sidebarWidth, contentHeight - 40.0f * g_dpiScale), true);

#if defined(_DEBUG) || !defined(NDEBUG)
    const char* categories[] = { "Account", "Theme", "Hotkey", "Audio", "Setting", "Stat", "Debug Tools" };
    const int numCategories = 7;
#else
    const char* categories[] = { "Account", "Theme", "Hotkey", "Audio", "Setting", "Stat" };
    const int numCategories = 6;
#endif
    for (int i = 0; i < numCategories; ++i) {
        bool selected = (g_state.settingsCategory == i);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.14f, 0.18f, 1.0f)); // Wine accent
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.11f, 0.14f, 1.0f));
        }

        if (ImGui::Button(categories[i], ImVec2(sidebarWidth - 28.0f * g_dpiScale, 36.0f * g_dpiScale))) {
            g_state.settingsCategory = i;
        }
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 16.0f * g_dpiScale);

    ImGui::BeginChild("##settings_content", ImVec2(contentWidth - sidebarWidth - 36.0f * g_dpiScale, contentHeight - 40.0f * g_dpiScale), true);

    if (g_state.settingsCategory == 3) { // Audio
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Audio Settings");
        ImGui::Separator();
        ImGui::Spacing();

        static int inputDev = 0;
        const char* inputDevices[] = { "Default System Microphone", "Headset (HyperX Cloud II)", "Realtek Audio Line-In" };
        ImGui::Text("Microphone Device:");
        ImGui::Combo("##micdev", &inputDev, inputDevices, IM_ARRAYSIZE(inputDevices));

        static int outputDev = 0;
        const char* outputDevices[] = { "Default System Speakers", "Headphones (HyperX Cloud II)", "Realtek Digital Output" };
        ImGui::Spacing();
        ImGui::Text("Speaker / Headphone Device:");
        ImGui::Combo("##spkdev", &outputDev, outputDevices, IM_ARRAYSIZE(outputDevices));

        ImGui::Spacing();
        static float micVol = 0.9f;
        ImGui::SliderFloat("Microphone Volume", &micVol, 0.0f, 1.0f, "%.0f%%");

        ImGui::Spacing();
        ImGui::Checkbox("microphone RNNoise - remove background sound", &g_state.rnnoiseNoiseSuppression);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Real-time neural network noise suppression engine for crystal-clear voice");
        }
    } else if (g_state.settingsCategory == 1) { // Theme
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Theme & Appearance");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Color Theme Preset:");
        const char* themes[] = { "Charcoal Sketch (Default)", "Dracula Night", "Deep Midnight" };
        ImGui::Combo("##themecombo", &g_state.selectedTheme, themes, IM_ARRAYSIZE(themes));

        ImGui::Spacing();
        ImGui::SliderFloat("UI Scale", &g_state.uiScale, 0.8f, 1.4f, "%.2fx");
        ImGui::Spacing();
        if (ImGui::Button("Reset to Default Sketch Theme")) {
            ApplySketchTheme();
        }
    } else if (g_state.settingsCategory == 5) { // Stat
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Network & P2P Stream Stats");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("P2P Engine: Direct Mesh Transport (Tailscale Encrypted)");
        ImGui::Text("Ping: 12 ms");
        ImGui::Text("Packet Loss: 0.00 %");
        ImGui::Text("Framerate Presentation: 60.0 FPS (VSync On)");
        ImGui::Text("Graphics API: DirectX 11.0 (Hardware SwapChain)");
#if defined(_DEBUG) || !defined(NDEBUG)
    } else if (g_state.settingsCategory == 6) { // Debug Tools
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.5f, 1.0f), "Debug & Simulation Tools (DEBUG BUILD)");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Checkbox("Show Floating Debug Window (Hotkey: F12)", &g_debug.showDebugWindow);
        ImGui::Checkbox("Show Topbar Debug Button", &g_debug.showDebugButton);
        ImGui::Spacing();

        ImGui::Text("Simulated Video Stream Grid (Current Connection Tab):");
        ImGui::SliderInt("##settings_sim_streams", &g_debug.simulatedStreamCount, 0, 8, "%d active feeds");
        if (ImGui::Button("View Streams in Current Connection")) {
            if (g_debug.simulatedStreamCount == 0) g_debug.simulatedStreamCount = 2;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::Spacing();
        ImGui::Separator();

        ImGui::Text("Fake Participants Quick Actions:");
        if (ImGui::Button("+ Add Fake Peer (Online)", ImVec2(180.0f * g_dpiScale, 30.0f * g_dpiScale))) {
            Peer fp;
            fp.ip = "100.113." + std::to_string(100 + g_debug.fakePeerCounter) + "." + std::to_string(g_debug.fakePeerCounter);
            fp.name = "FakePeer_" + std::to_string(g_debug.fakePeerCounter++);
            fp.status = PeerStatus::Online;
            fp.latencyMs = 15;
            g_state.peers.push_back(fp);
            SavePeers(g_state);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Add Waiting Peer", ImVec2(160.0f * g_dpiScale, 30.0f * g_dpiScale))) {
            Peer fp;
            fp.ip = "100.113." + std::to_string(100 + g_debug.fakePeerCounter) + "." + std::to_string(g_debug.fakePeerCounter);
            fp.name = "WaitingPeer_" + std::to_string(g_debug.fakePeerCounter++);
            fp.status = PeerStatus::Waiting;
            fp.latencyMs = 45;
            g_state.peers.push_back(fp);
            SavePeers(g_state);
        }

        ImGui::Spacing();
        ImGui::Checkbox("Simulate 'me' speaking", &g_state.isMicSpeaking);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-cycle VAD (simulate talkers)", &g_debug.autoCycleVAD);
#endif
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s Settings", categories[g_state.settingsCategory]);
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("Configuration options for %s are ready and integrated.", categories[g_state.settingsCategory]);
    }

    ImGui::EndChild();
}

// Modals for Rename and Delete
static void RenderModals() {
    // Rename Modal
    if (g_state.showRenameModal) {
        ImGui::OpenPopup("Rename Peer##modal");
    }

    if (ImGui::BeginPopupModal("Rename Peer##modal", &g_state.showRenameModal, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_state.renamePeerIndex >= 0 && g_state.renamePeerIndex < (int)g_state.peers.size()) {
            ImGui::Text("Rename peer: %s", g_state.peers[g_state.renamePeerIndex].ip.c_str());
            ImGui::Spacing();
            ImGui::InputText("New Name", g_state.renameBuffer, sizeof(g_state.renameBuffer));
            ImGui::Spacing();

            if (ImGui::Button("Save", ImVec2(100.0f * g_dpiScale, 32.0f * g_dpiScale))) {
                if (strlen(g_state.renameBuffer) > 0) {
                    g_state.peers[g_state.renamePeerIndex].name = g_state.renameBuffer;
                    SavePeers(g_state);
                }
                g_state.showRenameModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100.0f * g_dpiScale, 32.0f * g_dpiScale))) {
                g_state.showRenameModal = false;
                ImGui::CloseCurrentPopup();
            }
        } else {
            g_state.showRenameModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Delete Modal
    if (g_state.showDeleteModal) {
        ImGui::OpenPopup("Delete Peer##modal");
    }

    if (ImGui::BeginPopupModal("Delete Peer##modal", &g_state.showDeleteModal, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_state.deletePeerIndex >= 0 && g_state.deletePeerIndex < (int)g_state.peers.size()) {
            const auto& peer = g_state.peers[g_state.deletePeerIndex];
            ImGui::Text("Are you sure you want to remove this peer?");
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s (%s)", peer.name.c_str(), peer.ip.c_str());
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Delete", ImVec2(100.0f * g_dpiScale, 32.0f * g_dpiScale))) {
                g_state.peers.erase(g_state.peers.begin() + g_state.deletePeerIndex);
                SavePeers(g_state);
                g_state.showDeleteModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100.0f * g_dpiScale, 32.0f * g_dpiScale))) {
                g_state.showDeleteModal = false;
                ImGui::CloseCurrentPopup();
            }
        } else {
            g_state.showDeleteModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

#if defined(_DEBUG) || !defined(NDEBUG)
void RenderDebugWindow() {
    ImGui::SetNextWindowSize(ImVec2(500.0f * g_dpiScale, 620.0f * g_dpiScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(30.0f * g_dpiScale, 90.0f * g_dpiScale), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("🛠 Debug & Simulation Tools##dbg_floating", &g_debug.showDebugWindow, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.55f, 1.0f), "Poupou P2P Stream - Internal Debugger");
    ImGui::TextDisabled("Press [F12] or click [🛠 Debug] in topbar to show/hide this panel.");
    ImGui::Separator();
    ImGui::Spacing();

    // 1. Virtual Video Stream Grid Simulation
    if (ImGui::CollapsingHeader("📹 Simulated Video Streams / Visio Grid", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Simulate reception of 1 or more virtual video streams:");
        ImGui::SliderInt("##dbg_stream_slider", &g_debug.simulatedStreamCount, 0, 8, "%d active streams");
        ImGui::Spacing();

        if (ImGui::Button("0: Avatars Only", ImVec2(105.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 0;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("1 Stream", ImVec2(75.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 1;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("2 Streams", ImVec2(75.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 2;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("3 Streams", ImVec2(75.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 3;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("4 Streams", ImVec2(75.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 4;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("6 Streams", ImVec2(75.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 6;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("8 Streams", ImVec2(75.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 8;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::Spacing();
        ImGui::Text("VS Code Grid Layout Override:");
        if (ImGui::Button("Auto-fit Grid", ImVec2(100.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.gridLayoutMode = 0;
        ImGui::SameLine();
        if (ImGui::Button("Split 1x2 (H)", ImVec2(100.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.gridLayoutMode = 1;
        ImGui::SameLine();
        if (ImGui::Button("Split 2x1 (V)", ImVec2(100.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.gridLayoutMode = 2;
        ImGui::SameLine();
        if (ImGui::Button("Grid 2x2", ImVec2(80.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.gridLayoutMode = 3;
        ImGui::Spacing();
    }

    // 2. Fake Participants Generator
    if (ImGui::CollapsingHeader("👥 Fake Participants Generator (Stress Test)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Add fake peers on the fly to test responsive scaling & mesh:");

        if (ImGui::Button("+ Add Fake Peer (Online)", ImVec2(190.0f * g_dpiScale, 30.0f * g_dpiScale))) {
            Peer fp;
            fp.ip = "100.113." + std::to_string(100 + g_debug.fakePeerCounter) + "." + std::to_string(g_debug.fakePeerCounter);
            fp.name = "FakePeer_" + std::to_string(g_debug.fakePeerCounter++);
            fp.status = PeerStatus::Online;
            fp.latencyMs = 12 + (rand() % 25);
            fp.isSpeaking = false;
            g_state.peers.push_back(fp);
            SavePeers(g_state);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Add Waiting Peer (Yellow)", ImVec2(200.0f * g_dpiScale, 30.0f * g_dpiScale))) {
            Peer fp;
            fp.ip = "100.113." + std::to_string(100 + g_debug.fakePeerCounter) + "." + std::to_string(g_debug.fakePeerCounter);
            fp.name = "WaitingPeer_" + std::to_string(g_debug.fakePeerCounter++);
            fp.status = PeerStatus::Waiting;
            fp.latencyMs = 45;
            fp.isSpeaking = false;
            g_state.peers.push_back(fp);
            SavePeers(g_state);
        }

        ImGui::Spacing();
        if (ImGui::Button("+ Add 3 Online Peers", ImVec2(160.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            for (int k = 0; k < 3; ++k) {
                Peer fp;
                fp.ip = "100.113." + std::to_string(100 + g_debug.fakePeerCounter) + "." + std::to_string(g_debug.fakePeerCounter);
                fp.name = "User_" + std::to_string(g_debug.fakePeerCounter++);
                fp.status = PeerStatus::Online;
                fp.latencyMs = 10 + (rand() % 30);
                g_state.peers.push_back(fp);
            }
            SavePeers(g_state);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All Fake Peers", ImVec2(160.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            for (auto it = g_state.peers.begin(); it != g_state.peers.end(); ) {
                if (it->name.find("FakePeer_") == 0 || it->name.find("WaitingPeer_") == 0 || it->name.find("User_") == 0) {
                    it = g_state.peers.erase(it);
                } else {
                    ++it;
                }
            }
            SavePeers(g_state);
        }
        ImGui::Spacing();
    }

    // 3. Instant Peer Status & Voice Activity Switcher
    if (ImGui::CollapsingHeader("⚡ Instant Peer Status & Voice Switcher", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Simulate 'me' speaking (Voice Activity Ring)", &g_state.isMicSpeaking);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-cycle VAD (talkers)", &g_debug.autoCycleVAD);

        ImGui::Spacing();
        ImGui::Text("Registered Peers (%zu total):", g_state.peers.size());

        ImGui::BeginChild("##debug_peer_list", ImVec2(0, 220.0f * g_dpiScale), true);
        for (size_t i = 0; i < g_state.peers.size(); ++i) {
            auto& peer = g_state.peers[i];
            ImGui::PushID((int)i);

            // Status dot
            ImVec4 stCol = (peer.status == PeerStatus::Online) ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f) :
                           ((peer.status == PeerStatus::Waiting) ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.85f, 0.3f, 0.3f, 1.0f));
            ImGui::TextColored(stCol, "●");
            ImGui::SameLine();
            ImGui::Text("%s", peer.name.c_str());

            // Status buttons
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 230.0f * g_dpiScale);
            if (ImGui::SmallButton("Offline")) {
                peer.status = PeerStatus::Offline;
                peer.isSpeaking = false;
                SavePeers(g_state);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Waiting")) {
                peer.status = PeerStatus::Waiting;
                peer.isSpeaking = false;
                SavePeers(g_state);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Online")) {
                peer.status = PeerStatus::Online;
                SavePeers(g_state);
            }
            ImGui::SameLine();
            ImGui::Checkbox("Voice", &peer.isSpeaking);

            ImGui::SameLine();
            if (ImGui::SmallButton("✕")) {
                g_state.peers.erase(g_state.peers.begin() + i);
                SavePeers(g_state);
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
            ImGui::Separator();
        }
        if (g_state.peers.empty()) {
            ImGui::TextDisabled("No peers registered. Click 'Add Fake Peer' above.");
        }
        ImGui::EndChild();
    }

    // 4. Viewport Resolution & Scaling Quick Test
    if (ImGui::CollapsingHeader("🔍 UI Scaling Quick Test")) {
        ImGui::Text("Simulate different DPI / UI Scales:");
        if (ImGui::Button("1.00x", ImVec2(60.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.uiScale = 1.0f;
        ImGui::SameLine();
        if (ImGui::Button("1.25x", ImVec2(60.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.uiScale = 1.25f;
        ImGui::SameLine();
        if (ImGui::Button("1.50x", ImVec2(60.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.uiScale = 1.5f;
        ImGui::SameLine();
        if (ImGui::Button("2.00x", ImVec2(60.0f * g_dpiScale, 26.0f * g_dpiScale))) g_state.uiScale = 2.0f;
    }

    ImGui::End();
}
#endif

void RenderGui(HWND hWnd, Dx11Context& dx) {
    if (ImGui::GetCurrentContext() == nullptr) return;
    if (ImGui::GetIO().BackendRendererUserData == nullptr) return;

    RECT rect;
    GetClientRect(hWnd, &rect);
    float windowWidth = (float)(rect.right - rect.left);
    float windowHeight = (float)(rect.bottom - rect.top);
    if (windowWidth < 100.0f || windowHeight < 100.0f) return;

    // Requirement 3: Dynamic viewport & resolution scaling
    // Smoothly scale UI elements and font based on actual viewport resolution (1080p, 2K, 4K)
    float scaleX = windowWidth / 1180.0f;
    float scaleY = windowHeight / 720.0f;
    float dynamicScale = (scaleX < scaleY) ? scaleX : scaleY;

    // Also factor in monitor DPI scale if higher
    UINT winDpi = GetDpiForWindow(hWnd);
    if (winDpi > 0) {
        float monitorDpiScale = (float)winDpi / 96.0f;
        if (dynamicScale < monitorDpiScale) {
            dynamicScale = monitorDpiScale;
        }
    }
    dynamicScale *= g_state.uiScale;
    if (dynamicScale < 0.85f) dynamicScale = 0.85f;
    if (dynamicScale > 3.50f) dynamicScale = 3.50f;

    UpdateDpiScale(dynamicScale);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

#if defined(_DEBUG) || !defined(NDEBUG)
    // F12 Hotkey to toggle Debug window
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
        g_debug.showDebugWindow = !g_debug.showDebugWindow;
    }

    // Auto-cycle VAD simulation
    if (g_debug.autoCycleVAD) {
        g_debug.cycleTimer += ImGui::GetIO().DeltaTime;
        if (g_debug.cycleTimer >= 1.5f) {
            g_debug.cycleTimer = 0.0f;
            for (auto& p : g_state.peers) {
                if (p.status == PeerStatus::Online) {
                    p.isSpeaking = ((rand() % 3) == 0);
                } else {
                    p.isSpeaking = false;
                }
            }
            g_state.isMicSpeaking = ((rand() % 4) == 0);
        }
    }
#endif

    // Fullscreen main ImGui viewport window covering the entire client area
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
    ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoDecoration | 
                                 ImGuiWindowFlags_NoMove | 
                                 ImGuiWindowFlags_NoResize | 
                                 ImGuiWindowFlags_NoSavedSettings | 
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##PoupouMainViewport", nullptr, mainFlags);
    ImGui::PopStyleVar(2);

    // Active Tab Content & Header Layering
    if (g_state.currentTab == AppTab::CurrentConnection) {
        // Fullscreen edge-to-edge video canvas first
        RenderCurrentConnectionView(windowWidth, windowHeight);
    } else {
        float contentWidth = windowWidth;
        float contentHeight = windowHeight - 84.0f * g_dpiScale;
        if (g_state.currentTab == AppTab::Home) {
            RenderHomeView(contentWidth, contentHeight);
        } else if (g_state.currentTab == AppTab::Setting) {
            RenderSettingsView(contentWidth, contentHeight);
        }
    }

    // TopBar header drawn strictly on top of all views (highest z-order, transparent overlay)
    RenderHeader(hWnd, windowWidth);

    // 3. Modals
    RenderModals();

    ImGui::End();

#if defined(_DEBUG) || !defined(NDEBUG)
    if (g_debug.showDebugWindow) {
        RenderDebugWindow();
    }
#endif

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
