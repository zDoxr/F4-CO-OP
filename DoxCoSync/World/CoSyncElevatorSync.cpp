#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncElevatorSync.h"

#include "CoSyncDoorHook.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "PapyrusVM.h"
#include "PapyrusInterfaces.h"
#include "PapyrusValue.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();
extern RelocAddr<_LookupFormByID> LookupFormByID;

namespace
{
    // TESObjectREFR linked ref by keyword (nullptr keyword = the default
    // link): sub_1405648B0(ref, keyword) -- the body of the Papyrus native
    // ObjectReference.GetLinkedRef (sub_1411814B0), 68 engine callers; returns
    // the ref as a raw pointer. IDA 1.11.240, 2026-10-05.
    constexpr uintptr_t kRVA_GetLinkedRef = 0x005648B0;
    constexpr uint8_t   kPro_GetLinkedRef[10] = { 0x48,0x8B,0x89,0x00,0x01,0x00,0x00,0x41,0xB0,0x01 };

    // Fallout4.esm LinkCustom01..10 (the keywords the elevator masters use).
    constexpr uint32_t kLinkKeywords[] = {
        0x0005D5E6, 0x0005D5E7, 0x0005D5E8, 0x0005D5FE, 0x0003094F,
        0x00030950, 0x00030951, 0x00030952, 0x00030953, 0x00030972 };

    const char* const kMovingMasters[] = { "ElevatorMasterScript", "SimpleElevatorMasterScript" };
    const char* const kLoadScripts[]   = { "LoadElevatorPanelScript", "LoadElevatorDoorScript", "LoadElevatorMasterScript" };

    constexpr uint8_t kFormType_ACTI_ = 0x1B;   // matches CoSyncDoorHook's type reads
    constexpr uint8_t kFormType_DOOR_ = 0x20;

    using GetLinkedRef_t = TESObjectREFR*(__fastcall*)(TESObjectREFR*, TESForm*);
    GetLinkedRef_t g_getLinkedRef = nullptr;

    // ── Scan results (written on the game thread, read by the door hook) ────
    std::mutex                             g_mx;
    std::unordered_map<uint32_t, uint32_t> g_controlToMaster;   // moving control -> master ref
    std::unordered_set<uint32_t>           g_loadRefs;          // load-elevator panels / doors / masters

    TESObjectCELL* g_scannedCell = nullptr;
    double         g_nextScan = 0.0;

    // ── Received ELEV, applied on the game thread ───────────────────────────
    std::mutex             g_rxMx;
    std::vector<uint32_t>  g_rx;   // control ref form IDs

    // Last activation per master on this machine (local press or replay):
    // two players pressing together move the car once.
    std::mutex                           g_lastMx;   // door hook thread + CoSync tick
    std::unordered_map<uint32_t, double> g_lastByMaster;
    constexpr double kDedupeSeconds = 1.5;

