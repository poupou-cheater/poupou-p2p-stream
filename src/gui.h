#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include "dx11.h"

enum class PeerStatus {
    Offline,
    Waiting,
    Online
};

struct Peer {
    std::string ip;
    std::string name;
    PeerStatus status = PeerStatus::Offline;
    bool isFavorite = false;
    int latencyMs = 0;
    float volume = 1.0f;
    bool isMuted = false;
    bool isDeafened = false;
    bool isSpeaking = false;
    float pokeTimer = 0.0f; // Visual feedback timer when poked
};

enum class AppTab {
    Home,
    CurrentConnection,
    Setting
};

struct GuiState {
    AppTab currentTab = AppTab::Home;
    std::vector<Peer> peers;
    
    // Add peer inputs
    char inputIp[128] = "";
    char inputName[128] = "";

    // Rename modal
    bool showRenameModal = false;
    int renamePeerIndex = -1;
    char renameBuffer[128] = "";

    // Delete modal
    bool showDeleteModal = false;
    int deletePeerIndex = -1;

    // Active connected peer
    std::string activeConnectedIp;
    bool isStreaming = false;
    bool isMicMuted = false;
    bool isAudioDeafened = false;
    bool isDrawMode = false;
    bool showCursorOnOtherScreen = true;
    bool isFullscreen = false;
    float streamVolume = 0.85f;
    bool rnnoiseNoiseSuppression = true;

    // Settings category
    int settingsCategory = 0; // 0: account, 1: theme, 2: hotkey, 3: audio, 4: setting, 5: stat
    float uiScale = 1.0f;
    int selectedTheme = 0; // 0: Dark Charcoal, 1: Dracula, 2: Midnight
};

extern float g_dpiScale;
void UpdateDpiScale(float newScale);

void InitGui(HWND hWnd, Dx11Context& dx);
void RenderGui(HWND hWnd, Dx11Context& dx);
void ShutdownGui();

void LoadPeers(GuiState& state);
void SavePeers(const GuiState& state);

