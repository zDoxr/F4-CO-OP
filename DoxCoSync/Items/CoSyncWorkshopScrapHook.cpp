#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncWorkshopScrapHook.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncWorldSync.h"
#include "CoSyncRequirementsLog.h"

#include "GameWorkshop.h"
#include "GameReferences.h"

#include "MinHook.h"

#include <atomic>
#include <cstdio>

// Workshop::ScrapReference
// Signature: void(Workshop::ContextData*, NiPointer<TESObjectREFR>*, BSTArray<...>*)
static constexpr uintptr_t kScrapReference_RVA = 0x393EC6;
//static constexpr uintptr_t kScrapReference_RVA = 0x030EC618;

using ScrapReferenceFn = void(__fastcall*)(
    Workshop::ContextData*,
    NiPointer<TESObjectREFR>*,
    void*);

static ScrapReferenceFn     g_origScrap     = nullptr;
static std::atomic<bool>    g_hookInstalled{ false };

// UPDATE 2026-06-17: this was the only one of the 5 hooks installed at
// hosting-start (WorkshopScrap/DoorHook/PickupHook/ModEventHook/QuestHook)
// that dereferenced an engine-supplied pointer raw, with no SEH/SafeRead
// guard -- every sibling hook (CoSyncDoorHook.cpp, CoSyncPickupHook.cpp)
// already treats refs as untrusted via SafeReadU32/SafeReadPtr-style
// helpers. Root-cause theory: the user's real repro is starting a hosted
// session from the MAIN MENU (before any save is loaded) -- all 5 hooks
// go live immediately, then the actual world/save load proceeds with them
// already active. If the engine touches a workshop ref in a transient
// state during that load window, this unguarded triple-dereference
// (ref->formID, ref->baseForm->formID, ref->pos) was a real AV/corruption
// risk with nothing here to catch it. `__declspec(noinline)` is required
// on the SEH-guarded helper itself, not just style -- a non-noinline
// static __try/__except function can be inlined away with its SEH scope
// silently dropped under this project's /GL+/LTCG Release config
// (confirmed and fixed for an unrelated logging bug earlier this session;
// the same risk applies to every `static` SEH helper in this codebase,
// including CoSyncDoorHook.cpp/CoSyncPickupHook.cpp's own SafeReadU32/
// SafeReadPtr/SafeReadU8, which are NOT marked noinline -- worth auditing
// those too if this exact crash class resurfaces there).
__declspec(noinline) static bool SafeReadRefSnapshot(
    TESObjectREFR* ref, uint32_t& refFormID, uint32_t& baseFormID,
    float& posX, float& posY, float& posZ)
{
    LOG_REQUIREMENT("CoSyncWorkshopScrapHook::SafeReadRefSnapshot", "Needs a non-null reference. Wrapped in a crash guard so a transient or invalid reference fails safely instead of crashing.");
    __try
    {
        refFormID = ref->formID;
        if (ref->baseForm)
            baseFormID = ref->baseForm->formID;
        posX = ref->pos.x;
        posY = ref->pos.y;
        posZ = ref->pos.z;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void __fastcall Detour_ScrapReference(
    Workshop::ContextData*     contextData,
    NiPointer<TESObjectREFR>*  akRef,
    void*                      materials)
{
    LOG_REQUIREMENT("CoSyncWorkshopScrapHook::Detour_ScrapReference", "Needs the hook to be correctly installed on the real scrap function, and the networking layer to be initialized for the connection/host/entity-ID calls used when broadcasting.");
    // round 94 diagnostic: unconditional fire-heartbeat, confirms the
    // round-93 RVA fix (kScrapReference_RVA) is actually being reached.
    {
        static std::atomic<uint32_t> s_n{ 0 };
        const uint32_t n = ++s_n;
        if (n <= 5 || n % 200 == 0)
            LOG_INFO("[WorkshopScrap] scrap hook fired #%u", n);
    }

    // Capture before original runs — ref is freed/deleted inside ScrapReference
    uint32_t refFormID  = 0;
    uint32_t baseFormID = 0;
    float    posX = 0.f, posY = 0.f, posZ = 0.f;

    if (akRef && akRef->m_pObject)
    {
        if (!SafeReadRefSnapshot(akRef->m_pObject, refFormID, baseFormID, posX, posY, posZ))
        {
            LOG_WARN("[WorkshopScrap] reading the reference crashed — it was in an "
                     "invalid/transient state, skipping the broadcast for this scrap");
            refFormID = baseFormID = 0;
        }
    }

    if (g_origScrap)
        g_origScrap(contextData, akRef, materials);

    // Stop tracking the scrapped ref so the deletion sweep does not report
    // it a second time and any remote->local mapping to it is dropped.
    if (refFormID != 0)
        CoSyncWorldSync::NotifyLocalBuildingRemoved(refFormID);

    // Both roles broadcast their own scraps; host also relays to other peers.
    if (baseFormID == 0 || !CoSyncNet::IsConnected())
        return;

    CoSyncWorldSync::WorkshopScrapFullPacket p{};
    p.entityID   = CoSyncNet::GetMyEntityID();
    p.refFormID  = refFormID;
    p.baseFormID = baseFormID;
    p.posX       = posX;
    p.posY       = posY;
    p.posZ       = posZ;

    CoSyncTransport::Send(CoSyncWorldSync::SerializeWorkshopScrapFull(p));
    LOG_UPDATED("[WorkshopScrap] sent scrap: ref=0x%08X base=0x%08X pos=(%.0f,%.0f,%.0f)",
        refFormID, baseFormID, posX, posY, posZ);
}

void InstallWorkshopScrapHook()
{
    LOG_REQUIREMENT("InstallWorkshopScrapHook",
        "Needs the target offset to be the verified real entry point of the workshop scrap function.");

    if (g_hookInstalled.exchange(true))
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    void* target = reinterpret_cast<void*>(base + kScrapReference_RVA);

    LOG_INFO("[WorkshopScrap] base=%p  target=%p  offset=0x%zX",
        (void*)base, target, kScrapReference_RVA);

    // Dump first 16 bytes so we can compare against IDA.
    const uint8_t* p = reinterpret_cast<const uint8_t*>(target);
    LOG_INFO(
        "[WorkshopScrap] bytes at target: %02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X %02X",
        p[0], p[1], p[2], p[3],
        p[4], p[5], p[6], p[7],
        p[8], p[9], p[10], p[11],
        p[12], p[13], p[14], p[15]);

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(target, &mbi, sizeof(mbi)))
    {
        LOG_INFO(
            "[WorkshopScrap] memory protect=0x%08X state=0x%08X type=0x%08X",
            mbi.Protect,
            mbi.State,
            mbi.Type);
    }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG_ERROR("[WorkshopScrap] hook system init failed: %d", (int)st);
        g_hookInstalled.store(false);
        return;
    }

    st = MH_CreateHook(
        target,
        reinterpret_cast<void*>(&Detour_ScrapReference),
        reinterpret_cast<void**>(&g_origScrap));

    if (st != MH_OK)
    {
        LOG_ERROR(
            "[WorkshopScrap] creating the hook failed (%d) target=%p original=%p",
            (int)st,
            target,
            g_origScrap);

        g_hookInstalled.store(false);
        return;
    }

    st = MH_EnableHook(target);

    if (st != MH_OK)
    {
        LOG_ERROR("[WorkshopScrap] enabling the hook failed: %d", (int)st);
        MH_RemoveHook(target);
        g_hookInstalled.store(false);
        return;
    }

    LOG_INFO("[WorkshopScrap] hook installed at %p (offset 0x%zX)",
        target,
        kScrapReference_RVA);
}

void UninstallWorkshopScrapHook()
{
    LOG_REQUIREMENT("UninstallWorkshopScrapHook", "Needs the hook to have already been installed (guarded by a flag) before disabling and removing it.");
    if (!g_hookInstalled.exchange(false))
        return;

    const uintptr_t base   = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
    void*           target = reinterpret_cast<void*>(base + kScrapReference_RVA);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    LOG_INFO("[WorkshopScrap] hook removed");
}
