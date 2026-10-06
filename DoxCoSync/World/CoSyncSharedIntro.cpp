#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSharedIntro.h"

#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPlayer.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncProfiles.h"
#include "CoSyncRuntime.h"
#include "CoSyncWorld.h"
#include "CoSyncWorldTransfer.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "GameForms.h"
#include "GameReferences.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();
extern RelocAddr<_MoveRefrToPosition> MoveRefrToPosition;
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{
    // ── Fallout4.esm (read from the ESM, 2026-10-05) ────────────────────────
    constexpr uint32_t kQuestMQ101          = 0x0001ED86;   // War Never Changes
    constexpr uint32_t kQuestTutorial       = 0x00160147;   // runs alongside the intro
    constexpr uint32_t kWorldPrewar         = 0x000A7FF4;   // SanctuaryHillsWorld
    constexpr uint32_t kCellPrewarVault111  = 0x000CA260;   // the freeze
    constexpr uint32_t kCellVault111Cryo    = 0x000016D8;   // the wake-up (same layout)
    // MQ101PlayerStartMarker01 (0xA7D33): where the player stands at the mirror.
    constexpr float    kMirrorPos[3] = { -80303.f, 90828.f, 7856.f };
    constexpr float    kMirrorRotZ   = 2.54f;

    // The scripted player pod and the six resident pods, in the order players
    // are given them (nearest the player pod first, then the spouse's row).
    struct Pod { float x, y, z, rz; };
    constexpr Pod kPlayerPod = { 640.f, -288.f, -72.f, 0.f };
    constexpr Pod kSlots[] = {
        { 384.f, -288.f, -72.f, 0.f },       { 128.f, -288.f, -72.f, 0.f },       { -64.f, -288.f, -72.f, 0.f },
        { 832.f, -736.f, -72.f, 3.14159f },  { 384.f, -736.f, -72.f, 3.14159f },  { 128.f, -736.f, -72.f, 3.14159f },
    };
    constexpr size_t kSlotCount = sizeof(kSlots) / sizeof(kSlots[0]);
    constexpr float  kInPodRadius = 90.f;       // standing in a pod (XY)
    constexpr float  kResidentRadius = 60.f;    // a resident seated in a slot pod (XY)

    // Host state.
    bool   s_hostSpecialOpen = false;    // its SPECIAL menu was open last tick
    bool   s_hostSpecialDone = false;    // the rep's SPECIAL has happened this intro
    double s_nextStateSend   = 0.0;

    // Client state.
    std::atomic<bool> s_hostReachedSpecial{ false };
    bool   s_characterMade  = false;     // mirror done in this intro
    bool   s_specialShown   = false;     // our SPECIAL menu opened in this intro

    // Vault 111 entry.
    bool     s_wasInVault = false;
    struct VaultCall { uint32_t eid; float x, y, z; };
    std::mutex             s_vaultMx;
    std::vector<VaultCall> s_vaultCalls;

    // Residents disabled on this machine.
    std::unordered_set<uint32_t> s_disabledResidents;
    double                       s_nextResidentScan = 0.0;

    double Now() { return static_cast<double>(GetTickCount64()) / 1000.0; }

    bool IsSharedIntroCheck();   // = CoSyncSharedIntro::IsSharedIntro (defined at the end)

    bool SessionLive() { return CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected(); }

    __declspec(noinline) uint32_t SafeCellID(TESObjectREFR* ref)
    {
        __try { return (ref && ref->parentCell) ? ref->parentCell->formID : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    bool InPodRoom(uint32_t cell) { return cell == kCellPrewarVault111 || cell == kCellVault111Cryo; }

    // Game.ShowSPECIALMenu (sub_14112A0B0 -> sub_140AFFB40, queues SPECIALMenu).
    bool ShowSpecialMenu()
    {
        static const uint8_t kPrologue[9] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x1D };
        const uintptr_t fn = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + 0x00AFFB40;
        if (std::memcmp(reinterpret_cast<const void*>(fn), kPrologue, sizeof(kPrologue)) != 0)
            return false;
        reinterpret_cast<void(*)()>(fn)();
        return true;
    }

    void Teleport(PlayerCharacter* pc, TESObjectCELL* cell, TESWorldSpace* ws, const NiPoint3& pos, float rotZ)
    {
        NiPoint3 p = pos;
        NiPoint3 r(0.f, 0.f, rotZ);
        UInt32 nullHandle = 0;
        MoveRefrToPosition(reinterpret_cast<TESObjectREFR*>(pc), &nullHandle, cell, ws, &p, &r);
    }

    // Other players in a stable order (entity ID) -- each gets a pod slot.
    int SlotOf(uint32_t entityID)
    {
        std::vector<uint32_t> others;
        const uint32_t me = CoSyncNet::GetMyEntityID();
        for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
        {
            const CoSyncPlayer* p = kv.second.get();
            if (p && !p->isLocalPlayer && p->entityID != me && p->createType == CoSyncEntityType::Player)
                others.push_back(p->entityID);
        }
        std::sort(others.begin(), others.end());
        for (size_t i = 0; i < others.size(); ++i)
            if (others[i] == entityID)
                return static_cast<int>(i % kSlotCount);   // more players than pods: stacked
        return -1;
    }

    // Residents leave the six resident pods (on this machine), so the other
    // players can be shown there. The spouse (spouse pod), the player pod and
    // anyone walking the aisle are untouched.
    __declspec(noinline) void ScanResidentsImpl(TESObjectCELL* cell, PlayerCharacter* pc, std::vector<TESObjectREFR*>& out)
    {
        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* r = cell->objectList.entries[i];
            if (!r || r->formType != kFormType_ACHR || r == reinterpret_cast<TESObjectREFR*>(pc))
                continue;
            for (const Pod& s : kSlots)
            {
                const float dx = r->pos.x - s.x, dy = r->pos.y - s.y;
                if (dx * dx + dy * dy <= kResidentRadius * kResidentRadius && std::fabs(r->pos.z - s.z) < 150.f)
                {
                    out.push_back(r);
                    break;
                }
            }
        }
    }

    __declspec(noinline) bool SafeScanResidents(TESObjectCELL* cell, PlayerCharacter* pc, std::vector<TESObjectREFR*>& out)
    {
        __try { ScanResidentsImpl(cell, pc, out); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void ClearResidents(PlayerCharacter* pc)
    {
        TESObjectCELL* cell = pc->parentCell;
        if (!cell)
            return;
        std::vector<TESObjectREFR*> found;
        if (!SafeScanResidents(cell, pc, found))
            return;
        for (TESObjectREFR* r : found)
        {
            const CoSyncPlayer* asPlayer = g_CoSyncPlayerManager.GetPlayerByActorFormID(r->formID);
            if (asPlayer && asPlayer->createType == CoSyncEntityType::Player)
                continue;   // another player's proxy shown in its pod
            if (!s_disabledResidents.insert(r->formID).second)
                continue;
            CoSyncGameAPI::DisableWorldActor(r);
            LOG_UPDATED("[SharedIntro] resident 0x%08X left its cryopod (base 0x%08X) -- the pod is for a player",
                r->formID, r->baseForm ? r->baseForm->formID : 0);
        }
    }

    void HostTick(double now, PlayerCharacter* pc)
    {
        const bool open = CoSyncRuntime_IsMenuOpen("SPECIALMenu");
        if (open && !s_hostSpecialOpen && IsSharedIntroCheck())
        {
            s_hostSpecialDone = true;
            CoSyncTransport::Send("INTRO|special");
            LOG_UPDATED("[SharedIntro] the Vault-Tec rep's SPECIAL is open on the host -- every player gets theirs");
        }
        s_hostSpecialOpen = open;
        if (now >= s_nextStateSend)
        {
            s_nextStateSend = now + 5.0;
            CoSyncTransport::Send(std::string("INTRO|state|") + (s_hostSpecialDone ? "1" : "0"));
        }
        (void)pc;
    }

    void ClientTick(PlayerCharacter* pc)
    {
        (void)pc;
        if (s_characterMade && !s_specialShown && s_hostReachedSpecial.load() && !CoSyncProfiles::IsEditorBusy() &&
            !CoSyncRuntime_IsMenuOpen("LooksMenu") && !CoSyncRuntime_IsMenuOpen("LoadingMenu"))
        {
            s_specialShown = true;
            const bool ok = ShowSpecialMenu();
            LOG_UPDATED("[SharedIntro] SPECIAL menu (points and name) -- the host is at the Vault-Tec rep: %s",
                ok ? "opened" : "unavailable on this game version");
        }
    }

    void VaultTick(PlayerCharacter* pc)
    {
        const uint32_t cell = SafeCellID(reinterpret_cast<TESObjectREFR*>(pc));
        const bool inVault = InPodRoom(cell);
        if (inVault && !s_wasInVault && cell == kCellPrewarVault111)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "INTRO|vault|%u|%.1f|%.1f|%.1f",
                CoSyncNet::GetMyEntityID(), pc->pos.x, pc->pos.y, pc->pos.z);
            CoSyncTransport::Send(buf);
            LOG_UPDATED("[SharedIntro] I entered Vault 111 -- bringing everyone still outside");
        }
        s_wasInVault = inVault;

        std::vector<VaultCall> calls;
        {
            std::lock_guard<std::mutex> lk(s_vaultMx);
            calls.swap(s_vaultCalls);
        }
        if (calls.empty() || inVault)
            return;
        TESForm* f = LookupFormByID(kCellPrewarVault111);
        TESObjectCELL* vault = (f && f->formType == kFormType_CELL) ? static_cast<TESObjectCELL*>(f) : nullptr;
        if (!vault)
            return;
        const VaultCall& c = calls.front();
        // Beside the one who arrived (they are all on the same lift platform).
        const int slot = (std::max)(0, SlotOf(c.eid) + 1);
        const NiPoint3 pos(c.x + 60.f * static_cast<float>(slot % 3), c.y + 60.f * static_cast<float>(slot / 3), c.z);
        Teleport(pc, vault, nullptr, pos, pc->rot.z);
        s_wasInVault = true;
        LOG_UPDATED("[SharedIntro] entity=%u entered Vault 111 -- moved there too (%.0f,%.0f,%.0f)", c.eid, pos.x, pos.y, pos.z);
    }
}

