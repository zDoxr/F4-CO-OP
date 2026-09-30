#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>
#include <bcrypt.h>

#include "CoSyncContentManifest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#pragma comment(lib, "bcrypt.lib")

namespace CoSyncContent
{
    // ── Text helpers ────────────────────────────────────────────────────────

    std::string Lower(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    bool IEquals(const std::string& a, const std::string& b)
    {
        return a.size() == b.size() && Lower(a) == Lower(b);
    }

    std::string FileExtLower(const std::string& name)
    {
        const size_t slash = name.find_last_of('\\');
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return std::string();
        return Lower(name.substr(dot));
    }

    std::string FileBase(const std::string& name)
    {
        const size_t slash = name.find_last_of('\\');
        const std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
        const size_t dot = file.find_last_of('.');
        return dot == std::string::npos ? file : file.substr(0, dot);
    }

    std::string FormatBytes(uint64_t n)
    {
        char buf[48];
        if (n >= (1ull << 30))      std::snprintf(buf, sizeof(buf), "%.2f GB", n / 1073741824.0);
        else if (n >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", n / 1048576.0);
        else if (n >= 1024)         std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
        else                        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
        return buf;
    }

    namespace
    {
        bool ParseU64(const std::string& s, uint64_t& v)
        {
            if (s.empty() || s.size() > 20) return false;
            v = 0;
            for (char c : s)
            {
                if (c < '0' || c > '9') return false;
                const uint64_t d = static_cast<uint64_t>(c - '0');
                if (v > (UINT64_MAX - d) / 10) return false;
                v = v * 10 + d;
            }
            return true;
        }

        std::vector<std::string> Split(const std::string& s, char sep)
        {
            std::vector<std::string> out;
            size_t pos = 0;
            for (;;)
            {
                const size_t bar = s.find(sep, pos);
                if (bar == std::string::npos) { out.push_back(s.substr(pos)); break; }
                out.push_back(s.substr(pos, bar - pos));
                pos = bar + 1;
            }
            return out;
        }

        bool EndsWithI(const std::string& s, const std::string& suffix)
        {
            return s.size() >= suffix.size() && IEquals(s.substr(s.size() - suffix.size()), suffix);
        }
    }

    // ── Classification ──────────────────────────────────────────────────────

    bool IsPluginFileName(const std::string& name)
    {
        if (name.empty() || name.find('\\') != std::string::npos || name.find('/') != std::string::npos)
            return false;
        const std::string ext = FileExtLower(name);
        return (ext == ".esp" || ext == ".esm" || ext == ".esl") && name.size() > 4;
    }

    bool IsBaseGamePlugin(const std::string& name)
    {
        // The official masters named in Fallout4.exe 1.11.240 (string table
        // next to "Fallout4.ccc", plus DLCUltraHighResolution.esm at 0x255E264).
        static const char* const kOfficial[] = {
            "fallout4.esm", "dlcrobot.esm", "dlcworkshop01.esm", "dlccoast.esm", "dlcworkshop02.esm",
            "dlcworkshop03.esm", "dlcnukaworld.esm", "dlcultrahighresolution.esm", "dlcdummy10thanniversary.esm",
        };
        const std::string l = Lower(name);
        for (const char* o : kOfficial)
            if (l == o) return true;
        return false;
    }

    namespace
    {
        // Bethesda's Creation Club file naming as seen in Fallout4.ccc:
        // "cc" + 3 letters (publisher) + "FO4" + 3 digits + "-". Used on top of
        // the ccc list so paid content is never sent even if the list is gone.
        bool LooksLikeCreationClubName(const std::string& name)
        {
            const std::string l = Lower(name);
            if (l.size() < 12 || l.compare(0, 2, "cc") != 0) return false;
            for (int i = 2; i < 5; ++i) if (!std::isalpha(static_cast<unsigned char>(l[i]))) return false;
            if (l.compare(5, 3, "fo4") != 0) return false;
            for (int i = 8; i < 11; ++i) if (!std::isdigit(static_cast<unsigned char>(l[i]))) return false;
            return l[11] == '-';
        }
    }

    Origin ClassifyPlugin(const std::string& name, const std::vector<std::string>& cccNames,
                          const std::string& contentCatalogText)
    {
        if (IsBaseGamePlugin(name))
            return Origin::BaseGame;
        for (const std::string& c : cccNames)
            if (IEquals(c, name))
                return Origin::CreationClub;
        if (LooksLikeCreationClubName(name))
            return Origin::CreationClub;
        if (!contentCatalogText.empty())
        {
            const std::string needle = "\"" + Lower(name) + "\"";
            if (Lower(contentCatalogText).find(needle) != std::string::npos)
                return Origin::Creations;
        }
        return Origin::Mod;
    }

    std::vector<std::string> ParseNameList(const std::string& text)
    {
        std::vector<std::string> out;
        for (std::string line : Split(text, '\n'))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            size_t b = 0;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            line = line.substr(b);
            if (line.empty() || line[0] == '#')
                continue;
            if (line[0] == '*')
                line = line.substr(1);
            if (!line.empty())
                out.push_back(line);
        }
        return out;
    }

    std::vector<std::string> CompanionNamesFor(const std::string& pluginName)
    {
        const std::string base = FileBase(pluginName);
        return {
            base + " - Main.ba2",
            base + " - Textures.ba2",
            base + " - Geometry.csg",
            base + " - Geometry.psg",
            base + ".cdx",
        };
    }

    bool IsCompanionOf(const std::string& fileName, const std::string& pluginName)
    {
        if (fileName.find('\\') != std::string::npos || !IsPluginFileName(pluginName))
            return false;
        for (const std::string& c : CompanionNamesFor(pluginName))
            if (IEquals(c, fileName))
                return true;
        // "<base> - Voices_<language>.ba2"
        const std::string prefix = Lower(FileBase(pluginName)) + " - voices_";
        const std::string l = Lower(fileName);
        if (l.size() <= prefix.size() + 4 || l.compare(0, prefix.size(), prefix) != 0 || !EndsWithI(l, ".ba2"))
            return false;
        const std::string lang = l.substr(prefix.size(), l.size() - prefix.size() - 4);
        if (lang.empty() || lang.size() > 16)
            return false;
        for (char c : lang)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return false;
        return true;
    }

    // ── Path safety ─────────────────────────────────────────────────────────

    bool IsSafeRelativePath(const std::string& rel, std::string& why)
    {
        if (rel.empty())                    { why = "empty path"; return false; }
        if (rel.size() > kMaxPathChars)     { why = "path too long"; return false; }
        for (unsigned char c : rel)
        {
            if (c < 0x20 || c > 0x7E)       { why = "non-ASCII or control character"; return false; }
            if (std::strchr("<>:\"/|?*", c)) { why = "forbidden character"; return false; }
        }
        if (rel[0] == '\\' || rel.back() == '\\') { why = "leading/trailing separator"; return false; }

        const std::vector<std::string> parts = Split(rel, '\\');
        if (parts.size() > kMaxPathDepth)   { why = "path too deep"; return false; }
        static const char* const kDevices[] = {
            "con", "prn", "aux", "nul", "conin$", "conout$",
            "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
            "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
        };
        for (const std::string& p : parts)
        {
            if (p.empty())                  { why = "empty path component"; return false; }
            if (p == "." || p == "..")      { why = "relative component"; return false; }
            if (p.back() == '.' || p.back() == ' ' || p.front() == ' ')
                                            { why = "component ends with a dot or space"; return false; }
            // 8.3 short-name aliases ("PROGRA~1") could name another folder.
            const size_t tilde = p.find('~');
            if (tilde != std::string::npos && tilde + 1 < p.size() && std::isdigit(static_cast<unsigned char>(p[tilde + 1])))
                                            { why = "short-name alias"; return false; }
            const std::string stem = Lower(p.substr(0, p.find('.')));
            for (const char* d : kDevices)
                if (stem == d)              { why = "device name"; return false; }
        }
        return true;
    }

    bool IsAllowedLoosePath(const std::string& rel, std::string& why)
    {
        const std::vector<std::string> parts = Split(rel, '\\');
        if (parts.size() < 2)               { why = "loose file must be inside an asset folder"; return false; }

        // Asset folders the engine reads loose files from. Deliberately absent:
        // F4SE (native plugins), PrismaUI_F4 (HTML/JS run by the UI), Video
        // (vanilla movies), CoSyncContent (our own staging), anything else.
        static const char* const kFolders[] = {
            "meshes", "textures", "materials", "sound", "music", "scripts",
            "interface", "strings", "lodsettings", "vis", "seq",
        };
        const std::string top = Lower(parts[0]);
        bool folderOk = false;
        for (const char* f : kFolders)
            if (top == f) { folderOk = true; break; }
        if (!folderOk)                      { why = "folder '" + parts[0] + "' is not synchronized"; return false; }

        // Data formats only. Nothing the OS or a native loader would execute.
        static const char* const kExts[] = {
            ".nif", ".dds", ".bgsm", ".bgem", ".hkx", ".tri", ".btr", ".bto", ".btt",
            ".wav", ".xwm", ".fuz", ".lip",
            ".pex", ".psc",                      // Papyrus (runs in the game's script VM)
            ".swf",                              // Scaleform UI movie
            ".strings", ".dlstrings", ".ilstrings", ".txt",
            ".lod", ".seq", ".uvd",
        };
        const std::string ext = FileExtLower(rel);
        for (const char* e : kExts)
            if (ext == e) return true;
        why = "file type '" + ext + "' is not synchronized";
        return false;
    }

    bool ValidateEntryPath(const Entry& e, std::string& why)
    {
        if (!IsSafeRelativePath(e.relPath, why))
            return false;
        switch (e.kind)
        {
        case Kind::Plugin:
            if (!IsPluginFileName(e.relPath)) { why = "not a plugin file name"; return false; }
            return true;
        case Kind::Companion:
            if (!IsPluginFileName(e.owner) || !IsCompanionOf(e.relPath, e.owner))
            { why = "not a companion file of '" + e.owner + "'"; return false; }
            return true;
        case Kind::Loose:
            if (!e.owner.empty() && !IsPluginFileName(e.owner)) { why = "bad owner"; return false; }
            return IsAllowedLoosePath(e.relPath, why);
        }
        why = "unknown kind";
        return false;
    }

    // ── Hashing ─────────────────────────────────────────────────────────────

    namespace
    {
        BCRYPT_ALG_HANDLE Sha256Alg()
        {
            static BCRYPT_ALG_HANDLE s_alg = []() -> BCRYPT_ALG_HANDLE
            {
                BCRYPT_ALG_HANDLE h = nullptr;
                if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
                    return nullptr;
                return h;
            }();
            return s_alg;
        }

        std::string ToHex(const uint8_t* p, size_t n)
        {
            static const char kHex[] = "0123456789abcdef";
            std::string out(n * 2, '0');
            for (size_t i = 0; i < n; ++i)
            {
                out[2 * i] = kHex[p[i] >> 4];
                out[2 * i + 1] = kHex[p[i] & 0xF];
            }
            return out;
        }

        bool FromHex(const std::string& s, std::vector<uint8_t>& out)
        {
            if (s.size() % 2) return false;
            out.clear();
            for (size_t i = 0; i < s.size(); i += 2)
            {
                auto nib = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return -1;
                };
                const int hi = nib(s[i]), lo = nib(s[i + 1]);
                if (hi < 0 || lo < 0) return false;
                out.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
            return true;
        }

        struct FileHandle
        {
            HANDLE h = INVALID_HANDLE_VALUE;
            ~FileHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        };

        bool OpenRead(const std::wstring& path, FileHandle& fh, DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN)
        {
            fh.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | flags, nullptr);
            return fh.h != INVALID_HANDLE_VALUE;
        }

        bool ReadAt(HANDLE h, uint64_t offset, void* buf, DWORD n, DWORD& got)
        {
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
                return false;
            got = 0;
            return ::ReadFile(h, buf, n, &got, nullptr) != 0;
        }
    }

