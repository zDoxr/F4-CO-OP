#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncModEventHook.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"
#include "ConsoleLogger.h"
#include "MinHook.h"

#include "PapyrusArgs.h"
#include "PapyrusEvents.h"
#include "GameForms.h"
#include "PapyrusVM.h"
#include "GameTypes.h"
#include "CoSyncRequirementsLog.h"

#include <atomic>
#include <mutex>
#include <deque>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <string>

// ─── RVA ─────────────────────────────────────────────────────────────────────
// ScriptObject.SendCustomEvent internal — FO4 1.11.221
// RVA CORRECTED round 93: IDA confirms true function entry is 0x1134D80, not
// 0x1134E00 (same project-wide .pdata/Capstone disassembly-desync defect
// class found and fixed in rounds 90/93 elsewhere; the old value was
// mid-function, "verified non-zero prologue" was not sufficient evidence).
static constexpr uintptr_t kSendCustomEvent_RVA = 0x01135150;

// ─── Types ───────────────────────────────────────────────────────────────────
using SendCustomEventFn = void(__fastcall*)(
    VirtualMachine*      vm,
    uint64_t             unk1,
    VMIdentifier*        sender,
    const BSFixedString* eventName,
    VMValue*             args);

static SendCustomEventFn g_orig = nullptr;
static std::atomic<bool> g_hookInstalled{ false };

// Suppress re-entrant hook fire when we call SendCustomEvent from the receive path
static thread_local bool g_inRemoteFire = false;

// ─── Pending re-fire queue (filled on transport thread, flushed on game thread) ─
struct PendingEvent
{
    uint32_t    formID;
    std::string eventName;
    std::string serializedArgs;
};
static std::deque<PendingEvent> g_pending;
static std::mutex               g_pendingMtx;

// ─── Arg serialization ───────────────────────────────────────────────────────
// Wire format for args field:  N|type:val|type:val|...
//   N     = element count
//   types: s=string  i=int  f=float  b=bool  _=skip
//   '|' inside string values is escaped to \x01

static std::string SerializeArgs(VMValue* args)
{
    LOG_REQUIREMENT("SerializeArgs", "Safe to call with empty or null args (returns \"0\"). Turns a list of Papyrus values into simple text so it can be sent over the network.");
    if (!args
        || (uint8_t)args->type.value != VMValue::kType_VariableArray
        || !args->data.arr)
    {
        return "0";
    }

    uint32_t      count   = args->data.arr->arr.count;
    VMValue*      entries = args->data.arr->arr.entries;
    std::ostringstream ss;
    ss << count;

    for (uint32_t i = 0; i < count; ++i)
    {
        VMValue& slot = entries[i];
        if ((uint8_t)slot.type.value != VMValue::kType_Variable || !slot.data.var) {
            ss << "|_";
            continue;
        }
        VMValue* v = slot.data.var;
        switch ((uint8_t)v->type.value)
        {
        case VMValue::kType_String:
        {
            const char* raw = v->data.GetStr() ? v->data.GetStr()->c_str() : "";
            std::string s(raw ? raw : "");
            for (char& c : s) if (c == '|') c = '\x01';
            ss << "|s:" << s;
            break;
        }
        case VMValue::kType_Int:   ss << "|i:" << v->data.i; break;
        case VMValue::kType_Float: ss << "|f:" << v->data.f; break;
        case VMValue::kType_Bool:  ss << "|b:" << (v->data.b ? 1 : 0); break;
        default:                   ss << "|_"; break;
        }
    }
    return ss.str();
}

// ─── Detour ──────────────────────────────────────────────────────────────────
static void __fastcall Detour_SendCustomEvent(
    VirtualMachine*      vm,
    uint64_t             unk1,
    VMIdentifier*        sender,
    const BSFixedString* eventName,
    VMValue*             args)
{
    LOG_REQUIREMENT("Detour_SendCustomEvent", "Runs whenever the game sends a custom mod event. Needs the hook to already be installed, and a real sender with a non-empty event name, before it forwards the event to other players.");
    // round 94 diagnostic: unconditional fire-heartbeat, confirms the
    // round-93 RVA fix (kSendCustomEvent_RVA) is reached -- fires for every
    // ScriptObject.SendCustomEvent call, host or client.
    {
        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 5 || n % 200 == 0)
            LOG_INFO("[ModEventHook] Detour_SendCustomEvent fired #%u", n);
    }

    if (g_orig)
        g_orig(vm, unk1, sender, eventName, args);

    if (g_inRemoteFire)          return;
    if (!CoSyncNet::IsConnected()) return;
    if (!sender)                 return;
    if (!eventName || !eventName->c_str() || eventName->c_str()[0] == '\0') return;

    uint64_t handle = sender->GetHandle();
    uint32_t formID = (uint32_t)(handle & 0xFFFFFFFF);

    // Skip null and dynamic/temporary forms (FF+ range are session-local)
    if (formID == 0 || formID >= 0xFF000000) return;

    std::string sa = SerializeArgs(args);

    char buf[1024];
    std::snprintf(buf, sizeof(buf), "CUSTOMEVENT|%u|%s|%s",
        formID, eventName->c_str(), sa.c_str());
    CoSyncTransport::Send(std::string(buf));

    LOG_INFO("[ModEventHook] TX form=0x%08X event='%s' args='%s'",
        formID, eventName->c_str(), sa.c_str());
}

