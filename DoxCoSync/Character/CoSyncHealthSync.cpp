#include "CoSyncHealthSync.h"

#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncActorValueSafety.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncOverlay.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameData.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cmath>

extern RelocPtr<PlayerCharacter*> g_player;
extern PlayerCharacter* CoSyncGetPlayer();

static constexpr uint32_t kHealthAVI_FormID = 0x0002D4;

static ActorValueInfo* GetHealthAVI()
{
    LOG_REQUIREMENT("CoSyncHealthSync::GetHealthAVI", "Needs the game to have finished loading so it can look up the Health stat definition.");
    static ActorValueInfo* s_avi = nullptr;
    if (!s_avi)
    {
        TESForm* form = LookupFormByID(kHealthAVI_FormID);
        if (form)
            s_avi = static_cast<ActorValueInfo*>(form);
    }
    return s_avi;
}

// The Rads actor value has no stable, install-independent formID we could
// hardcode (the QRADS engine getter is name-registry-keyed, not a LookupForm
// of a constant — IDA-confirmed 2026-07-15 at RVA 0x65EC40). Resolve it once
// by scanning DataHandler->arrAVIF (tArray<TESForm*>) for the AVIF whose full
// name is "Rads". SEH-guarded and cached; if it can't resolve, rads stay 0 and
// the health bar simply shows no red segment (graceful degradation).
extern RelocPtr<DataHandler*> g_dataHandler;

__declspec(noinline) static ActorValueInfo* ResolveRadsAVI_Impl()
{
    DataHandler* dh = g_dataHandler ? *g_dataHandler : nullptr;
    if (!dh) return nullptr;

    const UInt32 n = dh->arrAVIF.count;
    for (UInt32 i = 0; i < n; ++i)
    {
        TESForm* f = dh->arrAVIF[i];
        if (!f) continue;
        ActorValueInfo* avi = static_cast<ActorValueInfo*>(f);
        const char* nm = avi->fullName.name.c_str();
        if (nm && (_stricmp(nm, "Rads") == 0 || _stricmp(nm, "Radiation") == 0))
        {
            LOG_INFO("[HealthSync] Rads AVI resolved: formID=0x%08X name='%s'", f->formID, nm);
            return avi;
        }
    }
    LOG_WARN("[HealthSync] Rads AVI not found in arrAVIF (%u entries) — radiation bar disabled", n);
    return nullptr;
}

