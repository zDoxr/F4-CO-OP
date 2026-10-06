#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>

#include "CoSyncModSync.h"
#include "CoSyncFormIdMap.h"
#include "CoSyncGameAPI.h"
#include "GameData.h"

extern RelocPtr<DataHandler*> g_dataHandler;
#include "CoSyncTransport.h"
#include "ConsoleLogger.h"

#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstdint>

namespace CoSyncModSync
{
    // 0.4.0: the MODLIST/MODFILE push that lived here is gone. It sent any
    // plugin the client lacked as one hex-encoded message after HELLO and
    // wrote it straight into Data\ -- no hash, no size limit beyond GNS's
    // message cap, no archives, and only after the client had already loaded
    // the host's world. Pre-join content sync (CoSyncContentSync) replaces it.

    // ── Load order check (0.3.3, Stage 9) ───────────────────────────────────
    namespace
    {
        std::string Lower(const char* p)
        {
            std::string out = p ? p : "";
            std::transform(out.begin(), out.end(), out.begin(), ::tolower);
            return out;
        }

        // "F:a.esm,F:b.esp;L:c.esl" -> (full list, light list)
        void SplitLoadOrder(const std::string& lo, std::vector<std::string>& full, std::vector<std::string>& light)
        {
            full.clear();
            light.clear();
            std::stringstream ss(lo);
            std::string part;
            while (std::getline(ss, part, ','))
            {
                const size_t semi = part.find(';');
                std::vector<std::string> pieces;
                if (semi == std::string::npos) pieces.push_back(part);
                else { pieces.push_back(part.substr(0, semi)); pieces.push_back(part.substr(semi + 1)); }
                for (const std::string& pc : pieces)
                {
                    if (pc.size() < 3 || pc[1] != ':') continue;
                    (pc[0] == 'L' ? light : full).push_back(pc.substr(2));
                }
            }
        }

        // Human-readable differences, most important first; count = total.
        std::string DiffLoadOrders(const std::string& hostLo, const std::string& clientLo, int& count)
        {
            std::vector<std::string> hf, hl, cf, cl;
            SplitLoadOrder(hostLo, hf, hl);
            SplitLoadOrder(clientLo, cf, cl);
            std::vector<std::string> notes;
            count = 0;
            auto has = [](const std::vector<std::string>& v, const std::string& x)
            { return std::find(v.begin(), v.end(), x) != v.end(); };
            for (const std::string& m : hf) if (!has(cf, m)) { ++count; notes.push_back("missing " + m); }
            for (const std::string& m : hl) if (!has(cl, m)) { ++count; notes.push_back("missing " + m); }
            for (const std::string& m : cf) if (!has(hf, m)) { ++count; notes.push_back("extra " + m); }
            for (const std::string& m : cl) if (!has(hl, m)) { ++count; notes.push_back("extra " + m); }
            // Same plugins, different position: the IDs of everything after the
            // first moved plugin differ.
            const size_t n = (std::min)(hf.size(), cf.size());
            for (size_t i = 0; i < n; ++i)
                if (hf[i] != cf[i] && has(cf, hf[i]) && has(hf, cf[i]))
                {
                    ++count;
                    notes.push_back("order differs from slot " + std::to_string(i) + " (" + hf[i] + ")");
                    break;
                }
            const size_t nl = (std::min)(hl.size(), cl.size());
            for (size_t i = 0; i < nl; ++i)
                if (hl[i] != cl[i] && has(cl, hl[i]) && has(hl, cl[i]))
                {
                    ++count;
                    notes.push_back("light order differs from slot " + std::to_string(i) + " (" + hl[i] + ")");
                    break;
                }
            std::string out;
            for (size_t i = 0; i < notes.size() && i < 6; ++i)
            {
                if (i) out += "; ";
                out += notes[i];
            }
            if (notes.size() > 6)
                out += "; +" + std::to_string(notes.size() - 6) + " more";
            return out;
        }
    }

    std::string BuildLoadOrder()
    {
        DataHandler* dh = *g_dataHandler;
        if (!dh)
            return std::string();
        std::string out;
        for (UInt32 i = 0; i < dh->modList.loadedMods.count; ++i)
        {
            ModInfo* m = nullptr;
            if (!dh->modList.loadedMods.GetNthItem(i, m) || !m) continue;
            if (!out.empty()) out += ",";
            out += "F:" + Lower(m->name);
        }
        out += ";";
        bool first = true;
        for (UInt32 i = 0; i < dh->modList.lightMods.count; ++i)
        {
            ModInfo* m = nullptr;
            if (!dh->modList.lightMods.GetNthItem(i, m) || !m) continue;
            if (!first) out += ",";
            first = false;
            out += "L:" + Lower(m->name);
        }
        return out;
    }

    void ClientSendLoadOrder()
    {
        const std::string lo = BuildLoadOrder();
        if (lo.empty())
            return;
        CoSyncTransport::Send("LOADORDER|" + lo);
        LOG_INFO("[ModSync] Sent LOADORDER (%zu bytes)", lo.size());
    }

    bool IsLoadOrderMessage(const std::string& msg)
    {
        return msg.rfind("LOADORDER", 0) == 0;
    }

    void OnReceiveLoadOrderMessage(const std::string& msg, uint32_t conn, bool isHostRole,
                                   const std::string& senderName)
    {
        if (msg.rfind("LOADORDER|", 0) == 0)
        {
            if (!isHostRole)
                return;
            const std::string clientLo = msg.substr(10);
            const std::string hostLo = BuildLoadOrder();
            // 0.3.3: the client translates FormIDs against this list.
            CoSyncTransport::SendTo(conn, "LOADORDERHOST|" + hostLo);
            int count = 0;
            const std::string diff = DiffLoadOrders(hostLo, clientLo, count);
            if (count == 0)
            {
                CoSyncTransport::SendTo(conn, "LOADORDEROK");
                LOG_INFO("[ModSync] %s: load order matches the host's", senderName.c_str());
                return;
            }
            CoSyncTransport::SendTo(conn, "LOADORDERBAD|" + std::to_string(count) + "|" + diff);
            LOG_WARN("[ModSync] %s: load order differs from the host's (%d): %s",
                senderName.c_str(), count, diff.c_str());
            if (diff.find("missing") != std::string::npos || diff.find("extra") != std::string::npos)
            {
                char note[200];
                std::snprintf(note, sizeof(note), "%s has different mods. Some things won't sync.",
                    senderName.empty() ? "A player" : senderName.c_str());
                CoSyncGameAPI::QueueNotification(note);
            }
            return;
        }
        if (msg.rfind("LOADORDERHOST|", 0) == 0)
        {
            if (!isHostRole)
                CoSyncFormIdMap::SetHostLoadOrder(msg.substr(14));
            return;
        }
        if (msg == "LOADORDEROK")
        {
            LOG_INFO("[ModSync] Host confirmed: same load order");
            return;
        }
        if (msg.rfind("LOADORDERBAD|", 0) == 0)
        {
            const std::string rest = msg.substr(13);
            const size_t bar = rest.find('|');
            const std::string n = rest.substr(0, bar);
            const std::string what = (bar == std::string::npos) ? std::string() : rest.substr(bar + 1);
            LOG_WARN("[ModSync] Your load order differs from the host's (%s difference(s)): %s", n.c_str(), what.c_str());
            // Order differences are translated (CoSyncFormIdMap); only plugins
            // one side lacks remain a real problem.
            if (what.find("missing") != std::string::npos || what.find("extra") != std::string::npos)
                CoSyncGameAPI::QueueNotification("Your mods don't match the host's. Some things won't sync.");
        }
    }
}