namespace CoSyncSharedIntro
{
    __declspec(noinline) bool IsIntroRunning()
    {
        TESForm* q = LookupFormByID(kQuestMQ101);
        if (!q || q->formType != kFormType_QUST)
            return false;
        __try
        {
            const uint16_t flags = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(q) + 0xF4);
            return (flags & 0x1) && !(flags & 0x80) && !(flags & 0x2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool IsSharedIntro()
    {
        return SessionLive() && CoSyncWorld::IsWorldReady() && IsIntroRunning();
    }

    bool BlockClientStage(uint32_t /*questFormID*/)
    {
        // Every stage: in the intro the host's story is the only one. The
        // client's own fragments still run when the host's stages arrive
        // (applied past the hook), they just cannot start a branch of their own.
        return !CoSyncTransport::IsHost() && IsSharedIntro();
    }

    bool HostRefusesRequest(uint32_t questFormID)
    {
        return questFormID == kQuestMQ101 || questFormID == kQuestTutorial ||
               (IsSharedIntro() && CoSyncTransport::IsHost());
    }

    bool GetMirrorSpot(uint32_t& worldspace, NiPoint3& pos, float& rotZ)
    {
        if (!IsIntroRunning())
            return false;
        worldspace = kWorldPrewar;
        pos = NiPoint3(kMirrorPos[0], kMirrorPos[1], kMirrorPos[2]);
        rotZ = kMirrorRotZ;
        return true;
    }

    void OnLocalCharacterMade()
    {
        s_characterMade = true;
        s_specialShown = false;
    }

    bool RemapProxyIntoPod(uint32_t entityID, NiPoint3& pos, NiPoint3& rot)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc || !SessionLive() || !InPodRoom(SafeCellID(reinterpret_cast<TESObjectREFR*>(pc))))
            return false;
        const float dx = pos.x - kPlayerPod.x, dy = pos.y - kPlayerPod.y;
        if (dx * dx + dy * dy > kInPodRadius * kInPodRadius || std::fabs(pos.z - kPlayerPod.z) > 150.f)
            return false;   // not in the scripted pod: shown where they are
        const int slot = SlotOf(entityID);
        if (slot < 0)
            return false;
        const Pod& s = kSlots[slot];
        // Same place inside the pod, turned with it.
        const float turn = s.rz - kPlayerPod.rz;
        const float c = std::cos(turn), sn = std::sin(turn);
        pos.x = s.x + dx * c - dy * sn;
        pos.y = s.y + dx * sn + dy * c;
        pos.z = s.z + (pos.z - kPlayerPod.z);
        rot.z += turn;
        return true;
    }

