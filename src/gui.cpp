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
                else if (key == "name") currentPeer.name = val;
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
        if (wcsstr(cmd, L"--tab=conn") || wcsstr(cmd, L"--conn")) {
            g_state.currentTab = AppTab::CurrentConnection;
        }
#if defined(_DEBUG) || !defined(NDEBUG)
        if (wcsstr(cmd, L"--streams=2")) {
            g_state.currentTab = AppTab::CurrentConnection;
            g_debug.simulatedStreamCount = 2;
        } else if (wcsstr(cmd, L"--streams=0")) {
            g_state.currentTab = AppTab::CurrentConnection;
            g_debug.simulatedStreamCount = 0;
        } else if (wcsstr(cmd, L"--streams=1")) {
            g_state.currentTab = AppTab::CurrentConnection;
            g_debug.simulatedStreamCount = 1;
        }
        if (wcsstr(cmd, L"--mute")) {
            g_state.isMicMuted = true;
            for (auto& p : g_state.peers) p.isMuted = true;
        }
        if (wcsstr(cmd, L"--deafen")) {
            g_state.isAudioDeafened = true;
            for (auto& p : g_state.peers) p.isDeafened = true;
        }
        if (wcsstr(cmd, L"--hide-debug")) {
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


// Titlebar & Window Header matching the sketch
static void RenderHeader(HWND hWnd, float windowWidth) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 headerStart = ImGui::GetCursorScreenPos();
    float headerHeight = 70.0f * g_dpiScale;

    // Requirement 4: Topbar enlargement (height 70px, avatars 20px radius, larger nav pills & window buttons)
    float avatarRadius = 20.0f * g_dpiScale;
    float avatarMeRadius = 21.0f * g_dpiScale;
    // Top-left: Display ONLY active peers (Waiting [Yellow] or Online [Green]).
    // "me" and disconnected/offline peers are NOT shown in activeTopPeers per user specification.
    std::vector<Peer*> activeTopPeers;
    for (auto& peer : g_state.peers) {
        if (peer.status == PeerStatus::Waiting || peer.status == PeerStatus::Online) {
            activeTopPeers.push_back(&peer);
        }
    }

    // Requirement 1: Load default profile picture for avatar fallback
    ID3D11ShaderResourceView* defaultAvatarTex = IconManager::Get().GetImageTexture("ext/img/default profile picture.png");

    float topAvatarRadius = 17.0f * g_dpiScale;
    float topAvatarSpacing = 10.0f * g_dpiScale;
    float topAvatarY = (headerHeight - (topAvatarRadius * 2.0f)) * 0.5f;

    // Requirement 2: Strict order from left to right:
    // 1. Extreme left (x = 20px): Avatars of OTHER participants/peers
    float curX = 20.0f * g_dpiScale;

    for (size_t i = 0; i < activeTopPeers.size(); ++i) {
        Peer* peer = activeTopPeers[i];
        ImVec2 center(curX + topAvatarRadius, topAvatarY + topAvatarRadius);
        std::string btnId = "##top_peer_" + std::to_string(i) + "_" + peer->name;

        ImGui::SetCursorPos(ImVec2(curX, topAvatarY));
        ImGui::InvisibleButton(btnId.c_str(), ImVec2(topAvatarRadius * 2.0f, topAvatarRadius * 2.0f));
        bool isHovered = ImGui::IsItemHovered();

        // Draw avatar circular disc using default profile picture
        DrawCircularAvatar(drawList, defaultAvatarTex, center, topAvatarRadius, peer->name);

        // Status border
        if (peer->status == PeerStatus::Waiting) {
            // Yellow waiting ring with soft pulse
            float pulse = 1.0f + 0.08f * sinf((float)ImGui::GetTime() * 4.0f);
            drawList->AddCircle(center, topAvatarRadius * pulse, IM_COL32(255, 204, 0, 230), 32, 2.0f * g_dpiScale);
        } else if (peer->status == PeerStatus::Online) {
            // Green speech ring ONLY when actively speaking
            if (peer->isSpeaking) {
                drawList->AddCircle(center, topAvatarRadius + 2.0f * g_dpiScale, IM_COL32(72, 224, 110, 255), 32, 2.2f * g_dpiScale);
                float glow = 2.0f + 1.5f * sinf((float)ImGui::GetTime() * 6.0f);
                drawList->AddCircle(center, topAvatarRadius + (2.0f + glow) * g_dpiScale, IM_COL32(72, 224, 110, 120), 32, 1.5f * g_dpiScale);
            }
            // When silent: NO border or colored contour
        }

        // Hover Tooltip
        if (isHovered) {
            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.98f, 1.0f), "%s", peer->name.c_str());
            if (peer->status == PeerStatus::Waiting) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.1f, 1.0f), "Status: Waiting for connection...");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Status: Connected (Online)");
                ImGui::TextDisabled(peer->isSpeaking ? "Speaking" : "Idle");
            }
            ImGui::TextDisabled("Right-click for options");
            ImGui::EndTooltip();
        }

        // Right-click context menu directly on top-left small circle
        if (ImGui::BeginPopupContextItem(btnId.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
            ImGui::Text("%s Options", peer->name.c_str());
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
            ImGui::EndPopup();
        }

        curX += topAvatarRadius * 2.0f + topAvatarSpacing;
    }

    if (!activeTopPeers.empty()) {
        curX += 4.0f * g_dpiScale;
    }

    // 2. To the right of peers: Place avatar "me" (local host)
    float meRadius = 18.0f * g_dpiScale;
    float meSpacing = 14.0f * g_dpiScale;
    float meY = (headerHeight - (meRadius * 2.0f)) * 0.5f;
    float meX = curX;
    ImVec2 meCenter(meX + meRadius, meY + meRadius);

    ImGui::SetCursorPos(ImVec2(meX, meY));
    ImGui::InvisibleButton("##top_me_avatar", ImVec2(meRadius * 2.0f, meRadius * 2.0f));
    bool meHovered = ImGui::IsItemHovered();

    // Requirement 1: Draw avatar using default profile picture as circular disc
    DrawCircularAvatar(drawList, defaultAvatarTex, meCenter, meRadius, "Me");

    // Speech glowing green ring ONLY when speaking
    if (g_state.isMicSpeaking) {
        drawList->AddCircle(meCenter, meRadius + 2.0f * g_dpiScale, IM_COL32(72, 224, 110, 255), 32, 2.2f * g_dpiScale);
        float glow = 2.0f + 1.5f * sinf((float)ImGui::GetTime() * 6.0f);
        drawList->AddCircle(meCenter, meRadius + (2.0f + glow) * g_dpiScale, IM_COL32(72, 224, 110, 120), 32, 1.5f * g_dpiScale);
    }
    // When silent: NO colored ring or contour

    if (meHovered) {
        ImGui::BeginTooltip();
        ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.98f, 1.0f), "Host: Me (Local User)");
        ImGui::TextDisabled(g_state.isMicSpeaking ? "Status: Speaking" : "Status: Connected (Silent)");
        ImGui::TextDisabled("Right-click for audio / mic options");
        ImGui::EndTooltip();
    }

    if (ImGui::BeginPopupContextItem("##top_me_avatar", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::Text("Host Audio Options");
        ImGui::Separator();
        ImGui::MenuItem("Mute Microphone", nullptr, &g_state.isMicMuted);
        ImGui::MenuItem("Deafen Audio", nullptr, &g_state.isAudioDeafened);
        ImGui::MenuItem("RNNoise Noise Suppression", nullptr, &g_state.rnnoiseNoiseSuppression);
        ImGui::EndPopup();
    }

    curX += meRadius * 2.0f + meSpacing;

    // 3. Just to the right of avatar "me": Place [ home ] followed by [ current connection ] and [ setting ]
    float navBtnHeight = 40.0f * g_dpiScale;
    float paddingX = 22.0f * g_dpiScale;
    float spacing = 10.0f * g_dpiScale;
    float wHome = ImGui::CalcTextSize("home").x + paddingX * 2.0f;
    float wConn = ImGui::CalcTextSize("current connection").x + paddingX * 2.0f;
    float wSet  = ImGui::CalcTextSize("setting").x + paddingX * 2.0f;

    float navStartX = curX;
    float navY = (headerHeight - navBtnHeight) * 0.5f;
    ImGui::SetCursorPos(ImVec2(navStartX, navY));

    // Active pill color: #5c242e (exact wine/maroon tone from user's sketch)
    ImVec4 activeTabColor = ImVec4(0.361f, 0.141f, 0.180f, 1.00f); // #5c242e
    ImVec4 activeTabHover = ImVec4(0.440f, 0.170f, 0.220f, 1.00f);
    ImVec4 inactiveTabColor = ImVec4(0.094f, 0.098f, 0.118f, 1.00f);
    ImVec4 inactiveTabHover = ImVec4(0.145f, 0.153f, 0.188f, 1.00f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(paddingX, 9.0f * g_dpiScale));

    // [ home ] tab button
    bool isHome = (g_state.currentTab == AppTab::Home);
    ImGui::PushStyleColor(ImGuiCol_Button, isHome ? activeTabColor : inactiveTabColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isHome ? activeTabHover : inactiveTabHover);
    ImGui::PushStyleColor(ImGuiCol_Border, isHome ? ImVec4(0.49f, 0.19f, 0.25f, 1.0f) : ImVec4(0.23f, 0.23f, 0.27f, 1.0f));

    if (ImGui::Button("home", ImVec2(wHome, navBtnHeight))) {
        g_state.currentTab = AppTab::Home;
    }
    ImGui::PopStyleColor(3);

    // [ current connection ] tab button
    ImGui::SameLine(0, spacing);
    bool isConn = (g_state.currentTab == AppTab::CurrentConnection);
    ImGui::PushStyleColor(ImGuiCol_Button, isConn ? activeTabColor : inactiveTabColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isConn ? activeTabHover : inactiveTabHover);
    ImGui::PushStyleColor(ImGuiCol_Border, isConn ? ImVec4(0.49f, 0.19f, 0.25f, 1.0f) : ImVec4(0.23f, 0.23f, 0.27f, 1.0f));

    if (ImGui::Button("current connection", ImVec2(wConn, navBtnHeight))) {
        g_state.currentTab = AppTab::CurrentConnection;
    }
    ImGui::PopStyleColor(3);

    // [ setting ] tab button
    ImGui::SameLine(0, spacing);
    bool isSet = (g_state.currentTab == AppTab::Setting);
    ImGui::PushStyleColor(ImGuiCol_Button, isSet ? activeTabColor : inactiveTabColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isSet ? activeTabHover : inactiveTabHover);
    ImGui::PushStyleColor(ImGuiCol_Border, isSet ? ImVec4(0.49f, 0.19f, 0.25f, 1.0f) : ImVec4(0.23f, 0.23f, 0.27f, 1.0f));

    if (ImGui::Button("setting", ImVec2(wSet, navBtnHeight))) {
        g_state.currentTab = AppTab::Setting;
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    // Right: Frameless Window Controls (─, □, ✕) enlarged to 34px
    float btnSize = 34.0f * g_dpiScale;
    float controlsWidth = (btnSize * 3.0f + 14.0f * g_dpiScale);
    float controlsY = (headerHeight - btnSize) * 0.5f;

#if defined(_DEBUG) || !defined(NDEBUG)
    // Debug toggle button in topbar
    float dbgBtnW = 86.0f * g_dpiScale;
    float dbgBtnH = 30.0f * g_dpiScale;
    ImGui::SetCursorPos(ImVec2(windowWidth - controlsWidth - dbgBtnW - 22.0f * g_dpiScale, (headerHeight - dbgBtnH) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, g_debug.showDebugWindow ? ImVec4(0.48f, 0.20f, 0.25f, 1.0f) : ImVec4(0.14f, 0.15f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.24f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.38f, 0.40f, 0.55f, 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * g_dpiScale);
    if (ImGui::Button("🛠 Debug", ImVec2(dbgBtnW, dbgBtnH))) {
        g_debug.showDebugWindow = !g_debug.showDebugWindow;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Debug & Simulation Tools (F12)");
#endif

    ImGui::SetCursorPos(ImVec2(windowWidth - controlsWidth - 14.0f * g_dpiScale, controlsY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * g_dpiScale, 6.0f * g_dpiScale));

    // Minimize (─)
    ImVec2 minBtnPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.22f, 0.28f, 1.0f));
    if (ImGui::Button("##btn_min", ImVec2(btnSize, btnSize))) {
        ShowWindow(hWnd, SW_MINIMIZE);
    }
    ImGui::PopStyleColor(2);
    // Draw ─ line
    drawList->AddLine(ImVec2(minBtnPos.x + 9.0f * g_dpiScale, minBtnPos.y + btnSize * 0.5f),
                      ImVec2(minBtnPos.x + btnSize - 9.0f * g_dpiScale, minBtnPos.y + btnSize * 0.5f),
                      IM_COL32(220, 220, 230, 255), 1.6f * g_dpiScale);

    // Maximize / Restore (□)
    ImGui::SameLine(0, 5.0f * g_dpiScale);
    ImVec2 maxBtnPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.22f, 0.28f, 1.0f));
    if (ImGui::Button("##btn_max", ImVec2(btnSize, btnSize))) {
        if (IsZoomed(hWnd)) {
            ShowWindow(hWnd, SW_RESTORE);
        } else {
            ShowWindow(hWnd, SW_MAXIMIZE);
        }
    }
    ImGui::PopStyleColor(2);
    // Draw □ square
    float boxPad = 9.0f * g_dpiScale;
    drawList->AddRect(ImVec2(maxBtnPos.x + boxPad, maxBtnPos.y + boxPad),
                      ImVec2(maxBtnPos.x + btnSize - boxPad, maxBtnPos.y + btnSize - boxPad),
                      IM_COL32(220, 220, 230, 255), 1.0f, 0, 1.5f * g_dpiScale);

    // Close (✕)
    ImGui::SameLine(0, 5.0f * g_dpiScale);
    ImVec2 closeBtnPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.18f, 1.0f));
    if (ImGui::Button("##btn_close", ImVec2(btnSize, btnSize))) {
        PostMessage(hWnd, WM_CLOSE, 0, 0);
    }
    ImGui::PopStyleColor(2);
    // Draw ✕ cross
    float crossPad = 10.0f * g_dpiScale;
    drawList->AddLine(ImVec2(closeBtnPos.x + crossPad, closeBtnPos.y + crossPad),
                      ImVec2(closeBtnPos.x + btnSize - crossPad, closeBtnPos.y + btnSize - crossPad),
                      IM_COL32(220, 220, 230, 255), 1.6f * g_dpiScale);
    drawList->AddLine(ImVec2(closeBtnPos.x + btnSize - crossPad, closeBtnPos.y + crossPad),
                      ImVec2(closeBtnPos.x + crossPad, closeBtnPos.y + btnSize - crossPad),
                      IM_COL32(220, 220, 230, 255), 1.6f * g_dpiScale);
    ImGui::PopStyleVar(2);

    ImGui::SetCursorPos(ImVec2(16.0f * g_dpiScale, headerHeight + 10.0f * g_dpiScale));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}



