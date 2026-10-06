#include "CoSyncSaveLoad.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncGameAPI.h"

#include "MinHook.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstring>

namespace
{
    // ── Fallout4.exe 1.11.240 (IDA-verified 2026-09-25) ─────────────────────
    // Console 'load' (#117, execute 0x1405EFDF0):
    //   PreLoad(); flag=1; LoadGame(*mgr, name, -1, flags, true, false)
    // Console 'save' (#116, execute 0x1405EFCE0):
    //   PreSave(*mgr); SaveGame(*mgr, name, -1, flags, true)
    constexpr uintptr_t kRVA_SaveLoadMgr   = 0x032A8618;  // BGSSaveLoadManager* global
    constexpr uintptr_t kRVA_LoadGame      = 0x00BF98D0;
    constexpr uintptr_t kRVA_PreLoad       = 0x00C37720;
    constexpr uintptr_t kRVA_ConsoleLoadFlag = 0x032DD0FA; // byte the console path sets
    constexpr uintptr_t kRVA_SaveGame      = 0x00BF9330;
    constexpr uintptr_t kRVA_PreSave       = 0x00BFE8E0;
    constexpr uintptr_t kRVA_ShouldSaveRef = 0x004F59F0;  // TESObjectREFR vtable slot 16
    // Typed save (a2 = save type, 1 = autosave naming): builds the name,
    // creates the file, then calls the same writer as SaveGame (0x140BF9530).
    constexpr uintptr_t kRVA_SaveTyped     = 0x00BFD920;

    struct Prologue { uintptr_t rva; uint8_t bytes[10]; };
    constexpr Prologue kPrologues[] = {
        { kRVA_LoadGame,      { 0x48, 0x83, 0xEC, 0x48, 0x0F, 0xB6, 0x44, 0x24, 0x70, 0x88 } },
        { kRVA_PreLoad,       { 0x48, 0x83, 0xEC, 0x28, 0x80, 0x3D, 0xF5, 0x59, 0x6A, 0x02 } },
        { kRVA_SaveGame,      { 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 } },
        { kRVA_PreSave,       { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20 } },
        { kRVA_ShouldSaveRef, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 } },
        { kRVA_SaveTyped,     { 0x44, 0x88, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x41, 0x54 } },
    };

    using LoadGame_t = bool (*)(void* mgr, const char* name, int32_t slot, uint32_t flags, bool a5, bool a6);
    using PreLoad_t  = void (*)();
    using SaveGame_t = bool (*)(void* mgr, const char* name, int32_t slot, uint32_t flags, bool a5);
    using PreSave_t  = void (*)(void* mgr);
    using ShouldSaveRef_t = uint8_t (*)(TESObjectREFR* ref, void* saveBuffer);

    uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }

    int s_verified = -1;   // -1 unknown, 0 failed, 1 ok

    bool VerifyAll()
    {
        LOG_REQUIREMENT("(anonymous)::VerifyAll", "Reads code bytes of the running exe only.");
        if (s_verified >= 0)
            return s_verified == 1;
        const uintptr_t base = Base();
        for (const Prologue& p : kPrologues)
        {
            const auto* live = reinterpret_cast<const uint8_t*>(base + p.rva);
            if (std::memcmp(live, p.bytes, sizeof(p.bytes)) != 0)
            {
                LOG_ERROR("[SaveLoad] Prologue mismatch at RVA 0x%08llX -- not Fallout4.exe 1.11.240? "
                          "Shared-save features disabled.", (unsigned long long)p.rva);
                s_verified = 0;
                return false;
            }
        }
        s_verified = 1;
        LOG_INFO("[SaveLoad] Save/load entry points verified (1.11.240)");
        return true;
    }

    void* Manager()
    {
        return *reinterpret_cast<void**>(Base() + kRVA_SaveLoadMgr);
    }

    // ── Save filter ─────────────────────────────────────────────────────────
    ShouldSaveRef_t                 s_origShouldSave = nullptr;
    CoSyncSaveLoad::ProxyPredicate  s_isProxy = nullptr;

    uint8_t Hook_ShouldSaveRef(TESObjectREFR* ref, void* saveBuffer)
    {
        if (s_isProxy && ref && s_isProxy(ref))
            return 0;   // engine drops this reference's changes (BGSSaveLoadGame::SaveGame)
        return s_origShouldSave(ref, saveBuffer);
    }

    // ── Client save block ───────────────────────────────────────────────────
    using SaveGameHook_t  = uint64_t (*)(void* mgr, const char* name, uint32_t slot, uint32_t flags, char a5);
    using SaveTypedHook_t = char (*)(void* mgr, int type, char a3);

    SaveGameHook_t               s_origSaveGame  = nullptr;
    SaveTypedHook_t              s_origSaveTyped = nullptr;
    CoSyncSaveLoad::SaveBlocker  s_shouldBlock   = nullptr;
    ULONGLONG                    s_lastBlockNote = 0;

    bool RefuseSave(const char* what)
    {
        if (!s_shouldBlock || !s_shouldBlock())
            return false;
        LOG_INFO("[SaveLoad] Blocked a client %s -- the host's save is the shared world", what);
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastBlockNote > 3000)   // autosave bursts: one message
        {
            s_lastBlockNote = now;
            CoSyncGameAPI::QueueNotification("Only the host can save.");
        }
        return true;
    }

    uint64_t Hook_SaveGame(void* mgr, const char* name, uint32_t slot, uint32_t flags, char a5)
    {
        if (RefuseSave("save"))
            return 0;
        return s_origSaveGame(mgr, name, slot, flags, a5);
    }

    char Hook_SaveTyped(void* mgr, int type, char a3)
    {
        if (RefuseSave("autosave"))
            return 0;
        return s_origSaveTyped(mgr, type, a3);
    }
}

