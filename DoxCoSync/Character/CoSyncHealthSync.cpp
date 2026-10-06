#include "CoSyncHealthSync.h"

#include "ConsoleLogger.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncActorValueSafety.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncEnemySync.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncOverlay.h"
#include "CoSyncSessionRules.h"
#include "CoSyncCombatAuthority.h"

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
    return msg.size() > 3 && msg[0] == 'H' && msg[1] == 'P' && msg[2] == '|';
}

bool CoSyncHealthSync::IsKill(const std::string& msg)
{
    return msg.rfind("KILL|", 0) == 0;
}

bool CoSyncHealthSync::IsDmg(const std::string& msg)
{
    return msg.rfind("DMG|", 0) == 0;
}

std::string CoSyncHealthSync::SerializeHP(uint32_t entityID, float current, float maxHP, uint32_t level, float rads,
                                          float ap, float apMax)
{
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
    std::ostringstream ss;
    ss << "KILL|" << entityID;
    return ss.str();
}

std::string CoSyncHealthSync::SerializeDmg(uint32_t entityID, float amount, uint32_t attackerEid)
{
    std::ostringstream ss;
    ss << "DMG|" << entityID << "|" << amount;
    if (attackerEid != 0)
        ss << "|" << attackerEid;
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
    uint32_t killer   = 0;     // Kill only (0.4.1): the killer's entity, 0 = unknown
                               // Damage (0.5.1): the attacker's entity, 0 = unknown
    uint32_t flags    = 0;     // Kill only: bit0 = ragdoll instantly
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
    // 0.4.1: "KILL|eid|killer|flags" (older "KILL|eid" still parses).
    if (end && *end == '|')
    {
        char* e2 = nullptr;
        op.killer = (uint32_t)std::strtoul(end + 1, &e2, 10);
        if (e2 && *e2 == '|')
            op.flags = (uint32_t)std::strtoul(e2 + 1, nullptr, 10);
    }

    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.push_back(op);
    }
}

void CoSyncHealthSync::OnReceiveDmg(const std::string& msg)
{

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

    char* amtEnd = nullptr;
    float amount = std::strtof(end + 1, &amtEnd);
    uint32_t attacker = 0;
    if (amtEnd && *amtEnd == '|')
        attacker = static_cast<uint32_t>(std::strtoul(amtEnd + 1, nullptr, 10));
    if (!(amount > 0.f) || amount > 100000.f)
    {
        LOG_WARN("[HealthSync] DMG amount out of range: '%s'", msg.c_str());
        return;
    }

    HealthOp op{};
    op.type     = HealthOp::Damage;
    op.entityID = entityID;
    op.current  = amount;
    op.killer   = attacker;

    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.push_back(op);
    }
}

// 0.5.1: is this entity a PLAYER (this machine's, or another player's proxy)?
static bool IsPlayerEntity(uint32_t eid)
{
    if (eid == 0)
        return false;
    if (eid == CoSyncNet::GetMyEntityID())
        return true;
    const CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
    return p && p->createType == CoSyncEntityType::Player;
}

// 0.5.1: entity of an actor seen on this machine, by form ID (our player, a
// player proxy or a synced NPC), 0 when unknown.
static uint32_t EntityOfActorFormID(uint32_t formID)
{
    if (formID == 0)
        return 0;
    if (PlayerCharacter* pc = CoSyncGetPlayer())
        if (pc->formID == formID)
            return CoSyncNet::GetMyEntityID();
    if (const CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByActorFormID(formID))
        return p->entityID;
    return 0;
}