// Render Peer Card matching user sketch (with reactive cardWidth and unified vertical alignment)
static void RenderPeerCard(Peer& peer, int index, float cardWidth) {
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
    float btnDelWidth = elemHeight;
    float spacing = 8.0f * g_dpiScale;
    float actionsWidth = btnConnWidth + spacing + btnRenameWidth + spacing + btnDelWidth;
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

    const char* connLabel = (isConnected || isWaiting) ? "disconnect" : "connect";
    if (ImGui::Button(connLabel, ImVec2(btnConnWidth, elemHeight))) {
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
    std::string renameBtnId = "rename##" + std::to_string(index);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * g_dpiScale, 6.0f * g_dpiScale));
    if (ImGui::Button(renameBtnId.c_str(), ImVec2(btnRenameWidth, elemHeight))) {
        g_state.showRenameModal = true;
        g_state.renamePeerIndex = index;
        strncpy_s(g_state.renameBuffer, peer.name.c_str(), sizeof(g_state.renameBuffer) - 1);
    }
    ImGui::PopStyleVar(2);

    // [ ✕ ] Delete Button
    ImGui::SetCursorScreenPos(ImVec2(rightEdge - btnDelWidth, elemY));
    std::string delBtnId = "X##del_" + std::to_string(index);
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
    float startY = 70.0f * g_dpiScale + padTop;
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
    float totalWindowHeight = contentHeight + 70.0f * g_dpiScale;
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
        RenderPeerCard(g_state.peers[idx], idx, cardWidth);
    }

    if (g_state.peers.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 30.0f * g_dpiScale);
        ImGui::TextDisabled("No Tailscale peers registered yet. Add one with [ ip tailscale ] above.");
    }

    ImGui::EndChild();
}

