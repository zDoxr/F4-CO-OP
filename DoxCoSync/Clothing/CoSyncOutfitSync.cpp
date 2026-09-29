#include "CoSyncOutfitSync.h"
#include "CoSyncVMFormArg.h"
#include "CoSyncEquipHook.h"
#include "CoSyncGameAPI.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncRequirementsLog.h"
#include "GameForms.h"
#include "GameReferences.h"
#include "GameFormComponents.h"
#include "GameObjects.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <mutex>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

extern RelocAddr<_LookupFormByID>  LookupFormByID;
extern CoSyncEntityRegistry        g_CoSyncEntities;

namespace
{
    struct OutfitOp
    {
        uint32_t              entityID = 0;
        std::vector<uint32_t> armorFormIDs;
        double                firstSeen = 0.0;   // for spawn-wait retry TTL
    };

    std::mutex            s_mutex;
    std::vector<OutfitOp> s_pending;


    // Round 143: this set now means "CONFIRMED equipped" (verified present +
    // equipped on the proxy), not "we tried to equip it". Previously it was
    // set to `received` unconditionally right after queuing the AddItem call
    // — before anything confirmed the item actually landed — so a silent
    // AddItem/EquipItem failure (2026-07-06 live log: armor 0x000B2D8D and
    // weapon 0x0014831B both AddItem'd/EquipItem'd but verified present=0
    // 2s later) got marked "applied" anyway and NEVER retried, because the
    // "already applied, skip" check gates on this exact set. Now only the
    // verification pass (below) adds to it, on success. A piece still
    // in-flight (AddItem sent, not yet verified) is tracked in
    // s_inFlightOutfits so it isn't re-sent every packet while its verify is
    // still pending.
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> s_appliedOutfits;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> s_inFlightOutfits;

    // Phase-2 equips: AddItem is async through the VM/container queue, so an
    // EquipItem issued in the same tick can run before the item exists in the
    // proxy's inventory and silently no-op (live 2026-07-05 test: "Equipped"
    // was logged, nothing appeared on the proxy). Equips are therefore
    // scheduled ~0.75s after their AddItem.
    struct DelayedEquip
    {
        uint32_t entityID = 0;
        uint32_t formID   = 0;
        double   applyAt  = 0.0;
        bool     retried  = false;
    };
    std::vector<DelayedEquip> s_delayedEquips;

    // Round 141: post-equip ground-truth verification (see flush).
    struct PendingVerify
    {
        uint32_t entityID = 0;
        uint32_t formID   = 0;
        double   at       = 0.0;
    };
    std::vector<PendingVerify>              s_pendingVerifies;

    double OutfitNowSeconds()
    {
        LOG_REQUIREMENT("CoSyncOutfitSync::OutfitNowSeconds", "Doesn't touch the game -- just reads the system's high-precision clock.");
        LARGE_INTEGER freq, cnt;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&cnt);
        return static_cast<double>(cnt.QuadPart) / static_cast<double>(freq.QuadPart);
    }
}

static std::vector<uint32_t> GetEquippedArmorFormIDs(Actor* actor)
{
    LOG_REQUIREMENT("CoSyncOutfitSync::GetEquippedArmorFormIDs", "Safely handles an empty actor. A real actor needs a valid inventory list for this to safely walk through their items.");
    std::vector<uint32_t> result;
    if (!actor) return result;

    BGSInventoryList* invList = actor->inventoryList;
    if (!invList) return result;

    for (UInt32 i = 0; i < invList->items.count; ++i)
    {
        const BGSInventoryItem& item = invList->items[i];
        if (!item.form || item.form->formType != kFormType_ARMO)
            continue;


        if (!item.stack) continue;
        bool equipped = false;
        item.stack->Visit([&equipped](BGSInventoryItem::Stack* s) {

            if (s->flags & BGSInventoryItem::Stack::kFlagEquipped)
            {
                equipped = true;
                return false;
            }
            return true;
        });

        if (equipped)
            result.push_back(item.form->formID);
    }
    return result;
}

