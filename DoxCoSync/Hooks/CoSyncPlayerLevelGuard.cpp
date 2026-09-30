// Player level/XP guard (0.3.5 hotfix, host crash while loading a save,
// 2026-09-29 20:47, CoSyncUpdated_40292.log + CoSync_crash_40292.dmp).
//
// IDA 1.11.240: the player's level-up check sub_140D80110 (reached from the
// Experience actor-value change handler sub_140CCAC90, which also fires while
// a save loads) is
//     while (XP >= need(level + 1)) { level++ (u16); queue a "level up" HUD
//                                     message (sub_1407F8090); ... }
// with need(L) = 0.5*A*(L-1)*(L-2) + B*(L-1) (sub_1406CECC0), level read by
// sub_14030A7E0 from the player's TESNPC ACBS (+0x68: flags DWORD +0x08,
// level WORD +0x0E; with flag 0x80 "PC level mult" the level is computed from
// the player's own level * 0.001) and written by sub_14030A870. The level is
// 16-bit: once XP >= need(65535) the level wraps to 0 and the loop never
// ends -- the crash dump shows the wrapped level and the HUD message array
// grown to ~512 MB before memcpy ran off the end.
//
// Verified cause found in CoSync: SetLocalPlayerEssential(false) zeroed the
// player's level (+0x0E) through a widened 64-bit mask (logged flags
// 0x2003200000026 -> 0x24). Fixed at the source (ActorBaseFlags32). This
// guard repairs a player record that is already impossible -- so saves made
// before the fix load again -- and logs what it found:
//   * ACBS flag 0x80 on the PLAYER's own record (its level would be derived
//     from itself: always 0)            -> cleared
//   * level 0                           -> 1 (the loop then climbs normally)
//   * XP not finite or >= need(65000)   -> XP set to need(level), the value
//                                          the current level starts at
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncPlayerLevelGuard.h"
#include "CoSyncActorValueSafety.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "MinHook.h"

#include <cmath>
#include <cstring>

namespace
{
    constexpr uintptr_t kRVA_LevelUpCheck = 0x00D80110;
    constexpr uint8_t   kLevelUpCheckPrologue[14] =
        { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
    constexpr uintptr_t kRVA_XPForLevel   = 0x006CECC0;   // float(int level)
    constexpr uint32_t  kAV_Experience    = 0x000002C9;
    constexpr uint32_t  kACBS_PCLevelMult = 0x80;

    using LevelUp_t = int64_t(__fastcall*)(void* player);
    using XPFor_t   = float(__fastcall*)(int level);

    uintptr_t g_base = 0;
    LevelUp_t g_orig = nullptr;
    thread_local int t_in = 0;

    __declspec(noinline) void Repair_Impl(void* player)
    {
        uint8_t* npc = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(player) + 0xE0);
        if (!npc)
            return;
        uint8_t* acbs = npc + 0x68;
        uint32_t& flags = *reinterpret_cast<uint32_t*>(acbs + 0x08);
        uint16_t& level = *reinterpret_cast<uint16_t*>(acbs + 0x0E);
        if (flags & kACBS_PCLevelMult)
        {
            LOG_ERROR("[LevelGuard] the player's own record had 'PC level mult' (flags 0x%08X) -- cleared", flags);
            flags &= ~kACBS_PCLevelMult;
        }
        if (level == 0)
        {
            LOG_ERROR("[LevelGuard] player level was 0 -- set to 1 (XP decides the rest)");
            level = 1;
        }
        TESForm* f = LookupFormByID(kAV_Experience);
        ActorValueInfo* xpAV = (f && f->formType == kFormType_AVIF) ? reinterpret_cast<ActorValueInfo*>(f) : nullptr;
        if (!xpAV)
            return;
        float xp = 0.0f;
        if (!CoSyncAVSafety::SafeGetValue(static_cast<Actor*>(player), xpAV, xp))
            return;
        const XPFor_t need = reinterpret_cast<XPFor_t>(g_base + kRVA_XPForLevel);
        if (!std::isfinite(xp) || xp >= need(65000))
        {
            const float target = need(level);
            LOG_ERROR("[LevelGuard] player XP %g is impossible (level %u) -- set to %g", xp, level, target);
            CoSyncAVSafety::SafeSetBase(static_cast<Actor*>(player), xpAV, target);
        }
    }

    void Repair(void* player)
    {
        __try { Repair_Impl(player); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    int64_t __fastcall Detour_LevelUpCheck(void* player)
    {
        if (player && t_in == 0)
        {
            ++t_in;           // setting XP re-enters this check
            Repair(player);
            --t_in;
        }
        return g_orig(player);
    }
}

void CoSyncPlayerLevelGuard::Install()
{
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    void* target = reinterpret_cast<void*>(g_base + kRVA_LevelUpCheck);
    if (std::memcmp(target, kLevelUpCheckPrologue, sizeof(kLevelUpCheckPrologue)) != 0)
    {
        LOG_ERROR("[LevelGuard] level-up check prologue mismatch (not 1.11.240?) -- guard not installed");
        return;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return;
    st = MH_CreateHook(target, reinterpret_cast<void*>(&Detour_LevelUpCheck), reinterpret_cast<void**>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        LOG_ERROR("[LevelGuard] hook failed: %d", (int)st);
        return;
    }
    LOG_INFO("[LevelGuard] player level-up check guarded (base+0xD80110)");
}
