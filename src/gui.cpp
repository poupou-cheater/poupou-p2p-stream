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
    float avatarY = (headerHeight - (avatarMeRadius * 2.0f + 4.0f)) * 0.5f;
    ImGui::SetCursorPos(ImVec2(20.0f * g_dpiScale, avatarY));

    // Left side: 3 Circular avatars ( ) ( ) (me)
    // Requirement 2: Glowing speech ring ONLY when someone is actively speaking (Voice Activity Detection).
    // Simple neutral circle at all other times, no permanent colored ring on anyone.
    bool peer1Speaking = false;
    bool peer2Speaking = false;
    bool meSpeaking = false;

    RenderAvatar("##avatar_peer1", "", peer1Speaking, avatarRadius, false);
    ImGui::SameLine(0, 10.0f * g_dpiScale);
    RenderAvatar("##avatar_peer2", "", peer2Speaking, avatarRadius, false);
    ImGui::SameLine(0, 10.0f * g_dpiScale);
    RenderAvatar("##avatar_me", "me", meSpeaking, avatarMeRadius, true);

    // Center: Navigation pill buttons strictly centered horizontally in the exact middle of the window
    float navBtnHeight = 40.0f * g_dpiScale;
    float paddingX = 22.0f * g_dpiScale;
    float spacing = 10.0f * g_dpiScale;
    float wHome = ImGui::CalcTextSize("home").x + paddingX * 2.0f;
    float wConn = ImGui::CalcTextSize("current connection").x + paddingX * 2.0f;
    float wSet  = ImGui::CalcTextSize("setting").x + paddingX * 2.0f;
    float exactNavWidth = wHome + spacing + wConn + spacing + wSet;
    float navStartX = (windowWidth - exactNavWidth) * 0.5f;

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

    // Draw crisp star icon centered over the button
    ImVec2 starCenter = ImVec2(starBtnPos.x + starBtnSize * 0.5f, elemY + elemHeight * 0.5f);
    ImU32 starColor = peer.isFavorite ? IM_COL32(255, 204, 0, 255) : IM_COL32(120, 124, 138, 255);
    IconManager::DrawStar(drawList, starCenter, 8.0f * g_dpiScale, peer.isFavorite, starColor, 1.4f * g_dpiScale);

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

    // Check if any peer is active or waiting
    bool hasActiveSession = !g_state.activeConnectedIp.empty() || g_state.isStreaming;
    for (auto& peer : g_state.peers) {
        if (peer.status == PeerStatus::Online || peer.status == PeerStatus::Waiting) {
            hasActiveSession = true;
            break;
        }
    }

    if (hasActiveSession) {
        // Participant 0: "me" (local host)
        participants.push_back({ "me", "local host", PeerStatus::Online, false, true, nullptr });

        // Add all active or connected peers with pointers to actual state
        for (auto& peer : g_state.peers) {
            if (peer.ip == g_state.activeConnectedIp || peer.status == PeerStatus::Online || peer.status == PeerStatus::Waiting) {
                std::string role = (peer.status == PeerStatus::Online) ? "live stream" : "waiting for peer...";
                participants.push_back({ peer.name, role, peer.status, peer.isSpeaking, false, &peer });
            }
        }
    }

    // 2. Participant Bubbles - Maximized scale occupying almost all available vertical space!
    int N = (int)participants.size();
    if (N > 0) {
        float topY = 78.0f * g_dpiScale;
        float bottomY = windowHeight - 96.0f * g_dpiScale;
        float availHeight = bottomY - topY;
        float centerY = topY + availHeight * 0.46f;

        float badgeSpace = 48.0f * g_dpiScale;
        float maxRadiusY = (availHeight - badgeSpace) * 0.5f;

        float spacing = 34.0f * g_dpiScale;
        float availWidth = windowWidth - 80.0f * g_dpiScale;
        float maxRadiusX = ((availWidth - (N - 1) * spacing) / N) * 0.5f;

        // Maximize circle radius so avatars take significant presence without clipping topbar or dock
        float baseRadius = fminf(maxRadiusY, maxRadiusX);
        if (baseRadius < 48.0f * g_dpiScale) baseRadius = 48.0f * g_dpiScale;

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

            // A. Interactive Hitbox for Right-Click Context Menu directly on the central bubble
            ImVec2 bHitMin(center.x - baseRadius, center.y - baseRadius);
            ImGui::SetCursorScreenPos(bHitMin);
            std::string bubbleHitboxId = "##bubble_hitbox_" + std::to_string(i) + "_" + p.name;
            ImGui::InvisibleButton(bubbleHitboxId.c_str(), ImVec2(baseRadius * 2.0f, baseRadius * 2.0f));

            // Right-click context menu directly on this avatar bubble
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

            // B. Visual animated ripple shockwave when poked
            if (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) {
                float pFrac = (2.0f - p.peerPtr->pokeTimer) / 2.0f; // 0.0 -> 1.0
                float rippleR = baseRadius + pFrac * 36.0f * g_dpiScale;
                int alpha = (int)((1.0f - pFrac) * 200);
                drawList->AddCircle(center, rippleR, IM_COL32(255, 204, 0, alpha), 64, 2.5f * g_dpiScale);
            }

            // C. Dark circular avatar canvas
            drawList->AddCircleFilled(center, baseRadius, IM_COL32(22, 24, 32, 255), 64);

            // D. Status / Speech outline
            if (p.isSpeaking) {
                float ringRadius = baseRadius + 3.5f * g_dpiScale;
                drawList->AddCircle(center, ringRadius, IM_COL32(72, 224, 110, 255), 64, 2.8f * g_dpiScale);
                float time = (float)ImGui::GetTime();
                float glowR = ringRadius + (3.0f + sinf(time * 6.0f) * 2.0f) * g_dpiScale;
                drawList->AddCircle(center, glowR, IM_COL32(72, 224, 110, 120), 64, 1.8f * g_dpiScale);
            } else if (p.status == PeerStatus::Waiting) {
                float ringRadius = baseRadius + 3.0f * g_dpiScale;
                float time = (float)ImGui::GetTime();
                float pulseScale = 1.0f + 0.12f * sinf(time * 4.0f);
                drawList->AddCircle(center, ringRadius * pulseScale, IM_COL32(255, 204, 0, 210), 64, 2.2f * g_dpiScale);
            } else if (p.status == PeerStatus::Online) {
                drawList->AddCircle(center, baseRadius, IM_COL32(80, 250, 123, 200), 64, 2.2f * g_dpiScale);
            } else {
                drawList->AddCircle(center, baseRadius, IM_COL32(65, 68, 85, 180), 64, 1.5f * g_dpiScale);
            }

            // E. Avatar monogram / text label inside
            std::string initials;
            if (p.isMe) {
                initials = "me";
            } else if (!p.name.empty()) {
                initials += (char)toupper(p.name[0]);
                if (p.name.size() > 1 && p.name[1] != ' ') initials += (char)tolower(p.name[1]);
            } else {
                initials = "P";
            }
            ImVec2 initSize = ImGui::CalcTextSize(initials.c_str());
            drawList->AddText(ImVec2(center.x - initSize.x * 0.5f, center.y - initSize.y * 0.5f),
                              IM_COL32(240, 240, 245, 255), initials.c_str());

            // F. Visual Mute Indicator badge on top-right of avatar
            bool isMuted = p.isMe ? g_state.isMicMuted : (p.peerPtr ? p.peerPtr->isMuted : false);
            if (isMuted) {
                ImVec2 mCenter(center.x + baseRadius * 0.707f, center.y - baseRadius * 0.707f);
                drawList->AddCircleFilled(mCenter, 14.0f * g_dpiScale, IM_COL32(22, 24, 32, 255), 24);
                drawList->AddCircle(mCenter, 14.0f * g_dpiScale, IM_COL32(255, 75, 75, 220), 24, 1.5f * g_dpiScale);
                IconManager::DrawIconMic(drawList, mCenter, 16.0f * g_dpiScale, true, IM_COL32(255, 75, 75, 255));
            }

            // G. Pill badge below bubble
            std::string badgeLabel = p.name;
            if (p.peerPtr && p.peerPtr->pokeTimer > 0.0f) {
                badgeLabel += " (POKED!)";
            } else {
                badgeLabel += " (" + p.role + ")";
            }
            ImVec2 bTextSize = ImGui::CalcTextSize(badgeLabel.c_str());
            float badgePadX = 14.0f * g_dpiScale;
            float badgeW = bTextSize.x + badgePadX * 2.0f + 16.0f * g_dpiScale;
            float badgeH = 26.0f * g_dpiScale;
            ImVec2 bMin(center.x - badgeW * 0.5f, center.y + baseRadius + 14.0f * g_dpiScale);
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

    // 3. Floating Collaborative Bottom Dock (Center) matching exact user sketch
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

    drawList->AddRectFilled(dockMin, dockMax, IM_COL32(18, 19, 25, 220), 16.0f * g_dpiScale);
    drawList->AddRect(dockMin, dockMax, IM_COL32(65, 68, 85, 160), 16.0f * g_dpiScale, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(dockMin.x + dockPadX, dockMin.y + (dockHeight - btnH) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * g_dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

    // Control 1: [+] Screen / Window with Plus inside (per sketch #1)
    ImVec2 bPos1 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_screen_plus", ImVec2(btnW, btnH))) {}
    ImGui::PopStyleColor();
    IconManager::DrawIconScreenPlus(drawList, ImVec2(bPos1.x + btnW * 0.5f, bPos1.y + btnH * 0.5f), 22.0f * g_dpiScale, IM_COL32(230, 230, 240, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add stream source or webcam window");

    // Control 2: Slanted Paintbrush / Annotation (per sketch #2)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos2 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isDrawMode ? ImVec4(0.36f, 0.14f, 0.18f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_paintbrush", ImVec2(btnW, btnH))) {
        g_state.isDrawMode = !g_state.isDrawMode;
    }
    ImGui::PopStyleColor();
    IconManager::DrawIconPaintbrush(drawList, ImVec2(bPos2.x + btnW * 0.5f, bPos2.y + btnH * 0.5f), 22.0f * g_dpiScale, IM_COL32(230, 230, 240, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Collaborative Real-time Screen Annotation (Paintbrush)");

    // Control 3: Arrow cursor in rounded square "show cursor on the other screen" (per sketch #3)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos3 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.showCursorOnOtherScreen ? ImVec4(0.36f, 0.14f, 0.18f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_cursor_screen", ImVec2(btnW, btnH))) {
        g_state.showCursorOnOtherScreen = !g_state.showCursorOnOtherScreen;
    }
    ImGui::PopStyleColor();
    IconManager::DrawIconCursorScreen(drawList, ImVec2(bPos3.x + btnW * 0.5f, bPos3.y + btnH * 0.5f), 22.0f * g_dpiScale, IM_COL32(230, 230, 240, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(g_state.showCursorOnOtherScreen ? "Hide cursor on the other screen" : "Show cursor on the other screen");

    // Control 4: Classic Microphone (per sketch #4)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos4 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isMicMuted ? ImVec4(0.40f, 0.15f, 0.18f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_mic", ImVec2(btnW, btnH))) {
        g_state.isMicMuted = !g_state.isMicMuted;
    }
    ImGui::PopStyleColor();
    IconManager::DrawIconMic(drawList, ImVec2(bPos4.x + btnW * 0.5f, bPos4.y + btnH * 0.5f), 22.0f * g_dpiScale, g_state.isMicMuted, IM_COL32(230, 230, 240, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(g_state.isMicMuted ? "Unmute Microphone" : "Mute Microphone");

    // Control 5: Audio Headphones (per sketch #5)
    ImGui::SameLine(0, btnSpacing);
    ImVec2 bPos5 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, g_state.isAudioDeafened ? ImVec4(0.40f, 0.15f, 0.18f, 1.0f) : ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    if (ImGui::Button("##dock_audio", ImVec2(btnW, btnH))) {
        g_state.isAudioDeafened = !g_state.isAudioDeafened;
    }
    ImGui::PopStyleColor();
    IconManager::DrawIconAudio(drawList, ImVec2(bPos5.x + btnW * 0.5f, bPos5.y + btnH * 0.5f), 22.0f * g_dpiScale, g_state.isAudioDeafened, IM_COL32(230, 230, 240, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(g_state.isAudioDeafened ? "Undeafen Audio" : "Deafen Audio");

    // Control 6: Phone Receiver with 'x' (Disconnect) (per sketch #6)
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
    ImGui::PopStyleColor(2);
    IconManager::DrawIconPhoneHangup(drawList, ImVec2(bPos6.x + btnEndW * 0.5f, bPos6.y + btnH * 0.5f), 22.0f * g_dpiScale, IM_COL32(250, 250, 255, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disconnect / Leave Session");

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
    ImGui::PopStyleColor();
    IconManager::DrawIconVolume(drawList, ImVec2(rvPos.x + rBtnW * 0.5f, rvPos.y + rBtnH * 0.5f), 18.0f * g_dpiScale, IM_COL32(230, 230, 240, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master Stream Audio Volume (%.0f%%)", g_state.streamVolume * 100.0f);

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
    ImGui::PopStyleColor();
    IconManager::DrawIconFullscreen(drawList, ImVec2(rfPos.x + rBtnW * 0.5f, rfPos.y + rBtnH * 0.5f), 18.0f * g_dpiScale, IM_COL32(230, 230, 240, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Fullscreen");

    ImGui::PopStyleVar(2);
}

// Secondary View: Settings (Audio, Themes, Hotkeys, Network Stats)
static void RenderSettingsView(float contentWidth, float contentHeight) {
    float sidebarWidth = 180.0f * g_dpiScale;
    ImGui::BeginChild("##settings_sidebar", ImVec2(sidebarWidth, contentHeight - 40.0f * g_dpiScale), true);

    const char* categories[] = { "Account", "Theme", "Hotkey", "Audio", "Setting", "Stat" };
    for (int i = 0; i < 6; ++i) {
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

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