// Is formID in the proxy's inventory, and equipped? Liveness-checked and
// SEH-guarded: a proxy rebuilt on a cell change leaves a stale actor pointer
// behind in the registry, and walking its freed inventory list crashed the
// host (2026-09-26, FlushPendingOutfitOps+0x1B45 reading a garbage list).
__declspec(noinline) static bool SafeFindInProxyInventory(Actor* actor, uint32_t formID,
                                                          bool& present, bool& equipped, UInt32& invCount)
{
    present = false; equipped = false; invCount = 0;
    if (!actor || !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(actor)))
        return false;
    __try
    {
        BGSInventoryList* inv = actor->inventoryList;
        if (!inv)
            return true;
        invCount = inv->items.count;
        for (UInt32 k = 0; k < inv->items.count; ++k)
        {
            const BGSInventoryItem& item = inv->items[k];
            if (!item.form || item.form->formID != formID)
                continue;
            present = true;
            for (BGSInventoryItem::Stack* s = item.stack; s; s = s->next)
                if (s->flags & BGSInventoryItem::Stack::kFlagEquipped) { equipped = true; break; }
            break;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        present = false; equipped = false; invCount = 0;
        return false;
    }
}

// 0.3.3 (Stage 3): every ARMO the ENGINE currently has equipped on a proxy.
// Same liveness + SEH rules as SafeFindInProxyInventory; fixed-size output
// because a __try body cannot own objects that need unwinding.
static constexpr uint32_t kMaxWornArmor = 64;
__declspec(noinline) static uint32_t SafeListEquippedArmor(Actor* actor, uint32_t out[kMaxWornArmor])
{
    if (!actor || !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(actor)))
        return 0;
    uint32_t n = 0;
    __try
    {
        BGSInventoryList* inv = actor->inventoryList;
        if (!inv)
            return 0;
        for (UInt32 k = 0; k < inv->items.count && n < kMaxWornArmor; ++k)
        {
            const BGSInventoryItem& item = inv->items[k];
            if (!item.form || item.form->formType != kFormType_ARMO)
                continue;
            for (BGSInventoryItem::Stack* st = item.stack; st; st = st->next)
                if (st->flags & BGSInventoryItem::Stack::kFlagEquipped)
                {
                    out[n++] = item.form->formID;
                    break;
                }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Proxies whose last equip/unequip happened while they had no middle-high
// process data, so the engine dropped its own model-update request (see
// CoSyncGameAPI::RequestActorModelUpdate). Re-issued once it can stick.
static std::unordered_set<uint32_t> s_modelUpdatePending;

bool CoSyncOutfitSync::IsOutfit(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncOutfitSync::IsOutfit", "Doesn't touch the game -- just needs a valid message string.");
    return msg.rfind("OUTFIT|", 0) == 0;
}

static double s_lastOutfitCheck = 0.0;
static std::unordered_set<uint32_t> s_lastSentArmor;

void CoSyncOutfitSync::TickLocalOutfit(uint32_t myEntityID, Actor* player)
{
    LOG_REQUIREMENT("CoSyncOutfitSync::TickLocalOutfit", "Needs a valid player and an active connection before it works out and sends what changed about the outfit.");
    if (!player || !CoSyncNet::IsConnected()) return;

    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    const double now = static_cast<double>(cnt.QuadPart) / static_cast<double>(freq.QuadPart);

    if (now - s_lastOutfitCheck < 0.1) return;
    s_lastOutfitCheck = now;

    const std::vector<uint32_t> current = GetEquippedArmorFormIDs(player);
    const std::unordered_set<uint32_t> currentSet(current.begin(), current.end());

    // Change-only sending meant a peer who joined AFTER our last equip never
    // received our outfit at all (their proxy of us stayed in the default
    // body). Re-broadcast periodically as well — receivers dedup against
    // their per-entity applied-set, so an unchanged outfit is a no-op there.
    static double s_lastForceSend = 0.0;
    static constexpr double kOutfitRebroadcastInterval = 10.0;
    const bool forceSend = (now - s_lastForceSend) >= kOutfitRebroadcastInterval;

    if (!forceSend && currentSet == s_lastSentArmor) return;
    s_lastSentArmor = currentSet;
    s_lastForceSend = now;


    std::ostringstream ss;
    ss << "OUTFIT|" << myEntityID;
    for (uint32_t fid : current)
        ss << "|" << fid;

    CoSyncTransport::Send(ss.str());

    LOG_UPDATED("[OutfitSync TX] Outfit changed: entity=%u pieces=%u",
        myEntityID, (unsigned)current.size());
}

void CoSyncOutfitSync::OnReceiveOutfit(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncOutfitSync::OnReceiveOutfit", "Needs the message to be at least as long as its \"OUTFIT|\" prefix, and our own player ID to be valid.");

    const char* p = msg.c_str() + 7;

    std::stringstream ss(p);
    std::string tok;

    if (!std::getline(ss, tok, '|') || tok.empty())
    {
        LOG_UPDATED_WARN("[OutfitSync RX] Malformed OUTFIT packet");
        return;
    }

    OutfitOp op;
    op.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));


    if (op.entityID != 0 && op.entityID == CoSyncNet::GetMyEntityID())
        return;

    while (std::getline(ss, tok, '|'))
    {
        if (tok.empty()) continue;
        uint32_t fid = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
        if (fid != 0)
            op.armorFormIDs.push_back(fid);
    }

    LOG_UPDATED("[OutfitSync RX] OUTFIT queued: entity=%u pieces=%u",
        op.entityID, (unsigned)op.armorFormIDs.size());

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back(std::move(op));
}