static ActorValueInfo* GetRadsAVI()
{
    LOG_REQUIREMENT("CoSyncHealthSync::GetRadsAVI", "Needs the game's data handler to be ready. Looks up the Radiation stat once and remembers the result so it doesn't have to search again.");
    static ActorValueInfo* s_avi = nullptr;
    static bool            s_tried = false;
    if (!s_tried)
    {
        s_tried = true;
        __try { s_avi = ResolveRadsAVI_Impl(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[HealthSync] ResolveRadsAVI faulted -- radiation bar disabled");
            s_avi = nullptr;
        }
    }
    return s_avi;
}

ActorValueInfo* CoSyncHealthSync::GetRadsActorValue() { return GetRadsAVI(); }

bool CoSyncHealthSync::IsHP(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncHealthSync::IsHP", "Just checks the text of a message. No game state needed.");
    return msg.size() > 3 && msg[0] == 'H' && msg[1] == 'P' && msg[2] == '|';
}

bool CoSyncHealthSync::IsKill(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncHealthSync::IsKill", "Just checks the text of a message. No game state needed.");
    return msg.rfind("KILL|", 0) == 0;
}

bool CoSyncHealthSync::IsDmg(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncHealthSync::IsDmg", "Just checks the text of a message. No game state needed.");
    return msg.rfind("DMG|", 0) == 0;
}

std::string CoSyncHealthSync::SerializeHP(uint32_t entityID, float current, float maxHP, uint32_t level, float rads,
                                          float ap, float apMax)
{
    LOG_REQUIREMENT("CoSyncHealthSync::SerializeHP", "Just turns health numbers into text. Doesn't touch the game at all.");
    std::ostringstream ss;
    ss << "HP|" << entityID << "|" << current << "|" << maxHP;
    // rads rides on the optional level field: the player path always sends
    // level (>=1 in practice), so appending rads after it is unambiguous.
    // NPC paths send neither, so a bare 4-field packet still parses.
    // AP (with its max) extends the chain the same way; when present, all
    // earlier optional fields are forced so positions stay unambiguous.
    if (ap >= 0.f)
    {
        ss << "|" << level << "|" << rads << "|" << ap << "|" << apMax;
    }
    else if (level > 0 || rads > 0.f)
    {
        ss << "|" << level;
        if (rads > 0.f)
            ss << "|" << rads;
    }
    return ss.str();
}


std::string CoSyncHealthSync::SerializeKill(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncHealthSync::SerializeKill", "Just turns a player ID into text. Doesn't touch the game at all.");
    std::ostringstream ss;
    ss << "KILL|" << entityID;
    return ss.str();
}

std::string CoSyncHealthSync::SerializeDmg(uint32_t entityID, float amount)
{
    LOG_REQUIREMENT("CoSyncHealthSync::SerializeDmg", "Just turns a damage amount into text. Doesn't touch the game at all.");
    std::ostringstream ss;
    ss << "DMG|" << entityID << "|" << amount;
    return ss.str();
}

struct HealthOp
{
    enum Type { SetHealth, Kill, Damage };
    Type     type     = SetHealth;
    uint32_t entityID = 0;
    float    current  = 0.f;   // SetHealth: new HP.  Damage: amount to apply.
    float    maxHP    = 0.f;
    uint32_t level    = 0;     // SetHealth only, 0 = not carried
    float    rads     = 0.f;   // SetHealth only, current radiation (red bar chunk)
    float    ap       = -1.f;  // SetHealth only, current action points (-1 = absent)
    float    apMax    = 0.f;
};

static std::mutex            s_opMutex;
static std::vector<HealthOp> s_pendingOps;

// Damage-detection baseline: last HP we observed (or applied from the network)
// per entityID. A drop below the baseline between polls means someone in THIS
// world damaged that remote-owned actor, and the delta is broadcast as DMG.
static std::unordered_map<uint32_t, float> s_lastSeenHP;

// round 29 (P1 host crash, Fallout4.exe+0xCC934D) tried a settle-delay gate
// here on the theory this was the same class of bug as
// CoSyncPlayer.cpp's kAnimGraphSettleDelay (a proxy not yet "settled" after
// spawn). Round 30 disproved that: P2's own log caught the SAME RVA crashing
// a third time via CoSyncSpecialSync's SetBase, on an actor that had been
// alive for a while -- no amount of waiting helps, because the engine never
// allocates the sub-object this crash's vtable dispatch needs for an
// AI-disabled actor (every CoSync proxy gets DisableActorAI() synchronously
// at spawn, permanently, by design). The settle-delay gate is gone; the real
// fix is SEH at the point of contact, via CoSyncAVSafety (shared across every
// subsystem that touches a proxy's actorValueOwner -- see that header for
// the full writeup).

void CoSyncHealthSync::OnReceiveHP(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncHealthSync::OnReceiveHP", "Reads a health update out of text. A badly formed message is safely rejected, not crashed on.");

    if (!IsHP(msg))
        return;

    const char* p = msg.c_str() + 3;
    char* end = nullptr;

    uint32_t entityID = (uint32_t)std::strtoul(p, &end, 10);
    if (!end || *end != '|' || entityID == 0)
    {
        LOG_WARN("[HealthSync] Malformed HP packet: '%s'", msg.c_str());
        return;
    }
    p = end + 1;

    float current = std::strtof(p, &end);
    if (!end || *end != '|')
    {
        LOG_WARN("[HealthSync] Malformed HP current: '%s'", msg.c_str());
        return;
    }
    p = end + 1;

    float maxHP = std::strtof(p, &end);

    uint32_t level = 0;
    float    rads  = 0.f;
    float    ap    = -1.f;
    float    apMax = 0.f;
    if (end && *end == '|')
    {
        char* lvlEnd = nullptr;
        level = (uint32_t)std::strtoul(end + 1, &lvlEnd, 10);
        // Optional trailing fields: "...|level|rads[|ap|apMax]"
        if (lvlEnd && *lvlEnd == '|')
        {
            char* radsEnd = nullptr;
            rads = std::strtof(lvlEnd + 1, &radsEnd);
            if (radsEnd && *radsEnd == '|')
            {
                char* apEnd = nullptr;
                ap = std::strtof(radsEnd + 1, &apEnd);
                if (apEnd && *apEnd == '|')
                    apMax = std::strtof(apEnd + 1, nullptr);
            }
        }
    }

    HealthOp op{};
    op.type     = HealthOp::SetHealth;
    op.entityID = entityID;
    op.current  = current;
    op.maxHP    = maxHP;
    op.level    = level;
    op.rads     = rads;
    op.ap       = ap;
    op.apMax    = apMax;

    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.push_back(op);
    }
}

