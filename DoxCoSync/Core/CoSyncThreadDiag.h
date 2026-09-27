#pragma once

// Which thread does each engine hook actually run on?
//
// Phase 6/8 audit (2026-09-26): several CoSync hooks touch game-thread-owned
// state (the player manager, QuestSync maps, bone writes), but the engine's
// actor updates are scheduled through an "ActorUpdateJobList" (BSJob workers),
// so the calling thread of each hook is UNPROVEN. This records it.
//
// Hooks call Note(site) and nothing else: one GetCurrentThreadId() plus a
// lock-free scan of a tiny fixed table -- no logging, no allocation, no locks,
// because logging from inside engine hooks has crashed this project before.
// The main-thread tick calls Report() which prints any new (site, thread) pair
// or changed hit count at most every 5 seconds.
//
// Diagnostic only: it changes no behaviour.

#include <atomic>
#include <cstdint>
#include <windows.h>

#include "ConsoleLogger.h"

namespace CoSyncThreadDiag
{
    enum Site : uint32_t
    {
        kRemoteActorUpdate = 0,   // TickHook: vtable slot 21 on the shared Character vtable
        kLocalActorUpdate,        // TickHook: vtable slot 21 on PlayerCharacter
        kDoorActivate,            // CoSyncDoorHook Detour_ActivateWorker
        kPickup,                  // CoSyncPickupHook Detour_InvAddFromWorld
        kQuestStage,              // CoSyncQuestHook Detour_SetCurrentStageID
        kEquip,                   // CoSyncEquipHook Detour_EquipObject / Detour_UnequipObject
        kDialogueLine,            // CoSyncDialogSync Detour_BuildDialogueItem
        kAnimEvent,               // CoSyncAnimEventHook HookedReceiveEvent
        kLockFlip,                // CoSyncLockHook Detour_ForceLock / Detour_ForceUnlock
        kPapyrusGetActor,         // Papyrus native GetActorByEntityID (flagged NoWait)
        kSiteCount
    };

    inline const char* SiteName(uint32_t s)
    {
        static const char* const kNames[kSiteCount] = {
            "RemoteActorUpdate(slot21)", "LocalActorUpdate(slot21)", "DoorActivate",
            "Pickup", "QuestStage", "Equip", "DialogueLine", "AnimEvent", "LockFlip",
            "PapyrusGetActorByEntityID"
        };
        return s < kSiteCount ? kNames[s] : "?";
    }

    constexpr int kMaxThreadsPerSite = 8;

    struct SiteSlots
    {
        std::atomic<uint32_t> tid[kMaxThreadsPerSite];
        std::atomic<uint64_t> hits[kMaxThreadsPerSite];
    };

    // Static storage: zero-initialised before any hook can run.
    inline SiteSlots             g_sites[kSiteCount];
    inline std::atomic<uint32_t> g_mainTid{ 0 };   // current CoSync tick thread
    inline std::atomic<uint32_t> g_overflow{ 0 };

    // Can window-thread code run WHILE the tick is running? In game the tick
    // is the engine's "NonRendersafeAI" job (Fallout4 sub_140BD6170 in the
    // frame job list) on a worker thread, while WndProc and PrismaUI's UI
    // callbacks run on the window thread. Whether the two ever overlap is
    // UNCONFIRMED from IDA; this counts it. The tick holds a TickScope; the
    // window-thread entry points call NoteWindowEntry (no logging there).
    inline std::atomic<uint32_t> g_tickActive{ 0 };
    inline std::atomic<uint64_t> g_windowOverlaps{ 0 };
    inline std::atomic<uint64_t> g_windowEntries{ 0 };

    struct TickScope
    {
        TickScope()  { g_tickActive.fetch_add(1, std::memory_order_acq_rel); }
        ~TickScope() { g_tickActive.fetch_sub(1, std::memory_order_acq_rel); }
    };

    inline void NoteWindowEntry()
    {
        g_windowEntries.fetch_add(1, std::memory_order_relaxed);
        if (g_tickActive.load(std::memory_order_acquire) != 0 &&
            ::GetCurrentThreadId() != g_mainTid.load(std::memory_order_relaxed))
            g_windowOverlaps.fetch_add(1, std::memory_order_relaxed);
    }

