#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncPickpocket.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPlayer.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncDoorHook.h"
#include "CoSyncRuntime.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameExtraData.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();

namespace
{
    // Engine functions (Fallout4.exe 1.11.240, IDA-verified 2026-10-05).
    //  sub_140666050(Actor*) -> bool: "is sneaking" -- the test TESNPC's
    //    activate text (sub_140656D60) and activation (sub_140655D60) use to
    //    choose Pickpocket over Talk.
    //  sub_14028C290(ExtraDataList*, bool): body of Actor.AllowPCDialogue --
    //    sets (or adds) ExtraCanTalkToPlayer (0x54), which sub_140C72960 (the
    //    "Talk" test) reads before the race's Allow-PC-Dialogue flag.
    constexpr uintptr_t kRVA_IsSneaking        = 0x00666050;
    constexpr uintptr_t kRVA_SetCanTalkToPlayer = 0x0028C290;
    constexpr uint8_t kSneakPrologue[8]   = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x81 };
    constexpr uint8_t kCanTalkPrologue[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 };

    constexpr uint32_t kPipboyFormID = 0x00021B3B;
    constexpr size_t   kMaxOffer     = 256;

    uintptr_t Base() { static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); return b; }

    bool PrologueOk(uintptr_t rva, const uint8_t* bytes)
    {
        return std::memcmp(reinterpret_cast<const void*>(Base() + rva), bytes, 8) == 0;
    }

    double Now() { return static_cast<double>(GetTickCount64()) / 1000.0; }

    __declspec(noinline) bool CallIsSneaking(void* actor)
    {
        __try { return reinterpret_cast<bool(__fastcall*)(void*)>(Base() + kRVA_IsSneaking)(actor); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) void CallSetCanTalk(TESObjectREFR* ref, bool canTalk)
    {
        __try
        {
            ExtraDataList* x = ref ? ref->extraDataList : nullptr;
            if (x)
                reinterpret_cast<void(__fastcall*)(ExtraDataList*, bool)>(Base() + kRVA_SetCanTalkToPlayer)(x, canTalk);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    using Inventory = std::map<uint32_t, int32_t>;

    // ── Inventory reads (SEH) ───────────────────────────────────────────────
    // `takeableOnly`: what a pickpocket may take -- carried but not worn, not
    // a quest item (alias data on the stack), never the Pip-Boy.
    __declspec(noinline) bool ReadInvImpl(TESObjectREFR* ref, bool takeableOnly, uint32_t* forms, int32_t* counts, size_t cap, size_t& n)
    {
        n = 0;
        __try
        {
            BGSInventoryList* inv = ref ? ref->inventoryList : nullptr;
            if (!inv) return true;
            for (UInt32 i = 0; i < inv->items.count && n < cap; ++i)
            {
                const BGSInventoryItem& item = inv->items[i];
                if (!item.form) continue;
                if (takeableOnly && item.form->formID == kPipboyFormID) continue;
                int32_t total = 0;
                for (BGSInventoryItem::Stack* s = item.stack; s; s = s->next)
                {
                    if (takeableOnly)
                    {
                        if (s->flags & BGSInventoryItem::Stack::kFlagEquipped) continue;
                        if (s->extraData && s->extraData->HasType(kExtraData_AliasInstanceArray)) continue;
                    }
                    total += s->count;
                }
                if (total <= 0) continue;
                forms[n] = item.form->formID;
                counts[n] = total;
                ++n;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; return false; }
    }

    bool ReadInv(TESObjectREFR* ref, bool takeableOnly, Inventory& out)
    {
        static uint32_t forms[1024];
        static int32_t  counts[1024];
        size_t n = 0;
        out.clear();
        if (!ReadInvImpl(ref, takeableOnly, forms, counts, 1024, n))
            return false;
        for (size_t i = 0; i < n; ++i)
            out[forms[i]] += counts[i];
        return true;
    }

    std::string Body(const Inventory& inv)
    {
        std::string b;
        for (const auto& kv : inv)
        {
            if (kv.second <= 0) continue;
            if (!b.empty()) b += ';';
            b += std::to_string(kv.first) + ":" + std::to_string(kv.second);
        }
        return b;
    }

    void ParseBody(const std::string& s, Inventory& out)
    {
        out.clear();
        size_t a = 0;
        while (a < s.size() && out.size() < kMaxOffer)
        {
            size_t b = s.find(';', a);
            if (b == std::string::npos) b = s.size();
            const std::string tok = s.substr(a, b - a);
            const size_t c = tok.find(':');
            if (c != std::string::npos)
            {
                const uint32_t f = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
                const long v = std::strtol(tok.c_str() + c + 1, nullptr, 10);
                if (f != 0 && v > 0 && v < 100000)
                    out[f] += static_cast<int32_t>(v);
            }
            a = b + 1;
        }
    }

    std::vector<std::string> Fields(const std::string& msg, size_t maxParts)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (out.size() + 1 < maxParts)
        {
            const size_t b = msg.find('|', a);
            if (b == std::string::npos) break;
            out.push_back(msg.substr(a, b - a));
            a = b + 1;
        }
        out.push_back(msg.substr(a));
        return out;
    }

    uint32_t U(const std::string& s) { return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10)); }

    std::string NameOf(uint32_t eid)
    {
        if (CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid))
            if (!p->username.empty()) return p->username;
        return "Someone";
    }

    // ── Routing (host relays; sender checked) ───────────────────────────────
    uint32_t ConnOfEntity(uint32_t eid)
    {
        for (const auto& p : CoSyncNet::HostGetPeers())
            if (p.entityID == eid) return static_cast<uint32_t>(p.conn);
        return 0;
    }

    uint32_t EntityOfConn(uint32_t conn)
    {
        for (const auto& p : CoSyncNet::HostGetPeers())
            if (static_cast<uint32_t>(p.conn) == conn) return p.entityID;
        return 0;
    }

    void HandleLocal(const std::string& tag, const std::vector<std::string>& f);

    // Send to the machine that plays `toEid`: the host handles its own player
    // directly, a client goes through the host.
    void SendToPlayer(uint32_t toEid, const std::string& tag, const std::string& msg)
    {
        if (!CoSyncTransport::IsInitialized() || !CoSyncNet::IsConnected())
            return;
        if (!CoSyncTransport::IsHost())
        {
            CoSyncTransport::Send(msg);
            return;
        }
        if (toEid == CoSyncNet::GetMyEntityID())
        {
            HandleLocal(tag, Fields(msg.substr(tag.size() + 1), 5));
            return;
        }
        const uint32_t c = ConnOfEntity(toEid);
        if (c != 0)
            CoSyncTransport::SendTo(c, msg);
    }

    // ── Thief state ─────────────────────────────────────────────────────────
    enum class Phase { None, Requested, MenuOpening, InMenu, AwaitResult };
    struct Attempt
    {
        Phase     phase = Phase::None;
        uint32_t  victim = 0;
        uint32_t  token = 0;
        double    deadline = 0.0;
        Inventory baseline;      // the proxy's own items before the offer was put on it
        Inventory offered;       // what was put on the proxy
        Inventory taken;         // what the engine moved to us (provisional)
    };
    Attempt  s_att;
    uint32_t s_tokenSeq = 0;

    TESObjectREFR* VictimProxy(uint32_t eid)
    {
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        if (!p || !p->hasSpawned || !p->actorRef || p->isLocalPlayer || p->createType != CoSyncEntityType::Player)
            return nullptr;
        return reinterpret_cast<TESObjectREFR*>(p->actorRef);
    }

    void ClearAttempt(const char* why)
    {
        if (s_att.phase != Phase::None)
            LOG_INFO("[Pickpocket] attempt on entity=%u ended (%s)", s_att.victim, why);
        s_att = Attempt{};
    }

    // Take the offered items back off the proxy (whatever the engine did not
    // move), and anything the engine moved that was the proxy's OWN gear goes
    // back to it (those are copies of what the victim wears, not the
    // victim's items).
    void RestoreProxy(TESObjectREFR* proxy)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        Inventory now;
        if (!proxy || !ReadInv(proxy, false, now))
            return;
        for (const auto& kv : now)
        {
            const int32_t base = s_att.baseline.count(kv.first) ? s_att.baseline.at(kv.first) : 0;
            const int32_t extra = kv.second - base;
            if (extra > 0)
                if (TESForm* f = LookupFormByID(kv.first))
                    CoSyncGameAPI::NativeRemoveItem(proxy, f, static_cast<UInt32>(extra));
        }
        for (const auto& kv : s_att.baseline)
        {
            const int32_t have = now.count(kv.first) ? now.at(kv.first) : 0;
            const int32_t missing = kv.second - have;
            if (missing <= 0) continue;
            TESForm* f = LookupFormByID(kv.first);
            if (!f) continue;
            CoSyncGameAPI::NativeAddItem(proxy, f, static_cast<UInt32>(missing));
            if (pc) CoSyncGameAPI::NativeRemoveItem(reinterpret_cast<TESObjectREFR*>(pc), f, static_cast<UInt32>(missing));
            CoSyncGameAPI::QueueNotification("You can't take what they're wearing.");
        }
    }

    void FinishMenu()
    {
        TESObjectREFR* proxy = VictimProxy(s_att.victim);
        Inventory now;
        if (proxy) ReadInv(proxy, false, now);
        // Stolen = offered items no longer on the proxy.
        s_att.taken.clear();
        for (const auto& kv : s_att.offered)
        {
            const int32_t base = s_att.baseline.count(kv.first) ? s_att.baseline.at(kv.first) : 0;
            const int32_t have = now.count(kv.first) ? now.at(kv.first) : 0;
            const int32_t left = (std::max)(0, have - base);
            const int32_t gone = kv.second - left;
            if (gone > 0) s_att.taken[kv.first] = (std::min)(gone, kv.second);
        }
        RestoreProxy(proxy);
        if (s_att.taken.empty())
        {
            ClearAttempt("menu closed, nothing taken");
            return;
        }
        char head[96];
        std::snprintf(head, sizeof(head), "PPTAKE|%u|%u|%u|", CoSyncNet::GetMyEntityID(), s_att.victim, s_att.token);
        SendToPlayer(s_att.victim, "PPTAKE", head + Body(s_att.taken));
        s_att.phase = Phase::AwaitResult;
        s_att.deadline = Now() + 10.0;
        LOG_INFO("[Pickpocket] took %zu item type(s) from entity=%u -- asking its machine to remove them", s_att.taken.size(), s_att.victim);
    }

    // Give back what the victim's machine could not remove.
    void Reconcile(const Inventory& confirmed)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        int reverted = 0;
        for (const auto& kv : s_att.taken)
        {
            const int32_t ok = confirmed.count(kv.first) ? confirmed.at(kv.first) : 0;
            const int32_t excess = kv.second - ok;
            if (excess <= 0 || !pc) continue;
            if (TESForm* f = LookupFormByID(kv.first))
                if (CoSyncGameAPI::NativeRemoveItem(reinterpret_cast<TESObjectREFR*>(pc), f, static_cast<UInt32>(excess)))
                    ++reverted;
        }
        if (reverted)
            CoSyncGameAPI::QueueNotification("Some of it was already gone.");
        ClearAttempt(reverted ? "result applied (partly reverted)" : "result applied");
    }

    // ── Victim side ─────────────────────────────────────────────────────────
    void VictimOffer(uint32_t thief, uint32_t token)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return;
        Inventory inv;
        ReadInv(reinterpret_cast<TESObjectREFR*>(pc), true, inv);
        while (inv.size() > kMaxOffer) inv.erase(std::prev(inv.end()));
        char head[96];
        std::snprintf(head, sizeof(head), "PPINV|%u|%u|%u|", thief, CoSyncNet::GetMyEntityID(), token);
        SendToPlayer(thief, "PPINV", head + Body(inv));
        LOG_INFO("[Pickpocket] entity=%u is pickpocketing me: offered %zu item type(s)", thief, inv.size());
    }

    void VictimRemove(uint32_t thief, uint32_t token, const Inventory& want)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc) return;
        Inventory have;
        ReadInv(reinterpret_cast<TESObjectREFR*>(pc), true, have);
        Inventory removed;
        for (const auto& kv : want)
        {
            const int32_t n = (std::min)(kv.second, have.count(kv.first) ? have.at(kv.first) : 0);
            if (n <= 0) continue;
            TESForm* f = LookupFormByID(kv.first);
            if (f && CoSyncGameAPI::NativeRemoveItem(reinterpret_cast<TESObjectREFR*>(pc), f, static_cast<UInt32>(n)))
                removed[kv.first] = n;
        }
        char head[96];
        std::snprintf(head, sizeof(head), "PPRES|%u|%u|%u|", thief, CoSyncNet::GetMyEntityID(), token);
        SendToPlayer(thief, "PPRES", head + Body(removed));
        LOG_INFO("[Pickpocket] entity=%u took %zu item type(s) from me", thief, removed.size());
    }

    void HandleLocal(const std::string& tag, const std::vector<std::string>& f)
    {
        if (f.size() < 3) return;
        const uint32_t thief = U(f[0]), victim = U(f[1]), token = U(f[2]);
        const uint32_t me = CoSyncNet::GetMyEntityID();
        if (tag == "PPREQ" && victim == me)
        {
            VictimOffer(thief, token);
            return;
        }
        if (tag == "PPTAKE" && victim == me && f.size() >= 4)
        {
            Inventory want;
            ParseBody(f[3], want);
            VictimRemove(thief, token, want);
            return;
        }
        if (thief != me || victim != s_att.victim || token != s_att.token)
            return;   // not our current attempt
        if (tag == "PPINV" && s_att.phase == Phase::Requested && f.size() >= 4)
        {
            TESObjectREFR* proxy = VictimProxy(victim);
            PlayerCharacter* pc = CoSyncGetPlayer();
            if (!proxy || !pc)
            {
                ClearAttempt("proxy gone");
                return;
            }
            Inventory offer;
            ParseBody(f[3], offer);
            ReadInv(proxy, false, s_att.baseline);
            s_att.offered.clear();
            for (const auto& kv : offer)
                if (TESForm* item = LookupFormByID(kv.first))
                    if (CoSyncGameAPI::NativeAddItem(proxy, item, static_cast<UInt32>(kv.second)))
                        s_att.offered[kv.first] = kv.second;
            if (!CoSyncPickpocket::IsPlayerSneaking())
            {
                RestoreProxy(proxy);
                ClearAttempt("no longer sneaking");
                return;
            }
            // The engine's own pickpocket: activation as the player, past
            // CoSync's activation gate (ActivateAs).
            if (!CoSyncDoorHook::ActivateAs(proxy, reinterpret_cast<TESObjectREFR*>(pc)))
            {
                RestoreProxy(proxy);
                ClearAttempt("activation failed");
                return;
            }
            s_att.phase = Phase::MenuOpening;
            s_att.deadline = Now() + 3.0;
            LOG_INFO("[Pickpocket] %zu item type(s) offered by entity=%u -- pickpocket menu opening", s_att.offered.size(), victim);
            return;
        }
        if (tag == "PPRES" && s_att.phase == Phase::AwaitResult && f.size() >= 4)
        {
            Inventory confirmed;
            ParseBody(f[3], confirmed);
            Reconcile(confirmed);
        }
    }
}

