#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSettlementSync.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncTeams.h"
#include "CoSyncGameAPI.h"
#include "CoSyncWorkshopEngine.h"
#include "CoSyncWorld.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // WorkshopParent quest (Fallout4.esm 0x0002058E) property WorkshopRatings,
    // decoded from its VMAD: the actor values WorkshopParentScript keeps on
    // every workshop ref, in its own index order (0 Food, 1 Happiness,
    // 2 Population, 3 Safety, 4 Water, 5 Power, 6 Beds, ...), plus the
    // WorkshopRatingTotal* values the Pip-Boy workshop page reads.
    constexpr uint32_t kRatingAVs[] = {
        0x00000331, 0x00129157, 0x0012723E, 0x00000333, 0x00000332, 0x0000032E, 0x00000334,
        0x0012722C, 0x00127240, 0x00127241, 0x0012722F, 0x00127231, 0x00127239, 0x00127230,
        0x00127235, 0x00127234, 0x00127233, 0x00127232, 0x00127237, 0x00127236, 0x00127238,
        0x0012722E, 0x0012723A, 0x0012723F, 0x0010C847, 0x0012722D, 0x0012723C, 0x0012723D,
        0x0012723B, 0x00086748, 0x00086749, 0x0008674A, 0x0008674B, 0x000A46FD, 0x000A46FF,
        0x000A4700, 0x000A4701, 0x000A4702, 0x000A4703, 0x000A4704, 0x000A4705, 0x000A4706,
        0x000CFF76, 0x001E3272, 0x00127242, 0x00127243, 0x00127244, 0x00127245, 0x00127246,
    };
    constexpr uint32_t kAV_Food = 0x331, kAV_Water = 0x332, kAV_Power = 0x32E, kAV_Safety = 0x333,
                       kAV_Beds = 0x334, kAV_Happiness = 0x129157, kAV_Population = 0x12723E;

    constexpr size_t kMaxItemTypes = 2048;

    using Inventory = std::map<uint32_t, int32_t>;   // base form -> count (sorted: stable wire text)
    using Ratings   = std::map<uint32_t, float>;

    double Now() { return static_cast<double>(GetTickCount64()) / 1000.0; }

    bool AuthorityNow() { return !CoSyncTransport::IsInitialized() || CoSyncTransport::IsHost(); }
    bool SessionLive()  { return CoSyncTransport::IsInitialized() && CoSyncNet::IsConnected(); }

    // ── Engine reads / writes (tick thread, SEH-guarded) ────────────────────
    TESObjectREFR* RefByID(uint32_t id)
    {
        if (id == 0) return nullptr;
        TESForm* f = LookupFormByID(id);
        if (!f || f->formType < kFormType_REFR || f->formType > kFormType_PHZD)
            return nullptr;
        return static_cast<TESObjectREFR*>(f);
    }

    __declspec(noinline) TESObjectREFR* WorkshopRef(uint32_t ws)
    {
        __try
        {
            TESObjectREFR* r = RefByID(ws);
            return (r && CoSyncWorkshopEngine::IsWorkshopRef(r)) ? r : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    ActorValueInfo* AVById(uint32_t id)
    {
        TESForm* f = LookupFormByID(id);
        return (f && f->formType == kFormType_AVIF) ? static_cast<ActorValueInfo*>(f) : nullptr;
    }

    __declspec(noinline) bool ReadAV(TESObjectREFR* ref, ActorValueInfo* avi, float& out)
    {
        __try { out = ref->actorValueOwner.GetValue(avi); return std::isfinite(out); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) bool WriteAV(TESObjectREFR* ref, ActorValueInfo* avi, float v)
    {
        __try { ref->actorValueOwner.SetBase(avi, v); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    Ratings ReadRatings(TESObjectREFR* ws)
    {
        Ratings r;
        for (uint32_t id : kRatingAVs)
        {
            ActorValueInfo* avi = AVById(id);
            float v = 0.f;
            if (avi && ReadAV(ws, avi, v))
                r[id] = v;
        }
        return r;
    }

    __declspec(noinline) bool ReadInventoryImpl(TESObjectREFR* ref, uint32_t* forms, int32_t* counts, size_t cap, size_t& n)
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
                int32_t total = 0;
                for (BGSInventoryItem::Stack* s = item.stack; s; s = s->next)
                    total += static_cast<int32_t>(s->count);
                if (total <= 0) continue;
                forms[n] = item.form->formID;
                counts[n] = total;
                ++n;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; return false; }
    }

    bool ReadInventory(TESObjectREFR* ref, Inventory& out)
    {
        static uint32_t forms[kMaxItemTypes];
        static int32_t  counts[kMaxItemTypes];
        size_t n = 0;
        out.clear();
        if (!ReadInventoryImpl(ref, forms, counts, kMaxItemTypes, n))
            return false;
        for (size_t i = 0; i < n; ++i)
            out[forms[i]] += counts[i];
        return true;
    }

    // Adds / removes `delta` of `form` in the container. Returns what was done.
    int32_t ApplyDelta(TESObjectREFR* box, uint32_t formID, int32_t delta, int32_t have)
    {
        if (delta == 0) return 0;
        TESForm* f = LookupFormByID(formID);
        if (!f) return 0;
        if (delta > 0)
            return CoSyncGameAPI::NativeAddItem(box, f, static_cast<UInt32>(delta)) ? delta : 0;
        const int32_t take = (std::min)(-delta, (std::max)(have, 0));
        if (take <= 0) return 0;
        return CoSyncGameAPI::NativeRemoveItem(box, f, static_cast<UInt32>(take)) ? -take : 0;
    }

    // ── Wire text ───────────────────────────────────────────────────────────
    std::string InvBody(const Inventory& inv)
    {
        std::string b;
        for (const auto& kv : inv)
        {
            if (!b.empty()) b += ';';
            b += std::to_string(kv.first) + ":" + std::to_string(kv.second);
        }
        return b;
    }

    bool ParseInv(const std::string& s, Inventory& out, bool allowNegative)
    {
        out.clear();
        size_t a = 0;
        while (a < s.size() && out.size() < kMaxItemTypes)
        {
            size_t b = s.find(';', a);
            if (b == std::string::npos) b = s.size();
            const std::string tok = s.substr(a, b - a);
            const size_t c = tok.find(':');
            if (c != std::string::npos)
            {
                const uint32_t f = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
                const long v = std::strtol(tok.c_str() + c + 1, nullptr, 10);
                if (f != 0 && v != 0 && v > -100000 && v < 100000 && (allowNegative || v > 0))
                    out[f] += static_cast<int32_t>(v);
            }
            a = b + 1;
        }
        return true;
    }

    std::string RatingsBody(const Ratings& r)
    {
        std::string b;
        char num[48];
        for (const auto& kv : r)
        {
            if (!b.empty()) b += ';';
            std::snprintf(num, sizeof(num), "%u:%.3f", kv.first, kv.second);
            b += num;
        }
        return b;
    }

    std::vector<std::string> Split(const std::string& s, char sep, size_t maxParts)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (out.size() + 1 < maxParts)
        {
            const size_t b = s.find(sep, a);
            if (b == std::string::npos) break;
            out.push_back(s.substr(a, b - a));
            a = b + 1;
        }
        out.push_back(s.substr(a));
        return out;
    }

    // ── Host state ──────────────────────────────────────────────────────────
    struct HostSettlement
    {
        Inventory inv;
        Ratings   ratings;
        uint32_t  invVersion = 0, ratingsVersion = 0;
        bool      haveInv = false, haveRatings = false;
        std::unordered_map<uint32_t, uint32_t> sentInv, sentRatings;   // conn -> version
    };
    std::unordered_map<uint32_t, HostSettlement> s_hostSet;               // ws ->
    std::unordered_map<uint64_t, uint32_t>       s_ack;                // (conn << 32 | ws) -> last applied seq
    std::set<uint32_t>                            s_forceResend;        // conns that just joined
    double s_nextHostPass = 0.0;

    // ── Participant state ───────────────────────────────────────────────────
    struct LocalSettlement
    {
        Inventory baseline;          // what the workbench holds as far as the host knows
        bool      haveBaseline = false;
        Ratings   ratings;           // the host's last ratings
        bool      haveRatings = false;
        uint32_t  seq = 0;           // last SSD sent
        double    lastAssert = 0.0;
    };
    std::unordered_map<uint32_t, LocalSettlement> s_local;
    double s_nextLocalPass = 0.0;

    void HostSendTo(uint32_t conn, const std::string& msg)
    {
        if (conn != 0 && SessionLive())
            CoSyncTransport::SendTo(conn, msg);
    }

    // Host: one pass over every assigned settlement with remote participants.
    void HostPass()
    {
        const std::vector<uint32_t> assigned = CoSyncTeams::GetAssignedSettlements();
        // Forget settlements that are no longer assigned.
        for (auto it = s_hostSet.begin(); it != s_hostSet.end(); )
            it = std::find(assigned.begin(), assigned.end(), it->first) == assigned.end() ? s_hostSet.erase(it) : std::next(it);

        for (uint32_t ws : assigned)
        {
            std::vector<uint32_t> conns;
            if (!CoSyncTeams::GetSettlementAudience(ws, conns))
                continue;
            conns.erase(std::remove(conns.begin(), conns.end(), 0u), conns.end());
            if (conns.empty())
                continue;   // nobody but the host: nothing to stream
            TESObjectREFR* wsRef = WorkshopRef(ws);
            if (!wsRef)
                continue;
            HostSettlement& hs = s_hostSet[ws];

            const Ratings r = ReadRatings(wsRef);
            if (!hs.haveRatings || r != hs.ratings)
            {
                hs.ratings = r;
                hs.haveRatings = true;
                ++hs.ratingsVersion;
            }
            Inventory inv;
            TESObjectREFR* box = CoSyncWorkshopEngine::GetWorkbenchContainer(wsRef);
            if (box && ReadInventory(box, inv) && (!hs.haveInv || inv != hs.inv))
            {
                hs.inv = std::move(inv);
                hs.haveInv = true;
                ++hs.invVersion;
            }

            for (uint32_t c : conns)
            {
                const bool fresh = s_forceResend.count(c) != 0;
                if (hs.haveRatings && (fresh || hs.sentRatings[c] != hs.ratingsVersion))
                {
                    hs.sentRatings[c] = hs.ratingsVersion;
                    HostSendTo(c, "SSR|" + std::to_string(ws) + "|" + RatingsBody(hs.ratings));
                }
                if (hs.haveInv && (fresh || hs.sentInv[c] != hs.invVersion))
                {
                    hs.sentInv[c] = hs.invVersion;
                    const uint32_t ack = s_ack[(static_cast<uint64_t>(c) << 32) | ws];
                    HostSendTo(c, "SSI|" + std::to_string(ws) + "|" + std::to_string(ack) + "|" + InvBody(hs.inv));
                }
            }
        }
        s_forceResend.clear();
    }

    // Host: a participant's workbench change.
    void HostApplyDelta(uint32_t conn, uint32_t ws, uint32_t seq, const Inventory& delta)
    {
        if (!CoSyncTeams::HostIsParticipant(conn, ws))
        {
            LOG_WARN("[Settlement] SSD for 0x%08X from conn=%u, who does not take part in it -- refused", ws, conn);
            return;
        }
        uint32_t& last = s_ack[(static_cast<uint64_t>(conn) << 32) | ws];
        if (seq <= last)
            return;   // duplicate / reordered
        last = seq;
        TESObjectREFR* wsRef = WorkshopRef(ws);
        TESObjectREFR* box = wsRef ? CoSyncWorkshopEngine::GetWorkbenchContainer(wsRef) : nullptr;
        if (!box)
            return;
        Inventory have;
        ReadInventory(box, have);
        int applied = 0;
        for (const auto& kv : delta)
            if (ApplyDelta(box, kv.first, kv.second, have[kv.first]) != 0)
                ++applied;
        // Resend the snapshot (with this sender's ack) on the next pass.
        auto it = s_hostSet.find(ws);
        if (it != s_hostSet.end())
            it->second.haveInv = false;
        s_nextHostPass = 0.0;
        LOG_INFO("[Settlement] conn=%u changed workbench 0x%08X: %zu item type(s), %d applied (seq %u)",
            conn, ws, delta.size(), applied, seq);
    }

    // Participant: make the local workbench match the host's snapshot.
    void ClientApplySnapshot(uint32_t ws, uint32_t ack, const Inventory& host)
    {
        if (!CoSyncTeams::IsLocalParticipant(ws))
            return;
        LocalSettlement& ls = s_local[ws];
        if (ack < ls.seq)
            return;   // older than a change we already sent: the next snapshot carries it
        TESObjectREFR* wsRef = WorkshopRef(ws);
        TESObjectREFR* box = wsRef ? CoSyncWorkshopEngine::GetWorkbenchContainer(wsRef) : nullptr;
        if (!box)
            return;
        Inventory local;
        if (!ReadInventory(box, local))
            return;
        std::set<uint32_t> forms;
        for (const auto& kv : local) forms.insert(kv.first);
        for (const auto& kv : host) forms.insert(kv.first);
        int changed = 0;
        for (uint32_t f : forms)
        {
            const int32_t want = host.count(f) ? host.at(f) : 0;
            const int32_t have = local.count(f) ? local.at(f) : 0;
            if (want != have && ApplyDelta(box, f, want - have, have) != 0)
                ++changed;
        }
        // Baseline = what the workbench now holds (re-read: an add can merge
        // into an existing stack, a removal can be partial).
        ReadInventory(box, ls.baseline);
        ls.haveBaseline = true;
        if (changed)
            LOG_INFO("[Settlement] workbench 0x%08X matched to the host's (%d item type(s) changed)", ws, changed);
    }

    void ClientApplyRatings(uint32_t ws, const Ratings& r, bool force)
    {
        TESObjectREFR* wsRef = WorkshopRef(ws);
        if (!wsRef)
            return;
        int changed = 0;
        for (const auto& kv : r)
        {
            ActorValueInfo* avi = AVById(kv.first);
            if (!avi) continue;
            float cur = 0.f;
            if (ReadAV(wsRef, avi, cur) && std::fabs(cur - kv.second) <= 0.01f && !force)
                continue;
            if (WriteAV(wsRef, avi, kv.second))
                ++changed;
        }
        if (changed)
            LOG_UPDATED("[Settlement] ratings of 0x%08X set to the host's (%d value(s))", ws, changed);
    }

    // Participant: report local workbench changes; keep the host's ratings.
    void ClientPass(double t)
    {
        for (auto it = s_local.begin(); it != s_local.end(); )
        {
            const uint32_t ws = it->first;
            LocalSettlement& ls = it->second;
            if (!CoSyncTeams::IsLocalParticipant(ws))
            {
                it = s_local.erase(it);   // left the team / settlement released
                continue;
            }
            ++it;
            TESObjectREFR* wsRef = WorkshopRef(ws);
            if (!wsRef)
                continue;
            if (ls.haveBaseline)
            {
                TESObjectREFR* box = CoSyncWorkshopEngine::GetWorkbenchContainer(wsRef);
                Inventory local;
                if (box && ReadInventory(box, local) && local != ls.baseline)
                {
                    Inventory delta;
                    std::set<uint32_t> forms;
                    for (const auto& kv : local) forms.insert(kv.first);
                    for (const auto& kv : ls.baseline) forms.insert(kv.first);
                    for (uint32_t f : forms)
                    {
                        const int32_t d = (local.count(f) ? local.at(f) : 0) - (ls.baseline.count(f) ? ls.baseline.at(f) : 0);
                        if (d != 0) delta[f] = d;
                    }
                    if (!delta.empty() && SessionLive())
                    {
                        ++ls.seq;
                        CoSyncTransport::Send("SSD|" + std::to_string(ws) + "|" + std::to_string(ls.seq) + "|" + InvBody(delta));
                        LOG_INFO("[Settlement] my workbench 0x%08X changed: %zu item type(s) sent to the host (seq %u)",
                            ws, delta.size(), ls.seq);
                    }
                    ls.baseline = std::move(local);
                }
            }
            // This machine's own WorkshopParentScript may recompute ratings
            // from its local view; the host's values win (every 2 s).
            if (ls.haveRatings && t - ls.lastAssert >= 2.0)
            {
                ls.lastAssert = t;
                ClientApplyRatings(ws, ls.ratings, false);
            }
        }
    }
}

namespace CoSyncSettlementSync
{
    void OnSessionStart(bool)
    {
        s_hostSet.clear();
        s_ack.clear();
        s_forceResend.clear();
        s_local.clear();
        s_nextHostPass = s_nextLocalPass = 0.0;
    }

    void OnSessionEnd()
    {
        s_hostSet.clear();
        s_ack.clear();
        s_forceResend.clear();
        s_local.clear();
    }

    void HostOnPeerJoined(uint32_t conn)
    {
        s_forceResend.insert(conn);
        s_nextHostPass = 0.0;
    }

    void Tick(double now)
    {
        (void)now;
        if (!SessionLive() || !CoSyncWorld::IsWorldReady())
            return;
        const double t = Now();
        if (AuthorityNow())
        {
            if (t >= s_nextHostPass)
            {
                s_nextHostPass = t + 1.0;
                HostPass();
            }
            return;
        }
        if (t >= s_nextLocalPass)
        {
            s_nextLocalPass = t + 0.5;
            ClientPass(t);
        }
    }

    bool IsSettlementMessage(const std::string& msg)
    {
        return msg.size() > 4 && msg[0] == 'S' && msg[1] == 'S' && msg[3] == '|' &&
               (msg[2] == 'R' || msg[2] == 'I' || msg[2] == 'D');
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncSettlementSync::OnReceive", "CoSync tick. Untrusted input: the host checks the sender takes part in the settlement.");
        if (msg.size() > 64 * 1024)
            return;
        const char kind = msg[2];
        if (kind == 'D')
        {
            if (!isHostRole || conn == 0)
                return;
            const std::vector<std::string> v = Split(msg.substr(4), '|', 3);
            if (v.size() != 3) return;
            Inventory delta;
            ParseInv(v[2], delta, true);
            HostApplyDelta(conn, static_cast<uint32_t>(std::strtoul(v[0].c_str(), nullptr, 10)),
                           static_cast<uint32_t>(std::strtoul(v[1].c_str(), nullptr, 10)), delta);
            return;
        }
        if (isHostRole)
            return;   // SSR / SSI only come from the host
        if (kind == 'I')
        {
            const std::vector<std::string> v = Split(msg.substr(4), '|', 3);
            if (v.size() != 3) return;
            Inventory inv;
            ParseInv(v[2], inv, false);
            ClientApplySnapshot(static_cast<uint32_t>(std::strtoul(v[0].c_str(), nullptr, 10)),
                                static_cast<uint32_t>(std::strtoul(v[1].c_str(), nullptr, 10)), inv);
            return;
        }
        // SSR
        const std::vector<std::string> v = Split(msg.substr(4), '|', 2);
        if (v.size() != 2) return;
        const uint32_t ws = static_cast<uint32_t>(std::strtoul(v[0].c_str(), nullptr, 10));
        if (!CoSyncTeams::IsLocalParticipant(ws))
            return;
        Ratings r;
        size_t a = 0;
        const std::string& s = v[1];
        while (a < s.size() && r.size() < 128)
        {
            size_t b = s.find(';', a);
            if (b == std::string::npos) b = s.size();
            const std::string tok = s.substr(a, b - a);
            const size_t c = tok.find(':');
            if (c != std::string::npos)
            {
                const uint32_t id = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
                const float val = std::strtof(tok.c_str() + c + 1, nullptr);
                if (id != 0 && std::isfinite(val) && std::fabs(val) < 1.0e6f)
                    r[id] = val;
            }
            a = b + 1;
        }
        LocalSettlement& ls = s_local[ws];
        ls.ratings = r;
        ls.haveRatings = true;
        ls.lastAssert = Now();
        ClientApplyRatings(ws, r, false);
    }

    std::string BuildUiJson(uint32_t ws)
    {
        std::string j = "{\"id\":" + std::to_string(ws);
        TESObjectREFR* wsRef = ws ? WorkshopRef(ws) : nullptr;
        if (wsRef)
        {
            auto av = [&](const char* key, uint32_t id) {
                ActorValueInfo* avi = AVById(id);
                float v = 0.f;
                if (avi && ReadAV(wsRef, avi, v))
                {
                    char b[64];
                    std::snprintf(b, sizeof(b), ",\"%s\":%.0f", key, v);
                    j += b;
                }
            };
            av("food", kAV_Food);
            av("water", kAV_Water);
            av("power", kAV_Power);
            av("defense", kAV_Safety);
            av("beds", kAV_Beds);
            av("happiness", kAV_Happiness);
            av("population", kAV_Population);
        }
        auto it = s_local.find(ws);
        const bool synced = AuthorityNow() ? false : (it != s_local.end() && (it->second.haveRatings || it->second.haveBaseline));
        j += ",\"synced\":"; j += synced ? "true" : "false";
        j += "}";
        return j;
    }
}
