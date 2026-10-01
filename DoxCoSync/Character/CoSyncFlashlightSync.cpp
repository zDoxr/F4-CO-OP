#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncFlashlightSync.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncEntityTypes.h"
#include "CoSyncGameAPI.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "GameReferences.h"
#include "GameForms.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

extern PlayerCharacter*    CoSyncGetPlayer();
extern CoSyncPlayerManager g_CoSyncPlayerManager;

namespace
{
    // ── Engine (Fallout4.exe 1.11.240, IDA 2026-10-01; see the header) ──────
    constexpr uintptr_t kRVA_LightBuild       = 0x00454880;   // (LIGH, ref, parent, 1, 1, 0, &light, float, 0) -> holder
    constexpr uintptr_t kRVA_FindLightMarker  = 0x00D86850;   // (root, {&form, &node}) -> bool
    constexpr uintptr_t kRVA_OverrideKeyword  = 0x00374850;   // () -> PipboyLightOverrideKeyword_DO (KYWD)
    constexpr uintptr_t kRVA_OverrideLight    = 0x00374A90;   // () -> PipboyLightOverride_DO (LIGH)
    constexpr uintptr_t kRVA_RefHasKeyword    = 0x00507940;   // (ref, keyword) -> bool
    constexpr uintptr_t kRVA_DefaultObject    = 0x00308100;   // (index) -> form; 374 = the Pip-Boy light
    constexpr uintptr_t kRVA_ShadowRemove     = 0x021B9E50;   // (ShadowSceneNode*, &light)
    constexpr uintptr_t kRVA_ShadowSceneNode  = 0x03E5DF60;   // qword
    constexpr uintptr_t kRVA_LightFadeParam   = 0x02F349A0;   // float (8.0) passed by SetPipboyLight
    constexpr uintptr_t kPlayer_PipboyLight   = 0xC38;        // PlayerCharacter: light, non-null while ON
    constexpr int       kDO_PipboyLight       = 374;
    constexpr uint8_t   kFormType_LIGH_Local  = 34;

