#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncDialogSync.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncGameAPI.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameMenus.h"
#include "GameCamera.h"
#include "Relocation.h"

#include "MinHook.h"

#include <atomic>
#include <mutex>
#include <deque>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstdio>

// Was extern RelocPtr<PlayerCharacter*> g_player -- switched to CoSyncGetPlayer()
// (see GameGlobals.cpp), the established, confirmed-working replacement for
// raw g_player dereferences used throughout the rest of the codebase.
extern PlayerCharacter* CoSyncGetPlayer();
extern RelocPtr<UI*>              g_ui;
extern RelocAddr<_LookupFormByID> LookupFormByID;
extern CoSyncEntityRegistry       g_CoSyncEntities;

// ============================================================================
// Dialogue line sync -- engine addresses
// ============================================================================
//
// Both RVAs derived 2026-08-05 from Fallout4_disk.i64 (the canonical disk IDB,
// NOT the skewed dump IDB) via headless idat.exe.
//
// PLAYBACK: the internal that Papyrus ObjectReference.Say delegates to.
// The Say native (0x0115C710) is a pure argument shuffle that TAIL-JUMPS here,
// so the internal inherits the native's frame verbatim. Reading that shuffle
// off the raw disassembly pins the signature exactly:
//
//     Say native in:  rcx=vm  edx=stackId  r8=self  r9=akTopicToSay
//                     [rsp+28]=akActorToSpeakAs
//                     [rsp+30]=abSpeakInPlayersHead
//                     [rsp+38]=akTarget
//     shuffled to:    rcx=self  rdx=topic  r8=0  r9=akActorToSpeakAs
//                     [rsp+28]=akTarget  [rsp+30]=abSpeakInPlayersHead(byte)
//
// The Papyrus declaration in the game's own ObjectReference.psc --
//   Function Say(Topic akTopicToSay, Actor akActorToSpeakAs = None,
//                bool abSpeakInPlayersHead = false,
//                ObjectReference akTarget = None) native
// -- names those four slots, and the internal's prologue corroborates every
// one of them: it checks `cmp byte ptr [rbx+1Ah], 41h` (self must be an ACHR)
// and gates sub_140307DE0(0xC1), the in-player's-head voice lookup, on the
// [rsp+30] byte. r8 is 0 from Say; only SayCustom passes a resolved form
// there, so 0 is the correct value for our use.
//
// NOTE it takes no VM and no Papyrus stack ID -- unlike SetCurrentStageID,
// this one is genuinely safe to call directly from C++ (see the quest-hook
// writeup for why that distinction matters).
using SayInternalFn = void(__fastcall*)(
    void* self,          // rcx      speaking TESObjectREFR (must be an Actor)
    void* topic,         // rdx      the Topic form to speak
    void* unkZero,       // r8       0 for Say
    void* speakAs,       // r9       akActorToSpeakAs (null = self speaks)
    void* target,        // [rsp+28] akTarget
    bool  inPlayersHead);// [rsp+30] abSpeakInPlayersHead
static RelocAddr<SayInternalFn> g_sayInternal(0x01183550);

// DETECTION: BuildDialogueItem(topic, speaker, ...) -> heap DialogueItem*.
// This is the universal chokepoint -- 9 callers spanning menu-driven player
// dialogue (0x004F4580 / 0x004F5160), the line-start family (0x005C8880 /
// 0x005C8E70 / 0x00C7D400), scene dialogue (0x0063E900 / 0x00D18110) and
// Papyrus Say (0x01183200). Argument order confirmed independently at two
// call sites: the Say internal and 0x00C7D400 both pass (topic, speaker).
// Hooking here sees every spoken line together with who is speaking it.
static constexpr uintptr_t kBuildDialogueItem_RVA = 0x00698810;

using BuildDialogueItemFn = void* (__fastcall*)(
    void* topic, void* speaker, int a3, void* a4, void* a5, void* a6);
static BuildDialogueItemFn g_origBuildDialogueItem = nullptr;
static std::atomic<bool>   g_lineHookInstalled{ false };