    // Any thread. Must stay trivially cheap and side-effect free.
    inline void Note(Site s)
    {
        if (s >= kSiteCount)
            return;
        const uint32_t tid = ::GetCurrentThreadId();
        SiteSlots& ss = g_sites[s];
        for (int i = 0; i < kMaxThreadsPerSite; ++i)
        {
            uint32_t cur = ss.tid[i].load(std::memory_order_relaxed);
            if (cur == 0)
            {
                uint32_t expected = 0;
                if (ss.tid[i].compare_exchange_strong(expected, tid, std::memory_order_relaxed))
                    cur = tid;
                else
                    cur = expected;   // another thread claimed the slot first
            }
            if (cur == tid)
            {
                ss.hits[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        g_overflow.fetch_add(1, std::memory_order_relaxed);
    }

    // Main (game) thread only: called once per tick from CoSyncRuntime.
    inline void Report(double now)
    {
        // CoSync's tick is an F4SE task: it runs on whichever thread pumps that
        // queue, and that thread CHANGES (window thread at the main menu, a
        // different thread in game -- proven by the 2026-09-26 crash dump).
        // Track the current one instead of freezing the first.
        const uint32_t cur = ::GetCurrentThreadId();
        const uint32_t prev = g_mainTid.exchange(cur, std::memory_order_relaxed);
        // In game the tick hops between the engine's job threads almost every
        // frame (~96k of these lines per session in the 2026-09-26 logs), so
        // only the first few changes are logged.
        static uint32_t s_changesLogged = 0;
        if (prev != cur && s_changesLogged < 16)
        {
            ++s_changesLogged;
            LOG_INFO("[ThreadDiag] CoSync tick thread is now tid=%u (was %u)%s", cur, prev,
                s_changesLogged == 16 ? " -- further changes not logged" : "");
        }

        static double s_lastReport = 0.0;
        if (now - s_lastReport < 5.0)
            return;
        s_lastReport = now;

        static uint64_t s_lastHits[kSiteCount][kMaxThreadsPerSite] = {};
        const uint32_t mainTid = g_mainTid.load(std::memory_order_relaxed);
        for (uint32_t s = 0; s < kSiteCount; ++s)
        {
            for (int i = 0; i < kMaxThreadsPerSite; ++i)
            {
                const uint32_t tid = g_sites[s].tid[i].load(std::memory_order_relaxed);
                if (tid == 0)
                    break;
                const uint64_t hits = g_sites[s].hits[i].load(std::memory_order_relaxed);
                if (hits == s_lastHits[s][i])
                    continue;
                LOG_INFO("[ThreadDiag] site=%s tid=%u (%s) hits=%llu (+%llu)",
                    SiteName(s), tid, tid == mainTid ? "TICK" : "OTHER",
                    (unsigned long long)hits,
                    (unsigned long long)(hits - s_lastHits[s][i]));
                s_lastHits[s][i] = hits;
            }
        }
        const uint32_t overflow = g_overflow.load(std::memory_order_relaxed);
        static uint32_t s_lastOverflow = 0;
        if (overflow != s_lastOverflow)
        {
            LOG_INFO("[ThreadDiag] %u hook calls from threads beyond the per-site table (%d slots)",
                overflow - s_lastOverflow, kMaxThreadsPerSite);
            s_lastOverflow = overflow;
        }
        const uint64_t overlaps = g_windowOverlaps.load(std::memory_order_relaxed);
        static uint64_t s_lastOverlaps = 0;
        if (overlaps != s_lastOverlaps)
        {
            LOG_WARN("[ThreadDiag] window-thread code ran WHILE the tick was running: %llu times (+%llu) of %llu window entries",
                (unsigned long long)overlaps, (unsigned long long)(overlaps - s_lastOverlaps),
                (unsigned long long)g_windowEntries.load(std::memory_order_relaxed));
            s_lastOverlaps = overlaps;
        }
    }
}
