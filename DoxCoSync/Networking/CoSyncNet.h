#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "NiTypes.h"
#include "CoSyncAnimVarSystem.h"


using HSteamNetConnection = uint32_t;

class Actor;

class CoSyncNet
{
public:

    static void Init(bool isHost);
    static void ScheduleInit(bool isHost);
    static void PerformPendingInit();
    static void Shutdown();


    static void Tick(double now);


    static uint64_t GetMySteamID();
    static uint32_t GetMyEntityID();

    // Sends the client's HELLO once the local save has finished loading.
    // MUST be polled from the per-frame tick: the transport connect callback
    // fires only once, so a "not ready yet" answer there would otherwise mean
    // HELLO is never sent at all (which is exactly what happened 2026-08-04).
    static void TrySendHello();

    // Discrete animation events, edge-detected on the sender and replayed
    // one-for-one on peers. See CoSyncPlayer's m_ev* counters for why these
    // are one-shot messages rather than sampled booleans.
    enum AnimEventCode : uint32_t
    {
        kAnimEv_Fire          = 1,
        kAnimEv_ReloadStart   = 2,
        kAnimEv_ReloadEnd     = 3,
        kAnimEv_ThrowGrenade  = 4,
        kAnimEv_ThrowMine     = 5,
        kAnimEv_ThrowEnd      = 6,
    };

    // Reads the graph bools BY NAME on `actor` and broadcasts an AEV| for
    // every transition since the last call. Must be called at FRAME RATE for
    // the local player -- isFiring is true for about one frame per shot, so
    // anything slower silently drops shots. `entityID` is the sender's own
    // entity, `state` is per-actor edge memory owned by the caller.
    struct AnimEdgeState
    {
        bool prevFiring    = false;
        bool prevReloading = false;
        bool prevThrowing  = false;
        bool init          = false;
        // isFiring stays HIGH for the whole burst, so its rising edge yields
        // exactly one event per trigger-hold no matter how many rounds go out
        // (2026-08-04 log: 10 edges, all "x1", 1-2.5s apart = one per pull).
        // iSyncFireState is the engine's own per-shot networked-actor fire
        // counter/state; a change in it while firing means another round.
        int  prevFireState = 0;
    };
    static void SendAnimEdges(Actor* actor, uint32_t entityID, AnimEdgeState& state);
    static const char* GetMyName();
    static void SetMyName(const std::string& name);
    static void SetMySteamID(uint64_t sid);


    // Protocol/build number exchanged in VER| and advertised by discovery.
    static uint32_t GetBuildNumber();

    static bool IsHost();
    static bool IsInitialized();
    static bool IsConnected();

    static void SendMyEntityUpdate(
        uint32_t entityID,
        const NiPoint3& pos,
        const NiPoint3& rot,
        const NiPoint3& vel,
        double now,
        const AnimVarSnapshot* animVars = nullptr,
        uint32_t weaponFormID = 0);


    static void HostSpawnNpc(
        uint32_t entityID,
        uint32_t baseFormID,
        const NiPoint3& pos,
        const NiPoint3& rot);


    static void OnReceive(const std::string& msg, double now, HSteamNetConnection conn);
    static void OnGNSConnected();
    static void OnGNSDisconnected();

    static std::vector<uint32_t> GetConnectedPeerEntityIDs();

    // Live session roster for the UI, built from real state: this player,
    // plus every remote player proxy the player manager knows about.
    // GAME THREAD ONLY (reads the player manager).
    struct RosterEntry
    {
        std::string name;
        uint32_t    entityID = 0;
        bool        isHost   = false;
        bool        isLocal  = false;
        bool        inGame   = false;   // spawned / world loaded
    };
    static std::vector<RosterEntry> GetRoster();

    // Host only: clients that are connected at the transport level but have
    // not introduced themselves (HELLO) yet -- still loading their save.
    static int GetJoiningCount();
};