// TESForm layout (both verified against this build elsewhere in the project):
static constexpr size_t kForm_FormID_Offset   = 0x14;
static constexpr size_t kForm_FormType_Offset = 0x1A;
static constexpr uint8_t kFormType_ACHR_Local = 0x41;   // 65, actor reference

// Our own client-side replay calls the engine's Say, which builds a dialogue
// item and would therefore re-enter the detour. Thread-local so a replay on
// the main thread cannot suppress a genuine line detected on another.
static thread_local bool t_inOurPlayback = false;

typedef bool (*IsMenuOpen_t)(UI*, const BSFixedString&);

// RVA CORRECTED round 104: re-derived from scratch rather than re-guessing
// another offset near the same disproven neighborhood. F4SE's own SDK
// (f4se-0.7.8/f4se/GameMenus.h:476, already shipped with this exact project)
// declares UI::IsMenuOpen at 0x01A7FE60 -- a different, independently-sourced
// address from both CoSync's old broken 0x01A7FD40 and round 44's
// "real-function-start-of-something-else" 0x01A7FA40. Verified that address
// against the live 1.11.221 binary via IDA before trusting it (per this
// whole campaign's standing rule, not even F4SE's own SDK gets a pass): IDA
// shows 0x01A7FE60 itself is NOT a function entry either (entry_match=False)
// -- but it lands inside a genuinely real function bounded
// 0x1A7FDD0-0x1A7FE8C, with 3 real callers, that decompiles to exactly
// IsMenuOpen's expected shape: takes a read-write lock, walks a bucket array
// comparing each entry's key field against the passed-in BSFixedString's
// underlying pointer, returns a bool. This bounded function's start
// (0x1A7FDD0) -- not F4SE's literal declared address, not either of the two
// previously-tried addresses in this same crowded region of the binary -- is
// the real entry.
static RelocAddr<IsMenuOpen_t> s_IsMenuOpen(0x01A80320);

static constexpr char kDlgPrefix[]   = "DLG|";
static constexpr char kDlgOpen[]     = "DLG|O|";
static constexpr char kDlgClose[]    = "DLG|C|";
static constexpr char kDlgLine[]     = "DLG|L|";

namespace
{

    bool     s_dialogOpen     = false;


    uint32_t s_dialogEntityID = 0;

    // Camera state active just before the client entered remote-dialogue mode.
    // Saved on DLG|O so it can be restored on DLG|C.
    TESCameraState* s_savedCameraState = nullptr;

    enum class DialogOpType : uint8_t
    {
        Open,
        Close,
        Line,
    };

    struct DialogOp
    {
        DialogOpType type        = DialogOpType::Open;
        uint32_t     entityID    = 0;
        uint32_t     topicFormID = 0;
    };

    std::mutex           s_opsMutex;
    std::deque<DialogOp> s_dialogOps;

}

// Switch the client's camera to the dialogue state so the NPC proxy is
// framed correctly during host-driven dialogue. The DialogueCameraState
// slot (index 12) is pre-allocated by the engine at session start; we save
// the current state pointer so SafeRestoreDialogueCamera can put it back.
// Both functions are noinline so the SEH scope survives /GL+/LTCG inlining
// (same as every other SEH helper in this codebase).
__declspec(noinline) static void SafeEnterDialogueCamera()
{
    LOG_REQUIREMENT("CoSyncDialogSync::SafeEnterDialogueCamera",
        "Needs a valid player camera and an initialized dialogue camera state. "
        "Wrapped in a crash guard so a missing camera or state fails safely.");
    __try
    {
        if (!g_playerCamera || !*g_playerCamera) return;
        PlayerCamera* cam = *g_playerCamera;
        TESCameraState* dlgState = cam->cameraStates[PlayerCamera::kCameraState_Dialogue];
        if (!dlgState) return;
        s_savedCameraState = cam->cameraState;
        CALL_MEMBER_FN(cam, SetCameraState)(dlgState);
        LOG_UPDATED("[DialogSync] switched camera to dialogue view (saved previous=%p)", static_cast<void*>(s_savedCameraState));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s_savedCameraState = nullptr;
        LOG_ERROR("[DialogSync] switching to the dialogue camera crashed — camera left unchanged");
    }
}

