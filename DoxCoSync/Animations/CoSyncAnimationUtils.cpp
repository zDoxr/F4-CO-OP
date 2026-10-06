#include "CoSyncAnimationUtils.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameTypes.h"

#include <unordered_map>

// Verified via IDA against Fallout4.exe 1.11.221 by tracing
// IAnimationGraphManagerHolder::`vftable' (ea 0x142518A50) and cross-checking
// against the real Papyrus-registered GetAnimationVariableFloat/Int/Bool
// natives (which dispatch through slot[4]+152/160/168 = slots 19/20/21 -
// exact match). kSendAnimationEvent=1 also confirmed: its body internally
// calls slot 4 to fetch the graph then forwards the event name.
enum AnimHolderVtbl
{
    kSendAnimationEvent = 1,
    kGetBSAnimationGraph = 4,
    kGetVariableFloat    = 19,
    kGetVariableInt      = 20,
    kGetVariableBool     = 21,
};


// formID -> tick when 3D was first seen loaded (or last reset via
// ResetGraphGrace). Moved to file scope (was function-local static inside
// IsGraphReady) so ResetGraphGrace can clear an entry too. Heap-allocated on
// purpose: never destructed, so no atexit-order issues in a game DLL.
static CRITICAL_SECTION s_graphGraceCs;
static bool s_graphGraceCsInit = false;
static std::unordered_map<uint32_t, ULONGLONG>* s_first3d = nullptr;

static void EnsureGraphGraceInit()
{
    if (!s_graphGraceCsInit)
    {
        InitializeCriticalSection(&s_graphGraceCs);
        s_first3d = new std::unordered_map<uint32_t, ULONGLONG>();
        s_graphGraceCsInit = true;
    }
}

typedef bool (*_RealSetAnimVarFloat)(void* holder, BSFixedString* name, float value);
typedef bool (*_RealSetAnimVarInt)(void* holder, BSFixedString* name, int value);
typedef bool (*_RealSetAnimVarBool)(void* holder, BSFixedString* name, bool value);
static RelocAddr<_RealSetAnimVarFloat> g_realSetAnimVarFloat(0x008190F0);
static RelocAddr<_RealSetAnimVarInt>   g_realSetAnimVarInt(0x008190D0);
static RelocAddr<_RealSetAnimVarBool>  g_realSetAnimVarBool(0x008190B0);

static void* GetAnimHolder(Actor* actor)
{
    if (!actor)
        return nullptr;

    // Round 118: refuse to hand out the anim-graph holder while the actor's
    // 3D is not loaded. First anim write to a just-spawned remote proxy
    // (loadedData=0/rootNode=0, 02:16 session) hard-froze the main thread —
    // no fault, no VEH, no WER: consistent with spinning forever on a
    // mid-construction graph manager lock. Every Get*/Set*/SendEvent in this
    // file funnels through here, so this single gate makes them all no-op
    // (return false) until the engine finishes loading the actor's 3D.
    // Round 119: upgraded to IsGraphReady — 3D loaded was not enough; the
    // 02:36 session froze at the first engine SetInt 0.5s after 3D appeared
    // (loader thread still owned the graph-manager spin lock). Now also waits
    // a grace period after 3D load for non-player actors.
    if (!CoSyncAnimUtils::IsGraphReady(actor))
        return nullptr;

    void* holder = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(actor) + 0x48
        );

    if (!holder)
        return nullptr;

    void*** vtable = reinterpret_cast<void***>(holder);
    if (!vtable || !*vtable)
        return nullptr;

    return holder;
}

// ---------------------------------------------------------------------------
// Round 120: bounded try-lock around the engine's real SetAnimVar far-calls.
// IDA sub_14081AE50 (the worker behind all three setter thunks) acquires the
// BSAnimationGraphManager's re-entrant spin lock -- owner thread id at
// mgr+0xC8, recursion count at mgr+0xCC -- with an UNBOUNDED
// InterlockedCompareExchange/Sleep loop. Live sessions 02:36 and 15:22 froze
// the main thread forever at the FIRST such call on a remote proxy, even
// 3.5s after its 3D loaded (round-119 grace period held), so the lock is
// either owned permanently by another thread or the field holds garbage.
// Acquiring the lock ourselves first, with a bounded spin, converts that
// freeze into a skipped write plus a diagnostic logging the raw owner/count
// values. Once held, the engine's own acquire sees owner==CurrentThreadId
// and takes its re-entrant increment/decrement path -- it cannot block.