// ─── Queue (called from transport thread) ────────────────────────────────────
void QueueRemoteCustomEvent(uint32_t senderFormID,
                            const std::string& eventName,
                            const std::string& serializedArgs)
{
    LOG_REQUIREMENT("QueueRemoteCustomEvent", "Safe to call from any thread. Just adds an event to a queue so it can be replayed later on the main game thread.");
    std::lock_guard<std::mutex> lk(g_pendingMtx);
    g_pending.push_back({ senderFormID, eventName, serializedArgs });
}

// ─── Flush (called from CoSyncNet::Tick on game main thread) ─────────────────
extern RelocAddr<_LookupFormByID> LookupFormByID;

// UPDATE 2026-06-20 (round 41 audit): same unguarded peer-controlled
// ev.formID -> LookupFormByID crash pattern fixed elsewhere this session.
// SEH-wrapped, noinline (load-bearing under /GL+/LTCG).
__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    LOG_REQUIREMENT("SafeLookupForm", "The form ID comes from another player and might not exist locally. Looks it up safely and returns null instead of crashing if it's bad.");
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static void ReFireOne(const PendingEvent& ev)
{
    LOG_REQUIREMENT("ReFireOne", "Must run on the main game thread. Looks up the form the event was about, rebuilds its arguments, and fires the same custom event locally so this machine sees what the sender did.");
    TESForm* form = SafeLookupForm(ev.formID);
    if (!form) {
        LOG_WARN("[ModEventHook] RX: form 0x%08X not found, dropping '%s'",
            ev.formID, ev.eventName.c_str());
        return;
    }

    // Deserialize args
    VMArray<VMVariable> args;
    const char* p = ev.serializedArgs.c_str();
    char* endptr = nullptr;
    uint32_t count = (uint32_t)strtoul(p, &endptr, 10);
    p = endptr;

    for (uint32_t i = 0; i < count && p && *p == '|'; ++i)
    {
        ++p; // skip '|'
        VMVariable var;

        if (*p == '_') {
            ++p;
        } else if (*p != '\0' && *(p + 1) == ':') {
            char typeChar = *p;
            p += 2;
            const char* next = strchr(p, '|');
            size_t vlen = next ? (size_t)(next - p) : strlen(p);
            std::string val(p, vlen);
            p += vlen;
            for (char& c : val) if (c == '\x01') c = '|';

            switch (typeChar)
            {
            case 's': { BSFixedString s(val.c_str()); var.Set<BSFixedString>(&s); break; }
            case 'i': { SInt32 iv = (SInt32)strtol(val.c_str(), nullptr, 10); var.Set<SInt32>(&iv); break; }
            case 'f': { float fv = strtof(val.c_str(), nullptr); var.Set<float>(&fv); break; }
            case 'b': { bool bv = (val[0] == '1'); var.Set<bool>(&bv); break; }
            default:  break;
            }
        }
        args.Push(&var);
    }

    // Fire on this machine — suppress our outbound hook during the call
    g_inRemoteFire = true;
    BSFixedString evtName(ev.eventName.c_str());
    ::SendCustomEvent(form, evtName, args);
    g_inRemoteFire = false;

    LOG_INFO("[ModEventHook] RX fired form=0x%08X event='%s' argc=%u",
        ev.formID, ev.eventName.c_str(), count);
}

void FlushPendingCustomEvents()
{
    LOG_REQUIREMENT("FlushPendingCustomEvents", "Must run on the main game thread. Replays every custom event that came in from other players since the last flush.");
    std::deque<PendingEvent> local;
    {
        std::lock_guard<std::mutex> lk(g_pendingMtx);
        local.swap(g_pending);
    }
    for (const auto& ev : local)
        ReFireOne(ev);
}

// ─── Install / Uninstall ─────────────────────────────────────────────────────
void InstallModEventHook()
{
    LOG_REQUIREMENT("InstallModEventHook", "Only runs once. Hooks into the game's custom mod-event system so those events can be shared with other players.");
    if (g_hookInstalled.exchange(true)) return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kSendCustomEvent_RVA);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("[ModEventHook] MH_Initialize failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Detour_SendCustomEvent),
        reinterpret_cast<void**>(&g_orig));
    if (st != MH_OK) {
        LOG_ERROR("[ModEventHook] MH_CreateHook failed: %d  target=%p", (int)st, target);
        g_hookInstalled.store(false);
        return;
    }

    MH_EnableHook(target);
    LOG_INFO("[ModEventHook] Installed at %p  RVA=0x%zX", target, kSendCustomEvent_RVA);
}

void UninstallModEventHook()
{
    LOG_REQUIREMENT("UninstallModEventHook", "Only runs if the hook is currently installed. Removes the custom mod-event hook cleanly.");
    if (!g_hookInstalled.exchange(false)) return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kSendCustomEvent_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[ModEventHook] Uninstalled");
}