__declspec(noinline) static void SafeRestoreDialogueCamera()
{
    LOG_REQUIREMENT("CoSyncDialogSync::SafeRestoreDialogueCamera",
        "Needs a valid player camera and a saved camera state from an earlier "
        "SafeEnterDialogueCamera call. Wrapped in a crash guard; always clears the saved state afterward.");
    __try
    {
        if (!g_playerCamera || !*g_playerCamera || !s_savedCameraState) return;
        PlayerCamera* cam = *g_playerCamera;
        CALL_MEMBER_FN(cam, SetCameraState)(s_savedCameraState);
        LOG_UPDATED("[DialogSync] restored camera to previous=%p", static_cast<void*>(s_savedCameraState));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[DialogSync] restoring the camera crashed — it may be stuck");
    }
    s_savedCameraState = nullptr;
}

// Every raw-pointer read below is SEH-wrapped + noinline for the same reason
// as the rest of this codebase's engine accessors: a non-noinline __try can be
// inlined away under /GL+/LTCG, silently dropping the SEH scope.
__declspec(noinline) static uint32_t SafeReadFormID(void* form)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SafeReadFormID", "Reads a pointer handed to us by the game inside a hook. Wrapped in a crash guard because a form that's being torn down or built can fault.");
    if (!form) return 0;
    __try
    {
        return *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(form) + kForm_FormID_Offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

__declspec(noinline) static uint8_t SafeReadFormType(void* form)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SafeReadFormType", "Reads a pointer handed to us by the game inside a hook. Wrapped in a crash guard for the same reason as SafeReadFormID.");
    if (!form) return 0;
    __try
    {
        return *(reinterpret_cast<uint8_t*>(form) + kForm_FormType_Offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SafeLookupForm", "The form ID comes from a peer and isn't validated. Wrapped in a crash guard since unchecked peer-supplied form IDs have caused crashes before.");
    if (formID == 0) return nullptr;
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[DialogSync] looking up formID=0x%08X crashed", formID);
        return nullptr;
    }
}

// Replay a spoken line on a proxy. Sets the re-entrancy flag around the call
// so the detour ignores the dialogue item this itself creates.
__declspec(noinline) static void SafeSayTopic(Actor* speaker, TESForm* topic)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SafeSayTopic", "Needs a still-alive speaker actor (the caller re-checks this) and a valid topic form. Calls the game's own dialogue function, wrapped in a crash guard.");
    if (!speaker || !topic) return;

    t_inOurPlayback = true;
    __try
    {
        g_sayInternal(speaker, topic, nullptr, nullptr, nullptr, false);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[DialogSync] playing the line crashed — speaker=%p topic=%p",
            static_cast<void*>(speaker), static_cast<void*>(topic));
    }
    t_inOurPlayback = false;
}

static void SendDialogLine(uint32_t entityID, uint32_t topicFormID)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SendDialogLine", "Doesn't touch game state — just builds and sends a DLG|L packet. The topic ID is sent as decimal text since the receiver parses it that way.");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "DLG|L|%u|%u", entityID, topicFormID);
    CoSyncTransport::Send(std::string(buf));

    static std::atomic<uint32_t> s_n{ 0 };
    const uint32_t n = ++s_n;
    if (n <= 20 || n % 100 == 0)
        LOG_INFO("[DialogSync] sent line #%u entity=%u topic=0x%08X",
            n, entityID, topicFormID);
}