// UPDATE 2026-06-20 (round 41 audit): same unguarded peer-controlled FormID
// -> LookupFormByID crash pattern found and fixed elsewhere this session
// (DoorHook/LockHook/QuestSync/PowerArmorSync). armorFormIDs here come
// straight off the wire (op.armorFormIDs). SEH-wrapped, noinline (load-
// bearing under /GL+/LTCG).
__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    LOG_REQUIREMENT("CoSyncOutfitSync::SafeLookupForm", "Works safely with any ID value, even a bad one sent by another player, because the lookup runs inside a safety guard.");
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void CoSyncOutfitSync::FlushPendingOutfitOps()
{
    LOG_REQUIREMENT("CoSyncOutfitSync::FlushPendingOutfitOps", "Needs the entity registry to be valid so it can find each real actor, and each of those actors to have already spawned.");
    static const BSFixedString kUnequipItem("UnequipItem");
    static const BSFixedString kEquipItem("EquipItem");
    std::vector<OutfitOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        ops.swap(s_pending);
    }

    // Round 142: this used to `return` here when there were no NEW incoming
    // OUTFIT packets this tick — which also skipped the delayed-equip and
    // verification passes below (they used to live further down, unreached).
    // Outfit packets only arrive every ~10s (periodic re-broadcast) or on
    // change, so the 0.75s equip delay and 2s verify delay were only ever
    // serviced by ACCIDENT, whenever the next unrelated packet happened to
    // land after the deadline — up to ~10s of skew on real hardware, though
    // still within the observed working case. Now: only the "process new
    // ops" loop below is skipped when ops is empty; delayed-equip/verify
    // always run every call (this function is on every net tick).

    const double now = OutfitNowSeconds();
    static constexpr double kSpawnWaitTTL     = 60.0;   // retry window while proxy spawns
    static constexpr double kEquipDelaySec    = 0.75;   // AddItem -> EquipItem gap
    static constexpr double kSpawnSettleDelay = 2.0;    // wait out CoSyncSpawnTasks's post-spawn RefreshActor3D

    std::vector<OutfitOp> requeue;

    for (OutfitOp& op : ops)
    {
        Actor* actor = g_CoSyncEntities.GetActor(op.entityID);

        // 2026-08-02: a null check alone was not enough. An exterior->interior
        // transition frees every dynamic proxy actor, but the entity registry
        // keeps handing out the stale pointer, so this loop walked a freed actor
        // straight into DirectInjectInventoryItem and crashed the client (READ AV
        // fault 0xFFFFFFFFFFFFFFFF). Treat a dead actor exactly like a not-yet-
        // spawned one -- requeue on the same TTL and drop the cached outfit state
        // so the rebuilt proxy re-applies from scratch, which is precisely what
        // ClearAppliedOutfits already does for the reload case.
        if (actor && !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            // Freed/unloaded proxy (e.g. a remote player persistently out of
            // range in a far cell). Clear the cached outfit state and DROP this
            // op — do NOT fall through to the not-yet-spawned requeue below.
            // That path re-queues the op every tick, so a single out-of-range
            // host-player proxy spammed ~42K "freed — requeueing" + 42K
            // ClearAppliedOutfits lines in an 8-min populated-area test (build
            // 224) and burned per-tick CPU. The host re-broadcasts outfits ~every
            // 10s, so this re-applies cleanly once the proxy respawns in range.
            LOG_UPDATED_WARN("[OutfitSync FLUSH] entity=%u actor freed (unloaded) — dropping until respawn (host re-broadcast re-applies)",
                op.entityID);
            ClearAppliedOutfits(op.entityID);
            continue;
        }

        if (!actor)
        {
            // Live 2026-07-05 test: outfits arriving while the peer's proxy was
            // still spawning (or while the peer's save loaded) were dropped
            // forever here. Re-queue with a TTL instead.
            if (op.firstSeen == 0.0)
                op.firstSeen = now;

            if (now - op.firstSeen < kSpawnWaitTTL)
            {
                requeue.push_back(std::move(op));
            }
            else
            {
                LOG_UPDATED_WARN("[OutfitSync FLUSH] entity=%u never spawned within %.0fs — dropping outfit",
                    op.entityID, kSpawnWaitTTL);
            }
            continue;
        }

        // Round 143: CoSyncSpawnTasks queues its own Disable()+Enable() body
        // refresh moments after spawn. Live 2026-07-06 log: that refresh
        // fired at .870, and the FIRST outfit AddItem for the very same
        // actor queued 5-6ms later at .875 — one of the two armor pieces
        // (and the weapon, same window) then verified as never having
        // landed. Both are async CallFunctionNoWait calls on the SAME
        // actor's VM queue; queuing an AddItem while the spawn refresh is
        // still settling appears to be where it gets dropped. Give every
        // proxy a couple of seconds after spawn before its FIRST outfit
        // application, same treatment as CoSyncPlayer.cpp's
        // kAnimGraphSettleDelay for anim-graph access.
        CoSyncPlayer* proxy = g_CoSyncPlayerManager.GetPlayerByEntityID(op.entityID);
        if (proxy && (now - proxy->m_spawnTime) < kSpawnSettleDelay &&
            s_appliedOutfits[op.entityID].empty() && s_inFlightOutfits[op.entityID].empty())
        {
            requeue.push_back(std::move(op));
            continue;
        }

        const std::unordered_set<uint32_t> received(op.armorFormIDs.begin(), op.armorFormIDs.end());
        auto& applied  = s_appliedOutfits[op.entityID];
        auto& inFlight = s_inFlightOutfits[op.entityID];

        // 0.3.3 (Stage 3): reconcile against what the ENGINE has equipped on
        // the proxy, not against our own record of what we once equipped.
        //
        // The old test ("applied.count(fid) -> skip") trusted bookkeeping: once
        // a piece had verified as equipped it was never looked at again, so if
        // anything later took it off the proxy (a slot conflict, a 3D reload,
        // a proxy rebuild) the 10 s re-broadcast was a no-op and the piece
        // stayed missing until the real player unequipped and re-equipped it
        // -- the reported "only shows after re-equipping". Every OUTFIT
        // (including the periodic one) now converges the proxy's worn set to
        // the sender's.
        for (uint32_t fid : received)
        {
            if (inFlight.count(fid)) continue;     // mid equip/verify
            {
                bool present = false, equipped = false;
                UInt32 invCount = 0;
                if (SafeFindInProxyInventory(actor, fid, present, equipped, invCount) && equipped)
                {
                    applied.insert(fid);
                    continue;                      // already worn: nothing to do
                }
                applied.erase(fid);                // our record was stale
            }

            TESForm* form = SafeLookupForm(fid);
            if (!form) { LOG_UPDATED_WARN("[OutfitSync FLUSH] armorFormID=0x%08X not found", fid); continue; }


            // Round 145: bypass Papyrus AddItem (silently fails on the proxy —
            // see CoSyncGameAPI::DirectInjectInventoryItem for root cause).
            // Injection is synchronous; equip fires in the same tick.
            if (CoSyncGameAPI::DirectInjectInventoryItem(actor, form))
            {
                inFlight.insert(fid);
                s_delayedEquips.push_back({ op.entityID, fid, now + 0.05 });
                LOG_UPDATED("[OutfitSync APPLY] DirectInject entity=%u armor=0x%08X (equip in 50ms)",
                    op.entityID, fid);
            }
            else
            {
                LOG_UPDATED_WARN("[OutfitSync APPLY] DirectInject FAILED entity=%u armor=0x%08X",
                    op.entityID, fid);
            }
        }


        // Take off anything the proxy wears that the sender does not
        // (including pieces the engine put on by itself, e.g. the proxy base's
        // default outfit after a 3D reload). In-flight pieces are left to
        // their own verify pass.
        // Power Armor sync puts the PA pieces on this same actor; leave a
        // proxy in PA to it (Stage 5).
        if (!(proxy && proxy->m_isInPowerArmor))
        {
            uint32_t worn[kMaxWornArmor];
            const uint32_t wornCount = SafeListEquippedArmor(actor, worn);
            for (uint32_t w = 0; w < wornCount; ++w)
            {
                const uint32_t fid = worn[w];
                if (received.count(fid) || inFlight.count(fid)) continue;

                TESForm* form = SafeLookupForm(fid);
                if (!form) continue;

                const bool directOk = CoSyncEquipHook::UnequipItemDirect(actor, form);
                if (!directOk)
                {
                    VMArray<VMVariable> unArgs;
                    VMVariable vF; CoSyncVM::SetFormArg(vF, form); unArgs.Push(&vF);
                    bool prev = false;
                    VMVariable vP; vP.Set(&prev); unArgs.Push(&vP);
                    bool sil = true;
                    VMVariable vS; vS.Set(&sil);  unArgs.Push(&vS);
                    CallFunctionNoWait(actor, kUnequipItem, unArgs);
                }
                if (!CoSyncGameAPI::HasMiddleHighProcess(actor))
                    s_modelUpdatePending.insert(op.entityID);

                LOG_UPDATED("[OutfitSync APPLY] Unequipped entity=%u armor=0x%08X", op.entityID, fid);
            }
        }

        // Only DROP confirmed-applied pieces that are no longer in the
        // received set — items still in-flight/unverified are left alone
        // (their own verify pass decides their fate).
        for (auto it = applied.begin(); it != applied.end(); )
            it = received.count(*it) ? std::next(it) : applied.erase(it);

        LOG_UPDATED("[OutfitSync FLUSH] entity=%u: %u piece(s) requested, %u confirmed, %u in-flight",
            op.entityID, (unsigned)received.size(), (unsigned)applied.size(), (unsigned)inFlight.size());
    }

    if (!requeue.empty())
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        for (OutfitOp& op : requeue)
            s_pending.push_back(std::move(op));
    }

    // Phase-2 pass: fire due equips.
    for (size_t i = 0; i < s_delayedEquips.size(); )
    {
        DelayedEquip& de = s_delayedEquips[i];
        if (now < de.applyAt)
        {
            ++i;
            continue;
        }

        // Capture values before any push_back (avoids dangling ref on realloc).
        const uint32_t deEntityID = de.entityID;
        const uint32_t deFormID   = de.formID;
        const bool     deRetried  = de.retried;

        Actor*   actor = g_CoSyncEntities.GetActor(deEntityID);
        TESForm* form  = actor ? SafeLookupForm(deFormID) : nullptr;

        // Inventory pre-check: EquipItemDirect (and Papyrus EquipItem)
        // silently no-op when the item isn't in the proxy's container.
        // A stale proxy (rebuilt on a cell change) fails the check and is
        // skipped; the next OUTFIT pass targets the new one.
        bool itemInInventory = false, eqUnused = false;
        UInt32 cntUnused = 0;
        const bool live = actor && form &&
            SafeFindInProxyInventory(actor, deFormID, itemInInventory, eqUnused, cntUnused);

        if (live)
        {

            if (!itemInInventory)
            {
                // Phase-1 DirectInject should have landed the item already;
                // re-inject if somehow it didn't (idempotent, no-op if present).
                CoSyncGameAPI::DirectInjectInventoryItem(actor, form);
                LOG_UPDATED_WARN("[OutfitSync APPLY] armor 0x%08X not in inventory at phase-2 entity=%u -- re-injecting",
                    deFormID, deEntityID);
            }

            {
                // Equip — item is now guaranteed in inventory.
                const bool directOk = CoSyncEquipHook::EquipItemDirect(actor, form);
                if (!directOk)
                {
                    VMArray<VMVariable> eqArgs;
                    VMVariable vF; CoSyncVM::SetFormArg(vF, form); eqArgs.Push(&vF);
                    bool prev = true;
                    VMVariable vP; vP.Set(&prev); eqArgs.Push(&vP);
                    bool sil = true;
                    VMVariable vS; vS.Set(&sil); eqArgs.Push(&vS);
                    CallFunctionNoWait(actor, kEquipItem, eqArgs);
                }
                LOG_UPDATED("[OutfitSync APPLY] EquipItem %s entity=%u armor=0x%08X (inInventory=%d)",
                    directOk ? "DIRECT" : "PAPYRUS", deEntityID, deFormID,
                    itemInInventory ? 1 : 0);

                if (!CoSyncGameAPI::HasMiddleHighProcess(actor))
                    s_modelUpdatePending.insert(deEntityID);

                s_pendingVerifies.push_back({ deEntityID, deFormID, now + 2.0 });
            }
        }

        // Swap-and-pop: works correctly even after a push_back above
        // (the new entry ends up at position i; the old entry is discarded).
        s_delayedEquips[i] = s_delayedEquips.back();
        s_delayedEquips.pop_back();
        // Don't advance i.
    }

    // Verification pass + one 3D refresh per entity batch.
    for (size_t i = 0; i < s_pendingVerifies.size(); )
    {
        PendingVerify& pv = s_pendingVerifies[i];
        if (now < pv.at)
        {
            ++i;
            continue;
        }

        Actor* actor = g_CoSyncEntities.GetActor(pv.entityID);
        bool present = false, equipped = false;
        UInt32 invCount = 0;
        // Stale proxy (rebuilt) -> nothing to verify on it; the entry is dropped below.
        if (actor && SafeFindInProxyInventory(actor, pv.formID, present, equipped, invCount))
        {

            LOG_UPDATED("[OutfitSync VERIFY] entity=%u armor=0x%08X present=%d equipped=%d",
                pv.entityID, pv.formID, present ? 1 : 0, equipped ? 1 : 0);

            // Round 144: same diagnostic as WEAPON VERIFY MISS — a
            // persistent, repeatable (not one-off) failure for one specific
            // armor piece while another succeeds every time rules out pure
            // timing; dump ground truth so the next log settles whether this
            // is a leveled-list formID (AddItem(LVLI) rolls a different
            // concrete item, so an exact-formID check never matches even
            // though something real landed) or a resolution mismatch.
            if (!present)
            {
                TESForm* resolvedForm = SafeLookupForm(pv.formID);
                LOG_UPDATED_WARN("[OutfitSync VERIFY MISS] entity=%u formID=0x%08X resolvedFormType=%d invCount=%u",
                    pv.entityID, pv.formID,
                    resolvedForm ? (int)resolvedForm->formType : -1,
                    (unsigned)invCount);
            }

            // Round 143: this is the ONLY place `applied` (confirmed-equipped)
            // gets written now. Success -> mark applied, so the "already
            // done" skip in the ops loop above holds. Failure -> do NOT mark
            // applied; only drop it from in-flight, so the next OUTFIT
            // packet (periodic re-broadcast, ~10s) retries the AddItem from
            // scratch instead of silently giving up forever (live
            // 2026-07-06 bug: armor 0x000B2D8D verified present=0 but had
            // already been marked applied, so it was NEVER retried).
            s_inFlightOutfits[pv.entityID].erase(pv.formID);
            if (equipped)
                s_appliedOutfits[pv.entityID].insert(pv.formID);
            else
                LOG_UPDATED_WARN("[OutfitSync VERIFY] entity=%u armor=0x%08X FAILED verification -- will retry on next outfit packet",
                    pv.entityID, pv.formID);

            // 0.3.3 (Stage 3): the Disable+Enable "biped rebuild" that used to
            // follow here is gone. The engine rebuilds the worn 3D by itself
            // after an equip (AddWornItem -> update3DModel -> Actor::Update,
            // see CoSyncGameAPI::RequestActorModelUpdate), and the rebuild had
            // proven side effects: the proxy blinked out and back in, and
            // Enable() dropped the equipped weapon, which then had to be
            // re-equipped a second later (the same self-inflicted cycle the
            // weapon verify's own rebuild was removed for on 2026-08-26). The
            // one case the engine misses (no middle-high process data at equip
            // time) is handled by s_modelUpdatePending below.
        }
        else
        {
            // Actor vanished mid-verify (despawn race) — drop from in-flight
            // so a stale entry doesn't block retry forever if it respawns.
            s_inFlightOutfits[pv.entityID].erase(pv.formID);
        }

        s_pendingVerifies[i] = s_pendingVerifies.back();
        s_pendingVerifies.pop_back();
    }

    // Re-issue the engine's model-update request for proxies whose equip
    // request was dropped, as soon as it can stick (middle-high process data
    // and 3D present). Forget entities that are gone.
    for (auto it = s_modelUpdatePending.begin(); it != s_modelUpdatePending.end(); )
    {
        Actor* actor = g_CoSyncEntities.GetActor(*it);
        if (!actor || !CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(actor)))
        {
            it = s_modelUpdatePending.erase(it);
            continue;
        }
        if (CoSyncGameAPI::RequestActorModelUpdate(actor))
        {
            LOG_UPDATED("[OutfitSync] entity=%u worn 3D update re-requested (equip landed without middle-high process)", *it);
            it = s_modelUpdatePending.erase(it);
            continue;
        }
        ++it;
    }
}