// Secondary View: Current Connection (Live Screen, Maximal Participant Avatars, and Floating Collaborative Dock)
// Edge-to-edge full bleed rendering strictly following user sketch
static void RenderCurrentConnectionView(float windowWidth, float windowHeight) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // 1. Fullscreen Edge-to-Edge Stream canvas covering (0, 0) to (windowWidth, windowHeight)
    ImVec2 streamMin = ImVec2(0, 0);
    ImVec2 streamMax = ImVec2(windowWidth, windowHeight);
    drawList->AddRectFilled(streamMin, streamMax, IM_COL32(11, 12, 16, 255));

    // Gather active session participants (Me + connected/waiting peers)
    struct ParticipantBubble {
        std::string name;
        std::string role;
        PeerStatus status;
        bool isSpeaking;
        bool isMe;
        Peer* peerPtr = nullptr;
    };

    std::vector<ParticipantBubble> participants;

    // Requirement 1: In the central area, "me" is always shown, plus ONLY peers who are ACTUALLY connected (status == Online).
    // Peers in "Waiting" status are NEVER displayed as big central bubbles!
    participants.push_back({ "me", "local host", PeerStatus::Online, g_state.isMicSpeaking, true, nullptr });

    for (auto& peer : g_state.peers) {
        if (peer.status == PeerStatus::Online) {
            participants.push_back({ peer.name, "live stream", peer.status, peer.isSpeaking, false, &peer });
        }
    }

    int streamCount = 0;
