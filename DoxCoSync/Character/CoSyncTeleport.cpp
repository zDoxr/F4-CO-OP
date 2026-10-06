#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncTeleport.h"

#include "ConsoleLogger.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncGameAPI.h"
#include "GameReferences.h"

#include <mutex>
#include <vector>

extern CoSyncPlayerManager g_CoSyncPlayerManager;
extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    struct TeleportRequest
    {
        uint32_t targetEntityID   = 0;
        bool     moveLocalToTarget = true;  // false = pull target's proxy to the local player
    };

    std::mutex                  s_reqMutex;
    std::vector<TeleportRequest> s_pendingRequests;

    // First non-local, spawned player entry -- "the other player" for a
    // 2-person test session. Returns 0 if none found.
    uint32_t FindOtherConnectedEntityID()
    {
        const uint32_t localID = g_CoSyncPlayerManager.GetLocalEntityID();
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            const CoSyncPlayer* p = kv.second.get();
            if (!p || p->entityID == 0 || p->entityID == localID)
                continue;
            if (!p->hasSpawned || !p->actorRef)
                continue;
            return p->entityID;
        }
        return 0;
    }
}

void CoSyncTeleport::RequestTeleportLocalToEntity(uint32_t targetEntityID)
{
    if (targetEntityID == 0)
        return;

    std::lock_guard<std::mutex> lk(s_reqMutex);
    s_pendingRequests.push_back(TeleportRequest{ targetEntityID, true });
}

void CoSyncTeleport::RequestTeleportEntityToLocal(uint32_t targetEntityID)
{
    if (targetEntityID == 0)
        return;

    std::lock_guard<std::mutex> lk(s_reqMutex);
    s_pendingRequests.push_back(TeleportRequest{ targetEntityID, false });
}

void CoSyncTeleport::FlushPendingTeleport()
{
    std::vector<TeleportRequest> reqs;
    {
        std::lock_guard<std::mutex> lk(s_reqMutex);
        if (s_pendingRequests.empty())
            return;
        reqs.swap(s_pendingRequests);
    }

    for (const TeleportRequest& req : reqs)
    {
        PlayerCharacter* localPlayer = CoSyncGetPlayer();
        if (!localPlayer)
        {
            LOG_WARN("[Teleport] no local player -- skip request for entity=%u", req.targetEntityID);
            continue;
        }

        CoSyncPlayer* target = g_CoSyncPlayerManager.GetPlayerByEntityID(req.targetEntityID);
        if (!target || !target->actorRef || !target->hasSpawned)
        {
            LOG_WARN("[Teleport] entity=%u has no spawned proxy -- skip", req.targetEntityID);
            continue;
        }

        Actor* localActor = static_cast<Actor*>(localPlayer);
        if (req.moveLocalToTarget)
        {
            LOG_INFO("[Teleport] local -> entity=%u", req.targetEntityID);
            CoSyncGameAPI::TeleportActorTo(localActor, static_cast<TESObjectREFR*>(target->actorRef));
        }
        else
        {
            LOG_INFO("[Teleport] entity=%u -> local", req.targetEntityID);
            CoSyncGameAPI::TeleportActorTo(target->actorRef, static_cast<TESObjectREFR*>(localActor));
        }
    }
}

void CoSyncTeleport::PollHotkeys()
{
    static bool s_wasDown = false;

    const bool isDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool pressed = isDown && !s_wasDown;
    s_wasDown = isDown;

    if (!pressed)
        return;

    const uint32_t otherID = FindOtherConnectedEntityID();
    if (otherID == 0)
    {
        LOG_WARN("[Teleport] F6 pressed but no other connected/spawned player found");
        return;
    }

    const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (shiftDown)
        RequestTeleportEntityToLocal(otherID);
    else
        RequestTeleportLocalToEntity(otherID);
}
