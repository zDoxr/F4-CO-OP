#pragma once

// Pre-join content sync, part 1: what a session's required content IS.
//
// Pure logic -- no game headers, no engine calls -- so it can be unit tested
// outside Fallout 4 (see Tools/ContentSyncTests). CoSyncContentSync owns the
// protocol, the threads and the install; this file owns the rules:
//
//   * which files make up the session's content (plugins the host's engine
//     actually LOADED, each plugin's companion files, loose files a mod
//     manager deployed together with that plugin);
//   * which of them may ever be sent (never official content, never native
//     code, only allow-listed folders and extensions);
//   * path safety (no traversal, no absolute paths, no device names, no
//     reparse points below Data);
//   * hashing (SHA-256 identity, CRC32 per chunk, a nonce-keyed possession
//     proof), the manifest wire text, and the plugin (TES4) header.
//
// Engine facts used here (Fallout4.exe 1.11.240, IDA, 2026-09-30):
//   * sub_1402EEFF0 runs for every loaded .ESP/.ESM/.ESL (from the data
//     loader sub_1402E8020) and registers, if present, "<base> - Main.ba2",
//     "<base> - Textures.ba2" (via sub_1402EED40 / ArchiveRegistrationListener),
//     "<base> - Geometry.psg", "<base> - Geometry.csg" (PSGRegistrationListener),
//     "<base>.cdx" and "<base> - Voices_<language>.ba2". Suffix table at
//     0x2EDB248. No other archive is tied to a plugin.
//   * sub_1402EC650 also counts "<base>.ini" in a plugin's size, but nothing
//     found proves the engine LOADS it, and an INI can carry settings such as
//     sStartingConsoleCommand -- so a plugin .ini is deliberately never sent.
//   * Official masters named in the exe: Fallout4.esm, DLCRobot.esm,
//     DLCworkshop01.esm, DLCCoast.esm, DLCworkshop02.esm, DLCworkshop03.esm,
//     DLCNukaWorld.esm, DLCUltraHighResolution.esm, DLCDummy10thAnniversary.esm;
//     Creation Club files are listed in "<game>\Fallout4.ccc"; Bethesda.net
//     Creations in "%LOCALAPPDATA%\Fallout4\ContentCatalog.txt".

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CoSyncContent
{
    // ── Limits (both sides enforce them) ───────────────────────────────────
    constexpr int      kProtocolVersion = 1;
    constexpr uint64_t kMaxFileBytes    = 8ull << 30;    // one file (large texture BA2s are 2-4 GB)
    constexpr uint64_t kMaxTotalBytes   = 64ull << 30;   // everything one join may download
    constexpr size_t   kMaxEntries      = 4096;          // files in one manifest
    constexpr size_t   kMaxPathChars    = 200;
    constexpr size_t   kMaxPathDepth    = 10;
    constexpr uint32_t kChunkBytes      = 256 * 1024;    // < GNS's 512 KB message cap with header
    constexpr size_t   kProofBlocks     = 8;
    constexpr size_t   kProofBlockBytes = 16 * 1024;

    enum class Kind : char
    {
        Plugin    = 'P',   // .esp/.esm/.esl in Data\ root, loaded by the host's engine
        Companion = 'C',   // file the engine ties to a plugin by name (archives, csg/psg, cdx)
        Loose     = 'L',   // loose asset deployed with a plugin (or listed by the host)
    };

    enum class Origin : char
    {
        Mod          = 'M',   // user mod: may be transferred
        BaseGame     = 'B',   // Fallout4.esm / DLC: never transferred
        CreationClub = 'C',   // listed in Fallout4.ccc: never transferred
        Creations    = 'S',   // Bethesda.net Creation (ContentCatalog.txt): never transferred
    };

    struct Entry
    {
        Kind        kind = Kind::Plugin;
        Origin      origin = Origin::Mod;
        std::string relPath;          // relative to Data\, backslashes, as on the host
        std::string owner;            // Companion/Loose: the plugin it belongs to ("" = host-listed)
        uint64_t    size = 0;
        std::string sha256;           // 64 lowercase hex
        // Plugins only:
        int         slot = -1;        // position in the host's load order
        bool        light = false;    // loaded as a light plugin (FE xxx)
        std::vector<std::string> masters;

        bool Transferable() const { return origin == Origin::Mod; }
    };

    struct Manifest
    {
        uint32_t                 build = 0;
        std::vector<Entry>       entries;
        std::vector<std::string> nativePlugins;   // Data\F4SE\Plugins\*.dll names -- advisory only, never sent
    };

    // ── Text helpers ───────────────────────────────────────────────────────
    std::string Lower(std::string s);
    bool        IEquals(const std::string& a, const std::string& b);
    std::string FileExtLower(const std::string& name);     // ".esp"
    std::string FileBase(const std::string& name);         // "HOWA" for "HOWA.esp"
    std::string FormatBytes(uint64_t n);

    // ── Classification ─────────────────────────────────────────────────────
    bool IsPluginFileName(const std::string& name);         // *.esp / *.esm / *.esl, no folder
    bool IsBaseGamePlugin(const std::string& name);          // the exe's official master list
    // Origin of a plugin name given the Creation Club list (Fallout4.ccc) and
    // the Creations catalog text (ContentCatalog.txt, may be empty).
    Origin ClassifyPlugin(const std::string& name, const std::vector<std::string>& cccNames,
                          const std::string& contentCatalogText);
    // Parses Fallout4.ccc / plugins.txt style lists (one name per line, '#'
    // comments, optional leading '*'). Returned names keep their case.
    std::vector<std::string> ParseNameList(const std::string& text);

    // Companion file names the engine ties to a plugin (see header comment),
    // except Voices archives (language-specific; matched by IsCompanionOf).
    std::vector<std::string> CompanionNamesFor(const std::string& pluginName);
    bool IsCompanionOf(const std::string& fileName, const std::string& pluginName);

    // ── Path safety ────────────────────────────────────────────────────────
    // Syntax + policy check of an entry's relPath for its kind. Rejects
    // traversal, absolute/drive/UNC paths, device names, non-ASCII, bad
    // characters, excluded folders (F4SE, PrismaUI_F4, CoSyncContent, ...),
    // and anything not on the kind's extension allow-list.
    bool ValidateEntryPath(const Entry& e, std::string& why);
    bool IsSafeRelativePath(const std::string& rel, std::string& why);   // syntax only
    bool IsAllowedLoosePath(const std::string& rel, std::string& why);   // folder + extension policy

    // ── Hashing ────────────────────────────────────────────────────────────
    class Sha256
    {
    public:
        Sha256();
        ~Sha256();
        Sha256(const Sha256&) = delete;
        Sha256& operator=(const Sha256&) = delete;
        bool Ok() const { return m_hash != nullptr; }
        void Update(const void* data, size_t n);
        std::string FinalHex();                         // once
        void FinalRaw(uint8_t out[32]);                 // once
    private:
        void* m_hash = nullptr;                         // BCRYPT_HASH_HANDLE
        std::vector<uint8_t> m_object;
    };
    std::string Sha256Hex(const void* data, size_t n);
    bool        IsSha256Hex(const std::string& s);
    uint32_t    Crc32(const void* data, size_t n);

    // Hashes a whole file. Honors *cancel between 1 MB blocks. progress (may
    // be null) gets the running byte count.
    bool HashFile(const std::wstring& path, std::string& shaHex, uint64_t& size,
                  const std::atomic<bool>* cancel = nullptr, std::atomic<uint64_t>* progress = nullptr);

    // Possession proof: SHA-256 over nonce, size and kProofBlocks blocks at
    // offsets derived from the nonce. The host computes the same over its own
    // copy, so a client cannot claim a file by echoing the manifest's hash.
    bool ComputeProof(const std::wstring& path, uint64_t size, const std::string& nonceHex, std::string& proofHex);

    // ── Plugin header (TES4) ───────────────────────────────────────────────
    struct PluginHeader
    {
        bool     ok = false;
        uint32_t flags = 0;                       // record flags: 0x1 master, 0x200 light, 0x80 localized
        std::vector<std::string> masters;         // MAST subrecords, in order
    };
    PluginHeader ReadPluginHeader(const std::wstring& path);
    PluginHeader ParsePluginHeader(const uint8_t* data, size_t n);

    // ── Wire text ──────────────────────────────────────────────────────────
    std::string SerializeManifest(const Manifest& m);
    bool        ParseManifest(const std::string& text, Manifest& out, std::string& why);

    // ── Filesystem ─────────────────────────────────────────────────────────
    std::wstring Widen(const std::string& s);             // UTF-8 -> UTF-16
    std::string  Narrow(const std::wstring& s);           // UTF-16 -> UTF-8
    // root + rel, normalized, proven to stay under root, with no reparse point
    // (junction/symlink) at or below root on the way. The file itself need
    // not exist.
    bool ResolveInside(const std::wstring& root, const std::string& rel, std::wstring& full, std::string& why);
    bool GetFileSizeTime(const std::wstring& path, uint64_t& size, uint64_t& mtime);
    bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes);
    bool WriteWholeFileAtomic(const std::wstring& path, const std::string& data);   // temp + rename
    bool CreateDirectories(const std::wstring& dir);

    // Hash cache: "size|mtime|sha|relpath" per line; a file is re-hashed only
    // when its size or last-write time changed.
    class HashCache
    {
    public:
        void Load(const std::wstring& path);
        void Save(const std::wstring& path) const;
        bool Lookup(const std::string& rel, uint64_t size, uint64_t mtime, std::string& sha) const;
        void Store(const std::string& rel, uint64_t size, uint64_t mtime, const std::string& sha);
    private:
        struct Item { uint64_t size; uint64_t mtime; std::string sha; };
        std::unordered_map<std::string, Item> m_items;   // key = lower-case rel
    };
    // Hash through the cache (the cache is updated on a miss).
    bool HashFileCached(HashCache& cache, const std::wstring& full, const std::string& rel,
                        std::string& sha, uint64_t& size, const std::atomic<bool>* cancel = nullptr,
                        std::atomic<uint64_t>* progress = nullptr);

    // ── Vortex deployment manifest (Data\vortex.deployment.json) ───────────
    // Returns (relPath, source mod) pairs if the file is a Fallout 4 Vortex
    // deployment targeting dataDir. Other mod managers: no attribution.
    bool ParseVortexDeployment(const std::string& json, const std::string& dataDirUtf8,
                               std::vector<std::pair<std::string, std::string>>& files, std::string& why);

    // ── plugins.txt ────────────────────────────────────────────────────────
    // Returns the new plugins.txt text with every name in `activate` enabled
    // ('*' prefix). Existing lines are kept; a name that is missing is
    // inserted after the nearest preceding name of `hostOrder` already in the
    // file (else appended). changed = false when nothing had to change.
    std::string ActivateInPluginsTxt(const std::string& text, const std::vector<std::string>& activate,
                                     const std::vector<std::string>& hostOrder, bool& changed);
}