static void* GetGraphManagerAddRef(void* holder)
{
    // holder vtable slot 4 = GetBSAnimationGraph(holder, BSTSmartPointer& out)
    // -- AddRefs the manager into out. Pair every success with
    // ReleaseGraphManagerRef.
    void*** vtable = reinterpret_cast<void***>(holder);
    typedef bool (*GetBSAnimGraphFn)(void*, void**);
    auto fn = reinterpret_cast<GetBSAnimGraphFn>((*vtable)[kGetBSAnimationGraph]);
    void* mgr = nullptr;
    if (!fn(holder, &mgr))
        return nullptr;
    return mgr;
}

static void ReleaseGraphManagerRef(void* mgr)
{
    // Mirrors the engine worker's epilogue exactly: refcount at mgr+0x08; on
    // the last release, call vtable slot 0 (dtor) with (this, 1).
    if (!mgr) return;
    volatile LONG* rc = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<uint8_t*>(mgr) + 0x08);
    if (InterlockedExchangeAdd(rc, -1) == 1)
    {
        typedef void (*Slot0Fn)(void*, int);
        auto slot0 = reinterpret_cast<Slot0Fn>((*reinterpret_cast<void***>(mgr))[0]);
        slot0(mgr, 1);
    }
}

static bool TryAcquireMgrLock(void* mgr, uint32_t spins, DWORD* outOwner, LONG* outCount)
{
    volatile LONG*  count = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<uint8_t*>(mgr) + 0xCC);
    volatile DWORD* owner = reinterpret_cast<volatile DWORD*>(
        reinterpret_cast<uint8_t*>(mgr) + 0xC8);
    const DWORD tid = GetCurrentThreadId();
    if (*owner == tid)
    {
        // Already ours (e.g. a prior __except left it held) -- take the same
        // re-entrant path the engine does so acquire/release stay paired.
        InterlockedIncrement(count);
        return true;
    }
    for (uint32_t i = 0; i < spins; ++i)
    {
        if (InterlockedCompareExchange(count, 1, 0) == 0)
        {
            *owner = tid;
            MemoryBarrier();
            return true;
        }
        YieldProcessor();
    }
    *outOwner = *owner;
    *outCount = *count;
    return false;
}

static void ReleaseMgrLock(void* mgr)
{
    // Engine worker's exact release sequence.
    volatile LONG*  count = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<uint8_t*>(mgr) + 0xCC);
    volatile DWORD* owner = reinterpret_cast<volatile DWORD*>(
        reinterpret_cast<uint8_t*>(mgr) + 0xC8);
    if (*count == 1)
    {
        *owner = 0;
        MemoryBarrier();
        InterlockedCompareExchange(count, 0, 1);
    }
    else
    {
        InterlockedDecrement(count);
    }
}

// Round 117: this file is CoSync's own most-direct equivalent of the engine's
// crashing NpcAnimSync function -- every Get/Set call below dispatches
// through the exact same actor+0x48 IAnimationGraphManagerHolder vtable (or
// the same real native far-calls) that NpcAnimSync itself uses internally.
// None of it was breadcrumbed before now. `name` arguments at every call site
// are static string literals (CoSyncAnimUtils::Vars::*), so pointing these
// globals directly at them is safe -- no formatting, no lifetime issue, same
// plain-volatile-assignment convention as TickHook.cpp's NpcAnimSync
// diagnostics.
volatile const char* g_animUtils_lastOp      = nullptr;
volatile const char* g_animUtils_lastVarName = nullptr;
volatile void*       g_animUtils_lastActor   = nullptr;