    bool IsIntroMessage(const std::string& msg)
    {
        return msg.rfind("INTRO|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncSharedIntro::OnReceive", "Network side: parses and queues; the tick acts.");
        if (msg.rfind("INTRO|vault|", 0) == 0)
        {
            if (isHostRole)
                CoSyncTransport::Send(msg);   // relay to every client
            VaultCall c{};
            if (std::sscanf(msg.c_str() + 12, "%u|%f|%f|%f", &c.eid, &c.x, &c.y, &c.z) != 4 || c.eid == 0)
                return;
            if (c.eid == CoSyncNet::GetMyEntityID() || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
                return;
            std::lock_guard<std::mutex> lk(s_vaultMx);
            if (s_vaultCalls.size() < 8)
                s_vaultCalls.push_back(c);
            return;
        }
        if (isHostRole)
            return;   // special / state only come from the host
        if (msg == "INTRO|special")
        {
            s_hostReachedSpecial.store(true);
            return;
        }
        if (msg.rfind("INTRO|state|", 0) == 0 && msg.size() > 12 && msg[12] == '1')
            s_hostReachedSpecial.store(true);
    }

    void Tick(double /*now*/)
    {
        const double now = Now();
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc || !SessionLive() || !CoSyncWorld::IsWorldReady() || !IsIntroRunning())
            return;
        if (CoSyncTransport::IsHost())
            HostTick(now, pc);
        else
            ClientTick(pc);
        VaultTick(pc);
        if (now >= s_nextResidentScan && InPodRoom(SafeCellID(reinterpret_cast<TESObjectREFR*>(pc))))
        {
            s_nextResidentScan = now + 1.0;
            ClearResidents(pc);
        }
    }

    void Reset()
    {
        s_hostSpecialOpen = false;
        s_hostSpecialDone = false;
        s_nextStateSend = 0.0;
        s_hostReachedSpecial.store(false);
        s_characterMade = false;
        s_specialShown = false;
        s_wasInVault = false;
        {
            std::lock_guard<std::mutex> lk(s_vaultMx);
            s_vaultCalls.clear();
        }
        s_disabledResidents.clear();
        s_nextResidentScan = 0.0;
    }
}

namespace
{
    bool IsSharedIntroCheck() { return CoSyncSharedIntro::IsSharedIntro(); }
}
