
#include "CoSyncNet.h"
#include "CoSyncCombatAuthority.h"
#include "CoSyncWorldStateSync.h"
#include "CoSyncSharedDialogue.h"
#include "Version.h"

#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncSteamID.h"
#include "CoSyncExplosionSync.h"
#include "CoSyncThrowableSync.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncWorld.h"
#include "CoSyncLocalPlayer.h"

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "Packets_EntityDestroy.h"
#include "EntitySerialization.h"

#include "CoSyncWorldSync.h"
#include "CoSyncWorkshopSync.h"
#include "CoSyncTimeSync.h"
#include "CoSyncHealthSync.h"
#include "CoSyncReviveSync.h"
#include "CoSyncInventorySync.h"
#include "CoSyncMapSync.h"
#include "CoSyncQuestSync.h"
#include "CoSyncPowerArmorSync.h"
#include "CoSyncConsumableSync.h"
#include "CoSyncSpecialSync.h"
#include "CoSyncVatsSync.h"
#include "CoSyncOutfitSync.h"
#include "CoSyncAnimationUtils.h"
#include "CoSyncAnimEventHook.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncItemDropSync.h"
#include "CoSyncDialogSync.h"
#include "CoSyncOverlay.h"
#include "CoSyncLootSync.h"
#include "CoSyncCompanionSync.h"
#include "CoSyncEquipHook.h"
#include "GNS_Session.h"
#include "CoSyncSteamAPI.h"
#include "CoSyncSessionConfig.h"
#include "CoSyncWorldTransfer.h"
#include "CoSyncProfiles.h"
#include "CoSyncProxyLooks.h"
#include "CoSyncCompanions.h"
#include "CoSyncItemMods.h"
#include "CoSyncFactions.h"
#include "CoSyncChat.h"
#include "CoSyncVoice.h"
#include "CoSyncWorkshopAuthority.h"

#include <algorithm>
#include "../Animations/CoSyncPlayerAnimation.h"
#include "CoSyncDoorHook.h"
#include "CoSyncLockHook.h"
#include "CoSyncEngineResumeHook.h"
#include "CoSyncPickupHook.h"
#include "CoSyncContainerSync.h"
#include "CoSyncSceneSync.h"
#include "CoSyncWorkshopScrapHook.h"
#include "CoSyncAnimGraphHandleGuard.h"
#include "CoSyncMovementHandleGuard.h"
#include "CoSyncLocomotionCalcGuard.h"
#include "CoSyncSharedHandleLeafGuard.h"
#include "CoSyncActorLocomotionGuard.h"
#include "CoSyncActorProcessGuard.h"
#include "CoSyncMusicChangerCellTeardownGuard.h"
#include "CoSyncActorAudioScaleGuard.h"
#include "CoSyncModSync.h"
#include "CoSyncFormIdMap.h"
#include "CoSyncProxyWeaponAuthority.h"
#include "CoSyncModEventBus.h"
#include "CoSyncModEventHook.h"
#include "CoSyncQuestHook.h"
#include "CoSyncQuestObjectiveHook.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPapyrusHelper.h"

#include "GameReferences.h"
#include "GameObjects.h"

#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <functional>
#include <atomic>

extern PlayerCharacter* CoSyncGetPlayer();

// Safely reads the local player's current world position for use in CREATE
// packets. Guarded so a pre-world-ready call (player->pos not yet valid)
// never crashes — returns (0,0,0) as a fallback instead.
__declspec(noinline) static NiPoint3 CoSyncNet_GetLocalPlayerPos()
{
    __try
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (pc) return NiPoint3(pc->pos.x, pc->pos.y, pc->pos.z);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return NiPoint3(0.f, 0.f, 0.f);
}

namespace
{
    // UPDATE 2026-06-18 (round 23 audit): s_isHost/s_connected were plain,
    // non-atomic bools despite being written from OnGNSConnected() (which
    // runs on the GNS background thread) and read continuously from the main
    // game thread via IsHost()/IsConnected() -- a real, if previously
    // low-probability, data race. Flagged repeatedly in earlier rounds but
    // never fixed; made atomic now since it's a one-line-per-flag change with
    // no downside, instead of continuing to carry it as a known, lingering
    // risk. All existing read/write call sites are plain assignment/boolean
    // reads, so std::atomic<bool> is a drop-in replacement here.
    bool s_initialized = false;
    std::atomic<bool> s_isHost{ false };
    std::atomic<bool> s_connected{ false };
    bool s_helloSent = false;

    bool s_pendingInit = false;
    bool s_pendingHostFlag = false;

    std::string s_myName = "Player";
    uint64_t    s_mySteamID = 0;
    uint32_t    s_myEntityID = 0;

    bool s_hostCreatePublished = false;

    // Round 140: build-version handshake. Live 2026-07-05 14:39 session ran
    // host round-5 against a stale P2 DLL (companion CREATE arrived with the
    // pre-round-4 proxy base) and "a lot of stuff did not work" — mixed
    // versions must be loudly detectable from either side's log.
    // Network build number = COSYNC_VERSION (Core/Version.h, e.g. 0.2.4
    // -> 204). It used to be a separate hand-kept number (229, then 230 for the
    // BONES sequence field); one source now, so a version bump always makes
    // mismatched peers refuse each other through VER/WORLDREQ.
    constexpr uint32_t kCoSyncBuildNumber = COSYNC_VERSION;
    bool s_verSent = false;


    struct RemotePeer
    {
        uint64_t steamID = 0;
        std::string username;
        uint32_t entityID = 0;
        double lastSeen = 0.0;
        HSteamNetConnection conn = 0;   // host: the client's connection
    };

    // Client: entityID of the host (from the host's HELLO). 0 = unknown.
    uint32_t s_hostEntityID = 0;

    std::unordered_map<uint64_t, RemotePeer> s_peers;
    std::mutex s_peerMutex;
}

static uint64_t HashName(const std::string& s)
{
    return static_cast<uint64_t>(std::hash<std::string>{}(s));
}