namespace CoSyncPickpocket
{
    bool IsPickpocketMessage(const std::string& msg)
    {
        return msg.rfind("PPREQ|", 0) == 0 || msg.rfind("PPINV|", 0) == 0 ||
               msg.rfind("PPTAKE|", 0) == 0 || msg.rfind("PPRES|", 0) == 0;
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncPickpocket::OnReceive", "CoSync tick. The host forwards only messages whose sender is the thief / victim they name.");
        if (msg.size() > 32 * 1024)
            return;
        const size_t bar = msg.find('|');
        const std::string tag = msg.substr(0, bar);
        const std::vector<std::string> f = Fields(msg.substr(bar + 1), 4);
        if (f.size() < 3)
            return;
        const uint32_t thief = U(f[0]), victim = U(f[1]);
        if (thief == 0 || victim == 0 || thief == victim)
            return;
        if (isHostRole && conn != 0)
        {
            // The sender must be the side of the exchange the message speaks for.
            const bool fromThief = tag == "PPREQ" || tag == "PPTAKE";
            const uint32_t sender = EntityOfConn(conn);
            if (sender == 0 || sender != (fromThief ? thief : victim))
            {
                LOG_WARN("[Pickpocket] %s from conn=%u (entity %u) does not match the %s it names -- dropped",
                    tag.c_str(), conn, sender, fromThief ? "thief" : "victim");
                return;
            }
            const uint32_t to = fromThief ? victim : thief;
            if (to == CoSyncNet::GetMyEntityID())
            {
                HandleLocal(tag, f);
                return;
            }
            const uint32_t c = ConnOfEntity(to);
            if (c != 0)
                CoSyncTransport::SendTo(c, msg);
            return;
        }
        if (isHostRole)
            return;
        HandleLocal(tag, f);
    }

