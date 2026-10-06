#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncMapSync.h"

#include "ConsoleLogger.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"

#include "GameReferences.h"
#include "GameExtraData.h"
#include "GameForms.h"
#include "GameRTTI.h"

#include <mutex>
#include <vector>
#include <sstream>
#include <cstdlib>

struct MapMarkerData
{
    BSString fullName;
    UInt8    flags;
    UInt8    markerType;
};

struct ExtraMapMarker : public BSExtraData
{
    MapMarkerData* data;
};

static constexpr UInt8 kMarkerFlag_Visible      = 0x01;
static constexpr UInt8 kMarkerFlag_CanFastTravel = 0x02;

bool CoSyncMapSync::IsMapMarker(const std::string& msg)
{
    return msg.rfind("MM|", 0) == 0;
}

void CoSyncMapSync::BroadcastDiscovery(uint32_t markerFormID)
{
    if (markerFormID == 0)
        return;

    std::ostringstream ss;
    ss << "MM|" << markerFormID;
    CoSyncTransport::Send(ss.str());
    LOG_INFO("[MapSync] TX MM marker=0x%08X", markerFormID);
}

static std::mutex             s_opMutex;
static std::vector<uint32_t>  s_pendingFormIDs;

void CoSyncMapSync::OnReceiveMapMarker(const std::string& msg)
{
    if (!IsMapMarker(msg))
        return;

    const char* p = msg.c_str() + 3;
    char* end = nullptr;
    uint32_t formID = (uint32_t)std::strtoul(p, &end, 10);
    if (formID == 0)
    {
        LOG_WARN("[MapSync] Malformed MM packet: '%s'", msg.c_str());
        return;
    }

    std::lock_guard<std::mutex> lk(s_opMutex);
    s_pendingFormIDs.push_back(formID);
}

// UPDATE 2026-06-18 (round 23 audit, exhaustive pass): split into a noinline
// _Impl plus a thin SEH wrapper, consistent with the established convention
// for every other engine-memory-touching function in this codebase --
// ref/extra/em are all properly null-checked already, but there was no
// __try/__except backstop for a stale/freed object slipping through between
// checks, or GetByType/Runtime_DynamicCast faulting on corrupted internal
// engine state. This file didn't include <Windows.h> at all before.
__declspec(noinline) static void FlushPendingMapOps_Impl(const std::vector<uint32_t>& ids)
{
    for (uint32_t formID : ids)
    {
        TESForm* form = LookupFormByID(formID);
        if (!form)
        {
            LOG_WARN("[MapSync] Marker formID=0x%08X not found", formID);
            continue;
        }

        TESObjectREFR* ref = DYNAMIC_CAST(form, TESForm, TESObjectREFR);
        if (!ref)
        {
            LOG_WARN("[MapSync] formID=0x%08X is not a TESObjectREFR", formID);
            continue;
        }


        if (!ref->extraDataList) continue;
        BSExtraData* extra = ref->extraDataList->GetByType(kExtraData_MapMarker);
        if (!extra)
        {
            LOG_WARN("[MapSync] Ref 0x%08X has no ExtraMapMarker", formID);
            continue;
        }


        ExtraMapMarker* em = (ExtraMapMarker*)Runtime_DynamicCast(
            extra, RTTI_BSExtraData, RTTI_ExtraMapMarker);
        if (!em)
        {
            LOG_WARN("[MapSync] RTTI check failed for ExtraMapMarker on 0x%08X", formID);
            continue;
        }

        if (!em->data)
        {
            LOG_WARN("[MapSync] ExtraMapMarker::data is null for 0x%08X", formID);
            continue;
        }

        em->data->flags |= (kMarkerFlag_Visible | kMarkerFlag_CanFastTravel);
        LOG_INFO("[MapSync] Revealed map marker 0x%08X flags=0x%02X",
            formID, (unsigned)em->data->flags);
    }
}

// C2712: __try cannot share a function with local C++ objects requiring
// unwinding. Kept separate from FlushPendingMapOps() (which owns the
// std::vector) so this one only ever sees a reference parameter.
static void FlushPendingMapOpsSafe(const std::vector<uint32_t>& ids)
{
    __try
    {
        FlushPendingMapOps_Impl(ids);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG_ERROR("[MapSync] Crashed while revealing map markers -- recovered");
    }
}

void CoSyncMapSync::FlushPendingMapOps()
{
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lk(s_opMutex);
        if (s_pendingFormIDs.empty())
            return;
        ids.swap(s_pendingFormIDs);
    }

    FlushPendingMapOpsSafe(ids);
}