static uint32_t EntityIDFromSteamID(uint64_t sid)
{
    return static_cast<uint32_t>(sid & 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// P5/N7 (2026-09-26): sender authorization on the host.
// The host used to apply and relay entity messages from any client about any
// entity, so one client could move, delete or re-own every other player's
// proxy and every NPC. The sender is now the identity bound to its
// connection (first HELLO, GNS_Session::GetPeerSteamID) -- never an ID
// written inside the message -- and it may only speak for:
//   * its own player entity, or
//   * a world NPC the host's own records say it owns
//     (CoSyncEntityState::lastCreate.ownerEntityID, set from NPCSEEN/COMPOWN).
// Checked by the host only; clients trust the host.
// ---------------------------------------------------------------------------
// Connection -> the player entity it speaks as. Bound by the first message
// that names a player (HELLO or a SelfStateType message): a joining client
// sends VER/SPEC/IMODS/LOOKS ~14 s BEFORE its HELLO (it loads the host world
// and builds its character first -- host log 16236, 20:38:29 vs 20:38:43), so
// the HELLO-only binding in GNS_Session is not enough on its own. Same trust
// level as HELLO (a claim, unauthenticated until Steam sockets); what it
// prevents is switching identity afterwards. Game-thread only (OnReceive).
static std::unordered_map<HSteamNetConnection, uint32_t> s_connSenderEid;

static uint32_t HostSenderEntityID(HSteamNetConnection conn)
{
    if (conn == k_HSteamNetConnection_Invalid)
        return 0;
    auto it = s_connSenderEid.find(conn);
    if (it != s_connSenderEid.end())
        return it->second;
    const uint32_t eid = EntityIDFromSteamID(GNS_Session::Get().GetPeerSteamID(conn));
    if (eid != 0)
        s_connSenderEid[conn] = eid;
    return eid;
}

// First player-naming message on an unbound connection binds it. World NPC IDs
// never bind (a client only owns NPCs through the host's own records).
static uint32_t HostBindSender(HSteamNetConnection conn, uint32_t claimed)
{
    if (claimed == 0 || CoSyncNpcWorldSync::IsWorldActorEntityID(claimed))
        return 0;
    s_connSenderEid[conn] = claimed;
    LOG_INFO("[CoSyncNet] conn=%u speaks as entity=%u", conn, claimed);
    return claimed;
}

static bool HostSenderOwns(uint32_t senderEid, uint32_t entityID)
{
    if (senderEid == 0 || entityID == 0)
        return false;
    if (entityID == senderEid)
        return true;
    if (!CoSyncNpcWorldSync::IsWorldActorEntityID(entityID))
        return false;
    const auto& states = g_CoSyncPlayerManager.GetStates();
    auto it = states.find(entityID);
    return it != states.end() && it->second.hasCreate &&
           it->second.lastCreate.ownerEntityID == senderEid;
}

static void LogUnauthorized(const char* what, HSteamNetConnection conn, uint32_t senderEid, uint32_t entityID)
{
    static uint32_t s_dropped = 0;
    if ((s_dropped++ % 100) == 0)
        LOG_WARN("[CoSyncNet] Dropped %s from conn=%u (sender entity=%u) about entity=%u it does not own (%u dropped so far)",
            what, conn, senderEid, entityID, s_dropped);
}

// Message types whose FIRST field is the entity doing the thing (verified
// against every send site, 2026-09-26): a player's own vitals, gear, anim
// events, consumables, power armor, throwables, VATS, SPECIAL, looks and
// version. Returns the type name, or nullptr if msg is not one of them.
static const char* SelfStateType(const std::string& msg)
{
    static const char* const kTypes[] = {
        "HP", "INV", "OUTFIT", "EQUIP", "IMODS", "LOOKS", "DOWN",
        "PA", "PAI", "PAMODS", "SPEC", "AE", "AEV", "SW", "PB",
        "CONS", "SPELL", "THRP", "THRM", "THRX", "VATS_ENTER", "VATS_EXIT", "VER", "WS", "CORPSE",
        "SHOT",   // 0.3.5: the shooter must be the sender's own player or an NPC it owns
        "DLGP", "DLGQ",   // 0.3.5: a dialogue choice/request names the sender's own player
        "WM", "WD",   // U7: workshop-mode / legacy scrap notices name their sender too
    };
    const size_t bar = msg.find('|');
    if (bar == std::string::npos)
        return nullptr;
    for (const char* t : kTypes)
        if (msg.compare(0, bar, t) == 0 && std::strlen(t) == bar)
            return t;
    return nullptr;
}

// Text after the last '|' as a number (NPCSEEN / COMPOWN end with the ID the
// sender claims to be).
static uint32_t LastFieldU32(const std::string& msg)
{
    const size_t bar = msg.rfind('|');
    return bar == std::string::npos ? 0u
        : static_cast<uint32_t>(std::strtoul(msg.c_str() + bar + 1, nullptr, 10));
}

static bool ParseHello(const std::string& msg, std::string& outName, uint64_t& outSid, NiPoint3& outPos)
{
    outName.clear();
    outSid = 0;
    outPos = NiPoint3(0.f, 0.f, 0.f);

    if (msg.rfind("HELLO|", 0) != 0)
        return false;

    std::stringstream ss(msg.substr(6));

    if (!std::getline(ss, outName, '|') || outName.empty())
        return false;

    std::string tok;
    if (std::getline(ss, tok, '|') && !tok.empty())
    {
        try { outSid = std::stoull(tok); }
        catch (...) { outSid = 0; }
    }

    if (outSid == 0)
        outSid = HashName(outName);

    // Optional pos fields added so client's real spawn position reaches the host.
    // Old clients that don't send pos fields leave outPos at (0,0,0).
    std::string xt, yt, zt;
    try
    {
        if (std::getline(ss, xt, '|') && !xt.empty() &&
            std::getline(ss, yt, '|') && !yt.empty() &&
            std::getline(ss, zt)      && !zt.empty())
        {
            outPos.x = std::stof(xt);
            outPos.y = std::stof(yt);
            outPos.z = std::stof(zt);
        }
    }
    catch (...) {}

    return outSid != 0;
}

static void HostBroadcastPlayerCreate(
    uint32_t entityID,
    const NiPoint3& pos,
    const NiPoint3& rot,
    bool enqueueLocal,
    const std::string& displayName = std::string())
{
    EntityCreatePacket p{};
    p.entityID = entityID;
    p.type = CoSyncEntityType::Player;


    p.baseFormID = 0;

    p.ownerEntityID = entityID;
    p.spawnFlags = EntityCreatePacket::HiddenOnSpawn | EntityCreatePacket::RemoteControlled;
    p.spawnPos = pos;
    p.spawnRot = rot;


    p.username = displayName.empty() ? s_myName : displayName;

    LOG_INFO("[CoSyncNet] TX CREATE Player entity=%u name='%s' enqueueLocal=%d",
        entityID, p.username.c_str(), enqueueLocal ? 1 : 0);

    CoSyncTransport::Send(SerializeEntityCreate(p));

    if (enqueueLocal)
        g_CoSyncPlayerManager.EnqueueEntityCreate(p);
}

static void HostPublishHostCreateIfNeeded()
{
    if (!s_isHost || s_hostCreatePublished == true || !s_connected)
        return;

    const uint32_t eid = EntityIDFromSteamID(CoSyncNet::GetMySteamID());
    const NiPoint3 hostPos = CoSyncNet_GetLocalPlayerPos();

    HostBroadcastPlayerCreate(
        eid,
        hostPos,
        NiPoint3{ 0.f, 0.f, 0.f },
        false,
        s_myName
    );

    s_hostCreatePublished = true;
}

// Round 111: CoSyncNet::Init() previously had ZERO breadcrumbs between
// s_initialized=true (the very first line) and its own completion log (the
// very last line) -- a dozen sub-inits/hook-installs with no visibility into
// which one is live if any of them faults. A real crash (DLL+0x1A83xx,
// RDI=R8=0x14, call depth consistent with Run->TickGameThread->
// PerformPendingInit->Init->InstallXHook) happened in exactly this gap, on
// the FIRST test where a real save was loaded (so IsWorldReady() finally
// went true and Init() finally ran for the first time this entire 110-round
// campaign) -- previously misdiagnosed as "Init never ran at all" purely
// from g_lastTickStep staying frozen, which only proves CoSyncNet::Tick()
// never got a turn, not that Init() itself didn't start (s_initialized is
// set true on Init()'s very first line, before any of the installs that can
// fault). Logging every step so the next crash pins the exact call.
void CoSyncNet::Init(bool isHost)
{
    if (s_initialized)
        return;

    s_initialized = true;
    s_isHost = isHost;

    // Stamp role tag so every subsequent log line is prefixed [H] or [C].
    // Helps distinguish two Goldberg instances sharing the same game folder.
    CoSyncLogRoleTag() = isHost ? "[H]" : "[C]";

    s_helloSent = false;
    s_hostCreatePublished = false;
    s_verSent = false;

    LOG_INFO("[CoSyncNet] Init() entered, isHost=%d role=%s (CoSync build %u)",
        isHost ? 1 : 0, CoSyncLogRoleTag(), kCoSyncBuildNumber);

    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());
    LOG_INFO("[CoSyncNet] Init() step: SetLocalEntityID done");

    CoSyncLocalPlayer::Init();
    LOG_INFO("[CoSyncNet] Init() step: CoSyncLocalPlayer::Init done");

    CoSyncWorldSync::Init();
    LOG_INFO("[CoSyncNet] Init() step: CoSyncWorldSync::Init done");
    CoSyncWorkshopSync::Init();
    LOG_INFO("[CoSyncNet] Init() step: CoSyncWorkshopSync::Init done");
    CoSyncFactions::OnSessionStart(isHost);
    CoSyncWorkshopAuthority::OnSessionStart(isHost);
    CoSyncChat::OnSessionStart();
    CoSyncVoice::OnSessionStart();
    CoSyncTimeSync::Init();
    LOG_INFO("[CoSyncNet] Init() step: CoSyncTimeSync::Init done");

    // EquipHook: detects local player weapon draw/holster and broadcasts EQUIP|
    // packets so the receiver can apply them via the two-phase AddItem->EquipItem
    // path. RVAs kEquipObject_RVA/kUnequipObject_RVA verified via IDA round 93.
    // Detour filters to kFormType_WEAP only; OutfitSync handles ARMO independently.
    InstallEquipHook();
    LOG_INFO("[CoSyncNet] EquipHook ENABLED (weapons only, addresses verified)");
    // DialogueLineHook: detours BuildDialogueItem (0x006984F0), the chokepoint
    // every spoken line passes through carrying (topic, speaker), so the host
    // can broadcast DLG|L and clients can hear the conversation on the NPC
    // proxy. RVA derived 2026-08-05 from Fallout4_disk.i64 (entry-verified,
    // 9 callers spanning menu dialogue, scenes and Papyrus Say) -- NOT from
    // the skewed dump IDB that produced this project's earlier bad detours.
    CoSyncDialogSync::InstallDialogueLineHook();
    // Round 132 BISECTION: ALL anim/locomotion-adjacent MinHook patches
    // disabled at once. Ground truth that forced this: (a) even a CONSOLE
    // `player.placeatme 20749` raider A-poses while CoSync is loaded, but
    // pre-existing world NPCs animate fine — i.e. some hook of ours breaks
    // graph activation for any actor spawned AFTER install; (b) round 129
    // proved the dump-IDB every one of these guards' RVAs came from has
    // region-dependent address skew, so several of these are likely spliced
    // mid-function into engine anim code; (c) all four guards were fake-actor
    // -era crash armor — the fake actors they guarded against no longer
    // exist since the round-117 spawn fix. Re-enable individually (with RVAs
    // re-derived from Fallout4_disk.i64) only if their original crashes
    // actually return.
    // STAYS DISABLED 2026-08-03 — and the RVA is worse than "mid-function".
    // Checked properly this time, against the live binary:
    //   * .pdata (authoritative RUNTIME_FUNCTION boundaries) puts 0x393BA6
    //     **166 bytes inside** the function that starts at 0x393B00 — so the
    //     round-129 "mid-function splice" call was correct.
    //   * BUT 0x393B00 is not ScrapReference either. It takes NO arguments and
    //     its 3rd instruction is `mov ecx,[rip+0x2D58B03]` -> 0x030EC618, i.e.
    //     it reads hCurrentWorkshop and does the standard handle resolve. That
    //     is a *get current workshop* helper, not
    //     ScrapReference(ContextData*, NiPointer<TESObjectREFR>*, BSTArray*).
    // So correcting 0x393BA6 -> 0x393B00 would hook a completely unrelated
    // function with a mismatched signature. Do NOT "fix" it that way.
    // The real ScrapReference still has to be located before this can return.
    // LEAD (2026-08-04, traced but NOT yet verified — do not hook on this alone):
    //   "References Scrapped: %u Stored: %u." fmt string @ 0x024B6030 has one
    //   xref, at 0x5FA6A2, inside the ScrapAll console handler 0x5FA680. That
    //   handler's only real work is `call 0x37CD50` (the ScrapAll worker, which
    //   takes two out-params = the scrapped/stored counts). Inside 0x37CD50 the
    //   direct calls are 0x385400, 0x514110, 0xD5FC80, 0x5191A0, 0x3894D0,
    //   0x3A77B0 (x3), 0x3680A0, 0x2CD430. **0x3A77B0 is the strongest
    //   candidate** for the per-reference scrap: it is the only one invoked
    //   repeatedly inside the iteration. Confirm its signature matches
    //   (ContextData*, NiPointer<TESObjectREFR>*, BSTArray*) before hooking —
    //   note 0x37CC8A is also an hCurrentWorkshop xref, so this whole region is
    //   workshop-scrap code, which is corroborating but not proof.
    // NOTE: workshop BUILD mirroring does not depend on this hook — it comes
    // from the WBF poll scan plus CoSyncWorkshopSync::Tick (re-enabled today).
    // Only SCRAP/removal sync is lost while this stays off.
    //InstallWorkshopScrapHook();
    LOG_WARN("[CoSyncNet] WorkshopScrapHook DISABLED — the real scrap function in the "
             "game hasn't been located yet, so this hook stays off for now");
    //InstallAnimGraphHandleGuard();
    //InstallMovementHandleGuard();
    //InstallLocomotionCalcGuard();
    //InstallSharedHandleLeafGuard();
    LOG_WARN("[CoSyncNet] All anim/locomotion guards DISABLED (one of these hooks breaks animation setup for console-spawned NPCs too, still tracking down which one)");
    // 2026-08-12: unrelated to the anim/locomotion bisection above -- guards
    // Fallout4+0x4CAEA0 (BGSPlayerMusicChanger's cell-teardown resolver),
    // TTD-verified as the cause of a live 0xC0000005 crash during a beta
    // session. Narrow, cold code path (only runs on cell unload), RVA
    // verified against the canonical disk IDB, not the skewed dump-IDB that
    // caused the guards above to be disabled -- see
    // CoSyncMusicChangerCellTeardownGuard.h for the full writeup.
    InstallMusicChangerCellTeardownGuard();
    // 2026-08-11: SISTER GUARD to the line above -- same teardown, next global
    // over. TTD trace Traces\Fallout401.run caught both in one session: the
    // music-changer fault at Fallout4+0x4CAF53 (qword_1430E0290 null) was
    // CAUGHT by the guard above and survived, then ~0.3s later the process
    // died unhandled at Fallout4+0xE5B7B1 (qword_1430EEF50 null) -- exit code
    // 0xC0000005. sub_140C31370 writes BOTH globals (0xC314D8 and 0xC31675),
    // so one teardown nulls the pair; guarding only one of them was never
    // going to be enough. Unlike the anim/locomotion guards above, the RVA is
    // a .pdata-verified real function entry (0xE5B780-0xE5B954) from
    // Fallout4_disk.i64, not the skewed dump-IDB, and the substituted return
    // (1.0f) is the function's own documented default rather than a guess --
    // see CoSyncActorAudioScaleGuard.h for the full derivation. The guard
    // logs its observed return-value range so that choice can be confirmed
    // empirically in-game; watch for [ActorAudioScaleGuard] lines.
    InstallActorAudioScaleGuard();
    // BISECTION (round 19): round 18's fix (stop logging) didn't help -- new
    // crash is a write fault at [rax+8] with rax=NULL, INSIDE our own MinHook
    // trampoline buffer, called from this hook's own detour chain
    // (Fallout4.exe+0xCC8F45/+0xCC8796/+0xCC8808, all inside the 0xCC8AD8
    // function this hook calls). This is our own calling-convention guess
    // (fastcall(rcx,rdx)->float) being wrong for this specific function --
    // not a game bug. Disabling until the real signature is confirmed.
    //InstallActorLocomotionGuard();
    LOG_WARN("[CoSyncNet] ActorLocomotionGuard SKIPPED (an earlier calling-convention guess for this hook was wrong and caused a crash in our own code)");
    // BISECTION (round 24): now that round 21's RVA fix lets this hook
    // actually fire for the first time this whole campaign, its real
    // target -- Bethesda's own per-actor AI-processing round-robin queue
    // walker (see round 89's comment above InstallActorProcessGuard's
    // definition) -- crashed with a null-pointer vtable call (RAX=0,
    // fault=0x178) at Fallout4.exe+0xDB88B5, 8 bytes into the real function.
    // CallOriginalSafe's __try/__except did NOT catch it: no RecordFault
    // WARN line appears before the crash in the log, and the session log
    // just stops dead -- the process genuinely died despite the SEH wrap,
    // the same "structurally-correct hook whose call target dies in a way
    // SEH can't actually contain" pattern as ActorLocomotionGuard (round
    // 19, see comment above). This guard is pure defensive bisection
    // insurance, not load-bearing -- per round 89's own comment, "the RVA
    // fix alone may remove most/all faults here; revisit ... only if faults
    // still recur after this fix" -- they recurred, worse (fatal instead of
    // silently never firing). Disabling rather than chasing why SEH didn't
    // catch this one.
    //InstallActorProcessGuard();
    LOG_WARN("[CoSyncNet] ActorProcessGuard SKIPPED (the real target function crashes with a null-pointer read that our safety net couldn't catch)");
    // Round 133 BISECTION TIER 2: guards-off didn't fix the A-pose (round 132
    // test: console spawns still frozen, world NPCs fine). Now disabling EVERY
    // remaining engine-code patch whose target RVA came from the skewed
    // dump-IDB and has NOT been live-verified: Door (0x514180 = padding in
    // the real exe, inert anyway), Pickup (0x500430 = padding, inert),
    // ModEvent (0x1134D80, skew region), QuestStage (0x1185DD0, skew region),
    // QuestObjectives (0x1185A00/0x1185B50/0x11887F0 — round 129 proved these
    // land MID-FUNCTION in the real exe: live splices into unknown code),
    // EngineResume + FullSubsystemTeardown (0x1A7F920/0xC384F0 — raw JMP
    // patches into whatever the real exe has there). Any of these could be
    // corrupting the actor-activation path that only NEWLY SPAWNED actors
    // run. Only LockHook stays: its targets are live-byte-verified (round
    // 129). Re-add the rest one-by-one with RVAs re-derived from
    // Fallout4_disk.i64.
    // 2026-07-21: RE-ENABLED. Two independent fixes since this was last
    // disabled: (1) kActivateWorker_RVA was itself dead (padding, +0x10 skew
    // — same defect class as the old PickupHook RVA) until this session's
    // fix, re-derived to 0x514190 (59 callers, signature-verified) — the
    // hook detour had literally never fired before now, so it was never the
    // actual cause of the round-146 crash it got blamed for. (2) The
    // "RAX=FEEEFEEE at f4se+0x161F4" UAF this comment blamed IS the F4SE
    // ITaskDelegate vtable-slot-order bug root-caused and fixed this same
    // session (Core/Tasks.h had Run declared before the virtual destructor,
    // swapping slots 0/1 vs f4se's task-pump ABI — see build 151) — that
    // was a global task-queue corruption bug, not anything door-specific,
    // and every "opening a door corrupts something during the next spawn"
    // symptom is exactly what it produced. Both blockers are gone.
    InstallDoorHook();
    LOG_INFO("[CoSyncNet] Init() step: InstallDoorHook done");
    InstallLockHook();
    LOG_INFO("[CoSyncNet] Init() step: InstallLockHook done");
    // Round-133 disabled this because 0x500430 was "padding, inert" — that was
    // the +0x10 dump-IDB skew. RVA re-derived from Fallout4_disk.i64 via byte
    // pattern (2026-07-15): real entry is 0x500440. Re-enabling.
    InstallPickupHook();
    LOG_INFO("[CoSyncNet] Init() step: InstallPickupHook done");
    //InstallModEventHook();
    // kSetCurrentStageID_RVA = 0x1185DD0 was re-derived from Fallout4_disk.i64 and
    // confirmed in round 93 (debug strings "Stage %d set%s" and "(quest will be started
    // first)" at the exact entry). The round-133 bisection comment "skew region" was
    // wrong about this one — it was already on the disk-IDB-verified list. Re-enabling.
    InstallQuestStageHook();
    LOG_INFO("[CoSyncNet] Init() step: InstallQuestStageHook done (game address verified)");
    //InstallQuestObjectiveHooks();
    //InstallEngineResumeSafetyHook();
    //InstallFullSubsystemTeardownHook();
    LOG_WARN("[CoSyncNet] ModEvent/QuestObjectives/EngineResume/FullTeardown hooks still DISABLED (game addresses not yet verified). Door/Lock/Pickup/QuestStage ENABLED (all verified).");

    // round 128: reset NpcWorldSync maps so stale actor pointers from the
    // previous session don't shadow real actors in the new scan.
    CoSyncNpcWorldSync::Reset();
    CoSyncPowerArmorSync::Reset();
    CoSyncItemDropSync::Reset();

    // Damage-detection baselines from a previous session reference dead
    // entityIDs — clear alongside NpcWorldSync.
    CoSyncHealthSync::Reset();
    CoSyncReviveSync::Reset();
    CoSyncOutfitSync::Reset();
    CoSyncProxyWeaponAuthority::Reset();
    CoSyncFormIdMap::Reset();
    CoSyncOverlay_ClearLobby();

    LOG_INFO("[CoSyncNet] Init host=%d localEID=%u",
        isHost ? 1 : 0, GetMyEntityID());
}

void CoSyncNet::ScheduleInit(bool isHost)
{

    s_pendingInit = true;
    s_pendingHostFlag = isHost;


    CoSyncTransport::SetReceiveCallback(
        [](const std::string& msg, double now, HSteamNetConnection c)
        {
            CoSyncNet::OnReceive(msg, now, c);
        });

    CoSyncTransport::SetConnectedCallback(
        []()
        {
            CoSyncNet::OnGNSConnected();
        });

    CoSyncTransport::SetDisconnectedCallback(
        []()
        {
            CoSyncNet::OnGNSDisconnected();
        });


    // Init() itself always runs from PerformPendingInit() on the game-thread
    // tick (CoSyncRuntime), never here: ScheduleInit is called from
    // CoSync_Main's detached start-up threads, and Init() installs MinHook
    // detours and touches game state (Phase 1 audit, threading hazard).
    (void)isHost;
}

void CoSyncNet::PerformPendingInit()
{
    if (!s_pendingInit)
        return;

    if (!CoSyncWorld::IsWorldReady())
        return;

    // A client plays in a copy of the HOST's world: don't start the session
    // on whatever save was loaded before, wait until the host's is.
    if (!s_pendingHostFlag && !CoSyncWorldTransfer::IsClientWorldReady())
        return;

    s_pendingInit = false;
    Init(s_pendingHostFlag);
}

void CoSyncNet::Shutdown()
{
    if (!s_initialized && !s_pendingInit)
        return;

    LOG_INFO("[CoSyncNet] Shutdown");

    if (s_connected && s_initialized)
    {
        const char* reason = s_isHost ? "DISCONNECT|host_shutdown"
                                      : "DISCONNECT|client_shutdown";
        CoSyncTransport::Send(std::string(reason));
    }

    // L3 (2026-09-26): nothing removed proxies or restored hidden NPCs on
    // Stop/Leave -- OnGNSDisconnected's queued destroys never ran because the
    // tick that drains them stops right here. Tear everything down now.
    if (s_initialized)
        g_CoSyncPlayerManager.DespawnAllRemote("session shutdown");

    s_initialized = false;
    s_pendingInit = false;
    s_connSenderEid.clear();

    s_connected = false;
    s_isHost = false;

    s_helloSent = false;
    s_hostCreatePublished = false;

    {
        std::lock_guard<std::mutex> lk(s_peerMutex);
        s_peers.clear();
    }
    s_hostEntityID = 0;

    // Delete any in-flight stand-ins before the tick that owns them stops
    // running -- a THRX that never arrives (which a disconnect guarantees for
    // every airborne projectile) would otherwise leave grenade models frozen
    // in mid-air in the player's solo game.
    CoSyncThrowableSync::Reset();
    CoSyncCombatAuthority::Reset();
    CoSyncWorldStateSync::Reset();
    CoSyncSharedDialogue::Reset();

    CoSyncWorldSync::Shutdown();
    CoSyncWorkshopSync::Shutdown();
    CoSyncTimeSync::Shutdown();
    CoSyncFactions::OnSessionEnd();
    CoSyncWorkshopAuthority::OnSessionEnd();
    CoSyncChat::OnSessionEnd();
    CoSyncVoice::OnSessionEnd();

    //UninstallEquipHook();
    UninstallWorkshopScrapHook();
    UninstallDoorHook();
    UninstallLockHook();
    UninstallPickupHook();
    UninstallModEventHook();
    UninstallQuestStageHook();
    UninstallQuestObjectiveHooks();

    CoSyncLocalPlayer::Shutdown();
    CoSyncTransport::Shutdown();
}

bool CoSyncNet::IsHost() { return s_isHost; }
uint32_t CoSyncNet::GetHostEntityID()
{
    std::lock_guard<std::mutex> lk(s_peerMutex);
    return s_hostEntityID;
}
uint32_t CoSyncNet::GetBuildNumber() { return kCoSyncBuildNumber; }
bool CoSyncNet::IsInitialized() { return s_initialized; }
bool CoSyncNet::IsConnected() { return s_connected; }

// Reads [Network] bSaltInstanceID from <FO4 dir>\Data\F4SE\Plugins\CoSync.ini.
// Cached -- this sits on the GetMySteamID path. The path is built from the
// running exe rather than passed relative, because GetPrivateProfileInt
// resolves a bare filename against the Windows directory, not the cwd.
static bool ReadSaltInstanceIDSetting()
{
    LOG_REQUIREMENT("(global)::ReadSaltInstanceIDSetting", "Doesn't touch the game — just reads an optional settings file next to the plugin. If the file or setting is missing, it defaults to the safer option.");
    static int s_cached = -1;
    if (s_cached >= 0)
        return s_cached != 0;

    char exePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        s_cached = 1;
        return true;
    }

    std::string ini(exePath);
    const size_t slash = ini.find_last_of("\\/");
    ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
    ini += "Data\\F4SE\\Plugins\\CoSync.ini";

    s_cached = ::GetPrivateProfileIntA("Network", "bSaltInstanceID", 1, ini.c_str());
    LOG_INFO("[CoSyncNet] bSaltInstanceID=%d (from '%s')", s_cached, ini.c_str());
    return s_cached != 0;
}

