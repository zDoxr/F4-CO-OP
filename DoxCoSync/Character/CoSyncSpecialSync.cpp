#include "CoSyncSpecialSync.h"
#include "CoSyncActorValues.h"
#include "CoSyncRequirementsLog.h"
#include "ConsoleLogger.h"
#include "CoSyncEntityRegistry.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"
#include "CoSyncActorValueSafety.h"

#include "GameReferences.h"
#include "GameForms.h"
#include "GameFormComponents.h"

#include <mutex>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

struct SpecialStatDef
{
    uint32_t    avFormID;
    const char* name;
};

static const SpecialStatDef kSpecialStats[] =
{
    { kAVI_Strength,     "Strength"     },
    { kAVI_Perception,   "Perception"   },
    { kAVI_Endurance,    "Endurance"    },
    { kAVI_Charisma,     "Charisma"     },
    { kAVI_Intelligence, "Intelligence" },
    { kAVI_Agility,      "Agility"      },
    { kAVI_Luck,         "Luck"         },
};
static constexpr int kNumSpecial = 7;

static ActorValueInfo* s_aviCache[kNumSpecial] = {};

static ActorValueInfo* GetAVI(int idx)
{
    LOG_REQUIREMENT("CoSyncSpecialSync::GetAVI", "Needs a valid SPECIAL stat index and the game to have finished loading before it can look up the stat.");
    if (idx < 0 || idx >= kNumSpecial)
        return nullptr;
    if (!s_aviCache[idx])
    {
        TESForm* form = LookupFormByID(kSpecialStats[idx].avFormID);
        if (form)
            s_aviCache[idx] = static_cast<ActorValueInfo*>(form);
    }
    return s_aviCache[idx];
}

struct SpecialOp
{
    uint32_t entityID;
    uint32_t avFormID;
    float    value;
    uint32_t retryCount = 0;
};

static std::mutex              s_mutex;
static std::vector<SpecialOp>  s_pending;

static double s_lastFlushTime    = 0.0;
static constexpr double kSpecFlushInterval = 1.0 / 20.0;