    Sha256::Sha256()
    {
        BCRYPT_ALG_HANDLE alg = Sha256Alg();
        if (!alg) return;
        DWORD objLen = 0, cb = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0)))
            return;
        m_object.resize(objLen);
        BCRYPT_HASH_HANDLE h = nullptr;
        if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, m_object.data(), objLen, nullptr, 0, 0)))
            m_hash = h;
    }

    Sha256::~Sha256()
    {
        if (m_hash) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash));
    }

    void Sha256::Update(const void* data, size_t n)
    {
        if (!m_hash) return;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (n > 0)
        {
            const ULONG part = static_cast<ULONG>((std::min)(n, static_cast<size_t>(1u << 30)));
            BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(m_hash), const_cast<PUCHAR>(p), part, 0);
            p += part;
            n -= part;
        }
    }

    void Sha256::FinalRaw(uint8_t out[32])
    {
        std::memset(out, 0, 32);
        if (m_hash)
            BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash), out, 32, 0);
    }

    std::string Sha256::FinalHex()
    {
        if (!m_hash) return std::string();
        uint8_t raw[32];
        FinalRaw(raw);
        return ToHex(raw, 32);
    }

    std::string Sha256Hex(const void* data, size_t n)
    {
        Sha256 h;
        h.Update(data, n);
        return h.FinalHex();
    }

    bool IsSha256Hex(const std::string& s)
    {
        if (s.size() != 64) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }

    uint32_t Crc32(const void* data, size_t n)
    {
        static uint32_t table[256];
        static const bool init = []
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            return true;
        }();
        (void)init;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i)
            crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    bool HashFile(const std::wstring& path, std::string& shaHex, uint64_t& size,
                  const std::atomic<bool>* cancel, std::atomic<uint64_t>* progress)
    {
        shaHex.clear();
        size = 0;
        FileHandle fh;
        if (!OpenRead(path, fh))
            return false;
        Sha256 h;
        if (!h.Ok())
            return false;
        std::vector<uint8_t> buf(1 << 20);
        for (;;)
        {
            if (cancel && cancel->load())
                return false;
            DWORD got = 0;
            if (!::ReadFile(fh.h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr))
                return false;
            if (got == 0)
                break;
            h.Update(buf.data(), got);
            size += got;
            if (progress) progress->fetch_add(got);
        }
        shaHex = h.FinalHex();
        return !shaHex.empty();
    }

    bool ComputeProof(const std::wstring& path, uint64_t size, const std::string& nonceHex, std::string& proofHex)
    {
        proofHex.clear();
        std::vector<uint8_t> nonce;
        if (!FromHex(nonceHex, nonce) || nonce.size() < 16 || nonce.size() > 64)
            return false;
        FileHandle fh;
        if (!OpenRead(path, fh, FILE_FLAG_RANDOM_ACCESS))
            return false;
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(fh.h, &li) || static_cast<uint64_t>(li.QuadPart) != size)
            return false;

        Sha256 h;
        h.Update(nonce.data(), nonce.size());
        h.Update(&size, sizeof(size));
        std::vector<uint8_t> buf(kProofBlockBytes);
        if (size <= kProofBlocks * kProofBlockBytes)
        {
            uint64_t off = 0;
            while (off < size)
            {
                DWORD got = 0;
                const DWORD want = static_cast<DWORD>((std::min<uint64_t>)(buf.size(), size - off));
                if (!ReadAt(fh.h, off, buf.data(), want, got) || got != want) return false;
                h.Update(buf.data(), got);
                off += got;
            }
        }
        else
        {
            for (uint32_t k = 0; k < kProofBlocks; ++k)
            {
                Sha256 seed;
                seed.Update(nonce.data(), nonce.size());
                seed.Update(&k, sizeof(k));
                uint8_t raw[32];
                seed.FinalRaw(raw);
                uint64_t r = 0;
                std::memcpy(&r, raw, sizeof(r));
                const uint64_t off = r % (size - kProofBlockBytes + 1);
                DWORD got = 0;
                if (!ReadAt(fh.h, off, buf.data(), static_cast<DWORD>(kProofBlockBytes), got) || got != kProofBlockBytes)
                    return false;
                h.Update(&k, sizeof(k));
                h.Update(&off, sizeof(off));
                h.Update(buf.data(), got);
            }
        }
        proofHex = h.FinalHex();
        return !proofHex.empty();
    }

    // ── Plugin header ───────────────────────────────────────────────────────
    // Record header (24 bytes): "TES4", u32 dataSize, u32 flags, u32 formID,
    // u32 vcs, u16 version, u16 unknown. Then subrecords: 4-char type, u16
    // size, data; an "XXXX" subrecord (size 4) carries the u32 size of the
    // next one. MAST = zero-terminated master file name.

    PluginHeader ParsePluginHeader(const uint8_t* data, size_t n)
    {
        PluginHeader h;
        if (!data || n < 24 || std::memcmp(data, "TES4", 4) != 0)
            return h;
        uint32_t dataSize = 0;
        std::memcpy(&dataSize, data + 4, 4);
        std::memcpy(&h.flags, data + 8, 4);
        const size_t end = (std::min)(n, static_cast<size_t>(24) + dataSize);
        size_t pos = 24;
        uint32_t bigNext = 0;
        while (pos + 6 <= end)
        {
            char type[5] = {};
            std::memcpy(type, data + pos, 4);
            uint16_t sz16 = 0;
            std::memcpy(&sz16, data + pos + 4, 2);
            pos += 6;
            size_t sz = bigNext ? bigNext : sz16;
            bigNext = 0;
            if (pos + sz > end)
                return h;   // truncated
            if (std::memcmp(type, "XXXX", 4) == 0 && sz == 4)
            {
                std::memcpy(&bigNext, data + pos, 4);
            }
            else if (std::memcmp(type, "MAST", 4) == 0 && sz > 0 && h.masters.size() < 255)
            {
                const char* s = reinterpret_cast<const char*>(data + pos);
                size_t len = 0;
                while (len < sz && s[len]) ++len;
                h.masters.emplace_back(s, len);
            }
            pos += sz;
        }
        h.ok = true;
        return h;
    }

    PluginHeader ReadPluginHeader(const std::wstring& path)
    {
        FileHandle fh;
        if (!OpenRead(path, fh))
            return PluginHeader();
        uint8_t head[24];
        DWORD got = 0;
        if (!ReadAt(fh.h, 0, head, sizeof(head), got) || got != sizeof(head) || std::memcmp(head, "TES4", 4) != 0)
            return PluginHeader();
        uint32_t dataSize = 0;
        std::memcpy(&dataSize, head + 4, 4);
        if (dataSize > (1u << 20))
            return PluginHeader();
        std::vector<uint8_t> buf(24 + dataSize);
        if (!ReadAt(fh.h, 0, buf.data(), static_cast<DWORD>(buf.size()), got) || got != buf.size())
            return PluginHeader();
        return ParsePluginHeader(buf.data(), buf.size());
    }

    // ── Wire text ───────────────────────────────────────────────────────────
    //   V|<protocol>|<build>
    //   P|<origin>|<size>|<sha>|<name>|<slot>|<light 0/1>|<master:master:...>
    //   C|<origin>|<size>|<sha>|<relpath>|<owner plugin>
    //   L|<origin>|<size>|<sha>|<relpath>|<owner plugin or empty>
    //   N|<native plugin dll name>
    //   E
    // Entry index = position among the P/C/L lines.

    std::string SerializeManifest(const Manifest& m)
    {
        std::string out = "V|" + std::to_string(kProtocolVersion) + "|" + std::to_string(m.build) + "\n";
        for (const Entry& e : m.entries)
        {
            out += static_cast<char>(e.kind);
            out += '|';
            out += static_cast<char>(e.origin);
            out += "|" + std::to_string(e.size) + "|" + e.sha256 + "|" + e.relPath;
            if (e.kind == Kind::Plugin)
            {
                out += "|" + std::to_string(e.slot) + "|" + (e.light ? "1" : "0") + "|";
                for (size_t i = 0; i < e.masters.size(); ++i)
                {
                    if (i) out += ':';
                    out += e.masters[i];
                }
            }
            else
            {
                out += "|" + e.owner;
            }
            out += "\n";
        }
        for (const std::string& n : m.nativePlugins)
            out += "N|" + n + "\n";
        out += "E\n";
        return out;
    }

    bool ParseManifest(const std::string& text, Manifest& out, std::string& why)
    {
        out = Manifest();
        std::vector<std::string> lines = Split(text, '\n');
        if (!lines.empty() && lines.back().empty())
            lines.pop_back();
        if (lines.empty())                   { why = "empty manifest"; return false; }

        const std::vector<std::string> v = Split(lines[0], '|');
        uint64_t proto = 0, build = 0;
        if (v.size() != 3 || v[0] != "V" || !ParseU64(v[1], proto) || !ParseU64(v[2], build))
        { why = "bad manifest header"; return false; }
        if (proto != static_cast<uint64_t>(kProtocolVersion))
        { why = "content protocol " + v[1] + " (this build speaks " + std::to_string(kProtocolVersion) + ")"; return false; }
        out.build = static_cast<uint32_t>(build);

        std::unordered_set<std::string> seen;
        std::unordered_set<std::string> plugins;
        bool ended = false;
        for (size_t i = 1; i < lines.size(); ++i)
        {
            const std::string& line = lines[i];
            if (ended)                       { why = "data after end marker"; return false; }
            if (line == "E")                 { ended = true; continue; }
            const std::vector<std::string> f = Split(line, '|');
            if (f.empty() || f[0].size() != 1) { why = "bad line"; return false; }

            if (f[0] == "N")
            {
                if (f.size() != 2 || out.nativePlugins.size() >= 256) { why = "bad native plugin line"; return false; }
                std::string w;
                if (!IsSafeRelativePath(f[1], w) || f[1].find('\\') != std::string::npos || FileExtLower(f[1]) != ".dll")
                { why = "bad native plugin name"; return false; }
                out.nativePlugins.push_back(f[1]);
                continue;
            }

            Entry e;
            const char k = f[0][0];
            if (k != 'P' && k != 'C' && k != 'L') { why = "unknown line type"; return false; }
            e.kind = static_cast<Kind>(k);
            if ((e.kind == Kind::Plugin && f.size() != 8) || (e.kind != Kind::Plugin && f.size() != 6))
            { why = "wrong field count"; return false; }
            if (f[1].size() != 1 || std::strchr("MBCS", f[1][0]) == nullptr) { why = "bad origin"; return false; }
            e.origin = static_cast<Origin>(f[1][0]);
            if (!ParseU64(f[2], e.size) || e.size > kMaxFileBytes) { why = "bad size"; return false; }
            if (!IsSha256Hex(f[3]))          { why = "bad hash"; return false; }
            e.sha256 = f[3];
            e.relPath = f[4];
            if (e.kind == Kind::Plugin)
            {
                uint64_t slot = 0;
                if (!ParseU64(f[5], slot) || slot > 0xFFFF || (f[6] != "0" && f[6] != "1"))
                { why = "bad plugin fields"; return false; }
                e.slot = static_cast<int>(slot);
                e.light = f[6] == "1";
                if (!f[7].empty())
                    for (const std::string& ms : Split(f[7], ':'))
                    {
                        if (!IsPluginFileName(ms) || e.masters.size() >= 255) { why = "bad master"; return false; }
                        e.masters.push_back(ms);
                    }
            }
            else
            {
                e.owner = f[5];
                if (!e.owner.empty() && plugins.find(Lower(e.owner)) == plugins.end())
                { why = "file '" + e.relPath + "' names an owner plugin that is not in the manifest"; return false; }
                if (e.kind == Kind::Companion && e.owner.empty()) { why = "companion without owner"; return false; }
            }
            std::string pathWhy;
            if (!ValidateEntryPath(e, pathWhy)) { why = "unsafe entry '" + e.relPath + "': " + pathWhy; return false; }
            if (!seen.insert(Lower(e.relPath)).second) { why = "duplicate entry '" + e.relPath + "'"; return false; }
            if (e.kind == Kind::Plugin)
                plugins.insert(Lower(e.relPath));
            if (out.entries.size() >= kMaxEntries) { why = "too many files"; return false; }
            out.entries.push_back(std::move(e));
        }
        if (!ended)                          { why = "manifest truncated"; return false; }
        return true;
    }

    // ── Filesystem ──────────────────────────────────────────────────────────

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
        return out;
    }

    std::string Narrow(const std::wstring& s)
    {
        if (s.empty()) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
        return out;
    }

    namespace
    {
        std::wstring FullPath(const std::wstring& p)
        {
            wchar_t buf[1024];
            const DWORD n = GetFullPathNameW(p.c_str(), 1024, buf, nullptr);
            if (n == 0 || n >= 1024) return std::wstring();
            std::wstring out(buf, n);
            while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/'))
                out.pop_back();
            return out;
        }

        bool StartsWithI(const std::wstring& s, const std::wstring& prefix)
        {
            return s.size() >= prefix.size() &&
                   CompareStringOrdinal(s.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                        static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
        }
    }

    bool ResolveInside(const std::wstring& root, const std::string& rel, std::wstring& full, std::string& why)
    {
        full.clear();
        if (!IsSafeRelativePath(rel, why))
            return false;
        const std::wstring r = FullPath(root);
        if (r.empty())                       { why = "bad root"; return false; }
        const std::wstring candidate = FullPath(r + L"\\" + Widen(rel));
        if (candidate.empty() || !StartsWithI(candidate, r + L"\\"))
        { why = "path escapes the destination folder"; return false; }

        // Every existing component below root must be a plain folder/file:
        // a junction or symlink there could point anywhere.
        size_t pos = r.size() + 1;
        for (;;)
        {
            const size_t slash = candidate.find(L'\\', pos);
            const std::wstring prefix = slash == std::wstring::npos ? candidate : candidate.substr(0, slash);
            const DWORD attrs = GetFileAttributesW(prefix.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES)
            {
                if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) { why = "a folder on the path is a link"; return false; }
                if (slash == std::wstring::npos && (attrs & FILE_ATTRIBUTE_DIRECTORY)) { why = "destination is a folder"; return false; }
                if (slash != std::wstring::npos && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) { why = "a file is in the way of a folder"; return false; }
            }
            if (slash == std::wstring::npos)
                break;
            pos = slash + 1;
        }
        full = candidate;
        return true;
    }

    bool GetFileSizeTime(const std::wstring& path, uint64_t& size, uint64_t& mtime)
    {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return false;
        size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        mtime = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        return true;
    }

    bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes)
    {
        out.clear();
        FileHandle fh;
        if (!OpenRead(path, fh))
            return false;
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(fh.h, &li) || li.QuadPart < 0 || static_cast<uint64_t>(li.QuadPart) > maxBytes)
            return false;
        out.resize(static_cast<size_t>(li.QuadPart));
        DWORD got = 0;
        if (!out.empty() && (!::ReadFile(fh.h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) || got != out.size()))
            return false;
        return true;
    }

    bool WriteWholeFileAtomic(const std::wstring& path, const std::string& data)
    {
        const std::wstring tmp = path + L".cosync-tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        DWORD wrote = 0;
        const bool ok = (data.empty() || (::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) && wrote == data.size()))
                        && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    bool CreateDirectories(const std::wstring& dir)
    {
        if (dir.empty()) return false;
        const DWORD attrs = GetFileAttributesW(dir.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES)
            return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 2 && !CreateDirectories(dir.substr(0, slash)))
            return false;
        return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    // ── Hash cache ──────────────────────────────────────────────────────────

    void HashCache::Load(const std::wstring& path)
    {
        m_items.clear();
        std::string text;
        if (!ReadWholeFile(path, text, 16u << 20))
            return;
        for (const std::string& line : Split(text, '\n'))
        {
            const std::vector<std::string> f = Split(line, '|');
            Item it{};
            if (f.size() != 4 || !ParseU64(f[0], it.size) || !ParseU64(f[1], it.mtime) || !IsSha256Hex(f[2]) || f[3].empty())
                continue;
            it.sha = f[2];
            m_items[Lower(f[3])] = it;
        }
    }

    void HashCache::Save(const std::wstring& path) const
    {
        std::string text;
        for (const auto& kv : m_items)
            text += std::to_string(kv.second.size) + "|" + std::to_string(kv.second.mtime) + "|" + kv.second.sha + "|" + kv.first + "\n";
        WriteWholeFileAtomic(path, text);
    }

    bool HashCache::Lookup(const std::string& rel, uint64_t size, uint64_t mtime, std::string& sha) const
    {
        const auto it = m_items.find(Lower(rel));
        if (it == m_items.end() || it->second.size != size || it->second.mtime != mtime)
            return false;
        sha = it->second.sha;
        return true;
    }

    void HashCache::Store(const std::string& rel, uint64_t size, uint64_t mtime, const std::string& sha)
    {
        m_items[Lower(rel)] = Item{ size, mtime, sha };
    }

    bool HashFileCached(HashCache& cache, const std::wstring& full, const std::string& rel,
                        std::string& sha, uint64_t& size, const std::atomic<bool>* cancel,
                        std::atomic<uint64_t>* progress)
    {
        uint64_t sz = 0, mt = 0;
        if (!GetFileSizeTime(full, sz, mt))
            return false;
        if (cache.Lookup(rel, sz, mt, sha))
        {
            size = sz;
            if (progress) progress->fetch_add(sz);
            return true;
        }
        if (!HashFile(full, sha, size, cancel, progress))
            return false;
        uint64_t sz2 = 0, mt2 = 0;
        // Only cache a hash the file still matches (not modified mid-read).
        if (GetFileSizeTime(full, sz2, mt2) && sz2 == size && mt2 == mt)
            cache.Store(rel, size, mt, sha);
        return true;
    }

    // ── Minimal JSON reader (Vortex deployment manifest) ────────────────────

    namespace
    {
        struct JVal
        {
            enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
            std::string s;
            std::vector<JVal> a;
            std::vector<std::pair<std::string, JVal>> o;
            const JVal* Get(const char* key) const
            {
                for (const auto& kv : o) if (kv.first == key) return &kv.second;
                return nullptr;
            }
        };

        class JParser
        {
        public:
            JParser(const char* p, const char* e) : m_p(p), m_e(e) {}
            bool Parse(JVal& v) { Ws(); if (!Value(v, 0)) return false; Ws(); return m_p == m_e; }
        private:
            const char* m_p;
            const char* m_e;
            void Ws() { while (m_p < m_e && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r')) ++m_p; }
            bool Lit(const char* s)
            {
                const size_t n = std::strlen(s);
                if (static_cast<size_t>(m_e - m_p) < n || std::memcmp(m_p, s, n) != 0) return false;
                m_p += n;
                return true;
            }
            static void PutUtf8(std::string& out, uint32_t cp)
            {
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            }
            bool Hex4(uint32_t& v)
            {
                if (m_e - m_p < 4) return false;
                v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const char c = *m_p++;
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= c - '0';
                    else if (c >= 'a' && c <= 'f') v |= 10 + c - 'a';
                    else if (c >= 'A' && c <= 'F') v |= 10 + c - 'A';
                    else return false;
                }
                return true;
            }
            bool Str(std::string& out)
            {
                if (m_p >= m_e || *m_p != '"') return false;
                ++m_p;
                while (m_p < m_e)
                {
                    const char c = *m_p++;
                    if (c == '"') return true;
                    if (static_cast<unsigned char>(c) < 0x20) return false;
                    if (c != '\\') { out += c; continue; }
                    if (m_p >= m_e) return false;
                    const char esc = *m_p++;
                    switch (esc)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        uint32_t cp = 0;
                        if (!Hex4(cp)) return false;
                        if (cp >= 0xD800 && cp < 0xDC00)
                        {
                            uint32_t lo = 0;
                            if (!Lit("\\u") || !Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        PutUtf8(out, cp);
                        break;
                    }
                    default: return false;
                    }
                }
                return false;
            }
            bool Value(JVal& v, int depth)
            {
                if (depth > 64 || m_p >= m_e) return false;
                const char c = *m_p;
                if (c == '{')
                {
                    v.t = JVal::T::Obj;
                    ++m_p; Ws();
                    if (m_p < m_e && *m_p == '}') { ++m_p; return true; }
                    for (;;)
                    {
                        Ws();
                        std::pair<std::string, JVal> kv;
                        if (!Str(kv.first)) return false;
                        Ws();
                        if (m_p >= m_e || *m_p != ':') return false;
                        ++m_p; Ws();
                        if (!Value(kv.second, depth + 1)) return false;
                        v.o.push_back(std::move(kv));
                        Ws();
                        if (m_p < m_e && *m_p == ',') { ++m_p; continue; }
                        if (m_p < m_e && *m_p == '}') { ++m_p; return true; }
                        return false;
                    }
                }
                if (c == '[')
                {
                    v.t = JVal::T::Arr;
                    ++m_p; Ws();
                    if (m_p < m_e && *m_p == ']') { ++m_p; return true; }
                    for (;;)
                    {
                        Ws();
                        JVal item;
                        if (!Value(item, depth + 1)) return false;
                        v.a.push_back(std::move(item));
                        Ws();
                        if (m_p < m_e && *m_p == ',') { ++m_p; continue; }
                        if (m_p < m_e && *m_p == ']') { ++m_p; return true; }
                        return false;
                    }
                }
                if (c == '"') { v.t = JVal::T::Str; return Str(v.s); }
                if (Lit("true") || Lit("false")) { v.t = JVal::T::Bool; return true; }
                if (Lit("null")) { v.t = JVal::T::Null; return true; }
                if (c == '-' || (c >= '0' && c <= '9'))
                {
                    v.t = JVal::T::Num;
                    while (m_p < m_e && std::strchr("+-.eE0123456789", *m_p)) ++m_p;
                    return true;
                }
                return false;
            }
        };

        std::string NormDir(std::string s)
        {
            for (char& c : s) if (c == '/') c = '\\';
            while (!s.empty() && s.back() == '\\') s.pop_back();
            return Lower(s);
        }
    }

    bool ParseVortexDeployment(const std::string& json, const std::string& dataDirUtf8,
                               std::vector<std::pair<std::string, std::string>>& files, std::string& why)
    {
        files.clear();
        if (json.size() > (64u << 20))       { why = "too large"; return false; }
        JVal root;
        JParser p(json.data(), json.data() + json.size());
        if (!p.Parse(root) || root.t != JVal::T::Obj) { why = "not valid JSON"; return false; }
        const JVal* game = root.Get("gameId");
        if (!game || game->t != JVal::T::Str || game->s != "fallout4") { why = "not a Fallout 4 deployment"; return false; }
        const JVal* target = root.Get("targetPath");
        if (!target || target->t != JVal::T::Str || NormDir(target->s) != NormDir(dataDirUtf8))
        { why = "deployment targets another folder"; return false; }
        const JVal* list = root.Get("files");
        if (!list || list->t != JVal::T::Arr) { why = "no file list"; return false; }
        for (const JVal& f : list->a)
        {
            if (f.t != JVal::T::Obj) continue;
            const JVal* rel = f.Get("relPath");
            const JVal* src = f.Get("source");
            if (!rel || !src || rel->t != JVal::T::Str || src->t != JVal::T::Str) continue;
            std::string r = rel->s;
            for (char& c : r) if (c == '/') c = '\\';
            files.emplace_back(r, src->s);
        }
        return true;
    }

    // ── plugins.txt ─────────────────────────────────────────────────────────

    std::string ActivateInPluginsTxt(const std::string& text, const std::vector<std::string>& activate,
                                     const std::vector<std::string>& hostOrder, bool& changed)
    {
        changed = false;
        const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        std::vector<std::string> lines = Split(text, '\n');
        for (std::string& l : lines)
            if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!lines.empty() && lines.back().empty())
            lines.pop_back();

        auto nameOf = [](const std::string& line) -> std::string
        {
            std::string s = line;
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#') return std::string();
            return s[0] == '*' ? s.substr(1) : s;
        };
        auto find = [&](const std::string& name) -> int
        {
            for (size_t i = 0; i < lines.size(); ++i)
                if (IEquals(nameOf(lines[i]), name)) return static_cast<int>(i);
            return -1;
        };

        for (const std::string& want : activate)
        {
            const int at = find(want);
            if (at >= 0)
            {
                if (lines[at][0] != '*') { lines[at] = "*" + nameOf(lines[at]); changed = true; }
                continue;
            }
            // Insert after the closest earlier plugin of the host's order that
            // this file already lists, so the host's relative order is kept.
            int insertAfter = -1;
            for (size_t i = 0; i < hostOrder.size(); ++i)
            {
                if (IEquals(hostOrder[i], want)) break;
                const int pos = find(hostOrder[i]);
                if (pos >= 0) insertAfter = pos;
            }
            const std::string newLine = "*" + want;
            if (insertAfter >= 0)
                lines.insert(lines.begin() + insertAfter + 1, newLine);
            else
                lines.push_back(newLine);
            changed = true;
        }

        std::string out;
        for (const std::string& l : lines)
            out += l + eol;
        return out;
    }
}