#if defined(_DEBUG) || !defined(NDEBUG)
    if (g_debug.simulatedStreamCount > 0) {
        streamCount = g_debug.simulatedStreamCount;
    }
#endif
    if (streamCount == 0 && g_state.isStreaming) {
        streamCount = 1;
    }

    // Load default profile picture texture & stream test texture via WIC
    ID3D11ShaderResourceView* defaultAvatarTex = IconManager::Get().GetImageTexture("ext/img/default profile picture.png");
    ID3D11ShaderResourceView* streamTex = IconManager::Get().GetImageTexture("ext/img/helldivers-2-1_33b62d4e81ea4ef68c12cba0363065df-4243320100.jpg");

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

    if (streamCount > 0) {
        // =========================================================================
        // Requirement 3: Full Container Layout (0 margins, 0 paddings, 100% width & height)
        // Contiguous side-by-side stream cards (0 space/margin between them) with Drag & Slide support
        // =========================================================================
        static float s_streamScrollX = 0.0f;
        static float s_streamScrollTargetX = 0.0f;
        static bool s_isDraggingStream = false;
        static float s_dragStartMouseX = 0.0f;
        static float s_dragStartScrollX = 0.0f;

        float cellW = (streamCount == 1) ? windowWidth : (windowWidth * 0.5f);
        float cellH = windowHeight;
        float totalWidth = streamCount * cellW;

        float minScrollX = fminf(0.0f, windowWidth - totalWidth);
        float maxScrollX = 0.0f;

        ImGuiIO& io = ImGui::GetIO();

        bool overDock = (io.MousePos.y >= dockMin.y - 10.0f * g_dpiScale && 
                         io.MousePos.y <= dockMax.y + 10.0f * g_dpiScale &&
                         io.MousePos.x >= dockMin.x - 10.0f * g_dpiScale && 
                         io.MousePos.x <= dockMax.x + 10.0f * g_dpiScale);
        bool overHeader = (io.MousePos.y <= 70.0f * g_dpiScale);

        if (totalWidth > windowWidth) {
            // Drag & Slide via mouse left button
            if (io.MouseDown[0] && !overDock && !overHeader && !ImGui::IsAnyItemHovered()) {
                if (!s_isDraggingStream && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    s_isDraggingStream = true;
                    s_dragStartMouseX = io.MousePos.x;
                    s_dragStartScrollX = s_streamScrollTargetX;
                }
                if (s_isDraggingStream) {
                    float delta = io.MousePos.x - s_dragStartMouseX;
                    s_streamScrollTargetX = s_dragStartScrollX + delta;
                    // Rubber-band resistance at boundaries
                    if (s_streamScrollTargetX > maxScrollX) {
                        float over = s_streamScrollTargetX - maxScrollX;
                        s_streamScrollTargetX = maxScrollX + over * 0.25f;
                    } else if (s_streamScrollTargetX < minScrollX) {
                        float under = minScrollX - s_streamScrollTargetX;
                        s_streamScrollTargetX = minScrollX - under * 0.25f;
                    }
                }
            } else {
                s_isDraggingStream = false;
                // Elastic snap back to boundary
                if (s_streamScrollTargetX > maxScrollX) {
                    s_streamScrollTargetX = ImLerp(s_streamScrollTargetX, maxScrollX, io.DeltaTime * 12.0f);
                } else if (s_streamScrollTargetX < minScrollX) {
                    s_streamScrollTargetX = ImLerp(s_streamScrollTargetX, minScrollX, io.DeltaTime * 12.0f);
                }
            }

            // Mouse wheel support for sliding
            if (!overDock && !overHeader && !ImGui::IsAnyItemActive()) {
                float wheelDelta = (io.MouseWheelH != 0.0f) ? io.MouseWheelH : io.MouseWheel;
                if (wheelDelta != 0.0f) {
                    s_streamScrollTargetX += wheelDelta * 85.0f * g_dpiScale;
                    s_streamScrollTargetX = ImClamp(s_streamScrollTargetX, minScrollX, maxScrollX);
                }
            }

            // Smooth animated lerp
            s_streamScrollX = ImLerp(s_streamScrollX, s_streamScrollTargetX, ImClamp(io.DeltaTime * 14.0f, 0.0f, 1.0f));
            if (fabsf(s_streamScrollX - s_streamScrollTargetX) < 0.2f) {
                s_streamScrollX = s_streamScrollTargetX;
            }
        } else {
            s_streamScrollX = 0.0f;
            s_streamScrollTargetX = 0.0f;
            s_isDraggingStream = false;
        }

        // Render each stream card contiguously
        for (int s = 0; s < streamCount; ++s) {
            float bx = s_streamScrollX + s * cellW;
            float endX = bx + cellW;

            // Frustum culling
            if (endX < 0.0f || bx > windowWidth) continue;

            ImVec2 bMin(bx, 0.0f);
            ImVec2 bMax(endX, cellH);

            // 1. Container background (0 margins, 0 paddings, 100% full screen)
            drawList->AddRectFilled(bMin, bMax, IM_COL32(12, 14, 18, 255));

            // Streamer Metadata
            std::string streamerName = (s < (int)participants.size()) ? participants[s].name : ("Peer " + std::to_string(s));
            Peer* pPeer = (s < (int)participants.size()) ? participants[s].peerPtr : nullptr;
            bool isMe = (s < (int)participants.size()) ? participants[s].isMe : false;
            bool isMuted = isMe ? g_state.isMicMuted : (pPeer ? pPeer->isMuted : false);
            bool isDeafened = isMe ? g_state.isAudioDeafened : (pPeer ? pPeer->isDeafened : false);
            bool isStreamerSpeaking = isMe ? g_state.isMicSpeaking : (pPeer ? pPeer->isSpeaking : false);

            // 2. Render Helldivers 2 video stream edge-to-edge
            if (streamTex) {
                drawList->AddImage((ImTextureID)streamTex, bMin, bMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE);
            } else {
                drawList->AddRectFilled(bMin, bMax, IM_COL32(20, 22, 28, 255));
            }

            // 3. Subtle vertical separator line between contiguous streams
            if (s < streamCount - 1) {
                drawList->AddLine(ImVec2(endX, 0.0f), ImVec2(endX, cellH), IM_COL32(45, 48, 62, 220), 2.0f * g_dpiScale);
            }

            // 4. Streamer Speaking Border Highlight
            if (isStreamerSpeaking) {
                drawList->AddRect(bMin, bMax, IM_COL32(72, 224, 110, 255), 0.0f, 0, 3.0f * g_dpiScale);
            }

            // =========================================================================
            // Requirement 4: Top-Left Streamer Status Overlay (Name + Mute + Deafen)
            // =========================================================================
            float badgeX = bMin.x + 20.0f * g_dpiScale;
            float badgeY = 82.0f * g_dpiScale; // Safely below topbar
            float iconSize = 16.0f * g_dpiScale;
            float badgeH = 32.0f * g_dpiScale;
            float padX = 12.0f * g_dpiScale;
            float spacingX = 8.0f * g_dpiScale;

            ImVec2 tSize = ImGui::CalcTextSize(streamerName.c_str());
            float innerW = tSize.x;
            if (isMuted) innerW += spacingX + iconSize;
            if (isDeafened) innerW += spacingX + iconSize;

            float badgeW = innerW + padX * 2.0f;
            ImVec2 pMin(badgeX, badgeY);
            ImVec2 pMax(badgeX + badgeW, badgeY + badgeH);

            // Sleek dark pill background
            drawList->AddRectFilled(pMin, pMax, IM_COL32(14, 16, 22, 230), 8.0f * g_dpiScale);
            if (isStreamerSpeaking) {
                drawList->AddRect(pMin, pMax, IM_COL32(72, 224, 110, 255), 8.0f * g_dpiScale, 0, 1.8f * g_dpiScale);
            } else {
                drawList->AddRect(pMin, pMax, IM_COL32(65, 70, 85, 170), 8.0f * g_dpiScale, 0, 1.0f);
            }

            // Streamer Name
            float curItemX = pMin.x + padX;
            float textY = pMin.y + (badgeH - tSize.y) * 0.5f;
            drawList->AddText(ImVec2(curItemX, textY), IM_COL32(240, 242, 248, 255), streamerName.c_str());
            curItemX += tSize.x;

            // Status Icon: Microphone Muted (MdiMicrophoneOff.svg)
            if (isMuted) {
                curItemX += spacingX;
                ImVec2 iconC(curItemX + iconSize * 0.5f, pMin.y + badgeH * 0.5f);
                IconManager::Get().DrawSvgIcon(drawList, "MdiMicrophoneOff.svg", iconC, iconSize, IM_COL32(255, 75, 75, 255));
                curItemX += iconSize;
            }

            // Status Icon: Audio Deafened / Headset Off (IcBaselineHeadsetOff.svg)
            if (isDeafened) {
                curItemX += spacingX;
                ImVec2 iconC(curItemX + iconSize * 0.5f, pMin.y + badgeH * 0.5f);
                IconManager::Get().DrawSvgIcon(drawList, "IcBaselineHeadsetOff.svg", iconC, iconSize, IM_COL32(255, 75, 75, 255));
                curItemX += iconSize;
            }

            // Interactive Tooltip & Context Menu on badge
            ImGui::SetCursorScreenPos(pMin);
            std::string badgeBtnId = "##str_badge_" + std::to_string(s);
            ImGui::InvisibleButton(badgeBtnId.c_str(), ImVec2(badgeW, badgeH));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.98f, 1.0f), "Streamer: %s", streamerName.c_str());
                ImGui::Separator();
                if (isStreamerSpeaking) ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Voice: Speaking");
                else ImGui::TextDisabled("Voice: Silent");
                if (isMuted) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Microphone: Muted");
                else ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Microphone: Active");
                if (isDeafened) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Audio: Deafened");
                else ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Audio: Active");
                ImGui::EndTooltip();
            }

            // =========================================================================
            // Top-Right Stream Overlay: Volume Control Button
            // =========================================================================
            if (!isMe && pPeer) {
                float volBtnSize = 32.0f * g_dpiScale;
                float volX = bMax.x - volBtnSize - 20.0f * g_dpiScale;
                float volY = 82.0f * g_dpiScale;
                ImVec2 vMin(volX, volY);
                ImVec2 vMax(volX + volBtnSize, volY + volBtnSize);

                drawList->AddRectFilled(vMin, vMax, IM_COL32(14, 16, 22, 230), 8.0f * g_dpiScale);
                drawList->AddRect(vMin, vMax, IM_COL32(65, 70, 85, 170), 8.0f * g_dpiScale, 0, 1.0f);

                const char* vIcon = (pPeer->volume <= 0.01f || pPeer->isMuted) ? "MaterialSymbolsNoSound.svg" : "MaterialSymbolsVolumeDown.svg";
                IconManager::Get().DrawSvgIcon(drawList, vIcon, ImVec2(volX + volBtnSize * 0.5f, volY + volBtnSize * 0.5f), 18.0f * g_dpiScale, IM_COL32(230, 235, 245, 255));

                ImGui::SetCursorScreenPos(vMin);
                std::string volBtnId = "##str_vol_" + std::to_string(s);
                if (ImGui::InvisibleButton(volBtnId.c_str(), ImVec2(volBtnSize, volBtnSize))) {
                    ImGui::OpenPopup(("##popup_vol_" + std::to_string(s)).c_str());
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Volume: %.0f%% (Click to adjust)", pPeer->volume * 100.0f);
                    ImGui::EndTooltip();
                }

                if (ImGui::BeginPopup(("##popup_vol_" + std::to_string(s)).c_str())) {
                    ImGui::Text("%s Volume", pPeer->name.c_str());
                    ImGui::Separator();
                    if (ImGui::SliderFloat("##vol_slider", &pPeer->volume, 0.0f, 1.5f, "%.0f%%")) {
                        SavePeers(g_state);
                    }
                    ImGui::MenuItem("Mute Audio", nullptr, &pPeer->isMuted);
                    ImGui::MenuItem("Deafen Audio", nullptr, &pPeer->isDeafened);
                    ImGui::EndPopup();
                }
            }
        }
    } else {
        // =========================================================================
        // Participant Bubbles Mode (When streamCount == 0)
        // Strict boundary protection: NO overlap with bottom dockbar, NO graphic residue
        // =========================================================================
        int N = (int)participants.size();
        if (N > 0) {
            float topLimit = 78.0f * g_dpiScale;
            float dockTopY = windowHeight - 56.0f * g_dpiScale - 24.0f * g_dpiScale;
            float bottomLimit = dockTopY - 24.0f * g_dpiScale; // Strict ceiling above dock
            float availableH = bottomLimit - topLimit;

            float badgeH = 26.0f * g_dpiScale;
            float badgeGap = 14.0f * g_dpiScale;
            float badgeTotalH = badgeH + badgeGap;

            float maxRadiusY = (availableH - badgeTotalH) * 0.5f;
            float spacing = 34.0f * g_dpiScale;
            float availWidth = windowWidth - 120.0f * g_dpiScale;
            float maxRadiusX = ((availWidth - (N - 1) * spacing) / N) * 0.5f;

            float baseRadius = fminf(maxRadiusY, maxRadiusX);
            if (baseRadius > 130.0f * g_dpiScale) baseRadius = 130.0f * g_dpiScale;
            if (baseRadius < 36.0f * g_dpiScale) baseRadius = 36.0f * g_dpiScale;

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

                // Interactive Hitbox for Right-Click Context Menu
                ImVec2 bHitMin(center.x - baseRadius, center.y - baseRadius);
                ImGui::SetCursorScreenPos(bHitMin);
                std::string bubbleHitboxId = "##bubble_hitbox_" + std::to_string(i) + "_" + p.name;
                ImGui::InvisibleButton(bubbleHitboxId.c_str(), ImVec2(baseRadius * 2.0f, baseRadius * 2.0f));

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

                // Poke ripple (strictly dampened to prevent overlapping dock)
                if (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) {
                    float pFrac = (2.0f - p.peerPtr->pokeTimer) / 2.0f;
                    float rippleR = baseRadius + pFrac * 20.0f * g_dpiScale;
                    int alpha = (int)((1.0f - pFrac) * 200);
                    drawList->AddCircle(center, rippleR, IM_COL32(255, 204, 0, alpha), 64, 2.5f * g_dpiScale);
                }

                // Requirement 1: Draw avatar using default profile picture as circular disc
                std::string initials = p.isMe ? "me" : p.name;
                DrawCircularAvatar(drawList, defaultAvatarTex, center, baseRadius, initials);

                // Voice Activity Detection: Glowing green ring ONLY when speaking
                if (p.isSpeaking) {
                    float ringRadius = baseRadius + 3.5f * g_dpiScale;
                    drawList->AddCircle(center, ringRadius, IM_COL32(72, 224, 110, 255), 64, 2.8f * g_dpiScale);
                    float time = (float)ImGui::GetTime();
                    float glowR = ringRadius + (3.0f + sinf(time * 6.0f) * 2.0f) * g_dpiScale;
                    drawList->AddCircle(center, glowR, IM_COL32(72, 224, 110, 120), 64, 1.8f * g_dpiScale);
                }
                // When silent: NO colored ring or contour!

                // Mute indicator badge on avatar
                bool isMuted = p.isMe ? g_state.isMicMuted : (p.peerPtr ? p.peerPtr->isMuted : false);
                if (isMuted) {
                    ImVec2 mCenter(center.x + baseRadius * 0.707f, center.y - baseRadius * 0.707f);
                    drawList->AddCircleFilled(mCenter, 14.0f * g_dpiScale, IM_COL32(22, 24, 32, 255), 24);
                    drawList->AddCircle(mCenter, 14.0f * g_dpiScale, IM_COL32(255, 75, 75, 220), 24, 1.5f * g_dpiScale);
                    IconManager::Get().DrawSvgIcon(drawList, "MdiMicrophoneOff.svg", mCenter, 16.0f * g_dpiScale, IM_COL32(255, 75, 75, 255));
                }

                // Pill badge below bubble
                std::string badgeLabel = p.name;
                if (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) {
                    badgeLabel += " (POKED!)";
                } else {
                    badgeLabel += " (" + p.role + ")";
                }
                ImVec2 bTextSize = ImGui::CalcTextSize(badgeLabel.c_str());
                float badgePadX = 14.0f * g_dpiScale;
                float badgeW = bTextSize.x + badgePadX * 2.0f + 16.0f * g_dpiScale;
                ImVec2 bMin(center.x - badgeW * 0.5f, center.y + baseRadius + badgeGap);
                ImVec2 bMax(bMin.x + badgeW, bMin.y + badgeH);

                drawList->AddRectFilled(bMin, bMax, IM_COL32(18, 19, 25, 220), 12.0f * g_dpiScale);
                drawList->AddRect(bMin, bMax, (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) ? IM_COL32(255, 204, 0, 220) : IM_COL32(50, 52, 65, 180), 12.0f * g_dpiScale, 0, 1.0f);

                // Status dot in badge
                ImVec2 dotC(bMin.x + 12.0f * g_dpiScale, bMin.y + badgeH * 0.5f);
                IconManager::DrawStatusIndicator(drawList, dotC, 3.5f * g_dpiScale, p.status, true);

                // Badge text
                drawList->AddText(ImVec2(bMin.x + 22.0f * g_dpiScale, bMin.y + (badgeH - bTextSize.y) * 0.5f),
                                  IM_COL32(230, 232, 240, 255), badgeLabel.c_str());
            }
        } else {
            const char* msg = "No active peer stream. Select a peer on the 'Home' tab and click [ connect ].";
            ImVec2 ms = ImGui::CalcTextSize(msg);
            drawList->AddText(ImVec2((windowWidth - ms.x) * 0.5f, windowHeight * 0.46f),
                              IM_COL32(150, 154, 170, 255), msg);
        }
    }

    // =========================================================================
    // 3. Floating Collaborative Bottom Dock (Center) in Overlay
    // =========================================================================
    // Fully opaque background with crisp border: NO circle artifacts or residual graphics visible underneath
    drawList->AddRectFilled(dockMin, dockMax, IM_COL32(18, 19, 25, 250), 16.0f * g_dpiScale);
    drawList->AddRect(dockMin, dockMax, IM_COL32(65, 68, 85, 200), 16.0f * g_dpiScale, 0, 1.2f);

    ImGui::SetCursorScreenPos(ImVec2(dockMin.x + dockPadX, dockMin.y + (dockHeight - btnH) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

    // Control 1: [+] Screen / Window with Plus inside (MaterialSymbolsAddPhotoAlternate.svg per sketch #1)
    ImVec2 bPos1 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_screen_plus", ImVec2(btnW, btnH))) {}
    bool hovered1 = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
    ImU32 col1 = hovered1 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255);
    IconManager::Get().DrawSvgIcon(drawList, "MaterialSymbolsAddPhotoAlternate.svg", ImVec2(bPos1.x + btnW * 0.5f, bPos1.y + btnH * 0.5f), 24.0f * g_dpiScale, col1);
    if (hovered1) ImGui::SetTooltip("Add stream source or webcam window");

    // Control 2: Slanted Paintbrush / Annotation (BoxiconsBrushFilled.svg per sketch #2)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos2 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isDrawMode ? ImVec4(0.36f, 0.14f, 0.18f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_paintbrush", ImVec2(btnW, btnH))) {
        g_state.isDrawMode = !g_state.isDrawMode;
    }
    bool hovered2 = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
    ImU32 col2 = g_state.isDrawMode ? IM_COL32(255, 120, 140, 255) : (hovered2 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255));
    IconManager::Get().DrawSvgIcon(drawList, "BoxiconsBrushFilled.svg", ImVec2(bPos2.x + btnW * 0.5f, bPos2.y + btnH * 0.5f), 24.0f * g_dpiScale, col2);
    if (hovered2) ImGui::SetTooltip("Toggle Collaborative Real-time Screen Annotation (Paintbrush)");

    // Control 3: Arrow cursor in rounded square "show cursor on the other screen" (TablerPointer2.svg per sketch #3)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos3 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.showCursorOnOtherScreen ? ImVec4(0.18f, 0.28f, 0.40f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_cursor_screen", ImVec2(btnW, btnH))) {
        g_state.showCursorOnOtherScreen = !g_state.showCursorOnOtherScreen;
    }
    bool hovered3 = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
    ImU32 col3 = g_state.showCursorOnOtherScreen ? IM_COL32(100, 180, 255, 255) : (hovered3 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255));
    IconManager::Get().DrawSvgIcon(drawList, "TablerPointer2.svg", ImVec2(bPos3.x + btnW * 0.5f, bPos3.y + btnH * 0.5f), 24.0f * g_dpiScale, col3);
    if (hovered3) ImGui::SetTooltip(g_state.showCursorOnOtherScreen ? "Hide cursor on the other screen" : "Show cursor on the other screen");

    // Control 4: Classic Microphone (MdiMicrophone.svg / MdiMicrophoneOff.svg per sketch #4)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos4 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isMicMuted ? ImVec4(0.40f, 0.15f, 0.18f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_mic", ImVec2(btnW, btnH))) {
        g_state.isMicMuted = !g_state.isMicMuted;
    }
    bool hovered4 = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
    const char* micSvg = g_state.isMicMuted ? "MdiMicrophoneOff.svg" : "MdiMicrophone.svg";
    ImU32 col4 = g_state.isMicMuted ? IM_COL32(255, 80, 80, 255) : (hovered4 ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255));
    IconManager::Get().DrawSvgIcon(drawList, micSvg, ImVec2(bPos4.x + btnW * 0.5f, bPos4.y + btnH * 0.5f), 24.0f * g_dpiScale, col4);
    if (hovered4) ImGui::SetTooltip(g_state.isMicMuted ? "Unmute Microphone" : "Mute Microphone");

    // Control 5: Audio Headphones (IcBaselineHeadset.svg / IcBaselineHeadsetOff.svg per sketch #5)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos5 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isAudioDeafened ? ImVec4(0.40f, 0.15f, 0.18f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_audio", ImVec2(btnW, btnH))) {
        g_state.isAudioDeafened = !g_state.isAudioDeafened;
    }
    bool hovered5 = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
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

    ImGui::PopStyleVar(2);

    // 4. Floating Bottom-Right Controls: "all stream volume" (Speaker + Fullscreen per sketch!)
    float rDockW = 96.0f * g_dpiScale;
    float rDockH = 56.0f * g_dpiScale;
    ImVec2 rDockMin = ImVec2(windowWidth - rDockW - 20.0f * g_dpiScale, windowHeight - rDockH - 24.0f * g_dpiScale);
    ImVec2 rDockMax = ImVec2(rDockMin.x + rDockW, rDockMin.y + rDockH);

    drawList->AddRectFilled(rDockMin, rDockMax, IM_COL32(18, 19, 25, 220), 16.0f * g_dpiScale);
    drawList->AddRect(rDockMin, rDockMax, IM_COL32(65, 68, 85, 160), 16.0f * g_dpiScale, 0, 1.0f);

    float rBtnW = 36.0f * g_dpiScale;
    float rBtnH = 38.0f * g_dpiScale;
    ImGui::SetCursorScreenPos(ImVec2(rDockMin.x + 8.0f * g_dpiScale, rDockMin.y + (rDockH - rBtnH) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

    // Master Stream Volume button
    ImVec2 rvPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_vol", ImVec2(rBtnW, rBtnH))) {
        ImGui::OpenPopup("##volume_popover");
    }
    bool hoveredVol = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
    const char* volSvg = (g_state.streamVolume <= 0.01f) ? "MaterialSymbolsNoSound.svg" : "MaterialSymbolsVolumeDown.svg";
    ImU32 colVol = hoveredVol ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255);
    IconManager::Get().DrawSvgIcon(drawList, volSvg, ImVec2(rvPos.x + rBtnW * 0.5f, rvPos.y + rBtnH * 0.5f), 20.0f * g_dpiScale, colVol);
    if (hoveredVol) ImGui::SetTooltip("Master Stream Audio Volume (%.0f%%)", g_state.streamVolume * 100.0f);

    if (ImGui::BeginPopup("##volume_popover")) {
        ImGui::Text("Master Volume");
        ImGui::SliderFloat("##stream_vol_slider", &g_state.streamVolume, 0.0f, 1.0f, "%.0f%%");
        ImGui::EndPopup();
    }

    // Fullscreen toggle button
    ImGui::SameLine(0, 8.0f * g_dpiScale);
    ImVec2 rfPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_fullscreen", ImVec2(rBtnW, rBtnH))) {
        g_state.isFullscreen = !g_state.isFullscreen;
    }
    bool hoveredFs = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
    const char* fsSvg = g_state.isFullscreen ? "MaterialSymbolsFullscreenExit.svg" : "MaterialSymbolsFullscreen.svg";
    ImU32 colFs = hoveredFs ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 240, 255);
    IconManager::Get().DrawSvgIcon(drawList, fsSvg, ImVec2(rfPos.x + rBtnW * 0.5f, rfPos.y + rBtnH * 0.5f), 20.0f * g_dpiScale, colFs);
    if (hoveredFs) ImGui::SetTooltip("Toggle Fullscreen");

    ImGui::PopStyleVar(2);
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
        ImGui::Spacing();

        ImGui::Text("Simulated Video Stream Grid (Current Connection Tab):");
        ImGui::SliderInt("##settings_sim_streams", &g_debug.simulatedStreamCount, 0, 4, "%d active feeds");
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
        ImGui::SliderInt("##dbg_stream_slider", &g_debug.simulatedStreamCount, 0, 4, "%d active streams");
        ImGui::Spacing();

        if (ImGui::Button("0: Avatars Only", ImVec2(110.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 0;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("1 Stream", ImVec2(80.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 1;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("2 Streams", ImVec2(80.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 2;
            g_state.currentTab = AppTab::CurrentConnection;
        }
        ImGui::SameLine();
        if (ImGui::Button("4 Streams Grid", ImVec2(110.0f * g_dpiScale, 28.0f * g_dpiScale))) {
            g_debug.simulatedStreamCount = 4;
            g_state.currentTab = AppTab::CurrentConnection;
        }
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
        // Header drawn on top so topbar is completely transparent above video
        RenderHeader(hWnd, windowWidth);
    } else {
        // Standard header for Home and Settings
        RenderHeader(hWnd, windowWidth);
        float contentWidth = windowWidth;
        float contentHeight = windowHeight - 70.0f * g_dpiScale;
        if (g_state.currentTab == AppTab::Home) {
            RenderHomeView(contentWidth, contentHeight);
        } else if (g_state.currentTab == AppTab::Setting) {
            RenderSettingsView(contentWidth, contentHeight);
        }
    }

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
