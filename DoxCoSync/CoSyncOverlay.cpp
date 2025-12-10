#include "CoSyncOverlay.h"
#include "ConsoleLogger.h"
#include "imgui.h"
#include "HamachiUtil.h"
#include "F4MP_Main.h"
#include "GNS_Session.h"

#include <cstring>

static bool g_overlayVisible = true;
static bool g_overlayInitialized = false;

// Shared IP text field
static std::string g_ipField;

// Return value buffer for CoSyncOverlay_GetHostIP()
static std::string g_lastReturnedHostIP;

// ------------------------------------------------------------
// Visibility
// ------------------------------------------------------------
void CoSyncOverlay_ToggleVisible() { g_overlayVisible = !g_overlayVisible; }
void CoSyncOverlay_Show() { g_overlayVisible = true; }
void CoSyncOverlay_Hide() { g_overlayVisible = false; }
bool CoSyncOverlay_IsVisible() { return g_overlayVisible; }

// ------------------------------------------------------------
// Init / Shutdown
// ------------------------------------------------------------
void CoSyncOverlay_Init(IDXGISwapChain* swap)
{
    LOG_INFO("[Overlay] CoSyncOverlay_Init called");
    g_overlayInitialized = true;
}

void CoSyncOverlay_Shutdown()
{
    LOG_INFO("[Overlay] Shutdown");
    g_overlayInitialized = false;
}

// ------------------------------------------------------------
// Helper for external callers: returns last host IP
// ------------------------------------------------------------
const char* CoSyncOverlay_GetHostIP()
{
    g_lastReturnedHostIP = g_ipField;
    return g_lastReturnedHostIP.c_str();
}

// ------------------------------------------------------------
// Render UI
// ------------------------------------------------------------
void CoSyncOverlay_Render()
{
    if (!g_overlayInitialized || !g_overlayVisible)
        return;

    ImGui::Begin(
        "CoSync Multiplayer",
        nullptr,
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize
    );

    // ------------------------------------------------------------
    // Auto-detect Hamachi IP ONCE
    // ------------------------------------------------------------
    if (g_ipField.empty())
    {
        std::string ip = HamachiUtil::GetHamachiIPv4();
        if (!ip.empty())
        {
            g_ipField = ip;
            LOG_INFO("[Overlay] Auto-detected Hamachi IP: %s", ip.c_str());
        }
    }

    // ============================================================
    // HOST SECTION
    // ============================================================
    ImGui::Text("Host on Hamachi IP");

    char hostBuf[64] = {};
    strncpy_s(hostBuf, g_ipField.c_str(), sizeof(hostBuf) - 1);

    if (ImGui::InputText("##hostip", hostBuf, sizeof(hostBuf)))
        g_ipField = hostBuf;

    if (ImGui::Button("Start Hosting"))
    {
        if (g_ipField.empty())
        {
            LOG_ERROR("[Overlay] Cannot host — IP field empty");
        }
        else
        {
            LOG_INFO("[Overlay] Host clicked using IP: %s", g_ipField.c_str());
            f4mp::F4MP_Main::Get().StartHosting(g_ipField);
        }
    }

    // If hosting, show active endpoint
    std::string listenStr = GNS_Session::Get().GetHostConnectString();
    if (!listenStr.empty())
    {
        ImGui::Text("Listening on: %s", listenStr.c_str());
        ImGui::Separator();
    }

    // ============================================================
    // CLIENT SECTION
    // ============================================================
    ImGui::Text("Join Game (Hamachi IP:48000)");

    char joinBuf[64] = {};
    strncpy_s(joinBuf, g_ipField.c_str(), sizeof(joinBuf) - 1);

    if (ImGui::InputText("##joinip", joinBuf, sizeof(joinBuf)))
        g_ipField = joinBuf;

    if (ImGui::Button("Join Game"))
    {
        if (g_ipField.empty())
        {
            LOG_ERROR("[Overlay] Cannot join — IP field empty");
        }
        else
        {
            std::string connectTo = g_ipField + ":48000";
            LOG_INFO("[Overlay] Join clicked -> %s", connectTo.c_str());
            f4mp::F4MP_Main::Get().StartJoining(connectTo);
        }
    }

    ImGui::Separator();
    ImGui::Text("Press INSERT to toggle overlay");

    ImGui::End();
}