    double Now()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return double(c.QuadPart) / double(f.QuadPart);
    }

    bool LinkedRefReady()
    {
        static int s_ok = -1;
        if (s_ok < 0)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            s_ok = std::memcmp(reinterpret_cast<const void*>(base + kRVA_GetLinkedRef), kPro_GetLinkedRef, sizeof(kPro_GetLinkedRef)) == 0 ? 1 : 0;
            if (s_ok)
                g_getLinkedRef = reinterpret_cast<GetLinkedRef_t>(base + kRVA_GetLinkedRef);
            else
                LOG_ERROR("[ElevSync] GetLinkedRef prologue mismatch (not 1.11.240?) -- moving elevators not shared");
        }
        return s_ok == 1;
    }

    __declspec(noinline) TESObjectREFR* SafeLinked(TESObjectREFR* ref, TESForm* keyword)
    {
        __try { return ref ? g_getLinkedRef(ref, keyword) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    __declspec(noinline) uint8_t SafeBaseType(TESObjectREFR* ref)
    {
        __try { return (ref && ref->baseForm) ? ref->baseForm->formType : 0xFF; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFF; }
    }

    // Does `ref` have a bound script object of this type? The same
    // GetObjectIdentifier lookup F4SE uses to pass a form to Papyrus.
    bool HasScript(VirtualMachine* vm, UInt64 handle, const char* typeName)
    {
        VMIdentifier* id = nullptr;
        const BSFixedString name(typeName);
        if (!vm->GetObjectIdentifier(handle, name, 0, &id, 0) || !id)
            return false;
        if (!id->DecrementLock())
            id->Destroy();
        return true;
    }

    // One cell: masters' linked controls + load-elevator refs. Game thread.
    void ScanCell(TESObjectCELL* cell, std::unordered_map<uint32_t, uint32_t>& controls, std::unordered_set<uint32_t>& loadRefs)
    {
        VirtualMachine* vm = (*g_gameVM) ? (*g_gameVM)->m_virtualMachine : nullptr;
        if (!vm || !cell)
            return;
        IObjectHandlePolicy* policy = vm->GetHandlePolicy();
        if (!policy)
            return;
        std::vector<TESForm*> keywords;
        keywords.push_back(nullptr);
        for (uint32_t k : kLinkKeywords)
            if (TESForm* f = LookupFormByID(k))
                keywords.push_back(f);

        std::vector<TESObjectREFR*> masters;
        for (UInt32 i = 0; i < cell->objectList.count; ++i)
        {
            TESObjectREFR* ref = cell->objectList.entries[i];
            if (!ref || !ref->baseForm)
                continue;
            const UInt64 handle = policy->Create(TESObjectREFR::kTypeID, ref);
            if (handle == policy->GetInvalidHandle())
                continue;
            for (const char* s : kLoadScripts)
                if (HasScript(vm, handle, s)) { loadRefs.insert(ref->formID); break; }
            for (const char* s : kMovingMasters)
                if (HasScript(vm, handle, s)) { masters.push_back(ref); break; }
        }

        for (TESObjectREFR* m : masters)
        {
            std::unordered_set<TESObjectREFR*> seen;
            std::vector<TESObjectREFR*> found;
            for (TESForm* kw : keywords)
            {
                // A chain for each keyword (SimpleElevatorMasterScript walks
                // its buttons with GetLinkedRefChain(LinkCustom02)).
                TESObjectREFR* r = SafeLinked(m, kw);
                for (int depth = 0; r && depth < 16 && seen.insert(r).second; ++depth)
                {
                    found.push_back(r);
                    r = kw ? SafeLinked(r, kw) : nullptr;
                }
            }
            // ElevatorMasterScript also listens to its panel's own linked ref.
            const size_t direct = found.size();
            for (size_t i = 0; i < direct; ++i)
                if (TESObjectREFR* r = SafeLinked(found[i], nullptr))
                    if (seen.insert(r).second)
                        found.push_back(r);
            for (TESObjectREFR* r : found)
            {
                const uint8_t t = SafeBaseType(r);
                if ((t == kFormType_ACTI_ || t == kFormType_DOOR_) && !loadRefs.count(r->formID))
                    controls[r->formID] = m->formID;
            }
        }
    }

    __declspec(noinline) bool SafeScan(TESObjectCELL* cell, std::unordered_map<uint32_t, uint32_t>& controls, std::unordered_set<uint32_t>& loadRefs)
    {
        __try { ScanCell(cell, controls, loadRefs); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uint32_t MasterOf(uint32_t control)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_controlToMaster.find(control);
        return it == g_controlToMaster.end() ? 0u : it->second;
    }

    // True when this elevator moved within kDedupeSeconds; otherwise records
    // `now` as its move and returns false.
    bool AlreadyMoving(uint32_t master, double now)
    {
        std::lock_guard<std::mutex> lk(g_lastMx);
        auto it = g_lastByMaster.find(master);
        if (it != g_lastByMaster.end() && now - it->second < kDedupeSeconds)
            return true;
        g_lastByMaster[master] = now;
        return false;
    }

    __declspec(noinline) TESObjectREFR* SafeLookupRef(uint32_t formID)
    {
        __try
        {
            TESForm* f = LookupFormByID(formID);
            return (f && f->formType == kFormType_REFR) ? static_cast<TESObjectREFR*>(f) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void ApplyRemote(uint32_t control)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        TESObjectREFR* ref = SafeLookupRef(control);
        if (!pc || !ref)
            return;
        // Only where this player is: the same cell as the control (the scan
        // has it). Anyone elsewhere finds the car where they last left it --
        // the elevator's own script handles a call from another floor.
        const uint32_t master = MasterOf(control);
        if (!master || ref->parentCell != pc->parentCell)
        {
            LOG_INFO("[ElevSync] ELEV 0x%08X not applied: not in this player's cell", control);
            return;
        }
        if (AlreadyMoving(master, Now()))
        {
            LOG_INFO("[ElevSync] ELEV 0x%08X ignored: elevator 0x%08X just moved", control, master);
            return;
        }
        // As OUR player: the master script only acts on Game.GetPlayer().
        // ActivateAs marks it as an applied activation, so it is not sent back.
        const bool ok = CoSyncDoorHook::ActivateAs(ref, reinterpret_cast<TESObjectREFR*>(pc));
        LOG_UPDATED("[ElevSync] ELEV applied: control 0x%08X of elevator 0x%08X activated as the local player -> %s",
            control, master, ok ? "ok" : "refused by the engine");
    }
}

namespace CoSyncElevatorSync
{
    Kind Classify(uint32_t refFormID)
    {
        if (!refFormID)
            return Kind::None;
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_loadRefs.count(refFormID))
            return Kind::LoadElevator;
        if (g_controlToMaster.count(refFormID))
            return Kind::MovingControl;
        return Kind::None;
    }

    void OnLocalControlActivated(uint32_t refFormID)
    {
        if (!CoSyncNet::IsConnected())
            return;
        const uint32_t master = MasterOf(refFormID);
        if (!master)
            return;
        // The master's own script activations of its doors right after a press
        // are part of the same move -- one ELEV per move.
        if (AlreadyMoving(master, Now()))
            return;
        const std::string msg = "ELEV|" + std::to_string(CoSyncNet::GetMyEntityID()) + "|" + std::to_string(refFormID);
        CoSyncTransport::Send(msg);
        LOG_UPDATED("[ElevSync] TX ELEV control 0x%08X (elevator 0x%08X)", refFormID, master);
    }

    bool IsElevatorMessage(const std::string& msg)
    {
        return msg.rfind("ELEV|", 0) == 0;
    }

    void OnReceive(const std::string& msg, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncElevatorSync::OnReceive", "Network side; the host relays, the game thread applies.");
        const char* p = msg.c_str() + 5;
        char* end = nullptr;
        const uint32_t sender = static_cast<uint32_t>(std::strtoul(p, &end, 10));
        if (!end || *end != '|')
            return;
        const uint32_t control = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 10));
        if (!control)
            return;
        if (isHostRole)
            CoSyncTransport::Send(msg);   // to every client; the presser ignores its own
        if (sender == CoSyncNet::GetMyEntityID())
            return;
        std::lock_guard<std::mutex> lk(g_rxMx);
        if (g_rx.size() < 64)
            g_rx.push_back(control);
    }

    void Tick(double)
    {
        const double now = Now();
        PlayerCharacter* pc = CoSyncGetPlayer();
        TESObjectCELL* cell = pc ? pc->parentCell : nullptr;
        if (CoSyncNet::IsConnected() && LinkedRefReady() && (cell != g_scannedCell || now >= g_nextScan))
        {
            // Rescan on a cell change and every 10 s (scripts bind as refs load).
            std::unordered_map<uint32_t, uint32_t> controls;
            std::unordered_set<uint32_t> loadRefs;
            if (cell)
                SafeScan(cell, controls, loadRefs);
            const bool changed = cell != g_scannedCell;
            g_scannedCell = cell;
            g_nextScan = now + 10.0;
            if (changed && (!controls.empty() || !loadRefs.empty()))
                LOG_INFO("[ElevSync] cell %08X: %zu moving-elevator controls, %zu load-elevator refs",
                    cell ? cell->formID : 0u, controls.size(), loadRefs.size());
            std::lock_guard<std::mutex> lk(g_mx);
            g_controlToMaster.swap(controls);
            g_loadRefs.swap(loadRefs);
        }

        std::vector<uint32_t> rx;
        {
            std::lock_guard<std::mutex> lk(g_rxMx);
            rx.swap(g_rx);
        }
        for (uint32_t c : rx)
            ApplyRemote(c);
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_controlToMaster.clear();
            g_loadRefs.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_rxMx);
            g_rx.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_lastMx);
            g_lastByMaster.clear();
        }
        g_scannedCell = nullptr;
        g_nextScan = 0.0;
    }
}
