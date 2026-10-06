#include "CoSyncTimeSync.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"

#include "GameForms.h"
#include "GameRTTI.h"
#include "Relocation.h"

#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <mutex>
#include <string>

// Confirmed via IDA against Fallout4.exe 1.11.221: 0x31E6970 IS the right
// global -- it's the exact same lazy-init cache slot the real "Weather"
// Papyrus natives (GetCurrentWeather, GetSkyMode) read via their shared
// accessor (sub_1402F2410, RVA 0x2F2410). The +0x48 field offset below also
// matches exactly (GetCurrentWeather reads "accessor()+72", and 72 decimal
// == 0x48 hex). So the address and offset were never the actual problem.
// The real issue: this is a LAZY singleton -- qword_1431E6970 only gets
// populated the first time ANYTHING calls the accessor function. Reading the
// raw global directly (the old approach) depends on something else in the
// engine having already triggered that construction first; if CoSync's tick
// runs before that happens (e.g. early in a session, or right after a cell
// load), it reads pre-construction garbage that happens to be small and
// non-zero, passing the `!skyMgr` null check without being a valid pointer --
// exactly the documented "garbage(0x20)+0x48=0x68" crash. Fix: call the real
// accessor function directly so construction is guaranteed before use,
// instead of trusting something else got there first.
typedef void* (*_GetSkyManagerSingleton)();
static RelocAddr<_GetSkyManagerSingleton> GetSkyManagerSingleton(0x002F2730);


static constexpr uint32_t kFormID_GameHour = 0x00000039;

static TESGlobal* GetGameHourGlobal()
{
    static TESGlobal* s_cached = nullptr;
    if (!s_cached)
    {
        TESForm* form = LookupFormByID(kFormID_GameHour);
        if (form && form->formType == kFormType_GLOB)
            s_cached = static_cast<TESGlobal*>(form);
    }
    return s_cached;
}

// See the GetSkyManagerSingleton comment above for the real root cause
// (lazy-init timing, not a stale address). SEH-guard + latch kept as a
// defense-in-depth safety net regardless (same pattern as CoSyncVatsSync's
// SafeReadVatsCameraState) instead of crashing or re-triggering the VEH's
// log+minidump spam every tick.
static bool s_skyMgrKnownBad = false;