    void BeginAttempt(uint32_t victim)
    {
        if (!CoSyncTransport::IsInitialized() || !CoSyncNet::IsConnected())
            return;
        if (s_att.phase != Phase::None)
            return;   // one at a time
        if (!VictimProxy(victim))
            return;
        s_att = Attempt{};
        s_att.phase = Phase::Requested;
        s_att.victim = victim;
        s_att.token = (static_cast<uint32_t>(GetTickCount64()) * 2654435761u) ^ ++s_tokenSeq;
        s_att.deadline = Now() + 5.0;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "PPREQ|%u|%u|%u", CoSyncNet::GetMyEntityID(), victim, s_att.token);
        SendToPlayer(victim, "PPREQ", msg);
        LOG_INFO("[Pickpocket] trying to pickpocket entity=%u (%s)", victim, NameOf(victim).c_str());
    }

    bool IsPlayerSneaking()
    {
        static const bool ok = PrologueOk(kRVA_IsSneaking, kSneakPrologue);
        if (!ok) return false;
        PlayerCharacter* pc = CoSyncGetPlayer();
        return pc && CallIsSneaking(pc);
    }

    void ApplyNoTalk(Actor* proxy)
    {
        static const bool ok = PrologueOk(kRVA_SetCanTalkToPlayer, kCanTalkPrologue);
        if (!ok || !proxy) return;
        CallSetCanTalk(reinterpret_cast<TESObjectREFR*>(proxy), false);
    }

    void Tick(double now)
    {
        (void)now;
        const double t = Now();

        // Player proxies never offer "Talk" (re-asserted: a rebuilt proxy has
        // a fresh extra data list).
        static double s_nextNoTalk = 0.0;
        if (t >= s_nextNoTalk)
        {
            s_nextNoTalk = t + 2.0;
            for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
            {
                const CoSyncPlayer* p = kv.second.get();
                if (p && p->hasSpawned && p->actorRef && !p->isLocalPlayer && p->createType == CoSyncEntityType::Player)
                    ApplyNoTalk(p->actorRef);
            }
        }

        switch (s_att.phase)
        {
        case Phase::None:
            return;
        case Phase::Requested:
            if (t > s_att.deadline)
            {
                CoSyncGameAPI::QueueNotification("They moved away.");
                ClearAttempt("no answer");
            }
            return;
        case Phase::MenuOpening:
            if (CoSyncRuntime_IsMenuOpen("ContainerMenu"))
            {
                s_att.phase = Phase::InMenu;
                return;
            }
            if (t > s_att.deadline)
            {
                // The engine refused the pickpocket (caught at once, or no
                // longer possible): nothing moved.
                RestoreProxy(VictimProxy(s_att.victim));
                ClearAttempt("menu never opened");
            }
            return;
        case Phase::InMenu:
            if (!CoSyncRuntime_IsMenuOpen("ContainerMenu"))
                FinishMenu();
            return;
        case Phase::AwaitResult:
            if (t > s_att.deadline)
            {
                // The victim's machine never confirmed (left the game): the
                // items were never removed there, so they are not ours.
                Reconcile(Inventory{});
            }
            return;
        }
    }

    void Reset()
    {
        if (s_att.phase == Phase::InMenu || s_att.phase == Phase::MenuOpening)
            RestoreProxy(VictimProxy(s_att.victim));
        s_att = Attempt{};
    }
}
