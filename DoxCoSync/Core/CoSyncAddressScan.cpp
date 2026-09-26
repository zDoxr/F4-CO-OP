#include "CoSyncAddressScan.h"
#include "CoSyncAddressSignatures.h"
#include "ConsoleLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace CoSyncAddrScan
{
namespace
{
    constexpr uint16_t kWildcard = 0x100;

    // Locate .text from the PE headers already mapped at moduleBase, rather
    // than hardcoding yet another build-specific constant -- a scanner whose
    // own offsets go stale defeats the point.
    bool GetTextSection(uintptr_t base, const uint8_t*& outStart, size_t& outSize) noexcept
    {
        __try
        {
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

            auto* sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            {
                if (std::memcmp(sec->Name, ".text", 5) == 0)
                {
                    outStart = reinterpret_cast<const uint8_t*>(base + sec->VirtualAddress);
                    outSize  = sec->Misc.VirtualSize;
                    return true;
                }
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(noinline) bool MatchAt(const uint8_t* at, const uint16_t* pat, uint32_t len) noexcept
    {
        __try
        {
            for (uint32_t i = 0; i < len; ++i)
                if (pat[i] != kWildcard && at[i] != static_cast<uint8_t>(pat[i]))
                    return false;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Returns the number of matches found, and the first one via outRva.
    // Stops counting at 2 -- "unique" vs "ambiguous" is all the caller needs,
    // and bailing early keeps a bad signature from costing a full extra pass.
    uint32_t ScanText(const uint8_t* start, size_t size, uintptr_t base,
                      const uint16_t* pat, uint32_t len, uintptr_t& outRva) noexcept
    {
        if (len == 0 || size < len) return 0;

        // Fast reject on the first concrete byte; a leading wildcard would
        // force a full compare at every offset.
        uint32_t firstConcrete = 0;
        while (firstConcrete < len && pat[firstConcrete] == kWildcard) ++firstConcrete;
        if (firstConcrete >= len) return 0;   // all wildcards -- useless signature
        const uint8_t anchor = static_cast<uint8_t>(pat[firstConcrete]);

        uint32_t hits = 0;
        const size_t end = size - len;
        for (size_t i = 0; i <= end; ++i)
        {
            if (start[i + firstConcrete] != anchor) continue;
            if (!MatchAt(start + i, pat, len)) continue;
            if (hits == 0)
                outRva = static_cast<uintptr_t>(
                    reinterpret_cast<uintptr_t>(start + i) - base);
            if (++hits >= 2) break;
        }
        return hits;
    }

    int ReadIniFlag() noexcept
    {
        char exePath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return 0;
        std::string ini(exePath);
        const size_t slash = ini.find_last_of("\\/");
        ini = (slash == std::string::npos) ? std::string() : ini.substr(0, slash + 1);
        ini += "Data\\F4SE\\Plugins\\CoSync.ini";
        return ::GetPrivateProfileIntA("Diagnostics", "bScanAddresses", 0, ini.c_str());
    }

    int s_enabled = -1;
}

bool IsEnabled() noexcept
{
    if (s_enabled < 0) s_enabled = ReadIniFlag();
    return s_enabled != 0;
}

void Run(uintptr_t moduleBase) noexcept
{
    if (!IsEnabled())
        return;
    if (!moduleBase)
    {
        LOG_WARN("[AddrScan] no module base was given -- skipping the scan");
        return;
    }

    const uint8_t* text = nullptr;
    size_t textSize = 0;
    if (!GetTextSection(moduleBase, text, textSize))
    {
        LOG_ERROR("[AddrScan] could not find the code section in the file headers -- skipping the scan");
        return;
    }

    LOG_INFO("[AddrScan] ==== hardcoded-address integrity scan ====");
    LOG_INFO("[AddrScan] base=%p .text=%p size=0x%zX entries=%u",
        reinterpret_cast<void*>(moduleBase), text, textSize,
        (unsigned)kSignatureCount);

    uint32_t ok = 0, moved = 0, ambiguous = 0, missing = 0, unverifiable = 0;
    std::vector<std::string> problems;

    for (uint32_t i = 0; i < kSignatureCount; ++i)
    {
        const Signature& s = kSignatures[i];
        char line[320];

        if (!s.pattern || s.length == 0)
        {
            ++unverifiable;
            continue;   // data/vtable address: no prologue to fingerprint
        }

        const uint8_t* at = reinterpret_cast<const uint8_t*>(moduleBase + s.rva);
        if (MatchAt(at, s.pattern, s.length))
        {
            ++ok;
            continue;
        }

        // The engraved address no longer holds these bytes. Find where they went.
        uintptr_t found = 0;
        const uint32_t hits = ScanText(text, textSize, moduleBase, s.pattern, s.length, found);

        if (hits == 1)
        {
            ++moved;
            std::snprintf(line, sizeof(line),
                "MOVED      %-34s 0x%08X -> 0x%08zX  (delta %+lld)",
                s.name, s.rva, (size_t)found,
                (long long)found - (long long)s.rva);
        }
        else if (hits > 1)
        {
            ++ambiguous;
            std::snprintf(line, sizeof(line),
                "AMBIGUOUS  %-34s 0x%08X  signature matches more than once -- "
                "needs a longer pattern", s.name, s.rva);
        }
        else
        {
            ++missing;
            std::snprintf(line, sizeof(line),
                "MISSING    %-34s 0x%08X  signature not found anywhere in .text -- "
                "the function was changed or removed", s.name, s.rva);
        }
        problems.emplace_back(line);
        LOG_WARN("[AddrScan] %s", line);
    }

    LOG_INFO("[AddrScan] ---- %u ok, %u moved, %u ambiguous, %u missing, %u unverifiable ----",
        ok, moved, ambiguous, missing, unverifiable);

    if (problems.empty())
    {
        LOG_INFO("[AddrScan] every signed address still points at the bytes it was "
                 "harvested from. The hardcoded table matches this build.");
    }
    else
    {
        LOG_ERROR("[AddrScan] %zu address(es) no longer match. This isn't the game build "
                  "these addresses were recorded from -- don't trust any feature that "
                  "relies on a raw address until the table is updated.",
            problems.size());
    }
    LOG_INFO("[AddrScan] ==== end ====");
}

} // namespace CoSyncAddrScan
