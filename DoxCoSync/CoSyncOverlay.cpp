#include "CoSyncOverlay.h"
#include "ConsoleLogger.h"
#include "F4MP_Main.h"
#include "GNS_Session.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

CoSyncOverlay& CoSyncOverlay::Get()
{
    static CoSyncOverlay inst;
    return inst;
}

bool CoSyncOverlay::Init(IDXGISwapChain* chain)
{
    if (initialized)
        return true;

    LOG_INFO("[Overlay] Init called");

    if (!chain)
    {
        LOG_ERROR("[Overlay] Init failed: swapchain is null");
        return false;
    }

    HRESULT hr = chain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
    if (FAILED(hr) || !device)
    {
        LOG_ERROR("[Overlay] GetDevice failed (hr=0x%08X)", hr);
        return false;
    }

    device->GetImmediateContext(&context);
    if (!context)
    {
        LOG_ERROR("[Overlay] GetImmediateContext failed");
        return false;
    }

    // -----------------------------
    // ImGui setup
    // -----------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // do not write imgui.ini to game folder

    HWND gameHwnd = FindWindowA("Fallout4", nullptr);
    if (!gameHwnd)
    {
        LOG_WARN("[Overlay] Could not find Fallout4 window, using foreground");
        gameHwnd = GetForegroundWindow();
    }

    ImGui_ImplWin32_Init(gameHwnd);
    ImGui_ImplDX11_Init(device, context);

    initialized = true;
    LOG_INFO("[Overlay] Init successful");
    return true;
}

void CoSyncOverlay::ToggleVisible()
{
    visible = !visible;
}

//
// --------------------------------------------------------------------
// MAIN WINDOW
// --------------------------------------------------------------------
void CoSyncOverlay::ShowMainWindow()
{
    ImGui::Begin("CoSync Multiplayer", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoCollapse);

    f4mp::F4MP_Main& mp = f4mp::F4MP_Main::Get();

    GNS_Session& gns = GNS_Session::Get();

    bool isHost = gns.IsHost();
    bool isClient = gns.IsClient();
    bool isConnected = gns.IsConnected();

    ImGui::Text("CoSync Multiplayer System");
    ImGui::Separator();

    ImGui::Text("Status: %s",
        isHost ? "Host" :
        isClient ? "Client" :
        "Not connected");

    ImGui::Text("Connection: %s",
        isConnected ? "Connected" : "Not connected");
    ImGui::Separator();

    // -----------------------------------------------------------
    // BEFORE HOSTING / JOINING
    // -----------------------------------------------------------
    if (!isHost && !isClient)
    {
        static char joinString[64] = "127.0.0.1:27020";

        if (ImGui::Button("Host Game"))
        {
            LOG_INFO("[Overlay] Host button pressed");
            mp.StartHosting();
        }

        ImGui::Separator();
        ImGui::Text("Join a Friend:");

        ImGui::InputText("Connect String", joinString, IM_ARRAYSIZE(joinString));

        if (ImGui::Button("Join Game"))
        {
            LOG_INFO("[Overlay] Join button pressed");
            mp.StartJoining(joinString);
        }

        ImGui::End();
        return;
    }

    // -----------------------------------------------------------
    // AFTER HOST OR CLIENT CONNECTED
    // -----------------------------------------------------------

    if (isHost)
    {
        ImGui::Text("You are the HOST");

        // GetHostConnectString returns std::string — copy into temp buffer
        std::string connect = gns.GetHostConnectString();
        char buffer[128] = {};
        strncpy_s(buffer, connect.c_str(), sizeof(buffer) - 1);

        ImGui::Text("Share this Join Code:");
        ImGui::InputText("##joincode", buffer, sizeof(buffer),
            ImGuiInputTextFlags_ReadOnly);
    }
    else if (isClient)
    {
        ImGui::Text("Connected as CLIENT");
    }

    ImGui::Separator();
    ImGui::Text("Friends & Invites (placeholder)");

    static const char* dummyFriends[] =
    {
        "FriendA", "FriendB", "FriendC"
    };
    static int selectedFriend = -1;

    ImGui::ListBox("Friends", &selectedFriend,
        dummyFriends, IM_ARRAYSIZE(dummyFriends));

    static char newFriend[32] = "";
    ImGui::InputText("Add Friend", newFriend, IM_ARRAYSIZE(newFriend));

    if (ImGui::Button("Add"))
    {
        LOG_INFO("[Overlay] Add friend (TODO): %s", newFriend);
        newFriend[0] = '\0';
    }

    if (ImGui::Button("Invite Selected"))
    {
        if (selectedFriend >= 0)
        {
            LOG_INFO("[Overlay] Invite friend: %s",
                dummyFriends[selectedFriend]);
        }
    }

    ImGui::End();
}

void CoSyncOverlay::Render()
{
    if (!initialized || !visible)
        return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ShowMainWindow();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void CoSyncOverlay::Shutdown()
{
    if (!initialized)
        return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (context)
    {
        context->Release();
        context = nullptr;
    }

    if (device)
    {
        device->Release();
        device = nullptr;
    }

    initialized = false;
}