void CoSyncHealthSync::OnReceiveKill(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncHealthSync::OnReceiveKill", "Reads a kill notice out of text. Needs a real player ID in the message.");

    if (!IsKill(msg))
        return;

    const char* p = msg.c_str() + 5;
    char* end = nullptr;
    uint32_t entityID = (uint32_t)std::strtoul(p, &end, 10);
    if (entityID == 0)
    {
        LOG_WARN("[HealthSync] Malformed KILL packet: '%s'", msg.c_str());
        return;
    }

    HealthOp op{};
    op.type     = HealthOp::Kill;
    op.entityID = entityID;

    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.push_back(op);
    }
}

void CoSyncHealthSync::OnReceiveDmg(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncHealthSync::OnReceiveDmg", "Reads a damage update out of text; a badly formed message is safely rejected. Only queues it -- the actual damage is applied later on the main game thread.");

    if (!IsDmg(msg))
        return;

    const char* p = msg.c_str() + 4;
    char* end = nullptr;

    uint32_t entityID = (uint32_t)std::strtoul(p, &end, 10);
    if (!end || *end != '|' || entityID == 0)
    {
        LOG_WARN("[HealthSync] Malformed DMG packet: '%s'", msg.c_str());
        return;
    }

    float amount = std::strtof(end + 1, nullptr);
    if (!(amount > 0.f) || amount > 100000.f)
    {
        LOG_WARN("[HealthSync] DMG amount out of range: '%s'", msg.c_str());
        return;
    }

    HealthOp op{};
    op.type     = HealthOp::Damage;
    op.entityID = entityID;
    op.current  = amount;

    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.push_back(op);
    }
}

// Applies real damage via Papyrus Actor.DamageValue(ActorValue, float).
// Papyrus routes through the engine's full damage pipeline (kill handling,
// essential flags, difficulty scaling is already baked into the sent amount),
// and CallFunctionNoWait is async + safe from the game thread.
static void ApplyDamageValue(Actor* actor, float amount)
{
    LOG_REQUIREMENT("CoSyncHealthSync::ApplyDamageValue", "Needs a real, alive character -- the actual player or a normal NPC, not one of this mod's stand-in characters -- plus the Health stat to be available.");
    ActorValueInfo* healthAVI = GetHealthAVI();
    if (!actor || !healthAVI)
        return;

    VMArray<VMVariable> args;

    ActorValueInfo* avi = healthAVI;
    VMVariable vAV; vAV.Set(&avi);   args.Push(&vAV);

    float amt = amount;
    VMVariable vAmt; vAmt.Set(&amt); args.Push(&vAmt);

    static const BSFixedString kDamageValue("DamageValue");
    CallFunctionNoWait(actor, kDamageValue, args);
}