// Applies real damage via Papyrus Actor.DamageValue(ActorValue, float).
// Papyrus routes through the engine's full damage pipeline (kill handling,
// essential flags, difficulty scaling is already baked into the sent amount),
// and CallFunctionNoWait is async + safe from the game thread.
static void ApplyDamageValue(Actor* actor, float amount)
{
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
                // 0.5.1 (Bug #8): PVE -- another player's damage is never
                // applied here, whatever the sender's machine thought.
                if (op.killer != 0 && !CoSyncSessionRules::IsPvpEnabled() && IsPlayerEntity(op.killer))
                {
                    LOG_UPDATED("[HealthSync] RX DMG %.1f from player entity=%u refused -- the session is PVE", op.current, op.killer);
                    continue;
                }
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

            // 0.4.1: death is final. An HP that was in flight when the owner's
            // KILL was applied here must not write a corpse's health (the
            // engine kill may still be queued) -- docs/ENEMY_SYNC_REPORT.md P3.
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID) &&
                (CoSyncNpcWorldSync::IsEntityDead(op.entityID) || CoSyncGameAPI::IsDeadOrDying(actor)))
                continue;

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

            if (!CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID))
            {
                // 0.5.1: a PLAYER proxy mirrors its owner's CURRENT health.
                // This used SetBase(target): the local hit that started it all
                // stays on the proxy as a damage modifier, so after every HP
                // packet the proxy read (new base - that old damage) and
                // TickDamageDetection reported the same hit AGAIN -- 2026-10-05
                // client log: one 9.9 shot became "TX DMG 9.9" every ~165 ms
                // (hp 80->70.1, 78.1->68.2, 68.2->58.3 ...) until the host
                // went down; the host's own NPC hits on the client did the
                // same with 2.7. The damage-mode delta sets current = target
                // exactly, so the baseline below equals what is read next.
                float cur = 0.f;
                if (CoSyncAVSafety::SafeGetValue(actor, healthAVI, cur) && std::fabs(target - cur) >= 0.01f)
                    CoSyncAVSafety::SafeForceDamage(actor, healthAVI, target - cur);
            }
            else if (CoSyncGameAPI::PuppetModeEnabled())
            {
                // FalloutTogether/TiltedEvolution-style: apply enemy health the
                // DAMAGE way (ForceCurrent, AVO vtable slot 6 — IDA-verified) so it
                // clamps, updates the health bar, and triggers native death at <=0,
                // unlike SetBase (base value, never kills). delta = target - current.
                float cur = 0.f;
                CoSyncAVSafety::SafeGetValue(actor, healthAVI, cur);
                // 0.4.3: the owner's value -- the one Health write a puppet takes
                // (CoSyncEnemySync refuses every local one).
                CoSyncEnemySync::ScopedHealthWrite authoritative;
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
            // 0.3.3 (Stage 2): KILL must be idempotent. The host re-sends
            // CREATE + KILL for every known corpse every 5 s (so late joiners
            // see corpses), and this used to run the full death again each
            // time: SetLifeState(Dying) + KillDying on an actor that was
            // already Dead (0.3.2 client log: "Killing entity=65537/65538"
            // 440 times in 18 minutes). Skip when the ENGINE already has it
            // dying or dead -- not a bookkeeping flag, so a clone that was
            // freed and respawned alive still gets killed.
            if (CoSyncGameAPI::IsDeadOrDying(actor))
            {
                if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID))
                    CoSyncNpcWorldSync::MarkEntityKilled(op.entityID);
                s_lastSeenHP[op.entityID] = 0.f;
                continue;
            }
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID))
            {
                // 0.4.1: the machine that simulates an enemy decides its death.
                // A KILL for an enemy this machine owns (our own, relayed back,
                // or stale after a hand-off) never kills our real actor.
                const auto& kstates = g_CoSyncPlayerManager.GetStates();
                auto kIt = kstates.find(op.entityID);
                if (kIt != kstates.end() && kIt->second.isLocallyOwned)
                {
                    LOG_UPDATED("[HealthSync] KILL entity=%u ignored -- this machine owns it (its own engine decides its death)",
                        op.entityID);
                    continue;
                }
            }
            LOG_INFO("[HealthSync] Killing entity=%u killer=%u flags=%u", op.entityID, op.killer, op.flags);
            if (CoSyncNpcWorldSync::IsWorldActorEntityID(op.entityID) &&
                CoSyncGameAPI::PuppetModeEnabled())
            {
                // 0.4.1: the engine's own kill core on the puppet -- KillImpl
                // plays ActionDeath through this machine's graph (the death
                // animation), sends the death event with the killer, and gives
                // kill credit when the killer is our player. The old direct
                // SetLifeState(kDying) + KillDying (KillPuppet) is the fallback.
                if (!CoSyncEnemySync::ReplayDeath(op.entityID, actor, op.killer, op.flags))
                {
                    CoSyncEnemySync::ScopedHealthWrite authoritative;
                    CoSyncGameAPI::KillPuppet(actor);
                }
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
            // 0.4.1: with the ModActorValue hook live, every hit this machine
            // deals to an enemy goes out as an EHIT with its attacker
            // (CoSyncEnemySync). Polling the HP drop as well would report it
            // twice, without attribution.
            if (CoSyncEnemySync::IsHitCaptureActive())
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

        const float prev = it->second;
        const float drop = prev - cur;
        it->second = cur;

        // 0.3.4: a player proxy already at 0 HP mirrors an owner who is down
        // (the owner reports 0 while in bleedout). Anything that keeps
        // "hitting" it here is not new damage to that player: the 0.3.3 host
        // log relayed "hp 0.0->-5.3" 272 times in a row to a downed client,
        // several per second, on top of its revive.
        if (!isWorldNpc && prev <= 0.f)
            continue;

        if (drop >= 1.0f)
        {
            uint32_t attackerEid = 0;
            if (!isWorldNpc)
            {
                // 0.5.1: a player's health belongs to that player's machine.
                // The hit seen here is reported; the proxy itself goes back to
                // the value it had (the owner's HP -- for teammates -- or full),
                // so nothing here can ever re-report it.
                CoSyncAVSafety::SafeForceDamage(p->actorRef, healthAVI, drop);
                it->second = prev;
                const uint32_t aggFid = CoSyncCombatAuthority::RecentAggressorFormID(p->actorRef, 1.0);
                attackerEid = EntityOfActorFormID(aggFid);
                if (IsPlayerEntity(attackerEid) && !CoSyncSessionRules::IsPvpEnabled())
                {
                    LOG_UPDATED("[HealthSync] %.1f damage to player entity=%u by player entity=%u not sent -- the session is PVE",
                        drop, p->entityID, attackerEid);
                    continue;
                }
            }
            CoSyncTransport::Send(SerializeDmg(p->entityID, drop, attackerEid));
            LOG_UPDATED("[HealthSync] TX DMG entity=%u amount=%.1f attacker=%u (%s, hp %.1f->%.1f)",
                p->entityID, drop, attackerEid,
                isWorldNpc ? "world NPC, client report" : "player proxy (proxy restored: owner decides)",
                cur + drop, cur);
        }
    }
}