static bool s_steamIdentity = false;

void CoSyncNet::SetSteamIdentity(bool steam)
{
    if (s_steamIdentity == steam && s_mySteamID != 0)
        return;
    s_steamIdentity = steam;
    s_mySteamID = 0;
    s_myEntityID = 0;
    LOG_INFO("[CoSyncNet] Identity mode: %s", steam ? "Steam account (verified by Steam)" : "instance token");
}

uint64_t CoSyncNet::GetMySteamID()
{
    if (s_mySteamID == 0 && s_steamIdentity)
    {
        const uint64_t real = CoSyncSteamAPI::MySteamID();
        if (real != 0)
            s_mySteamID = real;
    }
    if (s_mySteamID == 0)
    {
        // Salting is a TEST-RIG setting, read from
        //   <FO4>\Data\F4SE\Plugins\CoSync.ini   ->   [Network] bSaltInstanceID
        // Default 1 (salted), because the default configuration must be the one
        // that cannot silently break: salting preserves uniqueness in BOTH
        // cases, whereas unsalted collides the moment two instances share an
        // account. Set it to 0 for a real two-machine / two-account session if
        // you want the literal Steam account id to be the identity key.
        //
        // Two instances behind a Steam EMU report the same account, which is
        // exactly the collision this exists for -- and that is a property of
        // the test rig, not of shipping multiplayer.
        const bool saltInstanceID = ReadSaltInstanceIDSetting();

        // This value is an INSTANCE IDENTITY TOKEN, not a Steam account id.
        // It is what goes out in HELLO, and BOTH peers derive the entityID
        // from it via EntityIDFromSteamID() -- so it must be unique per
        // running game process, not per Steam account.
        //
        // Regression 2026-08-03, caught on the first live test: returning the
        // raw real SteamID here looked like an upgrade but broke everything.
        // The standard test setup is two instances on ONE Steam account, so
        // both peers produced steamID=76561199986796316 -> the SAME
        // localEID=2026530588. Each side then treated the other's traffic as
        // its own echo ("Ignoring CREATE for local entity", "ProcessEntityUpdate
        // DROP(self)") and NO player proxy was ever spawned on either machine.
        // The original fake id mixed in the PID, which is exactly what made
        // same-account multi-instance work. Keep that property.
        //
        // The real account id and persona are still available for DISPLAY via
        // CoSyncSteamID -- they just must not be the identity key.
        const uint64_t pid    = static_cast<uint64_t>(::GetCurrentProcessId());
        const uint64_t realID = CoSyncSteamID::GetSteamID64();

        if (realID != 0)
        {
            s_mySteamID = saltInstanceID ? (realID ^ (pid * 2654435761ULL))
                                         : realID;
            LOG_INFO("[CoSyncNet] Instance token=%llu (real SteamID=%llu "
                     "persona='%s' pid=%llu) salted=%d — set [Network] "
                     "bSaltInstanceID=0 in CoSync.ini to use the raw Steam id "
                     "as the identity key (only safe with distinct accounts)",
                (unsigned long long)s_mySteamID,
                (unsigned long long)realID,
                CoSyncSteamID::GetPersonaName().c_str(),
                (unsigned long long)pid,
                saltInstanceID ? 1 : 0);
        }
        else
        {
            const uint64_t nameHash = HashName(s_myName);
            s_mySteamID = nameHash ^ (pid * 2654435761ULL);
            LOG_WARN("[CoSyncNet] Steam unavailable — fallback instance token=%llu "
                     "from name='%s' pid=%llu",
                (unsigned long long)s_mySteamID,
                s_myName.c_str(),
                (unsigned long long)pid);
        }

        if (s_mySteamID == 0)
            s_mySteamID = pid ? pid : 1;
    }
    return s_mySteamID;
}

uint32_t CoSyncNet::GetMyEntityID()
{
    if (s_myEntityID == 0)
        s_myEntityID = EntityIDFromSteamID(GetMySteamID());
    return s_myEntityID;
}

const char* CoSyncNet::GetMyName()
{
    // Default to the Steam persona rather than the literal "Player" this was
    // initialised to. An explicit SetMyName() still wins (it clears nothing
    // here -- s_myName simply stops being the placeholder).
    if (s_myName.empty() || s_myName == "Player")
    {
        const std::string& persona = CoSyncSteamID::GetPersonaName();
        if (!persona.empty())
            s_myName = persona;
    }
    return s_myName.c_str();
}

void CoSyncNet::SetMyName(const std::string& name)
{
    s_myName = name;
    s_mySteamID = 0;
    s_myEntityID = 0;


    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());
}

void CoSyncNet::SetMySteamID(uint64_t sid)
{
    s_mySteamID = sid;
    s_myEntityID = 0;

    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());
}

// UPDATE 2026-06-20 (round 41): breadcrumb of the last FlushPendingXOps-style
// step CoSyncNet::Tick was about to run, read by CoSync_Plugin.cpp's VEH
// crash-diagnostic block. Plain global, not atomic -- the VEH callback runs
// synchronously on the SAME thread that's executing Tick when a fault
// happens, so there's no cross-thread race to guard against; this exists
// purely so the next crash log says exactly which call was live instead of
// us inferring it from stale stack bytes (round 41's actual problem: 8 newly
// fixed unguarded-LookupFormByID sites are plausible but unconfirmed causes
// of the post-0x1A7FC54 fatal crash -- this settles it definitively next
// time it happens, whether or not this round's fixes were the real one).
volatile const char* g_lastTickStep = "Tick:start";