// UPDATE 2026-06-17: __declspec(noinline) is load-bearing -- see the
// fuller writeup in CoSyncVatsSync.cpp's SafeReadVatsCameraState and
// CoSyncDoorHook.cpp's SafeReadU32/SafeReadPtr. A non-noinline static
// __try/__except function risks silent SEH-removal via inlining under
// this project's /GL+/LTCG Release config.
__declspec(noinline) static bool SafeReadWeatherFormID(void* skyMgr, uint32_t& outFormID)
{
    __try
    {
        TESForm* w = *reinterpret_cast<TESForm**>(static_cast<char*>(skyMgr) + 0x48);
        outFormID = w ? w->formID : 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

__declspec(noinline) static bool SafeWriteWeatherToSky(void* skyMgr, TESForm* form)
{
    __try
    {
        *reinterpret_cast<TESForm**>(static_cast<char*>(skyMgr) + 0x48) = form;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// UPDATE 2026-06-18: the CALL to GetSkyManagerSingleton() itself was unguarded
// here -- only the post-call +0x48 dereference (SafeReadWeatherFormID/
// SafeWriteWeatherToSky above) had SEH. This accessor is a real call into
// Fallout4.exe (RVA 0x2F2410), not just a memory read; if its lazy-construction/
// internal bookkeeping path crashes (suspected: routes through an engine event-
// sink registration mechanism), nothing here was positioned to catch it. Matches
// the exact crash signature reproduced 2/2 (RIP=Fallout4.exe+0x1A7FC54, always
// immediately after this code path's "TX TD" log line, every time). noinline is
// load-bearing under this project's /GL+/LTCG config -- same pattern as every
// other Safe* helper in this codebase.
__declspec(noinline) static void* SafeCallGetSkyManagerSingleton()
{
    __try
    {
        return GetSkyManagerSingleton();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static uint32_t GetCurrentWeatherFormID()
{
    if (s_skyMgrKnownBad)
        return 0;

    void* skyMgr = SafeCallGetSkyManagerSingleton();
    if (!skyMgr)
    {
        s_skyMgrKnownBad = true;
        LOG_ERROR("[TimeSync] GetSkyManagerSingleton() crashed or returned null "
                   "-- weather sync disabled for this session");
        return 0;
    }

    uint32_t formID = 0;
    if (!SafeReadWeatherFormID(skyMgr, formID))
    {
        s_skyMgrKnownBad = true;
        LOG_ERROR("[TimeSync] Sky manager singleton accessor returned an invalid "
                   "pointer -- weather sync disabled for this session");
        return 0;
    }
    return formID;
}

static void WriteWeatherToSky(TESForm* form)
{
    if (s_skyMgrKnownBad)
        return;

    void* skyMgr = SafeCallGetSkyManagerSingleton();
    if (!skyMgr)
    {
        s_skyMgrKnownBad = true;
        LOG_ERROR("[TimeSync] GetSkyManagerSingleton() crashed or returned null "
                   "-- weather sync disabled for this session");
        return;
    }
    if (!form) return;

    if (!SafeWriteWeatherToSky(skyMgr, form))
    {
        s_skyMgrKnownBad = true;
        LOG_ERROR("[TimeSync] Sky manager singleton accessor returned an invalid "
                   "pointer -- weather sync disabled for this session");
    }
}

static std::string SerializeTime(float gameHour)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4);
    ss << "TD|" << gameHour;
    return ss.str();
}

static bool DeserializeTime(const std::string& msg, float& outGameHour)
{
    if (msg.rfind("TD|", 0) != 0) return false;
    const char* p = msg.c_str() + 3;
    char* end = nullptr;
    outGameHour = std::strtof(p, &end);
    if (end == p) return false;
    if (outGameHour < 0.0f)   outGameHour = 0.0f;
    if (outGameHour >= 24.0f) outGameHour = 23.9999f;
    return true;
}


static std::string SerializeWeather(uint32_t formID)
{
    std::ostringstream ss;
    ss << "WX|" << formID;
    return ss.str();
}

static bool DeserializeWeather(const std::string& msg, uint32_t& outFormID)
{
    if (msg.rfind("WX|", 0) != 0) return false;
    const char* p = msg.c_str() + 3;
    char* end = nullptr;
    outFormID = static_cast<uint32_t>(std::strtoul(p, &end, 10));
    return (end != p);
}

// UPDATE 2026-06-18 (round 23 audit, exhaustive pass): this was the one
// engine-memory touch in this file with no SEH -- every other Safe* helper
// here is noinline+__try/__except, this one read g->value bare. FormID
// 0x39 (GameHour) is a permanent vanilla engine global so the realistic risk
// is low, but kept consistent with this file's own established convention
// rather than leaving the one exception.
__declspec(noinline) static bool SafeReadGameHour(TESGlobal* g, float& outHour)
{
    __try
    {
        outHour = g->value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static float GetGameHour()
{
    TESGlobal* g = GetGameHourGlobal();
    if (!g) return -1.f;

    float hour = -1.f;
    return SafeReadGameHour(g, hour) ? hour : -1.f;
}

namespace
{
    bool   s_initialized       = false;
    double s_lastBroadcastTime = 0.0;

    constexpr double k_broadcastInterval = 30.0;


    std::mutex s_pendingMutex;
    bool       s_hasPending    = false;
    float      s_pendingHour   = 0.0f;
    bool       s_hasPendingWeather    = false;
    uint32_t   s_pendingWeatherFormID = 0;


    uint32_t   s_lastWeatherFormID = 0;

    std::mutex s_lockMutex;
    bool       s_hasLockedHour          = false;
    float      s_lockedHour             = 0.0f;
    bool       s_hasLockedWeather       = false;
    uint32_t   s_lockedWeatherFormID    = 0;
    TESForm*   s_lockedWeatherForm      = nullptr;
}

uint32_t CoSyncTimeSync::GetLockedWeatherFormID()
{
    std::lock_guard<std::mutex> lk(s_lockMutex);
    return s_hasLockedWeather ? s_lockedWeatherFormID : 0;
}

void CoSyncTimeSync::Init()
{
    s_initialized       = true;
    s_lastBroadcastTime = 0.0;
    s_lastWeatherFormID = 0;

    {
        std::lock_guard<std::mutex> lk(s_pendingMutex);
        s_hasPending           = false;
        s_pendingHour          = 0.0f;
        s_hasPendingWeather    = false;
        s_pendingWeatherFormID = 0;
    }
    {
        std::lock_guard<std::mutex> lk(s_lockMutex);
        s_hasLockedHour       = false;
        s_lockedHour          = 0.0f;
        s_hasLockedWeather    = false;
        s_lockedWeatherFormID = 0;
        s_lockedWeatherForm   = nullptr;
    }

    LOG_INFO("[TimeSync] Init OK");
}

void CoSyncTimeSync::Shutdown()
{
    s_initialized = false;

    std::lock_guard<std::mutex> lk(s_lockMutex);
    s_hasLockedHour     = false;
    s_hasLockedWeather  = false;
    s_lockedWeatherForm = nullptr;
}

void CoSyncTimeSync::ForceOverrideOnGameThread()
{
    if (!s_initialized || CoSyncNet::IsHost())
        return;

    bool   hasHour    = false;
    float  hour       = 0.0f;
    bool   hasWeather = false;
    TESForm* wForm    = nullptr;
    uint32_t wFormID  = 0;

    {
        std::lock_guard<std::mutex> lk(s_lockMutex);
        hasHour    = s_hasLockedHour;
        hour       = s_lockedHour;
        hasWeather = s_hasLockedWeather;
        wForm      = s_lockedWeatherForm;
        wFormID    = s_lockedWeatherFormID;
    }


    static uint32_t s_overrideCallCount = 0;
    ++s_overrideCallCount;
    const bool doLog = (s_overrideCallCount <= 3 || s_overrideCallCount % 600 == 0);

    if (hasHour)
    {
        TESGlobal* g = GetGameHourGlobal();
        if (g)
        {
            g->value = hour;
            if (doLog)
                LOG_UPDATED("[Weather APPLY] GameHour locked to %.4f (call#%u)", hour, s_overrideCallCount);
        }
        else if (doLog)
        {
            LOG_UPDATED_WARN("[Weather APPLY] GameHour global not found (call#%u)", s_overrideCallCount);
        }
    }
    else if (doLog)
    {
        LOG_UPDATED_WARN("[Weather APPLY] No locked hour — has host sent a TD packet yet? (call#%u)",
            s_overrideCallCount);
    }

    if (hasWeather && wFormID != 0)
    {


        TESForm* applyForm = wForm;
        if (!applyForm)
        {


            applyForm = LookupFormByID(wFormID);
            if (applyForm)
            {
                std::lock_guard<std::mutex> lk(s_lockMutex);
                s_lockedWeatherForm = applyForm;
            }
        }
        if (applyForm)
        {
            WriteWeatherToSky(applyForm);
            if (doLog)
                LOG_UPDATED("[Weather APPLY] Sky weather forced to 0x%08X (call#%u)", wFormID, s_overrideCallCount);
        }
        else if (doLog)
        {
            LOG_UPDATED_WARN("[Weather APPLY] Weather form 0x%08X not found on game thread (call#%u)",
                wFormID, s_overrideCallCount);
        }
    }
    else if (doLog)
    {
        LOG_UPDATED_WARN("[Weather APPLY] No locked weather — has host sent a WX packet yet? (call#%u)",
            s_overrideCallCount);
    }
}

void CoSyncTimeSync::Tick(double now)
{
    if (!s_initialized)
        return;


    {
        float    pendingHour          = 0.0f;
        bool     hasPending           = false;
        uint32_t pendingWeatherFormID = 0;
        bool     hasPendingWeather    = false;

        {
            std::lock_guard<std::mutex> lk(s_pendingMutex);
            hasPending             = s_hasPending;
            pendingHour            = s_pendingHour;
            s_hasPending           = false;
            hasPendingWeather      = s_hasPendingWeather;
            pendingWeatherFormID   = s_pendingWeatherFormID;
            s_hasPendingWeather    = false;
        }

        if (hasPending)
            LOG_UPDATED("[TimeSync] Locked host time: %.4f (applied every game frame)", pendingHour);

        if (hasPendingWeather)
            LOG_UPDATED("[TimeSync] Locked host weather: 0x%08X (applied every game frame)", pendingWeatherFormID);
    }

    if (!CoSyncNet::IsHost())
        return;

    if (!CoSyncNet::IsInitialized() || !CoSyncNet::IsConnected())
        return;


    if ((now - s_lastBroadcastTime) >= k_broadcastInterval)
    {
        s_lastBroadcastTime = now;

        const float gameHour = GetGameHour();
        if (gameHour >= 0.0f)
        {
            CoSyncTransport::Send(SerializeTime(gameHour));
            LOG_UPDATED("[TimeSync] TX TD: gameHour=%.4f", gameHour);
        }
    }


    {
        const uint32_t currentWeatherFormID = GetCurrentWeatherFormID();
        if (currentWeatherFormID != 0 && currentWeatherFormID != s_lastWeatherFormID)
        {
            s_lastWeatherFormID = currentWeatherFormID;
            CoSyncTransport::Send(SerializeWeather(currentWeatherFormID));
            LOG_UPDATED("[TimeSync] TX WX: weather=0x%08X", currentWeatherFormID);
        }
    }
}

void CoSyncTimeSync::OnReceiveTime(const std::string& msg)
{
    if (CoSyncNet::IsHost())
        return;

    float gameHour = 0.0f;
    if (!DeserializeTime(msg, gameHour))
    {
        LOG_UPDATED_WARN("[TimeSync] Malformed TD packet");
        return;
    }

    LOG_UPDATED("[TimeSync] RX TD: gameHour=%.4f (locking for every-frame override)", gameHour);


    {
        std::lock_guard<std::mutex> lk(s_lockMutex);
        s_hasLockedHour = true;
        s_lockedHour    = gameHour;
    }


    {
        std::lock_guard<std::mutex> lk(s_pendingMutex);
        s_hasPending  = true;
        s_pendingHour = gameHour;
    }
}

void CoSyncTimeSync::OnReceiveWeather(const std::string& msg)
{
    if (CoSyncNet::IsHost())
    {
        CoSyncTransport::Send(msg);
        return;
    }

    uint32_t formID = 0;
    if (!DeserializeWeather(msg, formID))
    {
        LOG_UPDATED_WARN("[TimeSync] Malformed WX packet");
        return;
    }

    LOG_UPDATED("[TimeSync] RX WX: weather=0x%08X (locking for every-frame override)", formID);

    {
        std::lock_guard<std::mutex> lk(s_lockMutex);
        s_hasLockedWeather    = true;
        s_lockedWeatherFormID = formID;
        s_lockedWeatherForm   = nullptr;
    }


    {
        std::lock_guard<std::mutex> lk(s_pendingMutex);
        s_hasPendingWeather    = true;
        s_pendingWeatherFormID = formID;
    }
}
