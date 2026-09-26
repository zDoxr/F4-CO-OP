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
        s_queues[mod].push_back({ evt, payload });
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

    outEventName = std::move(it->second.front().eventName);
    outPayload   = std::move(it->second.front().payload);
    it->second.pop_front();
    return true;
}
