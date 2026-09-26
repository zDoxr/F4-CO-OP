#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>

#include "CoSyncModSync.h"
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
    static const size_t kMaxAutoSendBytes = 10 * 1024 * 1024; // 10 MB

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

    void OnReceiveModList(const std::string& msg)
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
                CoSyncTransport::Send("MODSKIP|" + hmod + "|" + std::to_string(fileSize));
                skipped++;
                continue;
            }

            std::vector<uint8_t> data(fileSize);
            f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));

            std::string packet = "MODFILE|" + hmod + "|" + HexEncode(data);
            CoSyncTransport::Send(packet);
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

        // Skip if already present with identical size
        HANDLE hExist = CreateFileA(outPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hExist != INVALID_HANDLE_VALUE)
        {
            DWORD existSize = GetFileSize(hExist, nullptr);
            CloseHandle(hExist);
            if (existSize == static_cast<DWORD>(data.size()))
            {
                LOG_INFO("[ModSync] '%s' already present (same size)", filename.c_str());
                return;
            }
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
}