namespace CoSyncSaveLoad
{
    bool IsAvailable()
    {
        return VerifyAll();
    }

    bool SaveNamed(const char* name)
    {
        LOG_REQUIREMENT("CoSyncSaveLoad::SaveNamed", "MAIN THREAD ONLY. Synchronous.");
        if (!name || !*name || !VerifyAll())
            return false;
        void* mgr = Manager();
        if (!mgr)
        {
            LOG_ERROR("[SaveLoad] SaveNamed: save/load manager is null");
            return false;
        }
        LOG_INFO("[SaveLoad] Saving '%s'...", name);
        reinterpret_cast<PreSave_t>(Base() + kRVA_PreSave)(mgr);
        const bool ok = reinterpret_cast<SaveGame_t>(Base() + kRVA_SaveGame)(mgr, name, -1, 0, true);
        LOG_INFO("[SaveLoad] Save '%s' -> %s", name, ok ? "ok" : "FAILED");
        return ok;
    }

    bool LoadNamed(const char* name)
    {
        LOG_REQUIREMENT("CoSyncSaveLoad::LoadNamed", "MAIN THREAD ONLY (call from an F4SE task, like the console does). Synchronous.");
        if (!name || !*name || !VerifyAll())
            return false;
        void* mgr = Manager();
        if (!mgr)
        {
            LOG_ERROR("[SaveLoad] LoadNamed: save/load manager is null");
            return false;
        }
        LOG_INFO("[SaveLoad] Loading '%s'...", name);
        reinterpret_cast<PreLoad_t>(Base() + kRVA_PreLoad)();
        *reinterpret_cast<uint8_t*>(Base() + kRVA_ConsoleLoadFlag) = 1;
        // Arg 5 = "check for a vanilla save loaded with mods". When true the
        // engine (0x140BF9980) shows "$LoadVanillaSaveWithMods" ("Creations
        // are enabled...") and returns false WITHOUT loading -- the console
        // passes true, and that broke joining (2026-09-25). The host and the
        // client run the same mods (build + load-order checks still run), so
        // skip that prompt.
        const bool ok = reinterpret_cast<LoadGame_t>(Base() + kRVA_LoadGame)(mgr, name, -1, 0, false, false);
        LOG_INFO("[SaveLoad] Load '%s' -> %s", name, ok ? "ok" : "FAILED");
        return ok;
    }