static double SpecNowSeconds()
{
    LOG_REQUIREMENT("CoSyncSpecialSync::SpecNowSeconds", "Just reads the system clock. Works on any normal Windows PC.");
    static double s_freq = [] {
        LOG_REQUIREMENT("CoSyncSpecialSync::SpecNowSeconds::lambda", "Just reads the system clock. Works on any normal Windows PC.");
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / s_freq;
}

static float s_lastSent[kNumSpecial] = { -1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f };

bool CoSyncSpecialSync::IsSpecial(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncSpecialSync::IsSpecial", "Just checks the text of a message. No game state needed.");
    return msg.size() > 5 &&
           msg[0] == 'S' && msg[1] == 'P' && msg[2] == 'E' &&
           msg[3] == 'C' && msg[4] == '|';
}

std::string CoSyncSpecialSync::SerializeSpecial(uint32_t entityID, uint32_t avFormID, float value)
{
    LOG_REQUIREMENT("CoSyncSpecialSync::SerializeSpecial", "Just turns a SPECIAL stat change into text. Doesn't touch the game at all.");
    char buf[80];
    std::snprintf(buf, sizeof(buf), "SPEC|%u|%u|%f", entityID, avFormID, value);
    return buf;
}

void CoSyncSpecialSync::OnReceiveSpecial(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncSpecialSync::OnReceiveSpecial", "Reads a SPECIAL stat update out of text; a badly formed message is safely rejected. Needs the network to be set up first.");

    uint32_t eid = 0, fid = 0;
    float val = 0.f;
    if (std::sscanf(msg.c_str() + 5, "%u|%u|%f", &eid, &fid, &val) != 3)
    {
        LOG_UPDATED_WARN("[SpecSync RX] Malformed SPEC packet: '%s'", msg.c_str());
        return;
    }

    if (eid == CoSyncNet::GetMyEntityID())
    {
        LOG_TRACE("[SpecSync RX] Dropping loopback SPEC (entity=%u is local player)", eid);
        return;
    }

    LOG_UPDATED("[SpecSync RX] SPEC queued: entity=%u avFormID=0x%08X value=%.2f", eid, fid, val);

    std::lock_guard<std::mutex> lk(s_mutex);
    s_pending.push_back({ eid, fid, val });
}

static constexpr uint32_t kMaxSpecialRetries = 100;

// Round 106 audit: op.avFormID comes straight off the wire ("SPEC|entity|avFormID|value")
// and was fed into LookupFormByID with zero SEH, the same unguarded-peer-FormID
// crash pattern already found and fixed in QuestSync/EquipHook/OutfitSync/
// PowerArmorSync/DoorHook/LockHook (round 39-41) -- this sibling subsystem was
// missed. Same noinline-SEH-helper convention as those fixes.
__declspec(noinline) static TESForm* SafeLookupForm(uint32_t formID)
{
    LOG_REQUIREMENT("CoSyncSpecialSync::SafeLookupForm", "Looks up a game object by ID. If the other player sends a bad ID, it fails safely instead of crashing.");
    __try
    {
        return LookupFormByID(formID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void CoSyncSpecialSync::FlushPendingSpecialOps()
{
    LOG_REQUIREMENT("CoSyncSpecialSync::FlushPendingSpecialOps", "Looks up any stat IDs from other players safely, and checks each target character actually exists (with a few retries) before touching it.");

    const double now = SpecNowSeconds();
    if (now - s_lastFlushTime < kSpecFlushInterval)
        return;
    s_lastFlushTime = now;

    std::vector<SpecialOp> ops;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        ops.swap(s_pending);
    }
    if (ops.empty())
        return;

    LOG_UPDATED("[SpecSync FLUSH] Processing %u SPECIAL op(s)", (unsigned)ops.size());

    std::vector<SpecialOp> retryOps;
    static std::unordered_set<Actor*> s_badSpecActors;

    for (SpecialOp op : ops)
    {
        Actor* actor = g_CoSyncEntities.GetActor(op.entityID);
        if (!actor)
        {
            ++op.retryCount;
            if (op.retryCount <= kMaxSpecialRetries)
            {
                LOG_TRACE("[SpecSync FLUSH] entity=%u not spawned yet — retry %u/%u  avFormID=0x%08X",
                    op.entityID, op.retryCount, kMaxSpecialRetries, op.avFormID);
                retryOps.push_back(op);
            }
            else
            {
                LOG_UPDATED_WARN("[SpecSync FLUSH] entity=%u avFormID=0x%08X DROPPED after %u retries"
                                 " — actor never spawned",
                                 op.entityID, op.avFormID, op.retryCount);
            }
            continue;
        }

        if (s_badSpecActors.count(actor))
            continue;

        TESForm* form = SafeLookupForm(op.avFormID);
        if (!form)
        {
            LOG_UPDATED_WARN("[SpecSync FLUSH] avFormID=0x%08X not in form table", op.avFormID);
            continue;
        }

        ActorValueInfo* avi = static_cast<ActorValueInfo*>(form);

        // round 30 (P2 crash, Fallout4.exe+0xCC934D): this exact call was the
        // confirmed crash site -- see CoSyncActorValueSafety.h for the full
        // writeup (proxy actors have AI permanently disabled, so the engine
        // sub-object this dispatches through never gets allocated; SEH at the
        // call site is the only fix, no settle delay helps).
        // round 127: on crash, permanently blacklist the actor so the other 6
        // SPECIAL ops queued for the same spawn burst don't re-trigger the fault.
        if (!CoSyncAVSafety::SafeSetBase(actor, avi, op.value))
        {
            s_badSpecActors.insert(actor);
            continue;
        }

        LOG_UPDATED("[SpecSync APPLY] entity=%u avFormID=0x%08X value=%.2f actor=%p retry=%u",
            op.entityID, op.avFormID, op.value, static_cast<void*>(actor), op.retryCount);
    }


    if (!retryOps.empty())
    {
        std::lock_guard<std::mutex> lk(s_mutex);

        retryOps.insert(retryOps.end(), s_pending.begin(), s_pending.end());
        s_pending.swap(retryOps);
    }
}

void CoSyncSpecialSync::TickLocalSpecial(uint32_t myEntityID, Actor* player)
{
    LOG_REQUIREMENT("CoSyncSpecialSync::TickLocalSpecial", "Checks the player exists before reading any of their stats.");
    if (!player)
        return;

    for (int i = 0; i < kNumSpecial; ++i)
    {
        ActorValueInfo* avi = GetAVI(i);
        if (!avi)
            continue;


        float current = player->actorValueOwner.GetBase(avi);

        if (std::fabsf(current - s_lastSent[i]) < 0.5f)
            continue;

        s_lastSent[i] = current;

        std::string pkt = SerializeSpecial(myEntityID, kSpecialStats[i].avFormID, current);
        CoSyncTransport::Send(pkt);

        LOG_UPDATED("[SpecSync TX] %s changed: entity=%u value=%.2f",
            kSpecialStats[i].name, myEntityID, current);
    }
}