// The detour. Runs the original FIRST so a fault in our bookkeeping can never
// swallow a line the game was going to speak, then reports it.
static void* __fastcall Detour_BuildDialogueItem(
    void* topic, void* speaker, int a3, void* a4, void* a5, void* a6)
{
    void* result = g_origBuildDialogueItem
        ? g_origBuildDialogueItem(topic, speaker, a3, a4, a5, a6)
        : nullptr;

    // Dialogue is host-authoritative (same model as quest progress), so only
    // the host announces lines. Clients replaying a line must not echo it back.
    if (t_inOurPlayback || !topic || !speaker)
        return result;
    if (!CoSyncNet::IsConnected() || !CoSyncNet::IsHost())
        return result;

    // Only actors can be proxied, and only registered NPCs have an entityID --
    // that check alone limits traffic to NPCs this session actually syncs.
    if (SafeReadFormType(speaker) != kFormType_ACHR_Local)
        return result;

    const uint32_t topicFormID = SafeReadFormID(topic);
    if (topicFormID == 0)
        return result;

    const uint32_t entityID =
        CoSyncNpcWorldSync::GetEntityIDForActor(static_cast<Actor*>(speaker));
    if (entityID == 0)
        return result;

    // A single spoken line can build its dialogue item more than once (the
    // line-start path re-enters for sequenced topics). Collapse repeats of the
    // same pair so the client does not stutter the same voice line.
    static uint32_t s_lastEntity = 0;
    static uint32_t s_lastTopic  = 0;
    static double   s_lastTime   = 0.0;
    const double now = static_cast<double>(GetTickCount64()) / 1000.0;
    if (entityID == s_lastEntity && topicFormID == s_lastTopic &&
        (now - s_lastTime) < 0.5)
        return result;
    s_lastEntity = entityID;
    s_lastTopic  = topicFormID;
    s_lastTime   = now;

    SendDialogLine(entityID, topicFormID);
    return result;
}

static void SendDialogOpen(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SendDialogOpen", "Just needs a valid entity ID — sends a DLG|O packet to say dialogue has opened.");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "DLG|O|%u", entityID);
    CoSyncTransport::Send(std::string(buf));
    LOG_UPDATED("[DialogSync] sent dialogue-open for entity=%u", entityID);
}


static void SendDialogClose(uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SendDialogClose", "Just needs a valid entity ID — sends a DLG|C packet to say dialogue has closed.");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "DLG|C|%u", entityID);
    CoSyncTransport::Send(std::string(buf));
    LOG_UPDATED("[DialogSync] sent dialogue-close for entity=%u", entityID);
}

