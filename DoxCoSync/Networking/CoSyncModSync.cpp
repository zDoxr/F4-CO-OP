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
    // P4 (2026-09-26): a MODFILE is one GNS message, hex-encoded (2 bytes per
    // file byte), and GNS refuses anything over k_cbMaxSteamNetworkingSockets-
    // MessageSizeSend (512 KB) -- the old 10 MB limit let every plugin over
    // ~256 KB fail silently in SendText. Files that don't fit get MODSKIP.
    static const size_t kMaxMessageBytes  = 512 * 1024;
    static const size_t kMaxAutoSendBytes = (kMaxMessageBytes - 1024) / 2;

    // ---- Helpers ----

    static std::string GetDataDir()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(GetModuleHandleA("Fallout4.exe"), path, sizeof(path));
        char* slash = strrchr(path, '\\');
        if (slash) *(slash + 1) = '\0';
        return std::string(path) + "Data\\";
    }

    static std::string HexEncode(const std::vector<uint8_t>& data)
    {
        static const char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(data.size() * 2);
        for (uint8_t b : data) {
            out += kHex[b >> 4];
            out += kHex[b & 0xF];
        }
        return out;
    }

    static std::vector<uint8_t> HexDecode(const std::string& hex)
    {
        std::vector<uint8_t> out;
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
        {
            auto nibble = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
                if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
                return 0;
            };
            out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
        }
        return out;
    }

    static bool SafeFilename(const std::string& name)
    {
        if (name.find('\\') != std::string::npos) return false;
        if (name.find('/')  != std::string::npos) return false;
        if (name.find("..") != std::string::npos) return false;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return (lower.size() > 4) &&
               (lower.rfind(".esp") == lower.size() - 4 ||
                lower.rfind(".esm") == lower.size() - 4 ||
                lower.rfind(".esl") == lower.size() - 4);
    }

    // ---- Public API ----

    std::vector<std::string> GetInstalledPlugins()
    {
        std::string dataDir = GetDataDir();
        std::vector<std::string> plugins;

        for (const char* pattern : {"*.esp", "*.esm", "*.esl"})
        {
            std::string search = dataDir + pattern;
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(search.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    plugins.emplace_back(fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        std::sort(plugins.begin(), plugins.end());
        return plugins;
    }

    void ClientSendModList()
    {
        auto plugins = GetInstalledPlugins();

        std::ostringstream ss;
        ss << "MODLIST";
        for (const auto& p : plugins)
            ss << '|' << p;

        CoSyncTransport::Send(ss.str());
        LOG_INFO("[ModSync] Client sent MODLIST (%zu plugins)", plugins.size());
    }

    bool IsModList(const std::string& msg)
    {
        return msg.rfind("MODLIST|", 0) == 0;
    }

    bool IsModFile(const std::string& msg)
    {
        return msg.rfind("MODFILE|", 0) == 0 || msg.rfind("MODSKIP|", 0) == 0;
    }

    void OnReceiveModList(const std::string& msg, uint32_t conn)
    {
        // Parse client mod list: "MODLIST|mod1.esp|mod2.esp|..."
        std::vector<std::string> clientMods;
        {
            std::string body = msg.substr(8);
            std::stringstream ss(body);
            std::string tok;
            while (std::getline(ss, tok, '|'))
                if (!tok.empty()) clientMods.push_back(tok);
        }

        auto hostMods = GetInstalledPlugins();
        std::string dataDir = GetDataDir();

        int sent = 0, skipped = 0;

        for (const auto& hmod : hostMods)
        {
            bool clientHas = std::find(clientMods.begin(), clientMods.end(), hmod) != clientMods.end();
            if (clientHas) continue;

            std::string filepath = dataDir + hmod;
            std::ifstream f(filepath, std::ios::binary | std::ios::ate);
            if (!f) continue;

            size_t fileSize = static_cast<size_t>(f.tellg());
            f.seekg(0, std::ios::beg);

            if (fileSize > kMaxAutoSendBytes)
            {
                LOG_WARN("[ModSync] '%s' is %.1f MB — too large, notifying client",
                    hmod.c_str(), static_cast<double>(fileSize) / 1048576.0);
                CoSyncTransport::SendTo(conn, "MODSKIP|" + hmod + "|" + std::to_string(fileSize));
                skipped++;
                continue;
            }

            std::vector<uint8_t> data(fileSize);
            f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));

            std::string packet = "MODFILE|" + hmod + "|" + HexEncode(data);
            CoSyncTransport::SendTo(conn, packet);   // the requester only, not every client
            LOG_INFO("[ModSync] Sent '%s' (%zu bytes)", hmod.c_str(), fileSize);
            sent++;
        }

        LOG_INFO("[ModSync] Sync done — sent %d, skipped %d (too large)", sent, skipped);
    }

    void OnReceiveModFile(const std::string& msg)
    {
        if (msg.rfind("MODSKIP|", 0) == 0)
        {
            size_t p1 = msg.find('|', 8);
            std::string filename = (p1 != std::string::npos) ? msg.substr(8, p1 - 8) : msg.substr(8);
            std::string sizeStr  = (p1 != std::string::npos) ? msg.substr(p1 + 1) : "?";
            LOG_WARN("[ModSync] Host couldn't auto-send '%s' (%s bytes) — install manually",
                filename.c_str(), sizeStr.c_str());
            return;
        }

        // MODFILE|filename|hexdata
        size_t p1 = msg.find('|', 8);
        if (p1 == std::string::npos) return;
        size_t p2 = msg.find('|', p1 + 1);
        if (p2 == std::string::npos) return;

        std::string filename = msg.substr(8, p1 - 8);
        std::string hexData  = msg.substr(p2 + 1);

        if (!SafeFilename(filename))
        {
            LOG_WARN("[ModSync] Refused unsafe filename: '%s'", filename.c_str());
            return;
        }

        auto data = HexDecode(hexData);
        std::string outPath = GetDataDir() + filename;

        // P4 (2026-09-26): only ever ADD a missing plugin. This used to
        // overwrite any existing file whose size differed -- a host could
        // replace Fallout4.esm or a DLC master. An existing file of any size is
        // the player's own and is never touched; the mismatch is reported.
        const DWORD attrs = GetFileAttributesA(outPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES)
        {
            LOG_WARN("[ModSync] '%s' already exists here -- keeping the local copy (host sent %zu bytes)",
                filename.c_str(), data.size());
            return;
        }

        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            LOG_ERROR("[ModSync] Cannot write '%s'", outPath.c_str());
            return;
        }

        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        LOG_INFO("[ModSync] Downloaded '%s' (%zu bytes) -> Data\\", filename.c_str(), data.size());
    }

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
                std::snprintf(note, sizeof(note), "%s has different mods -- items from mods only one of you has will not sync",
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
                CoSyncGameAPI::QueueNotification("Your mods differ from the host's. Items, NPCs and places from mods only one of you has will not sync -- see the CoSync log.");
        }
    }
}
