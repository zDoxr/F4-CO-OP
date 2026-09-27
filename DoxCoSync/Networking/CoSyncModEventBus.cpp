#include "CoSyncModEventBus.h"
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"

#include <mutex>
#include <deque>
#include <unordered_map>
#include <cstring>

namespace
{
    struct ModEvent { std::string eventName; std::string payload; };
    std::unordered_map<std::string, std::deque<ModEvent>> s_queues;
    std::mutex s_mutex;

    // S-SEC2 (2026-09-26): any peer could queue MODEVENTs for any mod name and
    // nothing freed them unless a Papyrus mod polled that exact name, so a
    // single peer could grow memory without limit. These are bounded-memory
    // caps, not measured mod traffic (no mod in this setup uses the bus yet):
    // generous for script events, small enough that a flood can't hurt.
    constexpr size_t kMaxEventsPerMod = 256;
    constexpr size_t kMaxModQueues    = 64;
    constexpr size_t kMaxTotalBytes   = 4 * 1024 * 1024;
    size_t s_totalBytes = 0;
}

// Packet format: MODEVENT|modID|eventName|payload
// payload is the remainder of the string and may itself contain '|'
static bool ParseModEvent(const std::string& msg,
                          std::string& outMod,
                          std::string& outEvent,
                          std::string& outPayload)
{
    if (msg.rfind("MODEVENT|", 0) != 0)
        return false;

    const char* p = msg.c_str() + 9;

    const char* sep1 = strchr(p, '|');
    if (!sep1) return false;
    outMod.assign(p, sep1 - p);

    const char* sep2 = strchr(sep1 + 1, '|');
    if (!sep2) return false;
    outEvent.assign(sep1 + 1, sep2 - (sep1 + 1));

    outPayload.assign(sep2 + 1);

    return !outMod.empty() && !outEvent.empty();
}

void CoSyncModEventBus::OnReceive(const std::string& msg)
{
    std::string mod, evt, payload;
    if (!ParseModEvent(msg, mod, evt, payload))
        return;

    {
        std::lock_guard<std::mutex> lk(s_mutex);
        auto it = s_queues.find(mod);
        if (it == s_queues.end())
        {
            if (s_queues.size() >= kMaxModQueues)
            {
                LOG_WARN("[ModEventBus] dropped event for new mod '%.40s' -- %zu mod queues already exist",
                    mod.c_str(), kMaxModQueues);
                return;
            }
            it = s_queues.emplace(mod, std::deque<ModEvent>()).first;
        }
        auto& q = it->second;
        const size_t bytes = evt.size() + payload.size();
        while (!q.empty() && (q.size() >= kMaxEventsPerMod || s_totalBytes + bytes > kMaxTotalBytes))
        {
            s_totalBytes -= q.front().eventName.size() + q.front().payload.size();
            q.pop_front();
        }
        if (s_totalBytes + bytes > kMaxTotalBytes)
        {
            LOG_WARN("[ModEventBus] dropped oversized event for mod '%.40s' (%zu bytes)", mod.c_str(), bytes);
            return;
        }
        q.push_back({ evt, payload });
        s_totalBytes += bytes;
    }

    LOG_INFO("[ModEventBus] received mod='%s' event='%s' payload len=%zu",
        mod.c_str(), evt.c_str(), payload.size());
}

void CoSyncModEventBus::Send(const std::string& modID,
                             const std::string& eventName,
                             const std::string& payload)
{
    const std::string msg = "MODEVENT|" + modID + "|" + eventName + "|" + payload;
    CoSyncTransport::Send(msg);

    LOG_INFO("[ModEventBus] sent mod='%s' event='%s' payload len=%zu",
        modID.c_str(), eventName.c_str(), payload.size());
}

bool CoSyncModEventBus::HasPendingEvent(const std::string& modID)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    auto it = s_queues.find(modID);
    return it != s_queues.end() && !it->second.empty();
}

bool CoSyncModEventBus::PopEvent(const std::string& modID,
                                 std::string& outEventName,
                                 std::string& outPayload)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    auto it = s_queues.find(modID);
    if (it == s_queues.end() || it->second.empty())
        return false;

    s_totalBytes -= it->second.front().eventName.size() + it->second.front().payload.size();
    outEventName = std::move(it->second.front().eventName);
    outPayload   = std::move(it->second.front().payload);
    it->second.pop_front();
    return true;
}
