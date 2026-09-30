#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncSharedDialogue.h"
#include "CoSyncNpcWorldSync.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncGameAPI.h"
#include "CoSyncChat.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "GameHandle.h"
#include "GameMenus.h"
#include "GameObjects.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern PlayerCharacter*    CoSyncGetPlayer();
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{
    // ── Engine (1.11.240, IDA-verified 2026-09-29) ──────────────────────────
    constexpr uintptr_t kRVA_DialogueMenuVtbl     = 0x025AD118;
    constexpr uintptr_t kRVA_DialogueMenuProcMsg  = 0x01043460;   // slot 3
    constexpr uintptr_t kRVA_SetButtonTextType    = 0x01A869E0;   // -> &BSFixedString("DialogueSetButtonText")
    constexpr uintptr_t kRVA_MenuTopicManagerPtr  = 0x030E6E50;   // MenuTopicManager*
    constexpr uintptr_t kRVA_SelectOption         = 0x00BAF7C0;   // (manager, choice 0..3)
    constexpr uint8_t   kSelectOptionPrologue[10] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    constexpr uint8_t   kButtonTypePrologue[6]    = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };

    constexpr float kShowRange   = 900.0f;   // a client this close to the speaker sees the conversation
    constexpr float kChooseRange = 600.0f;   // and this close may choose

    using ProcMsg_t   = int64_t(__fastcall*)(void* menu, void* msg);
    using TypeFn_t    = void**(__fastcall*)();
    using Select_t    = int64_t(__fastcall*)(void* manager, uint32_t choice);

    uintptr_t g_base = 0;
    bool      g_installed = false;
    ProcMsg_t g_origProcMsg = nullptr;
    bool      g_selectOk = false;

    // Host: what the engine's dialogue menu currently shows.
    std::mutex                 g_optMx;
    std::array<std::string, 4> g_options;
    bool                       g_optDirty = false;

    // Host session state (game thread).
    uint32_t g_hostSpeakerEid = 0;
    uint32_t g_hostSeq = 0;
    bool     g_hostWasOpen = false;
    double   g_hostLastSend = 0.0;
    std::array<std::string, 4> g_hostSent;

    // Client state (game thread).
    uint32_t g_cliSpeakerEid = 0;
    uint32_t g_cliSeq = 0;
    std::array<std::string, 4> g_cliOptions;
    bool     g_cliShown = false;
    bool     g_keyWasDown[4] = {};

    __declspec(noinline) const char* SafeCStr(const void* data, uintptr_t off)
    {
        __try { return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(data) + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // Mirrors the engine's own test in sub_141043460 for the
    // "DialogueSetButtonText" UI data, then reads index/text from it.
    // SEH-only (no unwinding objects): the copy happens in the caller.
    __declspec(noinline) bool ReadButtonText(void* msg, uint8_t& idx, const char*& text)
    {
        __try
        {
            if (*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(msg) + 0x10) != 6)
                return false;
            using GetData_t = void*(__fastcall*)(void*);
            void* data = reinterpret_cast<GetData_t>((*reinterpret_cast<void***>(msg))[2])(msg);
            if (!data)
                return false;
            void** type = reinterpret_cast<TypeFn_t>(g_base + kRVA_SetButtonTextType)();
            if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(data) + 0x30) != *type)
                return false;
            idx = *(reinterpret_cast<uint8_t*>(data) + 0x18);
            text = *reinterpret_cast<const char* const*>(reinterpret_cast<uint8_t*>(data) + 0x20);
            if (text && !IsBadStringPtrA(text, 512)) return idx < 4;
            text = nullptr;
            return idx < 4;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void CaptureButtonText(void* msg)
    {
        uint8_t idx = 0;
        const char* text = nullptr;
        if (!ReadButtonText(msg, idx, text))
            return;
        std::string t = text ? text : "";
        std::lock_guard<std::mutex> lk(g_optMx);
        if (g_options[idx] != t)
        {
            g_options[idx] = std::move(t);
            g_optDirty = true;
        }
    }

    int64_t __fastcall Detour_DialogueProcessMessage(void* menu, void* msg)
    {
        if (msg && CoSyncNet::IsHost())
            CaptureButtonText(msg);
        return g_origProcMsg(menu, msg);
    }

    void* MenuTopicManager()
    {
        return *reinterpret_cast<void**>(g_base + kRVA_MenuTopicManagerPtr);
    }

    BSFixedString& DialogueMenuName()
    {
        static BSFixedString s_name("DialogueMenu");
        return s_name;
    }

    __declspec(noinline) bool SafeDialogueMenuOpenImpl(const BSFixedString& name)
    {
        __try
        {
            UI* ui = *g_ui;
            // UI::IsMenuOpen, 0x1A80320 (F4SE 0.7.9 for 1.11.240; also used by CoSyncDialogSync).
            using IsMenuOpen_t = bool(*)(UI*, const BSFixedString&);
            return ui && reinterpret_cast<IsMenuOpen_t>(g_base + 0x01A80320)(ui, name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeDialogueMenuOpen() { return SafeDialogueMenuOpenImpl(DialogueMenuName()); }

    Actor* CurrentSpeaker()
    {
        void* mgr = MenuTopicManager();
        if (!mgr)
            return nullptr;
        const uint32_t h = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(mgr) + 0x14);
        if (!h)
            return nullptr;
        NiPointer<TESObjectREFR> ref;
        if (!LookupREFRByHandle(h, ref) || !ref.m_pObject || ref.m_pObject->formType != kFormType_ACHR)
            return nullptr;
        return static_cast<Actor*>(ref.m_pObject);
    }

    std::string Clean(const std::string& s)
    {
        std::string o;
        for (char c : s)
            o += (c == '|' || c == '\r' || c == '\n') ? ' ' : c;
        if (o.size() > 140) o.resize(140);
        return o;
    }

    std::string ActorName(Actor* a)
    {
        if (a && a->baseForm && a->baseForm->formType == kFormType_NPC_)
        {
            const char* n = static_cast<TESNPC*>(a->baseForm)->fullName.name.c_str();
            if (n && *n) return n;
        }
        return "NPC";
    }

    float Dist(const NiPoint3& a, const NiPoint3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    void HostTick(double now)
    {
        const bool open = SafeDialogueMenuOpen();
        Actor* speaker = open ? CurrentSpeaker() : nullptr;
        const uint32_t eid = speaker ? CoSyncNpcWorldSync::GetEntityIDForActor(speaker) : 0;

        if (!open || eid == 0)
        {
            if (g_hostWasOpen && g_hostSpeakerEid != 0)
            {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "DLGX|%u", g_hostSpeakerEid);
                CoSyncTransport::Send(std::string(buf));
            }
            g_hostWasOpen = false;
            g_hostSpeakerEid = 0;
            std::lock_guard<std::mutex> lk(g_optMx);
            if (!open)
                for (auto& o : g_options) o.clear();
            return;
        }

        std::array<std::string, 4> opts;
        bool dirty;
        {
            std::lock_guard<std::mutex> lk(g_optMx);
            opts = g_options;
            dirty = g_optDirty;
            g_optDirty = false;
        }
        const bool changed = dirty && opts != g_hostSent;
        if (changed || eid != g_hostSpeakerEid)
            ++g_hostSeq;
        if (!(changed || eid != g_hostSpeakerEid || now - g_hostLastSend > 3.0))
            return;
        g_hostWasOpen = true;
        g_hostSpeakerEid = eid;
        g_hostSent = opts;
        g_hostLastSend = now;
        std::string m = "DLGM|" + std::to_string(eid) + "|" + std::to_string(g_hostSeq) + "|" + Clean(ActorName(speaker));
        for (const auto& o : opts)
            m += "|" + Clean(o);
        CoSyncTransport::Send(m);
        if (changed)
            LOG_UPDATED("[SharedDialogue] TX options speaker=%u seq=%u: [%s] [%s] [%s] [%s]", eid, g_hostSeq,
                opts[0].c_str(), opts[1].c_str(), opts[2].c_str(), opts[3].c_str());
    }

    void Reply(uint32_t clientEid, bool ok, const std::string& text)
    {
        CoSyncTransport::Send("DLGR|" + std::to_string(clientEid) + "|" + (ok ? "1|" : "0|") + Clean(text));
    }

    __declspec(noinline) bool SafeSelect(uint32_t choice)
    {
        __try
        {
            void* mgr = MenuTopicManager();
            if (!mgr) return false;
            reinterpret_cast<Select_t>(g_base + kRVA_SelectOption)(mgr, choice);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void HostOnChoice(uint32_t clientEid, uint32_t speakerEid, uint32_t seq, uint32_t choice)
    {
        CoSyncPlayer* cp = g_CoSyncPlayerManager.GetPlayerByEntityID(clientEid);
        Actor* speaker = SafeDialogueMenuOpen() ? CurrentSpeaker() : nullptr;
        const char* why = nullptr;
        std::string text;
        if (!g_selectOk) why = "choices are unavailable on this game version";
        else if (!speaker || CoSyncNpcWorldSync::GetEntityIDForActor(speaker) != speakerEid) why = "that conversation is over";
        else if (seq != g_hostSeq) why = "the options changed";
        else if (choice >= 4) why = "no such option";
        else
        {
            text = g_hostSent[choice];
            if (text.empty()) why = "no such option";
            else if (!cp || !cp->hasSpawned || !cp->actorRef) why = "your player is not here";
            else if (Dist(cp->actorRef->pos, speaker->pos) > kChooseRange) why = "you are too far away";
        }
        if (why)
        {
            LOG_UPDATED("[SharedDialogue] refused choice %u from entity=%u: %s", choice, clientEid, why);
            Reply(clientEid, false, why);
            return;
        }
        if (!SafeSelect(choice))
        {
            Reply(clientEid, false, "the game refused the choice");
            return;
        }
        LOG_UPDATED("[SharedDialogue] entity=%u chose %u '%s' (speaker=%u) -- executed by the host", clientEid, choice, text.c_str(), speakerEid);
        Reply(clientEid, true, text);
        CoSyncChat::AddLocalLine("Dialogue", (cp ? cp->username : std::string("A player")) + " chose: " + text);
    }

    void ClientTick()
    {
        if (!g_cliShown || g_cliSpeakerEid == 0)
            return;
        static const int kKeys[4] = { VK_F1, VK_F2, VK_F3, VK_F4 };
        for (int i = 0; i < 4; ++i)
        {
            const bool down = (GetAsyncKeyState(kKeys[i]) & 0x8000) != 0;
            const bool pressed = down && !g_keyWasDown[i];
            g_keyWasDown[i] = down;
            if (!pressed || g_cliOptions[i].empty())
                continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "DLGP|%u|%u|%u|%d", CoSyncNet::GetMyEntityID(), g_cliSpeakerEid, g_cliSeq, i);
            CoSyncTransport::Send(std::string(buf));
            CoSyncChat::AddLocalLine("Dialogue", "You asked for: " + g_cliOptions[i]);
        }
    }

    std::vector<std::string> Split(const std::string& s)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (true)
        {
            const size_t b = s.find('|', a);
            out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    }
}

namespace CoSyncSharedDialogue
{
    void Install()
    {
        LOG_REQUIREMENT("CoSyncSharedDialogue::Install", "Plugin load. Swaps DialogueMenu's ProcessMessage slot after checking it still points at the 1.11.240 function; checks the option-select and type-getter prologues.");
        if (g_installed)
            return;
        g_installed = true;
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        g_selectOk = std::memcmp(reinterpret_cast<void*>(g_base + kRVA_SelectOption), kSelectOptionPrologue, sizeof(kSelectOptionPrologue)) == 0;
        const bool typeOk = std::memcmp(reinterpret_cast<void*>(g_base + kRVA_SetButtonTextType), kButtonTypePrologue, sizeof(kButtonTypePrologue)) == 0;
        void** slot = reinterpret_cast<void**>(g_base + kRVA_DialogueMenuVtbl + 3 * sizeof(void*));
        if (!typeOk || reinterpret_cast<uintptr_t>(*slot) != g_base + kRVA_DialogueMenuProcMsg)
        {
            LOG_ERROR("[SharedDialogue] DialogueMenu layout does not match 1.11.240 -- shared dialogue disabled");
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
            return;
        g_origProcMsg = reinterpret_cast<ProcMsg_t>(*slot);
        *slot = reinterpret_cast<void*>(&Detour_DialogueProcessMessage);
        VirtualProtect(slot, sizeof(void*), old, &old);
        LOG_INFO("[SharedDialogue] DialogueMenu::ProcessMessage captured (options), select path %s",
            g_selectOk ? "verified" : "NOT verified -- clients cannot choose");
    }

    bool IsSharedDialogueMsg(const std::string& msg)
    {
        return msg.rfind("DLGM|", 0) == 0 || msg.rfind("DLGX|", 0) == 0 || msg.rfind("DLGP|", 0) == 0 ||
               msg.rfind("DLGR|", 0) == 0 || msg.rfind("DLGQ|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncSharedDialogue::OnReceive", "Game thread. Peer text is parsed defensively.");
        const std::vector<std::string> f = Split(msg);
        const std::string& k = f[0];
        if (k == "DLGP" && isHostRole && f.size() >= 5)
        {
            HostOnChoice(std::strtoul(f[1].c_str(), nullptr, 10), std::strtoul(f[2].c_str(), nullptr, 10),
                         std::strtoul(f[3].c_str(), nullptr, 10), std::strtoul(f[4].c_str(), nullptr, 10));
            return;
        }
        if (k == "DLGQ" && isHostRole && f.size() >= 3)
        {
            CoSyncPlayer* cp = g_CoSyncPlayerManager.GetPlayerByEntityID(std::strtoul(f[1].c_str(), nullptr, 10));
            CoSyncPlayer* np = g_CoSyncPlayerManager.GetPlayerByEntityID(std::strtoul(f[2].c_str(), nullptr, 10));
            Actor* npc = np ? np->actorRef : nullptr;
            const std::string line = (cp ? cp->username : std::string("A player")) + " wants to talk to " +
                                     ActorName(npc) + ". Start the conversation; they will see it and can choose.";
            CoSyncGameAPI::QueueNotification(line.c_str());
            CoSyncChat::AddLocalLine("Dialogue", line);
            return;
        }
        if (isHostRole)
            return;
        if (k == "DLGM" && f.size() >= 8)
        {
            const uint32_t eid = std::strtoul(f[1].c_str(), nullptr, 10);
            const uint32_t seq = std::strtoul(f[2].c_str(), nullptr, 10);
            CoSyncPlayer* np = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
            PlayerCharacter* me = CoSyncGetPlayer();
            const bool isNear = np && np->actorRef && me &&
                Dist(np->actorRef->pos, reinterpret_cast<TESObjectREFR*>(me)->pos) <= kShowRange;
            if (!isNear)
            {
                g_cliShown = false;
                return;
            }
            std::array<std::string, 4> opts{ f[4], f[5], f[6], f[7] };
            const bool fresh = !g_cliShown || eid != g_cliSpeakerEid || opts != g_cliOptions;
            g_cliSpeakerEid = eid;
            g_cliSeq = seq;
            g_cliOptions = opts;
            g_cliShown = true;
            if (fresh)
            {
                std::string line = f[3] + ":";
                for (int i = 0; i < 4; ++i)
                    if (!opts[i].empty())
                        line += "  [F" + std::to_string(i + 1) + "] " + opts[i];
                CoSyncChat::AddLocalLine("Dialogue", line);
            }
            return;
        }
        if (k == "DLGX" && f.size() >= 2)
        {
            if (std::strtoul(f[1].c_str(), nullptr, 10) == g_cliSpeakerEid)
            {
                g_cliShown = false;
                g_cliSpeakerEid = 0;
            }
            return;
        }
        if (k == "DLGR" && f.size() >= 4 && std::strtoul(f[1].c_str(), nullptr, 10) == CoSyncNet::GetMyEntityID())
        {
            CoSyncChat::AddLocalLine("Dialogue", f[2] == "1" ? ("The host said: " + f[3]) : ("Choice refused: " + f[3]));
            return;
        }
    }

    void Tick(double now)
    {
        if (!CoSyncNet::IsConnected())
            return;
        if (CoSyncNet::IsHost())
            HostTick(now);
        else
            ClientTick();
    }

    void OnClientTalkAttempt(uint32_t refFormID)
    {
        if (CoSyncNet::IsHost() || !CoSyncNet::IsConnected())
            return;
        TESForm* f = LookupFormByID(refFormID);
        if (!f || f->formType != kFormType_ACHR)
            return;
        const uint32_t eid = CoSyncNpcWorldSync::GetEntityIDForActor(static_cast<Actor*>(f));
        if (eid == 0)
            return;
        static double s_last = 0.0;
        const double now = static_cast<double>(GetTickCount64()) / 1000.0;
        if (now - s_last < 3.0)
            return;
        s_last = now;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "DLGQ|%u|%u", CoSyncNet::GetMyEntityID(), eid);
        CoSyncTransport::Send(std::string(buf));
        CoSyncChat::AddLocalLine("Dialogue", "The host runs conversations in co-op. They have been told you want to talk; "
                                             "when they do, the options appear here (F1-F4 to choose).");
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(g_optMx);
            for (auto& o : g_options) o.clear();
            g_optDirty = false;
        }
        g_hostSpeakerEid = 0; g_hostSeq = 0; g_hostWasOpen = false;
        g_cliSpeakerEid = 0; g_cliShown = false;
    }
}