void CoSyncNet::SendAnimEdges(Actor* actor, uint32_t entityID, AnimEdgeState& state)
{
    LOG_REQUIREMENT("CoSyncNet::SendAnimEdges", "Needs a non-null actor with a ready animation graph, checked. Every variable is looked up by name and simply fails quietly if the graph doesn't have it. Sends nothing when not connected.");
    if (!actor || !s_initialized || !s_connected || entityID == 0)
        return;

    bool firing = false, reloading = false, throwing = false;
    int  fireState = 0;
    CoSyncAnimUtils::GetBool(actor, CoSyncAnimUtils::Vars::isFiring,    firing);
    CoSyncAnimUtils::GetBool(actor, CoSyncAnimUtils::Vars::isReloading, reloading);
    CoSyncAnimUtils::GetBool(actor, CoSyncAnimUtils::Vars::bIsThrowing, throwing);
    CoSyncAnimUtils::GetInt (actor, CoSyncAnimUtils::Vars::iSyncFireState, fireState);

    // First call only establishes a baseline -- without this, whatever the
    // actor happens to be doing at connect time would replay as a spurious
    // event on every peer.
    if (!state.init)
    {
        state.init          = true;
        state.prevFiring    = firing;
        state.prevReloading = reloading;
        state.prevThrowing  = throwing;
        state.prevFireState = fireState;
        return;
    }

    char buf[64];
    auto send = [&](uint32_t code)
    {
        std::snprintf(buf, sizeof(buf), "AEV|%u|%u", entityID, code);
        CoSyncTransport::Send(std::string(buf));
    };

    // A shot is EITHER the start of firing, OR (while still firing) the engine
    // advancing its per-shot fire state. Without the second condition a held
    // trigger produces exactly one event for the whole burst -- confirmed in
    // the 2026-08-04 log, which is the "doesn't keep up with fast shooting"
    // report. fireState is logged on every send so the next test shows whether
    // iSyncFireState really does tick per round on this runtime; if it turns
    // out not to, the fallback is the equipped weapon's ammo count, which
    // decrements exactly once per shot.
    // 0.3.5: shots are no longer inferred from these graph variables. They
    // never changed for NPC graphs (0 fire events in 298 NPC AEVs, client log
    // 2026-09-29), and the engine's own TESObjectWEAP::Fire is now hooked
    // (CoSyncCombatAuthority): one SHOT per real shot, players and NPCs alike.
    // Sending AEV fire as well would replay every player shot twice.
    (void)firing;
    (void)fireState;

    if (reloading != state.prevReloading)
    {
        send(reloading ? kAnimEv_ReloadStart : kAnimEv_ReloadEnd);
        LOG_UPDATED("[AnimEdge] entity=%u reload %s", entityID, reloading ? "START" : "END");
    }

    if (throwing != state.prevThrowing)
    {
        if (throwing)
        {
            // The sender is the only side that can see which throwable is
            // equipped, so it decides grenade vs mine here rather than
            // shipping the formID and making the receiver re-derive it.
            const uint32_t throwableID =
                CoSyncThrowableSync::GetEquippedThrowableFormID(actor);
            const bool isMine = CoSyncThrowableSync::IsMineThrowable(throwableID);
            send(isMine ? kAnimEv_ThrowMine : kAnimEv_ThrowGrenade);
            LOG_UPDATED("[AnimEdge] entity=%u THROW start (throwable=0x%08X mine=%d)",
                entityID, throwableID, isMine ? 1 : 0);
        }
        else
        {
            send(kAnimEv_ThrowEnd);
            LOG_UPDATED("[AnimEdge] entity=%u THROW end", entityID);
        }
    }

    state.prevFiring    = firing;
    state.prevReloading = reloading;
    state.prevThrowing  = throwing;
    state.prevFireState = fireState;
}

void CoSyncNet::Tick(double now)
{
    g_lastTickStep = "CoSyncTransport::Tick";
    CoSyncTransport::Tick(now);

    // Retry the deferred HELLO every frame until the save has loaded. This is
    // the ONLY thing that ever sends it after a "world not ready" answer --
    // OnGNSConnected fires once and never again.
    TrySendHello();

    // Drain HUD toasts queued from the network thread. Deliberately OUTSIDE
    // the s_initialized/s_connected guards below: the disconnect toast is
    // queued by the very path that tears the session down, so gating this on
    // "still connected" would swallow the one message the player most needs.
    g_lastTickStep = "FlushNotifications";
    CoSyncGameAPI::FlushNotifications();


    PerformPendingInit();


    if (s_initialized)
    {
        g_lastTickStep = "g_CoSyncPlayerManager.Tick";
        g_CoSyncPlayerManager.Tick();
    }

    if (!s_initialized)
        return;

    // Local player's discrete anim events, sampled EVERY FRAME. Deliberately
    // not folded into SendMyEntityUpdate: that path is throttled (and drops to
    // a third rate when the player is standing still), and isFiring is true
    // for about one frame per shot -- sampling it there loses nearly every
    // shot, which is exactly what the 2026-08-04 test showed.
    if (s_connected)
    {
        g_lastTickStep = "SendAnimEdges";
        static AnimEdgeState s_localAnimEdges;
        SendAnimEdges(reinterpret_cast<Actor*>(CoSyncGetPlayer()),
                      GetMyEntityID(), s_localAnimEdges);
    }

    // Round 140: one-shot build-version broadcast so cross-version sessions
    // are loudly visible in both logs (see kCoSyncBuildNumber).
    if (s_connected && !s_verSent)
    {
        s_verSent = true;
        char verBuf[64];
        snprintf(verBuf, sizeof(verBuf), "VER|%u|%u", GetMyEntityID(), kCoSyncBuildNumber);
        CoSyncTransport::Send(std::string(verBuf));
        LOG_INFO("[CoSyncNet] TX VER build=%u", kCoSyncBuildNumber);
    }


    if (s_isHost && s_connected)
    {
        g_lastTickStep = "HostPublishHostCreateIfNeeded";
        HostPublishHostCreateIfNeeded();
    }

    g_lastTickStep = "HostSendNpcUpdates";
    g_CoSyncPlayerManager.HostSendNpcUpdates(now);

    // 2026-08-15: re-enabled for the client. This was host-only from
    // 2026-07-21 to 2026-08-15 (see git history for the old rationale --
    // proximity-based ownership was letting the client claim/drive NPCs,
    // which predates and is exactly what the 2026-08-08 "Tilted-style"
    // ownership arbitration (host spatial dedup via s_spatialClaims, owner-
    // vs-puppet split in ClientBindWorldActor_Impl) was built to fix. That
    // arbitration already existed but was unreachable dead code with this
    // gated off, because TickScanWorldActors_Impl's isHost==false branch
    // (the NPCSEEN report path) never ran. Confirmed via live log: with the
    // host-only gate, a client fighting raiders/ghouls in Vault 114 (a big,
    // multi-room interior) synced ZERO of them the entire session -- nothing
    // discovers what only the client is near. Now both roles scan; host
    // still self-registers directly, client reports via NPCSEEN and the
    // host arbitrates ownership as designed.
    if (s_connected)
    {
        g_lastTickStep = "TickScanWorldActors";
        CoSyncNpcWorldSync::TickScanWorldActors(now);
        if (s_isHost)
        {
            g_lastTickStep = "HostTickOwnershipHandoff";
            CoSyncNpcWorldSync::HostTickOwnershipHandoff(now);
        }
    }

    if (s_connected)
    {
        g_lastTickStep = "ItemDropSync::Tick";
        CoSyncItemDropSync::Tick(now);
    }

    if (!s_isHost && s_connected)
    {
        g_lastTickStep = "TickSuppressLocalDynamicSpawns";
        CoSyncNpcWorldSync::TickSuppressLocalDynamicSpawns(now);
    }

    // Both roles: remove save-persisted ghost proxy bodies (old 0x7-era
    // proxies etc.) that aren't registered to any live entity.
    if (s_connected)
    {
        g_lastTickStep = "CleanupStaleProxyBodies";
        CoSyncNpcWorldSync::CleanupStaleProxyBodies(now);
    }

    g_lastTickStep = "CoSyncWorldSync::Tick";       CoSyncWorldSync::Tick(now);

    // BISECTION (2026-06-24): re-enabled. Never actually proven guilty --
    // the real crash all night was CoSyncGameAPI::SpawnRemoteActor's
    // broken DYNAMIC_CAST on the freshly-spawned actor (a separate,
    // F4SE-task-queue code path, confirmed by lastTickStep reading
    // "RTGT:end" at crash time -- i.e. this whole tick chain had already
    // finished cleanly). Fixed at its source; re-enabling everything here
    // to actually test it for the first time.
    // BISECTION (2026-06-24): disabled. With HostTickObjects neutralized
    // (settle-delay fix), the crash moved to immediately after WS sent --
    // i.e. right where this call sits, the very next thing in the chain.
    // CoSyncWorkshopSync::Tick dereferences Workshop::hCurrentWorkshop, a
    // RelocPtr with a hardcoded RVA (0x030EC618, ThirdParty/Pap/
    // GameWorkshop.cpp) vendored from the F4SE Papyrus SDK and never
    // cross-checked against this game build -- same disease as round
    // 106's Runtime_DynamicCast_Internal (pointed at fclose) and round
    // 108's stale GameRTTI table. Disabling to confirm directly.
    // RE-ENABLED 2026-08-03 — the "suspect RVA" premise was checked and is WRONG.
    // 0x030EC618 was verified directly against the running 1.11.221 binary
    // (not assumed from the SDK): it has 10 live `lea` xrefs from .text, and
    // the code at rva 0xB2B9F0 does the textbook BSPointerHandle resolve on it
    //     lea rdx, [rip+0x25C0C21]   ; -> 0x030EC618
    //     lea rcx, [rsp+0x28]        ; out NiPointer<TESObjectREFR>
    //     call 0x21E170              ; handle -> refr
    //     test rbx, rbx
    // so it really is the current-workshop handle on this build. (The adjacent
    // 0x030EC61C is resolved the same way right after, consistent with a
    // workshop handle pair.) The 2026-06-24 bisection disabled this on
    // suspicion by analogy to the round-106/108 stale-RVA bugs, but never
    // actually checked this address — and it has been off ever since, which is
    // why workshop builds only ever appeared for the player who placed them.
    g_lastTickStep = "CoSyncWorkshopSync::Tick";
    CoSyncWorkshopSync::Tick(now);
    g_lastTickStep = "CoSyncTimeSync::Tick";         CoSyncTimeSync::Tick(now);
    g_lastTickStep = "TickLocalQuests";              CoSyncQuestSync::TickLocalQuests(now);
    g_lastTickStep = "FlushPendingQuestOps";         CoSyncQuestSync::FlushPendingQuestOps();
    g_lastTickStep = "FlushPendingQuestObjectiveOps"; CoSyncQuestSync::FlushPendingQuestObjectiveOps();
    g_lastTickStep = "FlushPendingPowerArmorOps";    CoSyncPowerArmorSync::FlushPendingPowerArmorOps();
    g_lastTickStep = "FlushPendingNpcDeadOps";       CoSyncNpcWorldSync::FlushPendingNpcDeadOps();
    g_lastTickStep = "FlushPendingSpecialOps";       CoSyncSpecialSync::FlushPendingSpecialOps();
    g_lastTickStep = "FlushPendingVatsOps";          CoSyncVatsSync::FlushPendingVatsOps();
    g_lastTickStep = "FlushPendingOutfitOps";        CoSyncOutfitSync::FlushPendingOutfitOps();
    g_lastTickStep = "FlushPendingEquipOps";         CoSyncEquipHook::FlushPendingEquipOps();
    g_lastTickStep = "FlushPendingDoorOps";          CoSyncDoorHook::FlushPendingDoorOps();
    g_lastTickStep = "FlushPendingLockOps";          CoSyncLockHook::FlushPendingLockOps();

    if (s_isHost && s_connected)
    {
        g_lastTickStep = "TickLocalDialogue";
        CoSyncDialogSync::TickLocalDialogue(now, GetMyEntityID());
        g_lastTickStep = "TickLocalScene";
        CoSyncSceneSync::TickLocalScene(now);
    }
    g_lastTickStep = "FlushPendingDialogOps";        CoSyncDialogSync::FlushPendingDialogOps();
    g_lastTickStep = "FlushPendingSceneOps";         CoSyncSceneSync::FlushPendingSceneOps();

    g_lastTickStep = "FlushPendingHealthOps";        CoSyncHealthSync::FlushPendingHealthOps();
    g_lastTickStep = "FlushPendingCorpses";          CoSyncHealthSync::FlushPendingCorpses();
    g_lastTickStep = "CoSyncChat::Tick";             CoSyncChat::Tick(now);
    g_lastTickStep = "CoSyncVoice::Tick";            CoSyncVoice::Tick(now);
    g_lastTickStep = "TickDamageDetection";          CoSyncHealthSync::TickDamageDetection(now);
    g_lastTickStep = "FlushPendingReviveOps";        CoSyncReviveSync::FlushPendingReviveOps();
    g_lastTickStep = "TickReviveChannel";            CoSyncReviveSync::TickReviveChannel(now);
    g_lastTickStep = "FlushPendingLootOps";          CoSyncLootSync::FlushPendingLootOps();
    g_lastTickStep = "FlushPendingCustomEvents";     FlushPendingCustomEvents();


    g_lastTickStep = "FlushPendingCompanionOps";     CoSyncCompanionSync::FlushPendingCompanionOps();
    g_lastTickStep = "WeaponAuthorityDiag";          CoSyncProxyWeaponAuthority::TickDiagnostics(now);
    // 0.3.3 (Stage 1): TickCompanionFollowNudge removed -- it teleported a
    // companion 300 units behind its owner whenever it was >1500 away (a
    // teleport loop, inert until now only because of the raw-position bug).
    // Companions follow through their own AI; see Stage 4.

    g_lastTickStep = "Tick:end";
}