void CoSyncHealthSync::FlushPendingHealthOps()
{
    LOG_REQUIREMENT("CoSyncHealthSync::FlushPendingHealthOps", "Needs the Health stat to be available, and checks each queued update's stand-in character is actually spawned before touching it. Updates aimed at the local player need a valid player too.");
    std::vector<HealthOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        if (s_pendingOps.empty())
            return;
        ops.swap(s_pendingOps);
    }

    ActorValueInfo* healthAVI = GetHealthAVI();
    if (!healthAVI)
        return;

    const uint32_t myEntityID = CoSyncNet::GetMyEntityID();

    for (const HealthOp& op : ops)
    {
        // ── DMG: damage routed to whoever owns the entity ────────────────
        if (op.type == HealthOp::Damage)
        {
            if (op.entityID == myEntityID)
            {
                // Someone damaged MY proxy in THEIR world — I take it for real.
                PlayerCharacter* player = CoSyncGetPlayer();
                if (player)
                {
                    ApplyDamageValue(player, op.current);
                    LOG_UPDATED("[HealthSync] RX DMG %.1f -> applied to LOCAL PLAYER", op.current);
                }
            }
            else if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID))
            {
                // Damage to a world NPC is applied by whichever machine owns
                // the real actor (host for host-discovered, the reporting
                // client for client-owned); the owner's periodic HP broadcast
                // then confirms the new value back to every peer.
                const auto& states = g_CoSyncPlayerManager.GetStates();
                auto sIt = states.find(op.entityID);
                if (sIt != states.end() && sIt->second.isLocallyOwned)
                {
                    CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(op.entityID);
                    if (proxy && proxy->actorRef && proxy->hasSpawned)
                    {
                        // 2026-08-16 pt.9: was ApplyDamageValue (Papyrus
                        // Actor.DamageValue via CallFunctionNoWait). Log
                        // evidence from a live kill: client dealt 4x12.0 to
                        // entity=65536 (its own local puppet HP tracked
                        // 35->23->11->-1->-13.1), host logged "applied" all 4
                        // times, but entity=65536 NEVER appeared in the host's
                        // own DIED log for the rest of the session -- the
                        // Papyrus call was reached (per the log) but never
                        // actually reduced the real actor's HP. Switched to
                        // the SAME native path already proven for the
                        // SetHealth branch just below (ForceCurrent via
                        // ActorValueOwner vtable slot 6, IDA-verified) --
                        // clamps, updates the health bar, and correctly
                        // triggers native death at <=0. Also finally gives a
                        // real success/failure result instead of firing into
                        // the void.
                        ActorValueInfo* dmgAVI = GetHealthAVI();
                        if (dmgAVI && CoSyncAVSafety::SafeForceDamage(proxy->actorRef, dmgAVI, -op.current))
                        {
                            LOG_UPDATED("[HealthSync] RX DMG %.1f -> applied to world NPC entity=%u (native ForceCurrent)",
                                op.current, op.entityID);
                        }
                        else
                        {
                            LOG_UPDATED_WARN("[HealthSync] RX DMG %.1f entity=%u FAILED -- SafeForceDamage rejected it (avi=%p actor=%p)",
                                op.current, op.entityID, static_cast<void*>(dmgAVI), static_cast<void*>(proxy->actorRef));
                        }
                    }
                    else
                    {
                        // 2026-08-16 pt.8 diag: this machine thinks it owns
                        // this entity but has no usable actor record for it --
                        // a relayed kill-shot lands here silently right now.
                        LOG_UPDATED_WARN("[HealthSync] RX DMG %.1f entity=%u DROPPED -- isLocallyOwned=1 but proxy=%p actorRef=%p hasSpawned=%d",
                            op.current, op.entityID, static_cast<void*>(proxy),
                            proxy ? static_cast<void*>(proxy->actorRef) : nullptr,
                            proxy ? (int)proxy->hasSpawned : -1);
                    }
                }
                else
                {
                    // Not owned here -- expected and silent for the common
                    // case (a non-owner puppet-holder, or the sender's own
                    // relay echoing back to itself; see reference-cosync-
                    // host-relay-echo memory). Live-confirmed 2026-08-16
                    // pt.9: the client's own 4 TX DMG sends echoed back and
                    // hit this branch every time -- correctly ignored, not a
                    // failure. Kept at INFO instead of the original bare
                    // silence purely so a genuine 3+-peer bystander case is
                    // still visible in the log if it ever matters.
                    LOG_INFO("[HealthSync] RX DMG %.1f entity=%u ignored -- not locally owned here (hasState=%d, isLocallyOwned=%d)",
                        op.current, op.entityID, sIt != states.end() ? 1 : 0,
                        sIt != states.end() ? (int)sIt->second.isLocallyOwned : -1);
                }
            }
            // else: a relayed DMG for an entity another machine owns — ignore.
            continue;
        }

        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(op.entityID);
        if (!proxy || !proxy->actorRef || !proxy->hasSpawned)
            continue;

        Actor* actor = proxy->actorRef;

        // round 127: AI-disabled proxy actors have no actorValueOwner sub-object.
        // GetMaximum AND SetBase both fault at +0xCC934D on the same actor in
        // the same flush tick. Blacklist on the first fault to eliminate both.
        static std::unordered_set<Actor*> s_badHealthActors;
        if (s_badHealthActors.count(actor))
            continue;

        if (op.type == HealthOp::SetHealth)
        {
            // Lobby bar first: the UI mirrors the NETWORK-reported vitals and
            // must stay live even when this proxy's ActorValue subsystem is
            // broken/blacklisted (the actor write below is best-effort).
            if (!CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID))
            {
                proxy->m_uiHealth    = op.current;
                if (op.maxHP > 0.f)
                    proxy->m_uiMaxHealth = op.maxHP;
                if (op.level > 0)
                    proxy->m_uiLevel = op.level;
                proxy->m_uiRads = op.rads;

                CoSyncOverlay_UpdateLobbyEntry(
                    op.entityID,
                    proxy->username.c_str(),
                    proxy->m_uiHealth,
                    proxy->m_uiMaxHealth,
                    (int)proxy->m_uiLevel,
                    false,
                    proxy->m_uiRads);

                if (op.ap >= 0.f)
                    CoSyncOverlay_UpdateLobbyAP(op.entityID, op.ap, op.apMax);
            }

            float effectiveMax = 0.f;
            if (!CoSyncAVSafety::SafeGetMaximum(actor, healthAVI, effectiveMax))
            {
                s_badHealthActors.insert(actor);
                LOG_UPDATED_WARN("[HealthSync] entity=%u actor=%p AV-BLACKLISTED (GetMaximum faulted) — HP writes disabled for it",
                    op.entityID, static_cast<void*>(actor));
                continue;
            }
            if (effectiveMax <= 0.f)
                effectiveMax = op.maxHP > 0.f ? op.maxHP : 100.f;

            float target = op.current;
            if (target < 0.f)   target = 0.f;
            if (target > effectiveMax) target = effectiveMax;

            if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID) &&
                CoSyncGameAPI::PuppetModeEnabled())
            {
                // FalloutTogether/TiltedEvolution-style: apply enemy health the
                // DAMAGE way (ForceCurrent, AVO vtable slot 6 — IDA-verified) so it
                // clamps, updates the health bar, and triggers native death at <=0,
                // unlike SetBase (base value, never kills). delta = target - current.
                float cur = 0.f;
                CoSyncAVSafety::SafeGetValue(actor, healthAVI, cur);
                CoSyncAVSafety::SafeForceDamage(actor, healthAVI, target - cur);
            }
            else
            {
                CoSyncAVSafety::SafeSetBase(actor, healthAVI, target);
            }

            // The network just told us this entity's authoritative HP —
            // re-baseline damage detection so this set is never mistaken
            // for local damage.
            s_lastSeenHP[op.entityID] = target;

            static int s_logCount = 0;
            if (++s_logCount % 60 == 1)
                LOG_INFO("[HealthSync] Set entity=%u health=%.1f/%.1f lvl=%u",
                    op.entityID, target, effectiveMax, op.level);
        }
        else
        {
            LOG_INFO("[HealthSync] Killing entity=%u", op.entityID);
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID) &&
                CoSyncGameAPI::PuppetModeEnabled())
            {
                // AI-alive enemy puppet: native death (SetLifeState(kDying) +
                // KillDying) — real ragdoll/loot, and avoids Papyrus Kill on the
                // proxy. Player proxies keep the Papyrus Kill path below.
                CoSyncGameAPI::KillPuppet(actor);
            }
            else
            {
                static const BSFixedString kKill("Kill");
                VMArray<VMVariable> emptyArgs;
                CallFunctionNoWait(actor, kKill, emptyArgs);
            }

            s_lastSeenHP[op.entityID] = 0.f;

            // Remember the kill so a later DESTROY keeps the corpse instead of
            // despawning it and re-enabling the (alive) original.
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID))
                CoSyncNpcWorldSync::MarkEntityKilled(op.entityID);

            if (!CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID))
            {
                proxy->m_uiHealth = 0.f;
                CoSyncOverlay_UpdateLobbyEntry(
                    op.entityID,
                    proxy->username.c_str(),
                    0.f,
                    proxy->m_uiMaxHealth,
                    (int)proxy->m_uiLevel,
                    false);
            }
        }
    }
}