// __declspec(noinline) is load-bearing here, same as every other raw-RVA SEH
// wrapper in this codebase (CoSyncDoorHook.cpp's SafeReadU32/SafeReadPtr,
// CoSyncLocomotionCalcGuard.cpp's CallOriginalSafe, etc.) -- a non-noinline
// __try/__except can be inlined away under this project's /GL+/LTCG Release
// config, silently dropping the SEH scope.
__declspec(noinline) static bool SafeIsDialogueMenuOpen(UI* ui, const BSFixedString& menuName)
{
    LOG_REQUIREMENT("CoSyncDialogSync::SafeIsDialogueMenuOpen", "Needs a non-null UI pointer (checked). Calls the game's own menu-open check, wrapped in a crash guard since a bad UI state can fault it.");
    if (!ui)
        return false;

    __try
    {
        return s_IsMenuOpen(ui, menuName);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

namespace CoSyncDialogSync
{

bool IsDialogue(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncDialogSync::IsDialogue", "Doesn't touch game state — just checks a string for a 'DLG|' prefix.");
    return msg.size() > 4
        && msg[0] == 'D'
        && msg[1] == 'L'
        && msg[2] == 'G'
        && msg[3] == '|';
}

void TickLocalDialogue(double , uint32_t myEntityID)
{
    LOG_REQUIREMENT("CoSyncDialogSync::TickLocalDialogue", "Needs a valid UI pointer and a valid player (both checked). Checks the dialogue menu state every tick. Only the host sends dialogue packets — clients only receive them.");

    // Host drives dialogue state; clients only receive and apply DLG packets.
    if (!CoSyncNet::IsConnected() || !CoSyncNet::IsHost()) return;

    if (!g_ui || !*g_ui)
        return;
    PlayerCharacter* playerChar = CoSyncGetPlayer();
    if (!playerChar)
        return;

    // RE-ENABLED round 104: round 44's disable (see git history / memory for
    // the full old writeup) was the right call at the time -- the address
    // then configured (0x01A7FD40) really was inside the same function as
    // the long-chased EventSinkTeardown fault. The real UI::IsMenuOpen entry
    // (0x1A7FDD0, see the corrected RelocAddr definition above) is a
    // completely different, unrelated function in this same crowded region
    // of the binary -- independently re-derived from F4SE's own SDK
    // declaration and verified via IDA decompile (3 real callers, correct
    // hashtable-walk shape) before re-enabling this call.
    static const BSFixedString kDialogueMenuName("DialogueMenu");
    const bool isOpen = SafeIsDialogueMenuOpen(*g_ui, kDialogueMenuName);

    if (isOpen && !s_dialogOpen)
    {

        s_dialogOpen = true;

        Actor* localPlayer = static_cast<Actor*>(playerChar);
        Actor* target = CoSyncNpcWorldSync::FindClosestRegisteredActor(
            localPlayer, 1000.f * 1000.f);

        if (target)
        {
            s_dialogEntityID = CoSyncNpcWorldSync::GetEntityIDForActor(target);
            if (s_dialogEntityID != 0)
            {
                SendDialogOpen(s_dialogEntityID);
                LOG_UPDATED("[DialogSync] dialogue menu opened — NPC entity=%u actor=%p",
                    s_dialogEntityID, static_cast<void*>(target));
            }
            else
            {
                LOG_UPDATED_WARN("[DialogSync] dialogue menu opened but the closest actor has no entity ID");
            }
        }
        else
        {
            LOG_UPDATED_WARN("[DialogSync] dialogue menu opened but no registered NPC is in range");
            s_dialogEntityID = 0;
        }
    }
    else if (!isOpen && s_dialogOpen)
    {

        s_dialogOpen = false;

        if (s_dialogEntityID != 0)
        {
            SendDialogClose(s_dialogEntityID);
            LOG_UPDATED("[DialogSync] dialogue menu closed — NPC entity=%u", s_dialogEntityID);
            s_dialogEntityID = 0;
        }
        else
        {
            LOG_UPDATED("[DialogSync] dialogue menu closed (no NPC was being tracked)");
        }
    }

    (void)myEntityID;
}

void OnReceiveDialogue(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncDialogSync::OnReceiveDialogue", "Needs a properly formatted message (checked). Parses the entity ID and topic ID out of a string sent by a peer, so those values aren't trusted blindly.");
    if (msg.size() < 7)
        return;


    char subCmd = msg[4];
    if (msg[5] != '|')
    {
        LOG_UPDATED_WARN("[DialogSync] got a badly formatted dialogue packet: '%s'", msg.c_str());
        return;
    }


    const char* p = msg.c_str() + 6;
    char* endPtr  = nullptr;
    uint32_t entityID = static_cast<uint32_t>(std::strtoul(p, &endPtr, 10));
    if (entityID == 0)
    {
        LOG_UPDATED_WARN("[DialogSync] dialogue packet has an empty entity ID: '%s'", msg.c_str());
        return;
    }

    DialogOp op{};
    op.entityID = entityID;

    switch (subCmd)
    {
    case 'O':
        op.type = DialogOpType::Open;
        break;

    case 'C':
        op.type = DialogOpType::Close;
        break;

    case 'L':
    {
        op.type = DialogOpType::Line;

        if (endPtr && *endPtr == '|')
        {
            op.topicFormID = static_cast<uint32_t>(
                std::strtoul(endPtr + 1, nullptr, 10));
        }
        break;
    }

    default:
        LOG_UPDATED_WARN("[DialogSync] unknown dialogue sub-command '%c': '%s'",
            subCmd, msg.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(s_opsMutex);
        s_dialogOps.push_back(op);
    }

    LOG_TRACE("[DialogSync] queued received dialogue op '%c' entity=%u", subCmd, entityID);
}

void InstallDialogueLineHook()
{
    LOG_REQUIREMENT("CoSyncDialogSync::InstallDialogueLineHook", "Needs the target function address to still point at the real game function it was verified against. Only installs once, guarded by a flag.");
    if (g_lineHookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void* target = reinterpret_cast<void*>(base + kBuildDialogueItem_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[DialogSync] hook system init failed: %d", (int)st);
        g_lineHookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_BuildDialogueItem),
        reinterpret_cast<void**>(&g_origBuildDialogueItem));
    if (st != MH_OK)
    {
        LOG_ERROR("[DialogSync] creating the dialogue-line hook failed: %d (target=%p)",
            (int)st, target);
        g_lineHookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[DialogSync] enabling the dialogue-line hook failed: %d", (int)st);
        g_lineHookInstalled.store(false);
        return;
    }

    LOG_INFO("[DialogSync] dialogue-line hook installed at %p (offset 0x%zX)",
        target, kBuildDialogueItem_RVA);
}

void FlushPendingDialogOps()
{
    LOG_REQUIREMENT("CoSyncDialogSync::FlushPendingDialogOps", "Needs the entity registry populated so open/close ops can find an actor. Meant to run once per frame to drain the queued dialogue ops.");

    std::deque<DialogOp> local;
    {
        std::lock_guard<std::mutex> lk(s_opsMutex);
        if (s_dialogOps.empty())
            return;
        local.swap(s_dialogOps);
    }

    for (const DialogOp& op : local)
    {
        switch (op.type)
        {
        case DialogOpType::Open:
        {
            Actor* actor = g_CoSyncEntities.GetActor(op.entityID);
            if (actor)
            {
                CoSyncGameAPI::DisableActorAI(actor);
                LOG_UPDATED("[DialogSync] turned off AI — entity=%u actor=%p (dialogue open)",
                    op.entityID, static_cast<void*>(actor));
            }
            else
            {
                LOG_UPDATED_WARN("[DialogSync] dialogue-open entity=%u — actor not in the registry",
                    op.entityID);
            }
            // Zoom the client camera into the dialogue camera state so the
            // NPC proxy is framed as if the client is watching the conversation.
            SafeEnterDialogueCamera();
            break;
        }

        case DialogOpType::Close:
        {
            Actor* actor = g_CoSyncEntities.GetActor(op.entityID);
            if (actor)
            {
                CoSyncGameAPI::EnableActorAI(actor);
                LOG_UPDATED("[DialogSync] turned AI back on — entity=%u actor=%p (dialogue closed)",
                    op.entityID, static_cast<void*>(actor));
            }
            else
            {
                LOG_UPDATED_WARN("[DialogSync] dialogue-close entity=%u — actor not in the registry",
                    op.entityID);
            }
            // Restore whichever camera state was active before the dialogue opened.
            SafeRestoreDialogueCamera();
            break;
        }

        case DialogOpType::Line:
        {
            Actor* actor = g_CoSyncEntities.GetActor(op.entityID);
            if (!actor)
            {
                LOG_UPDATED_WARN("[DialogSync] dialogue-line entity=%u — actor not in the registry",
                    op.entityID);
                break;
            }

            // A cached Actor* can outlive the actor: cell transitions free
            // proxies, not just reloads. Re-validate before every use.
            if (!CoSyncGameAPI::IsFormStillLive(reinterpret_cast<TESForm*>(actor)))
            {
                LOG_UPDATED_WARN("[DialogSync] dialogue-line entity=%u — actor is gone, skipping",
                    op.entityID);
                break;
            }

            TESForm* topic = SafeLookupForm(op.topicFormID);
            if (!topic)
            {
                LOG_UPDATED_WARN("[DialogSync] dialogue-line entity=%u — topic 0x%08X not found",
                    op.entityID, op.topicFormID);
                break;
            }

            SafeSayTopic(actor, topic);
            LOG_UPDATED("[DialogSync] spoke topic 0x%08X on entity=%u actor=%p",
                op.topicFormID, op.entityID, static_cast<void*>(actor));
            break;
        }

        default:
            break;
        }
    }
}

}