    // 0.4.2: the engine's own "can the game be saved right now" gate. SaveGame
    // (0xBF9330) calls it first and fails without writing a file when it says
    // no (player dying/dead, a menu that forbids saving, VATS, mid power-armor
    // enter/exit, scene/dialogue states, ...). IDA 1.11.240: sub_140BFB3A0(mgr).
    int EngineCanSave()
    {
        LOG_REQUIREMENT("CoSyncSaveLoad::EngineCanSave", "Main thread. Read-only engine check.");
        static const uint8_t kPro[16] = { 0x48,0x89,0x5C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF1,0x48,0x8B,0x0D };
        constexpr uintptr_t kRVA_CanSave = 0x00BFB3A0;
        const auto* fn = reinterpret_cast<const uint8_t*>(Base() + kRVA_CanSave);
        if (std::memcmp(fn, kPro, sizeof(kPro)) != 0)
            return -1;
        void* mgr = Manager();
        if (!mgr)
            return -1;
        return reinterpret_cast<bool (*)(void*)>(Base() + kRVA_CanSave)(mgr) ? 1 : 0;
    }

    std::string GetSaveFolder()
    {
        LOG_REQUIREMENT("CoSyncSaveLoad::GetSaveFolder", "Doesn't touch the game. Reads the player's INI files.");
        char docs[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
            return std::string();
        const std::string root = std::string(docs) + "\\My Games\\Fallout4\\";

        // SLocalSavePath: Fallout4Custom.ini overrides Fallout4.ini; default "Saves\".
        char rel[MAX_PATH] = {};
        GetPrivateProfileStringA("General", "SLocalSavePath", "", rel, sizeof(rel), (root + "Fallout4Custom.ini").c_str());
        if (!rel[0])
            GetPrivateProfileStringA("General", "SLocalSavePath", "Saves\\", rel, sizeof(rel), (root + "Fallout4.ini").c_str());
        std::string out = root + rel;
        if (!out.empty() && out.back() != '\\' && out.back() != '/')
            out += '\\';
        return out;
    }

    bool InstallSaveFilter(ProxyPredicate isCoSyncProxy)
    {
        LOG_REQUIREMENT("CoSyncSaveLoad::InstallSaveFilter", "Plugin load. Needs the verified 1.11.240 exe.");
        if (s_origShouldSave)
            return true;
        if (!VerifyAll())
            return false;
        s_isProxy = isCoSyncProxy;

        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[SaveLoad] MH_Initialize failed: %d", (int)st);
            return false;
        }
        void* target = reinterpret_cast<void*>(Base() + kRVA_ShouldSaveRef);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_ShouldSaveRef),
                          reinterpret_cast<void**>(&s_origShouldSave)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            LOG_ERROR("[SaveLoad] Could not hook the reference save check");
            s_origShouldSave = nullptr;
            return false;
        }
        LOG_INFO("[SaveLoad] Save filter installed -- CoSync proxies are no longer written into saves");
        return true;
    }

    bool InstallSaveBlock(SaveBlocker shouldBlock)
    {
        LOG_REQUIREMENT("CoSyncSaveLoad::InstallSaveBlock", "Plugin load. Needs the verified 1.11.240 exe.");
        if (s_origSaveGame)
            return true;
        if (!VerifyAll())
            return false;
        s_shouldBlock = shouldBlock;

        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_ERROR("[SaveLoad] MH_Initialize failed: %d", (int)st);
            return false;
        }
        void* saveGame  = reinterpret_cast<void*>(Base() + kRVA_SaveGame);
        void* saveTyped = reinterpret_cast<void*>(Base() + kRVA_SaveTyped);
        if (MH_CreateHook(saveGame, reinterpret_cast<void*>(&Hook_SaveGame),
                          reinterpret_cast<void**>(&s_origSaveGame)) != MH_OK ||
            MH_CreateHook(saveTyped, reinterpret_cast<void*>(&Hook_SaveTyped),
                          reinterpret_cast<void**>(&s_origSaveTyped)) != MH_OK ||
            MH_EnableHook(saveGame) != MH_OK || MH_EnableHook(saveTyped) != MH_OK)
        {
            LOG_ERROR("[SaveLoad] Could not hook the save entry points -- client saves are NOT blocked");
            MH_DisableHook(saveGame);
            MH_DisableHook(saveTyped);
            s_origSaveGame = nullptr;
            s_origSaveTyped = nullptr;
            return false;
        }
        LOG_INFO("[SaveLoad] Client save block installed");
        return true;
    }
}