void CoSyncNet::SendMyEntityUpdate(
    uint32_t entityID,
    const NiPoint3& pos,
    const NiPoint3& rot,
    const NiPoint3& vel,
    double now,
    const AnimVarSnapshot* animVars,
    uint32_t weaponFormID)
{
    if (!s_initialized || !s_connected)
        return;

    EntityUpdatePacket u{};
    u.entityID = entityID;
    u.pos = pos;
    u.rot = rot;
    u.vel = vel;
    u.timestamp = now;
    u.equippedWeaponFormID = weaponFormID;

    // Read straight off the local player rather than threading another
    // parameter through every caller of this function -- "MyEntityUpdate" is
    // the local player by definition, so there is no other actor this could
    // legitimately come from. Returns 0 whenever no throwable is equipped,
    // which is the overwhelmingly common case.
    Actor* localActor = reinterpret_cast<Actor*>(CoSyncGetPlayer());
    u.equippedThrowableFormID =
        CoSyncThrowableSync::GetEquippedThrowableFormID(localActor);

    // Read bIsThrowing BY NAME rather than trusting animVars.booleans bit 21
    // (raw indexed read of graph slot 208). See Packets_EntityUpdate.h for why.
    {
        bool namedThrowing = false;
        bool namedReloading = false;
        bool namedFiring = false;
        if (localActor)
        {
            CoSyncAnimUtils::GetBool(localActor, CoSyncAnimUtils::Vars::bIsThrowing, namedThrowing);
            CoSyncAnimUtils::GetBool(localActor, CoSyncAnimUtils::Vars::isReloading, namedReloading);
            CoSyncAnimUtils::GetBool(localActor, CoSyncAnimUtils::Vars::isFiring,    namedFiring);
        }
        u.isThrowing  = namedThrowing;
        u.isReloading = namedReloading;
        u.isFiring    = namedFiring;

        // Diagnostic that decides the open question from the 2026-08-04 test:
        // grenades were provably thrown (four EXPL broadcasts) yet bit 21
        // never rose. Logging BOTH sources on either edge separates the two
        // candidate causes without another guess-and-rebuild cycle:
        //   named=1 indexed=0  -> graph index 208 is wrong on this runtime
        //   named=0 indexed=0  -> the third-person graph genuinely never
        //                         carries bIsThrowing for the local player
        //                         (e.g. first-person camera), and the throw
        //                         trigger has to come from somewhere else --
        //                         the projectile spawn is the fallback.
        const bool indexedThrowing =
            animVars && ((animVars->booleans & (1ULL << 21)) != 0);

        static bool s_prevNamed   = false;
        static bool s_prevIndexed = false;
        if (namedThrowing != s_prevNamed || indexedThrowing != s_prevIndexed)
        {
            LOG_UPDATED("[LocalPlayer] bIsThrowing named=%d indexed(bit21)=%d "
                        "throwable=0x%08X",
                namedThrowing ? 1 : 0, indexedThrowing ? 1 : 0,
                u.equippedThrowableFormID);
            s_prevNamed   = namedThrowing;
            s_prevIndexed = indexedThrowing;
        }
    }

    if (animVars && animVars->isValid)
    {
        u.animVars    = *animVars;
        u.hasAnimVars = true;
    }

    CoSyncTransport::Send(SerializeEntityUpdate(u));
}

void CoSyncNet::HostSpawnNpc(
    uint32_t entityID,
    uint32_t baseFormID,
    const NiPoint3& pos,
    const NiPoint3& rot)
{
    if (!s_initialized || !s_isHost || !s_connected)
        return;

    EntityCreatePacket p{};
    p.entityID = entityID;
    p.type = CoSyncEntityType::NPC;
    p.baseFormID = baseFormID;
    p.ownerEntityID = 0;
    p.spawnFlags = EntityCreatePacket::RemoteControlled;
    p.spawnPos = pos;
    p.spawnRot = rot;

    LOG_INFO("[CoSyncNet] TX CREATE NPC entity=%u base=0x%08X", entityID, baseFormID);

    CoSyncTransport::Send(SerializeEntityCreate(p));


    g_CoSyncPlayerManager.EnqueueEntityCreate(p);
}