// BSFixedString construction interns the string by walking the StringCache hash
// chain. An entry with sentinel externData=0xFFFFFFFFFFFFFFF7 in that chain
// causes c_str() to read [0xFFFFFFFFFFFFFFFF] -> READ AV -- outside any __try.
// __declspec(noinline) is load-bearing: /GL+/LTCG drops __try scopes on inlined
// functions, negating the guard entirely.
__declspec(noinline) static bool SafeMakeKey(const char* name, BSFixedString& out)
{
    __try { out = BSFixedString(name); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Release() walks the same StringCache hash chain SafeMakeKey guards on
// construction (see comment above) to decrement/unlink the interned entry.
// Every call site in this file used to invoke it unguarded after the
// Get/Set __try block had already exited on the success path -- a corrupt
// chain there faults with no SEH scope active. Round 123: confirmed live
// fault site (DoxCoSync.dll+0x239E5, READ AV walking a hash bucket array)
// was exactly this unguarded post-__try release, right after a clean
// GetFloat("Speed") call. noinline for the same reason as SafeMakeKey.
__declspec(noinline) static void SafeRelease(BSFixedString* str)
{
    __try { CALL_MEMBER_FN(str, Release)(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

namespace CoSyncAnimUtils
{

    // __declspec(noinline) is load-bearing here -- see the comment above
    // GetFloat below. Confirmed 2026-07-26: a host crash's real fault site
    // was this function's GetObjectRootNode() call (DoxCoSync.dll+0xD4C1,
    // reached via IsGraphReady/SendEvent from FlushPendingVatsOps), landing
    // on a garbage vtable dispatch (RAX=0x100000000) that this function's own
    // __except should have caught but didn't -- this function and
    // IsGraphReady were the only two vtable/engine-touching helpers in this
    // file missing the annotation.
    __declspec(noinline) bool Is3DLoaded(Actor* actor)
    {
        if (!actor)
            return false;

        __try
        {
            TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
            NiNode* root = refr->GetObjectRootNode();
            return root != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool IsGraphReady(Actor* actor)
    {
        if (!actor) return false;
        if (!Is3DLoaded(actor)) return false;

        EnsureGraphGraceInit();

        constexpr ULONGLONG kGraphGraceMs = 3000;
        const ULONGLONG now = GetTickCount64();
        bool ready = false;

        EnterCriticalSection(&s_graphGraceCs);
        auto it = s_first3d->find(actor->formID);
        if (it == s_first3d->end())
            (*s_first3d)[actor->formID] = now;
        else
            ready = (now - it->second) >= kGraphGraceMs;
        LeaveCriticalSection(&s_graphGraceCs);

        if (!ready)
        {
            static uint32_t s_graceLogCount = 0;
            if ((s_graceLogCount++ % 200) == 0)
                LOG_INFO("[AnimUtils] graph grace period: actor=0x%08X anim writes deferred (occurrence #%u)",
                    actor->formID, s_graceLogCount);
        }
        return ready;
    }

    void ResetGraphGrace(Actor* actor)
    {
        if (!actor) return;

        EnsureGraphGraceInit();

        EnterCriticalSection(&s_graphGraceCs);
        s_first3d->erase(actor->formID);
        LeaveCriticalSection(&s_graphGraceCs);

        LOG_INFO("[AnimUtils] ResetGraphGrace: actor=0x%08X will get a fresh %s grace window", actor->formID, "3s");
    }

    // __declspec(noinline) is load-bearing on all four vtable-dispatch helpers
    // below. Under /GL+/LTCG the optimizer inlines cross-TU and silently drops
    // the __try/__except scope tables of the inlined copy — confirmed cause of
    // the 01:04:50 WRITE AV crash where GetFloat("Speed") reached the engine's
    // LOCK CMPXCHG and the SEH guard failed to catch it (same pattern as the
    // ConsoleLogger.h and SafeCallOrig* fixes). noinline forces each function to
    // keep its own UNWIND_INFO with a real scope table.
    __declspec(noinline) bool GetFloat(Actor* actor, const char* name, float& out)
    {
        void* holder = GetAnimHolder(actor);
        if (!holder || !name) return false;

        void*** vtable = reinterpret_cast<void***>(holder);
        if (!vtable || !*vtable) return false;

        BSFixedString key;
        if (!SafeMakeKey(name, key)) return false;
        typedef bool(*Fn)(void*, BSFixedString*, float*);
        auto fn = reinterpret_cast<Fn>((*vtable)[kGetVariableFloat]);

        g_animUtils_lastOp = "GetFloat"; g_animUtils_lastVarName = name; g_animUtils_lastActor = actor;
        bool result = false;
        __try
        {
            result = fn(holder, &key, &out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[AnimUtils] GetFloat(%s) crashed in vtable dispatch", name);
            SafeRelease(&key);
            return false;
        }
        SafeRelease(&key);
        return result;
    }

    __declspec(noinline) bool GetInt(Actor* actor, const char* name, int& out)
    {
        void* holder = GetAnimHolder(actor);
        if (!holder || !name) return false;

        void*** vtable = reinterpret_cast<void***>(holder);
        if (!vtable || !*vtable) return false;

        BSFixedString key;
        if (!SafeMakeKey(name, key)) return false;
        typedef bool(*Fn)(void*, BSFixedString*, int*);
        auto fn = reinterpret_cast<Fn>((*vtable)[kGetVariableInt]);

        g_animUtils_lastOp = "GetInt"; g_animUtils_lastVarName = name; g_animUtils_lastActor = actor;
        bool result = false;
        __try
        {
            result = fn(holder, &key, &out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[AnimUtils] GetInt(%s) crashed in vtable dispatch", name);
            SafeRelease(&key);
            return false;
        }
        SafeRelease(&key);
        return result;
    }

    __declspec(noinline) bool GetBool(Actor* actor, const char* name, bool& out)
    {

        void* holder = GetAnimHolder(actor);
        if (!holder || !name) return false;

        void*** vtable = reinterpret_cast<void***>(holder);
        if (!vtable || !*vtable) return false;



        auto fnPtr = (*vtable)[kGetVariableBool];

        if (!fnPtr || IsBadReadPtr(fnPtr, sizeof(void*)))
        {
            LOG_ERROR("[AnimUtils] Invalid vtable slot kGetVariableBool");
            return false;
        }

        BSFixedString key;
        if (!SafeMakeKey(name, key)) return false;
        typedef bool(*Fn)(void*, BSFixedString*, bool*);
        auto fn = reinterpret_cast<Fn>((*vtable)[kGetVariableBool]);

        g_animUtils_lastOp = "GetBool"; g_animUtils_lastVarName = name; g_animUtils_lastActor = actor;
        bool result = false;
        __try
        {
            result = fn(holder, &key, &out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[AnimUtils] EXCEPTION in %s actor=%p name=%s",
                "GetBool", actor, name);
            SafeRelease(&key);
            return false;
        }
        SafeRelease(&key);
        return result;
    }

    // Round 122: the engine named-setter far-calls have frozen the main thread
    // in every live session that ever reached them (02:16/02:36/15:22/15:28),
    // the last one with the mgr spin lock successfully pre-acquired — the hang
    // is somewhere deeper in the worker (string interning / per-graph name
    // lookup) and the call has NEVER completed once. Hard-disabled until a
    // hang dump shows where it parks; anim vars are written raw (bounds-
    // checked) by CoSyncAnimVarSystem instead.
    static bool EngineSettersDisabled(const char* op, const char* name)
    {
        static uint32_t s_disabledLog = 0;
        if ((s_disabledLog++ % 500) == 0)
            LOG_WARN("[AnimUtils] %s(%s) skipped — this path is disabled because it can hang the game (occurrence #%u)",
                op, name ? name : "?", s_disabledLog);
        return true;
    }

    bool SetFloat(Actor* actor, const char* name, float value)
    {
        if (EngineSettersDisabled("SetFloat", name)) return false;
        void* holder = GetAnimHolder(actor);
        if (!holder || !name) return false;

        void*** vtable = reinterpret_cast<void***>(holder);
        if (!vtable || !*vtable) return false;

        BSFixedString key(name);
        g_animUtils_lastOp = "SetFloat"; g_animUtils_lastVarName = name; g_animUtils_lastActor = actor;
        bool result = false;
        __try
        {
            void* mgr = GetGraphManagerAddRef(holder);
            if (!mgr) { SafeRelease(&key); return false; }
            DWORD lockOwner = 0; LONG lockCount = 0;
            if (!TryAcquireMgrLock(mgr, 5000, &lockOwner, &lockCount))
            {
                static uint32_t s_busyLog = 0;
                if ((s_busyLog++ % 100) == 0)
                    LOG_WARN("[AnimUtils] SetFloat(%s): mgr lock busy -- owner=0x%08X count=0x%08X mgr=%p (skip #%u)",
                        name, lockOwner, (unsigned)lockCount, mgr, s_busyLog);
                ReleaseGraphManagerRef(mgr);
                SafeRelease(&key);
                return false;
            }
            result = g_realSetAnimVarFloat(holder, &key, value);
            ReleaseMgrLock(mgr);
            ReleaseGraphManagerRef(mgr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[AnimUtils] SetFloat(%s, %.3f) crashed", name, value);
            SafeRelease(&key);
            return false;
        }
        SafeRelease(&key);
        return result;
    }

    bool SetInt(Actor* actor, const char* name, int value)
    {
        if (EngineSettersDisabled("SetInt", name)) return false;
        void* holder = GetAnimHolder(actor);
        if (!holder || !name) return false;

        void*** vtable = reinterpret_cast<void***>(holder);
        if (!vtable || !*vtable) return false;

        BSFixedString key(name);
        g_animUtils_lastOp = "SetInt"; g_animUtils_lastVarName = name; g_animUtils_lastActor = actor;
        bool result = false;
        __try
        {
            void* mgr = GetGraphManagerAddRef(holder);
            if (!mgr) { SafeRelease(&key); return false; }
            DWORD lockOwner = 0; LONG lockCount = 0;
            if (!TryAcquireMgrLock(mgr, 5000, &lockOwner, &lockCount))
            {
                static uint32_t s_busyLog = 0;
                if ((s_busyLog++ % 100) == 0)
                    LOG_WARN("[AnimUtils] SetInt(%s): mgr lock busy -- owner=0x%08X count=0x%08X mgr=%p (skip #%u)",
                        name, lockOwner, (unsigned)lockCount, mgr, s_busyLog);
                ReleaseGraphManagerRef(mgr);
                SafeRelease(&key);
                return false;
            }
            result = g_realSetAnimVarInt(holder, &key, value);
            ReleaseMgrLock(mgr);
            ReleaseGraphManagerRef(mgr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[AnimUtils] SetInt(%s, %d) crashed", name, value);
            SafeRelease(&key);
            return false;
        }
        SafeRelease(&key);
        return result;
    }

    bool SetBool(Actor* actor, const char* name, bool value)
    {
        if (EngineSettersDisabled("SetBool", name)) return false;
        void* holder = GetAnimHolder(actor);
        if (!holder || !name) return false;

        void*** vtable = reinterpret_cast<void***>(holder);
        if (!vtable || !*vtable) return false;

        BSFixedString key(name);
        g_animUtils_lastOp = "SetBool"; g_animUtils_lastVarName = name; g_animUtils_lastActor = actor;
        bool result = false;
        __try
        {
            void* mgr = GetGraphManagerAddRef(holder);
            if (!mgr) { SafeRelease(&key); return false; }
            DWORD lockOwner = 0; LONG lockCount = 0;
            if (!TryAcquireMgrLock(mgr, 5000, &lockOwner, &lockCount))
            {
                static uint32_t s_busyLog = 0;
                if ((s_busyLog++ % 100) == 0)
                    LOG_WARN("[AnimUtils] SetBool(%s): mgr lock busy -- owner=0x%08X count=0x%08X mgr=%p (skip #%u)",
                        name, lockOwner, (unsigned)lockCount, mgr, s_busyLog);
                ReleaseGraphManagerRef(mgr);
                SafeRelease(&key);
                return false;
            }
            result = g_realSetAnimVarBool(holder, &key, value);
            ReleaseMgrLock(mgr);
            ReleaseGraphManagerRef(mgr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[AnimUtils] SetBool(%s, %d) crashed", name, (int)value);
            SafeRelease(&key);
            return false;
        }
        SafeRelease(&key);
        return result;
    }

    __declspec(noinline) bool SendEvent(Actor* actor, const char* eventName)
    {
        void* holder = GetAnimHolder(actor);
        if (!holder || !eventName) return false;

        void*** vtable = reinterpret_cast<void***>(holder);
        if (!vtable || !*vtable) return false;

        BSFixedString ev(eventName);
        typedef bool(*Fn)(void*, BSFixedString*);
        auto fn = reinterpret_cast<Fn>((*vtable)[kSendAnimationEvent]);

        g_animUtils_lastOp = "SendEvent"; g_animUtils_lastVarName = eventName; g_animUtils_lastActor = actor;
        bool result = false;
        __try
        {
            result = fn(holder, &ev);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[AnimUtils] SendEvent(%s) crashed in vtable dispatch", eventName);
            SafeRelease(&ev);
            return false;
        }
        SafeRelease(&ev);
        return result;
    }

}