void CoSyncOutfitSync::ClearAppliedOutfits(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncOutfitSync::ClearAppliedOutfits", "Doesn't touch the game -- just clears this player's cached outfit state. Safe to call even if we never tracked them.");
    // Only touch/log when there is actually something cached — repeated calls on
    // an already-cleared entity (e.g. a persistently-freed proxy) would otherwise
    // spam. Use find() so we don't insert empty entries via operator[].
    auto ai = s_appliedOutfits.find(entityID);
    auto ii = s_inFlightOutfits.find(entityID);
    const bool hadAny = (ai != s_appliedOutfits.end() && !ai->second.empty()) ||
                        (ii != s_inFlightOutfits.end() && !ii->second.empty());
    if (ai != s_appliedOutfits.end()) ai->second.clear();
    if (ii != s_inFlightOutfits.end()) ii->second.clear();
    if (!hadAny)
        return;
    LOG_INFO("[OutfitSync] ClearAppliedOutfits: entity=%u will re-apply its outfit from scratch", entityID);
}

void CoSyncOutfitSync::Reset()
{
    LOG_REQUIREMENT("CoSyncOutfitSync::Reset", "Doesn't touch the game -- just clears out internal caches and queues.");
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_pending.clear();
    }
    s_appliedOutfits.clear();
    s_inFlightOutfits.clear();
    s_lastSentArmor.clear();
    s_delayedEquips.clear();
    s_pendingVerifies.clear();
    LOG_INFO("[OutfitSync] Reset — caches cleared for new session");
}