void CoSyncNet::OnReceive(const std::string& rawMsg, double now, HSteamNetConnection conn)
{

    const bool isHostRole = s_isHost || (s_pendingInit && s_pendingHostFlag);
    // 0.3.3: a client maps FormIDs from the host's load-order space to its own
    // before anything reads them (CoSyncFormIdMap; identity = no copy).
    std::string translated;
    if (!isHostRole && CoSyncFormIdMap::IsActive())
        translated = CoSyncFormIdMap::Inbound(rawMsg);
    const std::string& msg = translated.empty() ? rawMsg : translated;
    // Remote client messages are checked (P5/N7); conn==Invalid is a local
    // GNS notice (PEERLEFT) and clients trust everything from the host.
    const bool checkSender = isHostRole && conn != k_HSteamNetConnection_Invalid;
    uint32_t senderEid = checkSender ? HostSenderEntityID(conn) : 0;

    // Voice first: 50 packets a second per speaker, no identity field (the
    // host routes by the connection's bound entity).
    if (CoSyncVoice::IsVoiceMessage(msg))
    {
        CoSyncVoice::OnReceive(msg, conn, senderEid, isHostRole);
        return;
    }

    if (checkSender)
    {
        if (const char* type = SelfStateType(msg))
        {
            const uint32_t subject = static_cast<uint32_t>(
                std::strtoul(msg.c_str() + std::strlen(type) + 1, nullptr, 10));
            if (senderEid == 0)
                senderEid = HostBindSender(conn, subject);
            if (!HostSenderOwns(senderEid, subject))
            {
                LogUnauthorized(type, conn, senderEid, subject);
                return;
            }
        }
    }

    // Shared host save: world transfer + player profiles.
    if (CoSyncWorldTransfer::IsTransferMessage(msg))
    {
        CoSyncWorldTransfer::OnReceive(msg, conn, isHostRole);
        return;
    }
    if (CoSyncItemMods::IsItemModsMessage(msg))
    {
        CoSyncItemMods::OnReceive(msg, isHostRole);
        return;
    }
    if (CoSyncCompanions::IsCompanionMessage(msg))
    {
        // COMPOWN's last field is the owner the host will register.
        if (checkSender)
        {
            const uint32_t claimed = LastFieldU32(msg);
            if (senderEid == 0)
                senderEid = HostBindSender(conn, claimed);
            if (claimed != senderEid || senderEid == 0)
            {
                LogUnauthorized("COMPOWN", conn, senderEid, claimed);
                return;
            }
        }
        CoSyncCompanions::OnReceive(msg, isHostRole);
        return;
    }
    if (CoSyncProxyLooks::IsLooksMessage(msg))
    {
        CoSyncProxyLooks::OnReceive(msg, isHostRole);
        return;
    }
    if (CoSyncProfiles::IsProfileMessage(msg))
    {
        CoSyncProfiles::OnReceive(msg, conn, isHostRole);
        return;
    }
    // Factions: FQ (request, host acts on the connection's identity only),
    // FS/FR/FN (host -> clients).
    if (CoSyncFactions::IsFactionMessage(msg))
    {
        CoSyncFactions::OnReceive(msg, conn, isHostRole);
        return;
    }
    // Workshop: WKQ (request, host re-validates), WKA/WKV/WKX/WKR/WKC/WKN
    // (host results).
    if (CoSyncWorkshopAuthority::IsWorkshopMessage(msg))
    {
        CoSyncWorkshopAuthority::OnReceive(msg, conn, isHostRole);
        return;
    }
    // Text chat: CHAT (request; the host validates and picks recipients),
    // CHATM (host -> recipient).
    if (CoSyncChat::IsChatMessage(msg))
    {
        CoSyncChat::OnReceive(msg, conn, isHostRole);
        return;
    }


    if (CoSyncCompanionSync::IsCompanionRequest(msg))
    {
        if (isHostRole)
            CoSyncCompanionSync::OnReceiveRequest(msg);

        return;
    }


    if (CoSyncCompanionSync::IsCompanionDismiss(msg))
    {
        if (isHostRole)
            CoSyncCompanionSync::OnReceiveDismiss(msg);
        return;
    }


    if (CoSyncLootSync::IsSalt(msg))
    {
        if (!isHostRole)
            CoSyncLootSync::OnReceiveSalt(msg);
        return;
    }


    if (CoSyncLootSync::IsPersonalLoot(msg))
    {

        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncLootSync::OnReceivePersonalLoot(msg);
        return;
    }

    // Exactly one client left (GNS_Session, local-only: conn is Invalid, so a
    // remote peer cannot forge it). Remove just that peer; the session and
    // everyone else stay up. Its entity DESTROY was already sent/queued.
    if (msg.rfind("PEERLEFT|", 0) == 0)
    {
        if (!isHostRole || conn != 0)
            return;
        uint64_t sid = 0;
        try { sid = std::stoull(msg.substr(9)); } catch (...) { sid = 0; }

        std::string who;
        size_t remaining = 0;
        {
            std::lock_guard<std::mutex> lk(s_peerMutex);
            auto it = s_peers.find(sid);
            if (it != s_peers.end())
            {
                who = it->second.username;
                s_peers.erase(it);
            }
            remaining = s_peers.size();
        }
        if (sid != 0)
        {
            const uint32_t leftEid = EntityIDFromSteamID(sid);
            for (auto it = s_connSenderEid.begin(); it != s_connSenderEid.end(); )
                it = (it->second == leftEid) ? s_connSenderEid.erase(it) : std::next(it);
        }

        LOG_INFO("[CoSyncNet] Peer left: '%s' sid=%llu (%zu remaining)",
            who.c_str(), (unsigned long long)sid, remaining);

        CoSyncFactions::HostMarkDirty();

        // W2: the NPCs this player owned have no simulating owner any more.
        if (sid != 0)
            CoSyncNpcWorldSync::HostReleaseEntitiesOwnedBy(EntityIDFromSteamID(sid));

        char note[160];
        std::snprintf(note, sizeof(note), "%s left the game", who.empty() ? "A player" : who.c_str());
        CoSyncGameAPI::QueueNotification(note);
        return;
    }

    if (msg.rfind("DISCONNECT|", 0) == 0)
    {
        // On the HOST, a DISCONNECT from a client connection only announces
        // that ONE client is leaving. Its GNS close follows and PEERLEFT does
        // the per-peer cleanup. Running the whole-session teardown here used to
        // drop every other client's proxy too (and ran it twice per leave).
        if (isHostRole && conn != 0)
        {
            LOG_INFO("[CoSyncNet] RX '%s' from conn=%u — that client is leaving", msg.c_str(), conn);
            return;
        }

        LOG_WARN("[CoSyncNet] RX DISCONNECT: '%s' — triggering session cleanup", msg.c_str());

        // Name the peer if we still have them, so the toast says who left
        // rather than a bare "disconnected". Read before OnGNSDisconnected,
        // which clears the peer table.
        {
            std::string who;
            {
                std::lock_guard<std::mutex> lk(s_peerMutex);
                if (s_peers.size() == 1)
                    who = s_peers.begin()->second.username;
            }
            char note[160];
            if (!who.empty())
                std::snprintf(note, sizeof(note), "%s left the game", who.c_str());
            else
                std::snprintf(note, sizeof(note), "Disconnected from the session");
            CoSyncGameAPI::QueueNotification(note);
        }

        OnGNSDisconnected();
        return;
    }


    if (msg.rfind("HELLO|", 0) == 0)
    {
        std::string name;
        uint64_t sid = 0;
        NiPoint3 clientPos(0.f, 0.f, 0.f);
        if (!ParseHello(msg, name, sid, clientPos))
            return;

        const uint32_t eid = EntityIDFromSteamID(sid);

        // P5/N7: a connection keeps the identity of its first HELLO
        // (GNS_Session::BindPeerSteamID); a later HELLO naming someone else
        // must not create or overwrite that player.
        if (checkSender)
        {
            if (senderEid == 0)
                senderEid = HostBindSender(conn, eid);
            if (eid != senderEid)
            {
                LogUnauthorized("HELLO", conn, senderEid, eid);
                return;
            }
        }

        bool isNewPeer = false;
        {
            std::lock_guard<std::mutex> lk(s_peerMutex);
            isNewPeer = (s_peers.find(sid) == s_peers.end());
            s_peers[sid] = { sid, name, eid, now, conn };
            if (!isHostRole)
                s_hostEntityID = eid;   // clients only ever receive the host's HELLO
        }

        // On-screen join toast. Only on a genuinely NEW peer -- a HELLO can be
        // re-sent (retry, reconnect handshake) and re-announcing every one
        // would spam the HUD. Queued rather than shown directly: this runs on
        // the network receive path, and the HUD call must happen on the game
        // thread.
        if (isNewPeer)
        {
            char note[160];
            std::snprintf(note, sizeof(note), "%s joined the game",
                name.empty() ? "A player" : name.c_str());
            CoSyncGameAPI::QueueNotification(note);
        }

        LOG_INFO("[CoSyncNet] RX HELLO name='%s' sid=%llu eid=%u (isHostRole=%d) clientPos=(%.1f,%.1f,%.1f)",
            name.c_str(),
            (unsigned long long)sid,
            eid,
            isHostRole ? 1 : 0,
            clientPos.x, clientPos.y, clientPos.z);

        if (isHostRole)
        {
            LOG_INFO("[CoSyncNet] Processing HELLO from client eid=%u", eid);

            // Every look we know (ours included) before the CREATEs below, so
            // the newcomer spawns each proxy with its owner's face/body.
            CoSyncProxyLooks::HostSendAll();

            // Use the client's own position from HELLO so the proxy spawns
            // at the right location rather than world origin (0,0,0).
            HostBroadcastPlayerCreate(
                eid,
                clientPos,
                NiPoint3{ 0.f, 0.f, 0.f },
                true,
                name
            );

            LOG_INFO("[CoSyncNet] Sent CREATE for client entity=%u name='%s' pos=(%.1f,%.1f,%.1f)",
                eid, name.c_str(), clientPos.x, clientPos.y, clientPos.z);


            const uint32_t hostEID = GetMyEntityID();
            const NiPoint3 hostPos = CoSyncNet_GetLocalPlayerPos();

            HostBroadcastPlayerCreate(
                hostEID,
                hostPos,
                NiPoint3{ 0.f, 0.f, 0.f },
                false,
                s_myName
            );

            s_hostCreatePublished = true;

            LOG_INFO("[CoSyncNet] Sent CREATE for host entity=%u name='%s' pos=(%.1f,%.1f,%.1f)",
                hostEID, s_myName.c_str(), hostPos.x, hostPos.y, hostPos.z);

            // Host owns quest state, so a joining client must be brought to
            // the host's journal immediately rather than drifting until the
            // 15s safety-net rebroadcast.
            CoSyncQuestSync::RequestFullStateBroadcast();

            // Factions and territory: the newcomer needs the whole state,
            // and everyone's online list changed.
            CoSyncFactions::HostOnPeerJoined(conn);
            CoSyncWorkshopAuthority::HostOnPeerJoined(conn);
        }

        return;
    }


    if (IsEntityUpdate(msg))
    {
        EntityUpdatePacket u{};
        if (!DeserializeEntityUpdate(msg, u))
            return;

        if (checkSender && !HostSenderOwns(senderEid, u.entityID))
        {
            LogUnauthorized("EU", conn, senderEid, u.entityID);
            return;
        }

        if (isHostRole)
        {

            u.timestamp = now;
            // N2 (2026-09-26): not back to the sender. Its own EU would be
            // dropped anyway (ProcessEntityUpdate ignores the local entity and
            // entities this machine owns) -- measured 9365 wasted echoes in
            // one 16-minute two-player session.
            CoSyncTransport::SendExcept(SerializeEntityUpdate(u), conn);
        }

        g_CoSyncPlayerManager.EnqueueEntityUpdate(u);
        return;
    }


    if (IsEntityDestroy(msg))
    {
        EntityDestroyPacket d{};
        if (!DeserializeEntityDestroy(msg, d))
            return;

        if (checkSender && !HostSenderOwns(senderEid, d.entityID))
        {
            LogUnauthorized("DESTROY", conn, senderEid, d.entityID);
            return;
        }

        if (CoSyncNpcWorldSync::IsWorldActorEntityID(d.entityID))
        {
            if (isHostRole)
                CoSyncTransport::Send(msg);
            g_CoSyncPlayerManager.NoteEntityDestroyed(d.entityID);
            CoSyncNpcWorldSync::ClientUnbindWorldActor(d.entityID, d.reasonFlags);
            return;
        }

        g_CoSyncPlayerManager.EnqueueEntityDestroy(d);
        return;
    }


    if (IsEntityCreate(msg))
    {
        EntityCreatePacket p{};
        if (!DeserializeEntityCreate(msg, p))
            return;

        // A client may only (re)announce an entity it owns, and only in its
        // own name: a world NPC must name the sender as owner AND match the
        // host's record if the host already has one.
        if (checkSender)
        {
            const bool worldActor = CoSyncNpcWorldSync::IsWorldActorEntityID(p.entityID);
            bool ok;
            if (worldActor)
            {
                const auto& states = g_CoSyncPlayerManager.GetStates();
                auto it = states.find(p.entityID);
                ok = p.ownerEntityID == senderEid &&
                     (it == states.end() || !it->second.hasCreate ||
                      it->second.lastCreate.ownerEntityID == senderEid);
            }
            else
            {
                ok = p.entityID == senderEid;
            }
            if (!ok)
            {
                LogUnauthorized("CREATE", conn, senderEid, p.entityID);
                return;
            }
        }

        if (CoSyncOverlay_IsLoadingScreenActive())
            CoSyncOverlay_SignalLoadingComplete();

        if (CoSyncNpcWorldSync::IsWorldActorEntityID(p.entityID))
        {
            g_CoSyncPlayerManager.ClearEntityDestroyed(p.entityID);
            if (isHostRole)
            {
                // Only client-owned world-actor CREATEs (owner re-broadcasts)
                // are meaningful to the host — relay for other clients, then
                // bind/retry the host's own puppet.
                if (p.ownerEntityID != 0 && p.ownerEntityID != GetMyEntityID())
                {
                    CoSyncTransport::Send(msg);
                    CoSyncNpcWorldSync::ClientBindWorldActor(p);
                }
                return;
            }
            CoSyncNpcWorldSync::ClientBindWorldActor(p);
            return;
        }

        g_CoSyncPlayerManager.EnqueueEntityCreate(p);
        return;
    }


    if (CoSyncWorldSync::IsWorldSync(msg))
    {
        CoSyncWorldSync::WorldSyncPacket wsp{};
        if (CoSyncWorldSync::DeserializeWorldSync(msg, wsp))
            CoSyncWorldSync::OnReceiveWorldSync(wsp);
        return;
    }


    if (msg.rfind("FU|", 0) == 0)
    {
        CoSyncWorldSync::OnReceiveFurniture(msg);
        return;
    }


    // WM / WD: notices only (logged). The sender check above (SelfStateType)
    // makes sure a client can only speak for itself; the host passes them on
    // to everyone except the sender (U7: they used to go back to it as well).
    if (msg.rfind("WM|", 0) == 0)
    {
        if (isHostRole && conn != k_HSteamNetConnection_Invalid)
            CoSyncTransport::SendExcept(msg, conn);
        CoSyncWorkshopSync::OnReceiveWorkshopMode(msg);
        return;
    }


    if (msg.rfind("WD|", 0) == 0)
    {
        if (isHostRole && conn != k_HSteamNetConnection_Invalid)
            CoSyncTransport::SendExcept(msg, conn);
        CoSyncWorkshopSync::OnReceiveWorkshopDestroy(msg);
        return;
    }


    if (msg.rfind("TD|", 0) == 0)
    {
        CoSyncTimeSync::OnReceiveTime(msg);
        return;
    }


    if (msg.rfind("WX|", 0) == 0)
    {
        CoSyncTimeSync::OnReceiveWeather(msg);
        return;
    }


    if (CoSyncHealthSync::IsHP(msg))
    {

        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncHealthSync::OnReceiveHP(msg);
        return;
    }

    if (CoSyncHealthSync::IsKill(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncHealthSync::OnReceiveKill(msg);
        return;
    }

    // Authoritative corpse transform from the NPC's owner (sender-checked as
    // self-state: only the owner may speak for it).
    if (CoSyncHealthSync::IsCorpse(msg))
    {
        if (isHostRole)
            CoSyncTransport::SendExcept(msg, conn);
        CoSyncHealthSync::OnReceiveCorpse(msg);
        return;
    }

    if (msg.rfind("VER|", 0) == 0)
    {
        const char* p = msg.c_str() + 4;
        char* end = nullptr;
        const uint32_t peerEid = (uint32_t)strtoul(p, &end, 10);
        uint32_t peerBuild = 0;
        if (end && *end == '|')
            peerBuild = (uint32_t)strtoul(end + 1, nullptr, 10);

        if (peerBuild != kCoSyncBuildNumber)
        {
            LOG_ERROR("[CoSyncNet] ============================================================");
            LOG_ERROR("[CoSyncNet] BUILD MISMATCH: peer eid=%u runs CoSync build %u, this machine runs build %u!", peerEid, peerBuild, kCoSyncBuildNumber);
            LOG_ERROR("[CoSyncNet] ============================================================");

            // Mixed builds desync silently, so the session refuses them: the
            // host disconnects that client (it sees VERSION MISMATCH), and a
            // client facing a host that doesn't enforce this leaves by itself.
            if (isHostRole && conn != 0)
                GNS_Session::Get().KickClient(conn, CoSyncSessionConfig::kEndReason_VersionMismatch, "version_mismatch");
            else if (!isHostRole)
                GNS_Session::Get().LeaveWithReason(CoSyncSessionConfig::kEndReason_VersionMismatch, "version_mismatch");
            return;
        }
        else
        {
            LOG_INFO("[CoSyncNet] Peer eid=%u build %u matches — versions OK", peerEid, peerBuild);
        }
        if (isHostRole)
            CoSyncTransport::Send(msg);   // relay matching builds only
        return;
    }

    if (CoSyncHealthSync::IsDmg(msg))
    {
        // Host relays so every peer sees the damage report; each machine's
        // flush decides whether the entity is one it owns (see
        // CoSyncHealthSync::FlushPendingHealthOps).
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncHealthSync::OnReceiveDmg(msg);
        return;
    }

    // 0.3.3 (Stage 6): revive messages are sender-checked like the other
    // self-state messages. DOWN: only about yourself. REVIVEREQ: only as the
    // reviver. REVIVED: as the reviver, or about yourself (reviver 0 = got
    // up unaided).
    if (checkSender && (CoSyncReviveSync::IsDown(msg) || CoSyncReviveSync::IsReviveReq(msg) ||
                        CoSyncReviveSync::IsRevived(msg)))
    {
        const char* bar = std::strchr(msg.c_str(), '|');
        char* end = nullptr;
        const uint32_t target  = bar ? static_cast<uint32_t>(std::strtoul(bar + 1, &end, 10)) : 0u;
        const uint32_t reviver = (end && *end == '|') ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 10)) : 0u;
        bool ok;
        if (CoSyncReviveSync::IsDown(msg))           ok = (target == senderEid);
        else if (CoSyncReviveSync::IsReviveReq(msg)) ok = (reviver == senderEid);
        else                                          ok = (reviver == senderEid) || (reviver == 0 && target == senderEid);
        if (!ok)
        {
            LogUnauthorized("REVIVE", conn, senderEid, target);
            return;
        }
    }

    if (CoSyncReviveSync::IsDown(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncReviveSync::OnReceiveDown(msg);
        return;
    }

    if (CoSyncReviveSync::IsReviveReq(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncReviveSync::OnReceiveReviveReq(msg);
        return;
    }

    if (CoSyncReviveSync::IsRevived(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncReviveSync::OnReceiveRevived(msg);
        return;
    }


    if (CoSyncInventorySync::IsInventory(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncInventorySync::OnReceiveInventory(msg);
        return;
    }


    if (CoSyncMapSync::IsMapMarker(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncMapSync::OnReceiveMapMarker(msg);
        return;
    }


    if (CoSyncPowerArmorSync::IsPowerArmor(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncPowerArmorSync::OnReceivePowerArmor(msg);
        return;
    }

    if (CoSyncPowerArmorSync::IsPowerArmorOwnership(msg))
    {
        // Host-originated; the host never relays a peer's copy.
        if (!isHostRole)
            CoSyncPowerArmorSync::OnReceivePowerArmorOwnership(msg);
        return;
    }

    if (CoSyncPowerArmorSync::IsPowerArmorMods(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncPowerArmorSync::OnReceivePowerArmorMods(msg);
        return;
    }
    if (CoSyncPowerArmorSync::IsPowerArmorInventory(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncPowerArmorSync::OnReceivePowerArmorInventory(msg);
        return;
    }

    if (CoSyncNpcWorldSync::IsNpcDead(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        else
            CoSyncNpcWorldSync::OnReceiveNpcDead(msg);
        return;
    }

    if (CoSyncQuestSync::IsQuestStart(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncQuestSync::OnReceiveQuestStart(msg);
        return;
    }

    if (CoSyncQuestSync::IsQuestDone(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncQuestSync::OnReceiveQuestDone(msg);
        return;
    }

    if (CoSyncQuestSync::IsQuestObjAll(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncQuestSync::OnReceiveObjAll(msg);
        return;
    }

    if (CoSyncQuestSync::IsQuestObjSet(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncQuestSync::OnReceiveObjSet(msg);
        return;
    }

    if (CoSyncQuestSync::IsQuestObjDisp(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncQuestSync::OnReceiveObjDisp(msg);
        return;
    }

    if (CoSyncQuestSync::IsQuestStage(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncQuestSync::OnReceiveQuestStage(msg);
        return;
    }

    // Re-broadcast an entity's stored EntityCreate on request. A peer that lost
    // create state (loading-screen timeout, packet loss) would otherwise drop
    // that entity's updates forever with no way to recover -- see
    // ProcessEntityUpdate's no-create branch. Answered by anyone still holding
    // the create, which in practice is the host, and it works for players and
    // world NPCs alike since both keep lastCreate in their state.
    if (msg.rfind("REQENT|", 0) == 0)
    {
        const uint32_t reqEid = static_cast<uint32_t>(std::strtoul(msg.c_str() + 7, nullptr, 10));
        if (reqEid != 0)
        {
            const auto& states = g_CoSyncPlayerManager.GetStates();
            auto itReq = states.find(reqEid);
            if (itReq != states.end() && itReq->second.hasCreate)
            {
                CoSyncTransport::Send(SerializeEntityCreate(itReq->second.lastCreate));
                LOG_UPDATED("[CoSyncNet] REQENT entity=%u — re-broadcast CREATE", reqEid);
            }
            else
            {
                LOG_UPDATED_WARN("[CoSyncNet] REQENT entity=%u — no create held here, ignoring", reqEid);
            }
        }
        return;
    }

    if (msg.rfind("AE|", 0) == 0)
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);

        uint32_t eid = 0;
        char eventBuf[64] = {};

        if (std::sscanf(msg.c_str() + 3, "%u|%63s", &eid, eventBuf) == 2)
        {
            // Echo guard: GNS relays a broadcast back to its originator, so an
            // AE| the local player forwarded for itself comes right back with
            // its OWN entityID. Replaying it onto the local player would
            // double-drive its own graph -- relay it onward (done above) but do
            // not apply it here.
            if (eid == GetMyEntityID())
                return;

            CoSyncPlayer* pl = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
            if (pl && pl->actorRef)
            {

                CoSyncAnimEventHook::SetApplyingRemote(true);
                const bool acc = CoSyncAnimUtils::SendEvent(pl->actorRef, eventBuf);
                CoSyncAnimEventHook::SetApplyingRemote(false);
                LOG_UPDATED("[AnimEvent RX] entity=%u event='%s' proxy=%p accepted=%d",
                    eid, eventBuf, static_cast<void*>(pl->actorRef), acc ? 1 : 0);
            }
            else
            {
                LOG_UPDATED_WARN("[AnimEvent RX] entity=%u event='%s' — proxy not ready (will arrive via snapshot fallback)",
                    eid, eventBuf);
            }
        }
        return;
    }

    if (msg.rfind("SW|", 0) == 0)
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);

        uint32_t eid = 0; int swimVal = 0;
        if (std::sscanf(msg.c_str() + 3, "%u|%d", &eid, &swimVal) == 2)
        {
            CoSyncPlayer* pl = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
            if (pl)
            {
                pl->m_pendingSwimming = (swimVal != 0);
                pl->m_hasSwimPending  = true;
                LOG_UPDATED("[SwimSync RX] SW packet: entity=%u swimming=%d", eid, swimVal);
            }
            else
            {
                LOG_UPDATED_WARN("[SwimSync RX] SW packet: entity=%u not found (proxy not spawned yet)", eid);
            }
        }
        return;
    }


    if (msg.rfind("PB|", 0) == 0)
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);


        uint32_t eid = 0; int opening = 0;
        if (std::sscanf(msg.c_str() + 3, "%u|%d", &eid, &opening) == 2)
        {

            CoSyncPlayer* pl = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
            if (pl)
            {
                pl->m_pendingPipboyOpen = (opening != 0);
                LOG_UPDATED("[PipSync RX] PB packet applied: entity=%u opening=%d proxy=%p name='%s'",
                    eid, opening,
                    static_cast<void*>(pl),
                    pl->username.empty() ? "?" : pl->username.c_str());
            }
            else
            {
                LOG_UPDATED_WARN("[PipSync RX] PB packet: entity=%u not found (proxy not spawned yet)", eid);
            }
        }
        return;
    }


    if (CoSyncConsumableSync::IsConsumable(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncConsumableSync::OnReceiveConsumable(msg);
        return;
    }


    if (CoSyncConsumableSync::IsSpell(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncConsumableSync::OnReceiveSpell(msg);
        return;
    }


    if (CoSyncSpecialSync::IsSpecial(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncSpecialSync::OnReceiveSpecial(msg);
        return;
    }


    if (CoSyncVatsSync::IsVatsEnter(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncVatsSync::OnReceiveVatsEnter(msg);
        return;
    }
    if (CoSyncVatsSync::IsVatsExit(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncVatsSync::OnReceiveVatsExit(msg);
        return;
    }


    if (CoSyncWorldSync::IsObjectMove(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncWorldSync::OnReceiveObjectMove(msg);
        return;
    }

    if (CoSyncExplosionSync::IsExplosion(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncExplosionSync::OnReceiveExplosion(msg);
        return;
    }

    // Shared dialogue (0.3.5): host conversation shown to nearby clients;
    // their choices come to the host and are never relayed.
    if (CoSyncSharedDialogue::IsSharedDialogueMsg(msg))
    {
        CoSyncSharedDialogue::OnReceive(msg, isHostRole);
        return;
    }

    // World-object state (0.3.5): destruction, open/close, script enable.
    // Destruction/open reports are relayed from anyone; script enable/disable
    // is the host's alone (the receiver refuses it on the host).
    if (CoSyncWorldStateSync::IsWorldStateMsg(msg))
    {
        if (isHostRole && msg[2] != 'E')
            CoSyncTransport::Send(msg);
        CoSyncWorldStateSync::OnReceive(msg, isHostRole);
        return;
    }

    // A shot, decided on the machine that simulates the shooter (0.3.5).
    if (CoSyncCombatAuthority::IsShot(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncCombatAuthority::OnReceiveShot(msg);
        return;
    }

    // Companion to the explosion replay above: the grenade/missile IN FLIGHT,
    // relayed on the same role-independent terms (a projectile belongs to
    // whoever launched it, not to the host).
    if (msg.rfind("AEV|", 0) == 0)
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);

        const char* s = msg.c_str() + 4;
        char* e = nullptr;
        const uint32_t eid  = static_cast<uint32_t>(std::strtoul(s, &e, 10));
        if (!e || *e != '|' || eid == 0) return;
        const uint32_t code = static_cast<uint32_t>(std::strtoul(e + 1, nullptr, 10));

        // The host relays to every client including the originator, so drop
        // our own echo (same guard as the throwable messages).
        if (eid == GetMyEntityID()) return;

        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        if (!p) return;

        switch (code)
        {
        case kAnimEv_Fire:         p->m_evFire.fetch_add(1);         break;
        case kAnimEv_ReloadStart:  p->m_evReloadStart.fetch_add(1);  break;
        case kAnimEv_ReloadEnd:    p->m_evReloadEnd.fetch_add(1);    break;
        case kAnimEv_ThrowGrenade: p->m_evThrowGrenade.fetch_add(1); break;
        case kAnimEv_ThrowMine:    p->m_evThrowMine.fetch_add(1);    break;
        case kAnimEv_ThrowEnd:     p->m_evThrowEnd.fetch_add(1);     break;
        default: break;
        }
        return;
    }

    if (CoSyncThrowableSync::IsThrowableMsg(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncThrowableSync::OnReceiveThrowableMsg(msg);
        return;
    }


    if (CoSyncQuestSync::IsQuestTrack(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncQuestSync::OnReceiveQuestTrack(msg);
        return;
    }

    if (CoSyncWorldSync::IsStateRequest(msg))
    {
        CoSyncWorldSync::OnReceiveStateRequest();
        return;
    }


    if (CoSyncOutfitSync::IsOutfit(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncOutfitSync::OnReceiveOutfit(msg);
        return;
    }


    if (CoSyncDialogSync::IsDialogue(msg))
    {


        CoSyncDialogSync::OnReceiveDialogue(msg);
        return;
    }


    if (msg.rfind("PU|", 0) == 0)
    {
        CoSyncWorldSync::OnReceivePickup(msg);
        return;
    }

    if (CoSyncNpcWorldSync::IsLegendary(msg))
    {
        CoSyncNpcWorldSync::OnReceiveLegendary(msg);
        return;
    }

    if (CoSyncNpcWorldSync::IsNpcSeen(msg))
    {
        // The reporter (last field) becomes the NPC's owner.
        if (checkSender)
        {
            const uint32_t claimed = LastFieldU32(msg);
            if (senderEid == 0)
                senderEid = HostBindSender(conn, claimed);
            if (claimed != senderEid || senderEid == 0)
            {
                LogUnauthorized("NPCSEEN", conn, senderEid, claimed);
                return;
            }
        }
        CoSyncNpcWorldSync::OnReceiveNpcSeen(msg);
        return;
    }

    if (CoSyncItemDropSync::IsDropSeen(msg))
    {
        // Client->host only, never needs relay (only the host acts on it).
        CoSyncItemDropSync::OnReceiveDropSeen(msg);
        return;
    }

    if (CoSyncItemDropSync::IsDropCreate(msg))
    {
        // Always host-originated (HostAllocateDrop), so Send() from the host
        // already reaches every connected client directly — no relay needed.
        CoSyncItemDropSync::OnReceiveDropCreate(msg);
        return;
    }

    if (CoSyncItemDropSync::IsDropGone(msg))
    {
        // Can originate from ANY peer (whoever picked the item up) — relay
        // on host receipt so a 3rd+ peer also sees it, same pattern as
        // DMG/HP/KILL.
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncItemDropSync::OnReceiveDropGone(msg);
        return;
    }

    // NOTE: test CTRD| BEFORE CTR| -- "CTR|" is a prefix of neither, but the
    // deposit tag shares the first three chars, so ordering keeps it obvious.
    if (CoSyncContainerSync::IsContainerDeposit(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncContainerSync::OnReceiveContainerDeposit(msg);
        return;
    }

    if (CoSyncContainerSync::IsContainerSync(msg))
    {
        CoSyncContainerSync::OnReceiveContainerSync(msg);
        return;
    }

    if (CoSyncSceneSync::IsSceneSync(msg))
    {
        CoSyncSceneSync::OnReceiveSceneSync(msg);
        return;
    }


    if (CoSyncWorldSync::IsWorkshopBuildFull(msg))
    {
        CoSyncWorldSync::WorkshopBuildFullPacket wbp{};
        if (CoSyncWorldSync::DeserializeWorkshopBuildFull(msg, wbp))
            CoSyncWorldSync::OnReceiveWorkshopBuildFull(wbp);
        return;
    }

    if (CoSyncWorldSync::IsWorkshopScrapFull(msg))
    {
        CoSyncWorldSync::WorkshopScrapFullPacket wsp{};
        if (CoSyncWorldSync::DeserializeWorkshopScrapFull(msg, wsp))
            CoSyncWorldSync::OnReceiveWorkshopScrapFull(wsp);
        return;
    }

    if (CoSyncEquipHook::IsEquip(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncEquipHook::OnReceiveEquip(msg);
        return;
    }

    if (CoSyncPlayerAnimation::IsBonesPacket(msg))
    {
        // Relayed unreliable for the same reason it is sent unreliable --
        // see the send site in CoSynclocalplayer.cpp. The host relays roughly
        // 21 KB/s of pose data per connected peer; none of it is worth
        // retransmitting or blocking the stream for.
        // N2: not back to the sender -- a player has no proxy of itself, so
        // its own pose is never applied (measured: 48% of a client's BONES
        // download was its own echo, ~56 KB/s).
        // P5/N7: a player only ever sends its own skeleton.
        if (checkSender)
        {
            const uint32_t boneEid = static_cast<uint32_t>(std::strtoul(msg.c_str() + 6, nullptr, 10));
            if (boneEid != senderEid || senderEid == 0)
            {
                LogUnauthorized("BONES", conn, senderEid, boneEid);
                return;
            }
        }
        if (isHostRole)
            CoSyncTransport::SendExcept(msg, conn, /*reliable=*/false);
        CoSyncPlayerAnimation::OnReceiveBones(msg);
        return;
    }

    if (CoSyncDoorHook::IsDoork(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncDoorHook::OnReceiveDoor(msg);
        return;
    }

    if (CoSyncLockHook::IsLock(msg))
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncLockHook::OnReceiveLock(msg);
        return;
    }

    if (CoSyncModSync::IsLoadOrderMessage(msg))
    {
        std::string who;
        if (isHostRole)
            for (const RosterEntry& e : GetRoster())
                if (e.entityID == senderEid) { who = e.name; break; }
        CoSyncModSync::OnReceiveLoadOrderMessage(msg, conn, isHostRole, who);
        return;
    }

    if (CoSyncModSync::IsModList(msg))
    {
        if (isHostRole)
            CoSyncModSync::OnReceiveModList(msg, conn);
        return;
    }

    if (CoSyncModSync::IsModFile(msg))
    {
        if (!isHostRole)
            CoSyncModSync::OnReceiveModFile(msg);
        return;
    }

    if (msg.rfind("MODEVENT|", 0) == 0)
    {
        if (isHostRole)
            CoSyncTransport::Send(msg);
        CoSyncModEventBus::OnReceive(msg);
        return;
    }

    if (msg.rfind("CUSTOMEVENT|", 0) == 0)
    {
        // S-SEC1 (2026-09-26): a CUSTOMEVENT fires an arbitrary Papyrus custom
        // event (any form, event name and arguments) on every machine. The
        // sending side (InstallModEventHook) is disabled, so no legitimate
        // peer of this build ever sends one -- accept and relay only while
        // this machine has the feature enabled too.
        if (!IsModEventHookInstalled())
        {
            static uint32_t s_dropped = 0;
            if ((s_dropped++ % 50) == 0)
                LOG_WARN("[CoSyncNet] Dropped CUSTOMEVENT from conn=%u -- custom-event relay is disabled on this build (%u dropped)",
                    conn, s_dropped);
            return;
        }
        if (isHostRole)
            CoSyncTransport::Send(msg);

        const char* p = msg.c_str() + 12; // skip "CUSTOMEVENT|"
        const char* sep1 = strchr(p, '|');
        if (!sep1) return;
        uint32_t formID = (uint32_t)strtoul(p, nullptr, 10);

        const char* sep2 = strchr(sep1 + 1, '|');
        if (!sep2) return;
        std::string eventName(sep1 + 1, sep2 - (sep1 + 1));
        std::string serializedArgs(sep2 + 1);

        QueueRemoteCustomEvent(formID, eventName, serializedArgs);
        return;
    }

    (void)conn;
}

void CoSyncNet::OnGNSConnected()
{
    s_connected = true;

    // Crash guard: mark the local player Essential for the session so lethal
    // damage collapses/bleeds out instead of reaching the vanilla death menu ->
    // "Continue" -> LoadGame, which crashes on a CoSync proxy ref baked into the
    // save (2026-08-08 host crash via BGSLoadFormBuffer/ExtraAttachRef). NOT the
    // revive gameplay (removed) -- just death-menu avoidance. Cleared on disconnect.
    CoSyncGameAPI::SetLocalPlayerEssential(true);

    // Reflect connection state in the HUD immediately (1 player = us)

    // Transport-level toast. On the host this fires when the session opens
    // (peers get their own "X joined" from the HELLO handler); on a client it
    // is the "you're in" confirmation.
    CoSyncGameAPI::QueueNotification(
        (s_isHost || (s_pendingInit && s_pendingHostFlag))
            ? "CoSync: session started — waiting for players"
            : "CoSync: connected to host");

    g_CoSyncPlayerManager.SetLocalEntityID(GetMyEntityID());


    if (s_isHost || (s_pendingInit && s_pendingHostFlag))
    {
        LOG_INFO("[CoSyncNet] Host transport connected. localEID=%u", GetMyEntityID());

        if (CoSyncLootSync::GetSessionSalt() == 0)
        {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            uint32_t salt = static_cast<uint32_t>(qpc.LowPart ^ qpc.HighPart)
                          ^ static_cast<uint32_t>(GetMyEntityID() * 0x6364136Du);
            if (salt == 0) salt = 0xDEADBEEFu;

            CoSyncLootSync::SetSessionSalt(salt);
            CoSyncTransport::Send(CoSyncLootSync::SerializeSalt(salt));
            LOG_UPDATED("[CoSyncNet] TX SALT 0x%08X", salt);
        }

        return;
    }


    if (s_helloSent)
        return;

    // ── Do not announce ourselves until we can actually receive (item #10) ──
    // HELLO is what makes the host start streaming entity CREATEs, quest
    // state, NPC binds and world sync at us. Sending it the moment the
    // transport connects meant the host began that flood while this client was
    // still loading its save -- CoSyncNet::Init cannot run until
    // CoSyncWorld::IsWorldReady(), and the 2026-08-04 client log shows a
    // 20.7-SECOND gap between HELLO (22:11:01.762) and Init (22:11:22.450)
    // carrying 2484 [Transport] lines. Every one of those was buffered into an
    // inbox with a hard cap that silently drops from the FRONT when full, so
    // the client both stalled chewing through the backlog at Init AND could
    // lose the earliest state it was sent.
    //
    // Waiting for world-ready costs nothing (the session is unusable until
    // then regardless) and means the host starts talking exactly when we can
    // start listening.
    //
    // 2026-08-04 REGRESSION FIX: this used to `return` from OnGNSConnected
    // when the world was not ready. OnGNSConnected is a ONE-SHOT connect
    // callback -- it never runs again -- so the HELLO was never sent at all.
    // Live proof: the client logged "Holding HELLO..." and then no "Sent
    // HELLO" line for the whole session; the host consequently never spawned a
    // player proxy (no SpawnTask, zero type=Player), which took out the proxy,
    // the map marker and the overlay card in one go.
    //
    // The retry now lives in TrySendHello(), which CoSyncNet::Tick calls every
    // frame until it succeeds.
    //
    // Shared host save: first ask for the host's world. HELLO waits (see
    // TrySendHello) until it has been downloaded and loaded.
    CoSyncWorldTransfer::ClientBegin(CoSyncProfiles::GetMyProfileKey(), GetMyName());
    TrySendHello();
}

// Sends HELLO once the local save has finished loading. Safe to call every
// frame: no-ops if already sent, not connected, or the world is not ready yet.
void CoSyncNet::TrySendHello()
{
    LOG_REQUIREMENT("CoSyncNet::TrySendHello", "Safe to call every frame — does nothing unless connected, not already sent, and the world is ready. Needs to be reachable from the per-frame tick, since the connect callback only fires once.");
    if (s_helloSent || !s_connected)
        return;

    // Clients introduce themselves only once they are in the host's world,
    // wearing their OWN character (not the host's copy the save came with).
    if (!s_isHost && (!CoSyncWorldTransfer::IsClientWorldReady() || !CoSyncProfiles::IsCharacterApplied()))
        return;

    if (!CoSyncWorld::IsWorldReady())
    {
        static uint32_t s_waitTicks = 0;
        if ((s_waitTicks++ % 300) == 0)
            LOG_INFO("[CoSyncNet] Holding HELLO until the save has finished loading "
                     "(world not ready yet) — avoids the host streaming state into a backlog");
        return;
    }

    s_helloSent = true;

    // Our face/body first: HELLO is what makes the host announce our proxy,
    // and the look must already be there when it spawns (CoSyncProxyLooks).
    CoSyncProxyLooks::SendMine();

    std::ostringstream ss;
    const NiPoint3 myPos = CoSyncNet_GetLocalPlayerPos();
    ss << "HELLO|" << GetMyName() << "|" << GetMySteamID()
       << "|" << myPos.x << "|" << myPos.y << "|" << myPos.z;
    CoSyncTransport::Send(ss.str());

    // Send our plugin list so host can push any missing mods before we load
    CoSyncModSync::ClientSendModList();
    // 0.3.3 (Stage 9): and the ACTIVE load order, which FormIDs depend on.
    CoSyncModSync::ClientSendLoadOrder();

    CoSyncOverlay_ShowLoadingScreen(true);

    LOG_INFO("[CoSyncNet] Sent HELLO name='%s' localEID=%u",
        s_myName.c_str(), GetMyEntityID());
}

std::vector<CoSyncNet::RosterEntry> CoSyncNet::GetRoster()
{
    LOG_REQUIREMENT("CoSyncNet::GetRoster", "Game thread only: reads the player manager's live player table.");
    std::vector<RosterEntry> out;

    const uint32_t myEid = GetMyEntityID();
    const bool iAmHost = s_isHost || (s_pendingInit && s_pendingHostFlag);

    RosterEntry me;
    me.name     = GetMyName();
    me.entityID = myEid;
    me.isHost   = iAmHost;
    me.isLocal  = true;
    me.inGame   = CoSyncWorld::IsWorldReady();
    out.push_back(me);

    for (const auto& kv : g_CoSyncPlayerManager.GetAllPlayers())
    {
        const CoSyncPlayer* p = kv.second.get();
        if (!p || p->createType != CoSyncEntityType::Player)
            continue;
        if (p->isLocalPlayer || p->entityID == myEid)
            continue;

        RosterEntry e;
        e.name     = p->username.empty() ? std::string("Player") : p->username;
        e.entityID = p->entityID;
        e.isHost   = !iAmHost && p->entityID == s_hostEntityID;
        e.inGame   = p->hasSpawned;
        // Latency: the host measures each client's own connection; a client
        // only has a connection to the host (other clients go through it).
        if (iAmHost)
        {
            std::lock_guard<std::mutex> lk(s_peerMutex);
            for (const auto& pk : s_peers)
                if (pk.second.entityID == p->entityID)
                {
                    e.pingMs = GNS_Session::Get().GetPingMs(pk.second.conn);
                    break;
                }
        }
        else if (e.isHost)
        {
            e.pingMs = GNS_Session::Get().GetPingMs(k_HSteamNetConnection_Invalid);
        }
        out.push_back(e);
    }

    // Host first, then by name.
    std::stable_sort(out.begin() + 1, out.end(), [](const RosterEntry& a, const RosterEntry& b)
    {
        if (a.isHost != b.isHost) return a.isHost;
        return a.name < b.name;
    });
    return out;
}

int CoSyncNet::GetJoiningCount()
{
    if (!(s_isHost || (s_pendingInit && s_pendingHostFlag)))
        return 0;
    size_t introduced = 0;
    {
        std::lock_guard<std::mutex> lk(s_peerMutex);
        introduced = s_peers.size();
    }
    const int connected = GNS_Session::Get().GetConnectedClientCount();
    const int joining = connected - static_cast<int>(introduced);
    return joining > 0 ? joining : 0;
}

std::vector<CoSyncNet::PeerInfo> CoSyncNet::HostGetPeers()
{
    std::vector<PeerInfo> out;
    std::lock_guard<std::mutex> lk(s_peerMutex);
    for (const auto& kv : s_peers)
        out.push_back({ kv.second.conn, kv.second.entityID, kv.second.username });
    return out;
}

std::vector<uint32_t> CoSyncNet::GetConnectedPeerEntityIDs()
{
    std::vector<uint32_t> out;
    std::lock_guard<std::mutex> lk(s_peerMutex);
    out.reserve(s_peers.size());
    for (const auto& kv : s_peers)
        if (kv.second.entityID != 0 && kv.second.entityID != s_myEntityID)
            out.push_back(kv.second.entityID);
    return out;
}

void CoSyncNet::OnGNSDisconnected()
{

    const bool wasHost = s_isHost;

    s_connected           = false;
    s_helloSent           = false;
    s_hostCreatePublished = false;
    s_verSent             = false;
    s_hostEntityID        = 0;

    CoSyncWorldTransfer::Reset();
    CoSyncProfiles::Reset();
    CoSyncProxyLooks::Reset();
    CoSyncCompanions::Reset();
    CoSyncItemMods::Reset();

    // Clear the Essential crash guard so solo play afterward is unaffected.
    CoSyncGameAPI::SetLocalPlayerEssential(false);

    LOG_WARN("[CoSyncNet] Session ended — was host: %d", wasHost ? 1 : 0);

    // Reset the overlay connection state so the HUD shows "Server Offline"
    CoSyncOverlay_Disconnect();

    if (!wasHost)
        CoSyncOverlay_ShowLoadingScreen(false);

    const auto& allPlayers = g_CoSyncPlayerManager.GetAllPlayers();
    std::vector<uint32_t> entityIDs;
    entityIDs.reserve(allPlayers.size());
    for (const auto& kv : allPlayers)
        entityIDs.push_back(kv.first);

    for (uint32_t eid : entityIDs)
    {
        if (eid == GetMyEntityID())
            continue;

        EntityDestroyPacket dp{};
        dp.entityID = eid;
        g_CoSyncPlayerManager.EnqueueEntityDestroy(dp);
        LOG_UPDATED("[CoSyncNet] Disconnect cleanup: queued DESTROY entity=%u", eid);
    }
}
