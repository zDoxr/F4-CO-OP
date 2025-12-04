#include "CoSyncNet.h"
#include "ConsoleLogger.h"
#include "LocalPlayerState.h"
#include "PlayerStatePacket.h"
#include "GNS_Session.h"

namespace CoSyncNet
{
    static bool g_isHost = false;
    static bool g_initialized = false;

    void Init(bool host)
    {
        g_isHost = host;
        g_initialized = true;
        LOG_INFO("[CoSyncNet] Init (GNS-only) Host=%d", host ? 1 : 0);
    }

    void Shutdown()
    {
        LOG_INFO("[CoSyncNet] Shutdown");
        g_initialized = false;
    }

    void Tick(double)
    {
        // GNS tick already done in GNS_Session
    }

    // =====================================================================
    // *** THIS IS THE FUNCTION THE LINKER WANTS ***
    // =====================================================================
    void SendLocalPlayerState(const LocalPlayerState& state)
    {
        if (!g_initialized)
        {
            LOG_WARN("[CoSyncNet] Not initialized");
            return;
        }

        std::string msg;
        if (!SerializePlayerStateToString(state, msg))
        {
            LOG_ERROR("[CoSyncNet] SerializePlayerStateToString FAILED");
            return;
        }

        // Send string over GNS
        GNS_Session::Get().SendText(msg);
    }
    // =====================================================================

    void OnReceive(const std::string& msg)
    {
        if (!IsPlayerStateString(msg))
        {
            LOG_INFO("[CoSyncNet] Received non-player-state message");
            return;
        }

        LocalPlayerState remote{};
        if (!DeserializePlayerStateFromString(msg, remote))
        {
            LOG_ERROR("[CoSyncNet] Failed to decode PlayerState");
            return;
        }

        LOG_INFO("[CoSyncNet] Remote player pos=(%.2f %.2f %.2f)",
            remote.position.x, remote.position.y, remote.position.z
        );

        // TODO: Remote actor system
    }

} // namespace CoSyncNet