static void ResetCorpses();   // corpse section below

void CoSyncHealthSync::Reset()
{
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        s_pendingOps.clear();
    }
    s_lastSeenHP.clear();
    ResetCorpses();
    LOG_INFO("[HealthSync] Reset — baselines cleared for new session");
}


// ── Authoritative corpse (0.3.1, K4) ───────────────────────────────────────
namespace
{
    struct CorpseTx { double deadSince = 0.0; bool sent = false; };
    std::unordered_map<uint32_t, CorpseTx> s_corpseTx;          // owner side

    struct CorpseRx { uint32_t entityID; NiPoint3 pos; NiPoint3 rot; };
    std::mutex            s_corpseMx;
    std::vector<CorpseRx> s_corpseRx;                            // receiver side

    // How long the owner lets its ragdoll settle before reporting it.
    // UNVERIFIED as "at rest": the engine's ragdoll-at-rest state has not been
    // reverse-engineered, so this is a fixed delay (documented, not a guess
    // dressed up as a signal).
    constexpr double kCorpseSettleSeconds = 4.0;
}

bool CoSyncHealthSync::IsCorpse(const std::string& msg)
{
    return msg.rfind("CORPSE|", 0) == 0;
}

void CoSyncHealthSync::TickOwnedCorpse(uint32_t entityID, Actor* actor, double now)
{
    if (!actor || entityID == 0)
        return;
    CorpseTx& t = s_corpseTx[entityID];
    if (t.sent)
        return;
    if (t.deadSince <= 0.0)
    {
        t.deadSince = now;
        return;
    }
    if (now - t.deadSince < kCorpseSettleSeconds)
        return;
    NiPoint3 pos, rot;
    if (!CoSyncGameAPI::GetActorWorldTransform(actor, pos, rot))
        return;
    t.sent = true;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "CORPSE|%u|%.2f|%.2f|%.2f|%.4f|%.4f|%.4f",
        entityID, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z);
    CoSyncTransport::Send(std::string(buf));
    LOG_UPDATED("[HealthSync] TX corpse entity=%u at (%.0f,%.0f,%.0f)", entityID, pos.x, pos.y, pos.z);
}

void CoSyncHealthSync::OnReceiveCorpse(const std::string& msg)
{
    CorpseRx r{};
    if (std::sscanf(msg.c_str() + 7, "%u|%f|%f|%f|%f|%f|%f", &r.entityID,
            &r.pos.x, &r.pos.y, &r.pos.z, &r.rot.x, &r.rot.y, &r.rot.z) != 7 || r.entityID == 0)
        return;
    if (!std::isfinite(r.pos.x) || !std::isfinite(r.pos.y) || !std::isfinite(r.pos.z))
        return;
    std::lock_guard<std::mutex> lk(s_corpseMx);
    if (s_corpseRx.size() < 256)
        s_corpseRx.push_back(r);
}

void CoSyncHealthSync::FlushPendingCorpses()
{
    std::vector<CorpseRx> todo;
    {
        std::lock_guard<std::mutex> lk(s_corpseMx);
        todo.swap(s_corpseRx);
    }
    for (const CorpseRx& r : todo)
    {
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(r.entityID);
        if (!p || !p->actorRef || !p->hasSpawned)
            continue;
        // SnapActor honours location (never moves it into a world it is not in).
        CoSyncGameAPI::SnapActor(p->actorRef, r.pos, r.rot);
        LOG_UPDATED("[HealthSync] corpse entity=%u moved to its owner's corpse (%.0f,%.0f,%.0f)",
            r.entityID, r.pos.x, r.pos.y, r.pos.z);
    }
}

static void ResetCorpses()
{
    s_corpseTx.clear();
    std::lock_guard<std::mutex> lk(s_corpseMx);
    s_corpseRx.clear();
}