    constexpr uint8_t kPro_LightBuild[16]   = { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x44,0x88,0x48,0x20,0x4C,0x89,0x40,0x18,0x55 };
    constexpr uint8_t kPro_FindMarker[16]   = { 0x48,0x89,0x5C,0x24,0x20,0x41,0x56,0x48,0x83,0xEC,0x20,0x33,0xDB,0x4C,0x8B,0xF2 };
    constexpr uint8_t kPro_OvrKeyword[16]   = { 0x48,0x8B,0x05,0x41,0x1A,0xD8,0x02,0x48,0x85,0xC0,0x74,0x06,0x80,0x78,0x1A,0x04 };
    constexpr uint8_t kPro_OvrLight[16]     = { 0x48,0x8B,0x05,0x99,0x1C,0xD8,0x02,0x48,0x85,0xC0,0x74,0x06,0x80,0x78,0x1A,0x22 };
    constexpr uint8_t kPro_HasKeyword[16]   = { 0x48,0x83,0xEC,0x58,0x33,0xC0,0x48,0x89,0x54,0x24,0x30,0x0F,0x57,0xC0,0x48,0x89 };
    constexpr uint8_t kPro_DefaultObj[16]   = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x65,0x48,0x8B,0x04,0x25,0x58 };
    constexpr uint8_t kPro_ShadowRemove[15] = { 0x40,0x57,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x83,0xEC,0x38,0x45,0x33,0xE4 };

    using LightBuild_t   = void*   (__fastcall*)(TESForm*, void*, void*, char, char, char, void**, float, char);
    using FindMarker_t   = bool    (__fastcall*)(void*, void**);
    using FormGetter_t   = TESForm*(__fastcall*)();
    using RefHasKw_t     = bool    (__fastcall*)(TESObjectREFR*, TESForm*);
    using DefaultObj_t   = TESForm*(__fastcall*)(int);
    using ShadowRemove_t = void    (__fastcall*)(void*, void**);

    uintptr_t      g_base = 0;
    bool           g_ready = false;
    LightBuild_t   g_build = nullptr;
    FindMarker_t   g_findMarker = nullptr;
    FormGetter_t   g_ovrKeyword = nullptr;
    FormGetter_t   g_ovrLight = nullptr;
    RefHasKw_t     g_refHasKw = nullptr;
    DefaultObj_t   g_defaultObj = nullptr;
    ShadowRemove_t g_shadowRemove = nullptr;

    // Owner side.
    int    g_lastLocalOn = -1;
    double g_lastSent = -100.0;

    // Receiver side (game thread).
    struct ProxyLight
    {
        Actor*   actor  = nullptr;
        void*    root   = nullptr;
        TESForm* form   = nullptr;
        void*    light  = nullptr;   // NiLight (one reference held)
        void*    holder = nullptr;   // the builder's returned object (one reference held)
    };
    std::unordered_map<uint32_t, bool>       g_wantOn;
    std::unordered_map<uint32_t, ProxyLight> g_applied;
    uint32_t g_dBuilt = 0, g_dRemoved = 0, g_dTx = 0, g_dRx = 0;

    bool PrologueOk(uintptr_t rva, const uint8_t* bytes, size_t n, const char* what)
    {
        if (std::memcmp(reinterpret_cast<const void*>(g_base + rva), bytes, n) == 0)
            return true;
        LOG_ERROR("[Flashlight] %s prologue mismatch at base+0x%llX (not 1.11.240?) -- remote flashlights disabled",
            what, static_cast<unsigned long long>(rva));
        return false;
    }

    // NiRefObject: refcount at +8, DeleteThis = vfunc 1 (as the engine releases them).
    __declspec(noinline) void SafeIncRef(void* o)
    {
        __try { if (o) _InterlockedIncrement(reinterpret_cast<volatile long*>(reinterpret_cast<uint8_t*>(o) + 8)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __declspec(noinline) void SafeDecRef(void* o)
    {
        __try
        {
            if (!o) return;
            if (_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(reinterpret_cast<uint8_t*>(o) + 8), -1) == 1)
            {
                using Delete_t = void(__fastcall*)(void*);
                (*reinterpret_cast<Delete_t**>(o))[1](o);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    __declspec(noinline) bool SafeLocalLightOn(PlayerCharacter* pc, bool& on)
    {
        __try
        {
            if (!pc) return false;
            on = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(pc) + kPlayer_PipboyLight) != nullptr;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) void* SafeRoot3D(Actor* a)
    {
        __try
        {
            if (!a || (a->flags & TESObjectREFR::kFlag_IsDisabled) || !a->parentCell) return nullptr;
            return a->GetActorRootNode(false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // The engine's own form/parent choice for `actor` (sub_140D769C0, ON branch).
    // markerNode comes back with one reference the caller must release.
    __declspec(noinline) TESForm* SafeChooseLight(Actor* actor, void* root, void*& markerNode)
    {
        markerNode = nullptr;
        __try
        {
            uint64_t markerForm = 0;
            void*    node = nullptr;
            void*    ctx[2] = { &markerForm, &node };
            g_findMarker(root, ctx);
            markerNode = node;
            if (markerForm && node)
                return reinterpret_cast<TESForm*>(markerForm);
            TESForm* kw = g_ovrKeyword();
            if (kw && g_refHasKw(reinterpret_cast<TESObjectREFR*>(actor), kw))
                return g_ovrLight();
            TESForm* f = g_defaultObj(kDO_PipboyLight);
            return (f && f->formType == kFormType_LIGH_Local) ? f : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    __declspec(noinline) bool SafeBuild(TESForm* form, void* parent, void*& lightOut, void*& holderOut)
    {
        lightOut = holderOut = nullptr;
        __try
        {
            const float fade = *reinterpret_cast<const float*>(g_base + kRVA_LightFadeParam);
            void* light = nullptr;
            void* holder = g_build(form, nullptr, parent, 1, 1, 0, &light, fade, 0);
            lightOut = light;     // the builder stored it with a reference (PlayerCharacter+0xC38's)
            holderOut = holder;
            return light != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // The engine's OFF branch, in its order.
    __declspec(noinline) void SafeRemoveLight(void*& light, void*& holder)
    {
        __try
        {
            void* ssn = *reinterpret_cast<void**>(g_base + kRVA_ShadowSceneNode);
            if (ssn && light)
                g_shadowRemove(ssn, &light);
            if (holder)
            {
                void* inner = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(holder) + 0x28);
                if (inner)
                {
                    using Vf_t = void(__fastcall*)(void*);
                    (*reinterpret_cast<Vf_t**>(inner))[0x1E8 / 8](inner);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        SafeDecRef(light);
        SafeDecRef(holder);
        light = holder = nullptr;
    }

    void Remove(uint32_t eid, ProxyLight& pl, const char* why)
    {
        if (pl.light || pl.holder)
        {
            SafeRemoveLight(pl.light, pl.holder);
            ++g_dRemoved;
            LOG_UPDATED("[Flashlight] entity=%u light removed (%s)", eid, why);
        }
        pl = ProxyLight{};
    }

    Actor* ProxyOf(uint32_t eid)
    {
        CoSyncPlayer* p = g_CoSyncPlayerManager.GetPlayerByEntityID(eid);
        if (!p || p->isLocalPlayer || p->createType != CoSyncEntityType::Player || !p->hasSpawned || !p->actorRef)
            return nullptr;
        return CoSyncGameAPI::IsFormStillLive(static_cast<TESForm*>(p->actorRef)) ? p->actorRef : nullptr;
    }

    void ApplyRemote()
    {
        std::unordered_set<uint32_t> eids;
        for (const auto& kv : g_wantOn) eids.insert(kv.first);
        for (const auto& kv : g_applied) eids.insert(kv.first);
        for (uint32_t eid : eids)
        {
            auto w = g_wantOn.find(eid);
            const bool want = (w != g_wantOn.end()) && w->second;
            ProxyLight& pl = g_applied[eid];
            Actor* actor = ProxyOf(eid);
            void* root = actor ? SafeRoot3D(actor) : nullptr;
            if (!want || !actor || !root)
            {
                Remove(eid, pl, !want ? "owner turned it off" : "proxy has no 3D");
                continue;
            }
            void* marker = nullptr;
            TESForm* form = SafeChooseLight(actor, root, marker);
            void* parent = marker ? marker : root;
            const bool same = pl.light && pl.actor == actor && pl.root == root && pl.form == form;
            if (!same)
            {
                Remove(eid, pl, "proxy body or light form changed");
                void* light = nullptr;
                void* holder = nullptr;
                if (form && SafeBuild(form, parent, light, holder))
                {
                    SafeIncRef(holder);   // ours, as SetPipboyLight keeps it at +0xC40
                    pl.actor = actor; pl.root = root; pl.form = form; pl.light = light; pl.holder = holder;
                    ++g_dBuilt;
                    LOG_UPDATED("[Flashlight] entity=%u light ON: form 0x%08X on %s of proxy %p",
                        eid, form->formID, marker ? "its light marker" : "its root", static_cast<void*>(actor));
                }
            }
            SafeDecRef(marker);
        }
        for (auto it = g_applied.begin(); it != g_applied.end(); )
            it = (!it->second.light && !g_wantOn.count(it->first)) ? g_applied.erase(it) : std::next(it);
    }
}

namespace CoSyncFlashlightSync
{
    void Install()
    {
        LOG_REQUIREMENT("CoSyncFlashlightSync::Install", "Plugin load. Prologue-checks every engine function the light builder needs.");
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        g_ready = PrologueOk(kRVA_LightBuild, kPro_LightBuild, sizeof(kPro_LightBuild), "light builder") &&
                  PrologueOk(kRVA_FindLightMarker, kPro_FindMarker, sizeof(kPro_FindMarker), "light marker search") &&
                  PrologueOk(kRVA_OverrideKeyword, kPro_OvrKeyword, sizeof(kPro_OvrKeyword), "override keyword getter") &&
                  PrologueOk(kRVA_OverrideLight, kPro_OvrLight, sizeof(kPro_OvrLight), "override light getter") &&
                  PrologueOk(kRVA_RefHasKeyword, kPro_HasKeyword, sizeof(kPro_HasKeyword), "ref HasKeyword") &&
                  PrologueOk(kRVA_DefaultObject, kPro_DefaultObj, sizeof(kPro_DefaultObj), "default object getter") &&
                  PrologueOk(kRVA_ShadowRemove, kPro_ShadowRemove, sizeof(kPro_ShadowRemove), "shadow scene remove");
        if (g_ready)
        {
            g_build        = reinterpret_cast<LightBuild_t>(g_base + kRVA_LightBuild);
            g_findMarker   = reinterpret_cast<FindMarker_t>(g_base + kRVA_FindLightMarker);
            g_ovrKeyword   = reinterpret_cast<FormGetter_t>(g_base + kRVA_OverrideKeyword);
            g_ovrLight     = reinterpret_cast<FormGetter_t>(g_base + kRVA_OverrideLight);
            g_refHasKw     = reinterpret_cast<RefHasKw_t>(g_base + kRVA_RefHasKeyword);
            g_defaultObj   = reinterpret_cast<DefaultObj_t>(g_base + kRVA_DefaultObject);
            g_shadowRemove = reinterpret_cast<ShadowRemove_t>(g_base + kRVA_ShadowRemove);
        }
        LOG_INFO("[Flashlight] ready=%d (remote player lights built with the engine's own SetPipboyLight steps)", g_ready ? 1 : 0);
    }

    void Reset()
    {
        for (auto& kv : g_applied)
            Remove(kv.first, kv.second, "session reset");
        g_applied.clear();
        g_wantOn.clear();
        g_lastLocalOn = -1;
        g_lastSent = -100.0;
    }

    void Tick(double now)
    {
        LOG_REQUIREMENT("CoSyncFlashlightSync::Tick", "Game thread, every CoSync tick.");
        if (!CoSyncNet::IsConnected())
            return;

        // Owner: the engine's own ON state is PlayerCharacter+0xC38 != null.
        bool on = false;
        if (SafeLocalLightOn(CoSyncGetPlayer(), on))
        {
            const int v = on ? 1 : 0;
            if (v != g_lastLocalOn || now - g_lastSent >= 5.0)
            {
                const uint32_t me = CoSyncNet::GetMyEntityID();
                if (me != 0)
                {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "FL|%u|%d", me, v);
                    CoSyncTransport::Send(buf);
                    if (v != g_lastLocalOn)
                        LOG_UPDATED("[Flashlight] TX local light %s", on ? "ON" : "OFF");
                    g_lastLocalOn = v;
                    g_lastSent = now;
                    ++g_dTx;
                }
            }
        }

        static double s_nextApply = 0.0;
        if (g_ready && now >= s_nextApply)
        {
            s_nextApply = now + 0.2;
            ApplyRemote();
        }

        static double s_nextDiag = 0.0;
        if (now >= s_nextDiag)
        {
            s_nextDiag = now + 30.0;
            size_t lit = 0;
            for (const auto& kv : g_applied) if (kv.second.light) ++lit;
            if (g_dTx || g_dRx || g_dBuilt || g_dRemoved)
                LOG_UPDATED("[Flashlight] last 30 s: tx=%u rx=%u built=%u removed=%u | proxies lit now=%zu", g_dTx, g_dRx, g_dBuilt, g_dRemoved, lit);
            g_dTx = g_dRx = g_dBuilt = g_dRemoved = 0;
        }
    }

    bool IsFlashlightMsg(const std::string& msg)
    {
        return msg.rfind("FL|", 0) == 0;
    }

    void OnReceive(const std::string& msg)
    {
        LOG_REQUIREMENT("CoSyncFlashlightSync::OnReceive", "Game thread. Parses peer text defensively; only records the wanted state.");
        unsigned long eid = 0;
        int on = 0;
        if (std::sscanf(msg.c_str() + 3, "%lu|%d", &eid, &on) != 2 || eid == 0)
            return;
        if (eid == CoSyncNet::GetMyEntityID())
            return;
        ++g_dRx;
        const bool want = on != 0;
        auto it = g_wantOn.find(static_cast<uint32_t>(eid));
        if (it == g_wantOn.end() || it->second != want)
            LOG_UPDATED("[Flashlight] RX entity=%lu light %s", eid, want ? "ON" : "OFF");
        g_wantOn[static_cast<uint32_t>(eid)] = want;
    }

    void OnProxyRemoved(uint32_t entityID)
    {
        auto it = g_applied.find(entityID);
        if (it != g_applied.end())
        {
            Remove(entityID, it->second, "proxy removed");
            g_applied.erase(it);
        }
    }
}