void CoSyncHealthSync::TickDamageDetection(double now)
{
    LOG_REQUIREMENT("CoSyncHealthSync::TickDamageDetection", "Needs an active connection. Reads each character's health safely, and if a read ever fails for one, that character is skipped from then on instead of risking another crash. Must run on the main game thread.");

    static double s_lastTick = 0.0;
    static constexpr double kDetectInterval = 0.15;
    if (now - s_lastTick < kDetectInterval)
        return;
    s_lastTick = now;

    if (!CoSyncNet::IsConnected())
        return;

    ActorValueInfo* healthAVI = GetHealthAVI();
    if (!healthAVI)
        return;

    const uint32_t myEntityID = CoSyncNet::GetMyEntityID();

    // Client-side NPC damage reports are gated on the local player actually
    // being in combat. Without hit-source attribution, a client's own
    // divergent AI simulation (NPCs infighting locally) would otherwise
    // stream phantom damage up to the host.
    bool localInCombat = false;
    {
        PlayerCharacter* player = CoSyncGetPlayer();
        if (player)
            localInCombat = player->IsInCombat();
    }

    static std::unordered_set<Actor*> s_badDetectActors;

    for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
    {
        CoSyncPlayer* p = kv.second.get();
        if (!p || !p->hasSpawned || !p->actorRef)
            continue;
        if (p->isLocalPlayer || p->entityID == myEntityID)
            continue;

        const bool isWorldNpc = CoSyncNpcWorldSync::IsWorldActorEntityID(p->entityID);

        // The machine that owns a world NPC's real actor covers its damage
        // via the authoritative HP broadcast — only puppet-holders report
        // observed damage up to the owner.
        if (isWorldNpc)
        {
            const auto& states = g_CoSyncPlayerManager.GetStates();
            auto sIt = states.find(p->entityID);
            if (sIt != states.end() && sIt->second.isLocallyOwned)
                continue;
        }
        if (isWorldNpc && !localInCombat)
        {
            // Also relay damage when the host confirms this enemy is actively
            // in combat — handles player-initiated first strikes (stealth
            // attacks) where the client hasn't entered combat yet but the
            // enemy is clearly being fought. Without this, stealth damage is
            // silently absorbed into the idle-baseline update and never sent.
            if (!p->m_pendingCombatActive)
            {
                // Enemy is peaceful on host side — keep baseline moving to
                // avoid a large stale delta when combat eventually starts.
                float idleHP = 0.f;
                if (!s_badDetectActors.count(p->actorRef) &&
                    CoSyncAVSafety::SafeGetValue(p->actorRef, healthAVI, idleHP))
                    s_lastSeenHP[p->entityID] = idleHP;
                continue;
            }
            // Enemy is in active combat on host side — fall through and
            // relay HP drops regardless of localInCombat.
        }

        if (s_badDetectActors.count(p->actorRef))
            continue;

        float cur = 0.f;
        if (!CoSyncAVSafety::SafeGetValue(p->actorRef, healthAVI, cur))
        {
            s_badDetectActors.insert(p->actorRef);
            LOG_UPDATED_WARN("[HealthSync] detection: entity=%u actor=%p AV-BLACKLISTED (GetValue faulted) — damage relay disabled for it",
                p->entityID, static_cast<void*>(p->actorRef));
            continue;
        }

        // Live-diag: proves whether local attacks actually move this proxy's
        // HP (2026-07-05 test showed zero TX DMG — suspected friendly-fire
        // immunity from the old teammate flag; this line is the ground truth
        // for the next session).
        if (!isWorldNpc)
        {
            static std::unordered_map<uint32_t, double> s_lastHpDiag;
            double& lastDiag = s_lastHpDiag[p->entityID];
            if (now - lastDiag >= 5.0)
            {
                lastDiag = now;
                LOG_INFO("[HealthSync] proxy-HP diag entity=%u hp=%.1f", p->entityID, cur);
            }
        }

        auto it = s_lastSeenHP.find(p->entityID);
        if (it == s_lastSeenHP.end())
        {
            s_lastSeenHP[p->entityID] = cur;
            continue;
        }

        const float drop = it->second - cur;
        it->second = cur;

        if (drop >= 1.0f)
        {
            CoSyncTransport::Send(SerializeDmg(p->entityID, drop));
            LOG_UPDATED("[HealthSync] TX DMG entity=%u amount=%.1f (%s, hp %.1f->%.1f)",
                p->entityID, drop,
                isWorldNpc ? "world NPC, client report" : "player proxy",
                cur + drop, cur);
        }
    }
}

void CoSyncHealthSync::Reset()
{
    LOG_REQUIREMENT("CoSyncHealthSync::Reset", "Just clears some internal lists. Doesn't touch the game at all.");
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.clear();
    }
    s_lastSeenHP.clear();
    LOG_INFO("[HealthSync] Reset — baselines cleared for new session");
}
