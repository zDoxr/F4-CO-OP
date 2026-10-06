#include "CoSyncProfiles.h"
#include "CoSyncTeams.h"
#include "CoSyncEquipHook.h"
#include "CoSyncWorkshopAuthority.h"

#include "CoSyncWorldTransfer.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"
#include "CoSyncWorld.h"
#include "CoSyncSteamID.h"
#include "CoSyncActorValueSafety.h"
#include "CoSyncVMFormArg.h"
#include "CoSyncGameAPI.h"
#include "CoSyncRuntime.h"
#include "CoSyncPlayerManager.h"
#include "CoSyncPlayer.h"
#include "CoSyncProxyLooks.h"
#include "CoSyncCompanions.h"
#include "CoSyncPowerArmorSync.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "PluginAPI.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameReferences.h"
#include "GameData.h"
#include "GameFormComponents.h"
#include "GameExtraData.h"
#include "GameCustomization.h"
#include "PapyrusVM.h"
#include "PapyrusArgs.h"
#include "PapyrusEvents.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "CoSyncReviveSync.h"

#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

extern PlayerCharacter* CoSyncGetPlayer();
extern RelocAddr<_MoveRefrToPosition> MoveRefrToPosition;

namespace
{
    // ── Engine entry points (Fallout4.exe 1.11.240, IDA-verified 2026-09-25) ─
    // Found through the SetLevel / AddPerk / RemovePerk / HasPerk /
    // ShowLooksMenu script-command handlers (the same way as save/load). Every
    // call is gated on the prologue check below.
    constexpr uintptr_t kRVA_SetPlayerLevel  = 0x00D5E170;  // (PlayerCharacter*, level)
    constexpr uintptr_t kRVA_AddPerk         = 0x00C92010;  // (Actor*, BGSPerk*, rank)
    constexpr uintptr_t kRVA_RemovePerk      = 0x00C92110;  // (Actor*, BGSPerk*)
    constexpr uintptr_t kRVA_GetPerkRank     = 0x00C923C0;  // (Actor*, BGSPerk*) -> rank
    constexpr uintptr_t kRVA_ShowLooksMenu   = 0x00A85EE0;  // (ref or null=player, mode, 0, 0, 0)
    constexpr uintptr_t kRVA_CreateTintEntry = 0x003FFB10;  // (templateIndex<<16 | type) -> Entry*
    constexpr uintptr_t kRVA_ClearTints      = 0x004030E0;  // (tArray<Entry*>*)
    constexpr uintptr_t kRVA_DoUpdate3dModel = 0x00D27280;  // (AIProcess*, Actor*, RESET_3D_FLAGS)
    // Papyrus Game.ForceThirdPerson / ForceFirstPerson natives (registered at
    // 0x141124861 / 0x1411248F4): static, no arguments, each is
    // "mov rcx,[PlayerCamera]; jmp PlayerCamera::Force*Person".
    constexpr uintptr_t kRVA_ForceThirdPerson = 0x01129EA0;
    constexpr uintptr_t kRVA_ForceFirstPerson = 0x01129E90;
    // TESObjectREFR::RemoveAllItems (0x140505E40), what Papyrus
    // RemoveAllItems (0x14115BC50) and the console command (0x1405BE490) both
    // call: (ref, transferTo, mode). With no target and mode 3 it removes every
    // item one by one, synchronously, and for the player keeps quest items and
    // keys. Followed, like both callers, by the player's inventory-changed
    // update 0x140D60DA0(player, ref). IDA 1.11.240, 2026-09-26.
    constexpr uintptr_t kRVA_RemoveAllItems   = 0x00505E40;
    constexpr uintptr_t kRVA_PlayerInvChanged = 0x00D60DA0;
    // PlayerCamera* global both natives load. PlayerCamera::currentState is
    // at +0x28 and its first-person state at +0xE0 (ForceFirstPerson compares them).
    constexpr uintptr_t kRVA_PlayerCamera     = 0x030E6E58;

    struct Prologue { uintptr_t rva; uint8_t bytes[8]; };
    constexpr Prologue kPrologues[] = {
        { kRVA_SetPlayerLevel,  { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 } },
        { kRVA_AddPerk,         { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C } },
        { kRVA_RemovePerk,      { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 } },
        { kRVA_GetPerkRank,     { 0x48, 0x8B, 0x81, 0x00, 0x03, 0x00, 0x00, 0x48 } },
        { kRVA_ShowLooksMenu,   { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83 } },
        { kRVA_CreateTintEntry, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xD9 } },
        { kRVA_ClearTints,      { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x41 } },
        { kRVA_DoUpdate3dModel, { 0x40, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 } },
        { kRVA_ForceThirdPerson, { 0x48, 0x8B, 0x0D, 0xB1, 0xCF, 0xFB, 0x01, 0xE9 } },
        { kRVA_ForceFirstPerson, { 0x48, 0x8B, 0x0D, 0xC1, 0xCF, 0xFB, 0x01, 0xE9 } },
        { kRVA_RemoveAllItems,   { 0x48, 0x3B, 0xCA, 0x0F, 0x84, 0xD4, 0x01, 0x00 } },
        { kRVA_PlayerInvChanged, { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x55, 0x48 } },
    };

    using SetPlayerLevel_t  = void (*)(PlayerCharacter*, UInt32);
    using AddPerk_t         = void (*)(Actor*, BGSPerk*, UInt8);
    using RemovePerk_t      = void (*)(Actor*, BGSPerk*);
    using GetPerkRank_t     = UInt8 (*)(Actor*, BGSPerk*);
    using ShowLooksMenu_t   = void (*)(TESObjectREFR*, UInt32, void*, void*, void*);
    using CreateTintEntry_t = BGSCharacterTint::Entry* (*)(UInt32);
    using ClearTints_t      = UInt64 (*)(tArray<BGSCharacterTint::Entry*>*);
    using DoUpdate3dModel_t = void (*)(void* process, Actor*, UInt32 flags);
    using ForcePerson_t     = void (*)();
    using RemoveAllItems_t  = void (*)(TESObjectREFR* ref, TESObjectREFR* transferTo, int mode);
    using PlayerInvChanged_t = UInt64 (*)(PlayerCharacter* player, TESObjectREFR* ref);

    uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }
    template <typename T> T Fn(uintptr_t rva) { return reinterpret_cast<T>(Base() + rva); }

    bool EngineOk()
    {
        static int s_ok = -1;
        if (s_ok >= 0) return s_ok == 1;
        for (const Prologue& p : kPrologues)
        {
            if (std::memcmp(reinterpret_cast<const void*>(Base() + p.rva), p.bytes, sizeof(p.bytes)) != 0)
            {
                LOG_ERROR("[Profiles] Prologue mismatch at RVA 0x%08llX -- profiles disabled (not 1.11.240?)",
                    (unsigned long long)p.rva);
                s_ok = 0;
                return false;
            }
        }
        s_ok = 1;
        return true;
    }

    // Vanilla forms that every character keeps / starts with.
    constexpr UInt32 kPipBoyFormID    = 0x00021B3B;  // Pip-Boy (never removed)
    constexpr UInt32 kVaultSuitFormID = 0x0001EED7;  // Vault 111 jumpsuit (fresh start)
    constexpr int    kFreshSpecial    = 4;           // 7 x 4 = 28 = vanilla 7 + 21 points

    constexpr uint32_t kRecordType    = 'PROF';
    constexpr uint32_t kRecordVersion = 1;
    constexpr size_t   kMaxBlobBytes  = 256 * 1024;

    // ── Profile data ────────────────────────────────────────────────────────
    struct TintData { UInt32 type = 0; UInt16 index = 0; UInt8 percent = 0; UInt32 bgra = 0; SInt16 colorID = -1; };
    struct RegionData { UInt32 index = 0; float v[8] = {}; };

    // A weapon/armor instance with object mods (receivers, scopes, linings,
    // legendary effects...). Plain items are only formID + count.
    struct ModdedItem { UInt32 base = 0; bool equipped = false; std::vector<UInt32> mods; };

    struct Profile
    {
        std::string name;
        // 0.5.1: the CHARACTER's name (chosen at the Vault-Tec rep in the
        // intro). A display property only -- identity is the profile key
        // (SteamID64), so renaming never makes a new player.
        std::string charName;
        UInt32 level = 1;
        float  xp = 0.f;
        float  special[7] = { 1, 1, 1, 1, 1, 1, 1 };
        std::vector<UInt32> perks;
        std::vector<std::pair<UInt32, SInt32>> items;
        std::vector<UInt32> equipped;
        std::vector<ModdedItem> modded;
        std::vector<UInt32> companions;   // this player's own companion copies (base NPCs)
        // False until the player has finished the character editor once. An
        // unfinished character (left or crashed mid-editor) starts over fresh.
        bool   created = false;
        // Where they last stood: an interior cell or an exterior worldspace
        // (formID from the load order; runtime 0xFF forms are never stored).
        bool   hasPos = false;
        bool   posInterior = false;
        UInt32 posSpace = 0;
        float  pos[3] = {};
        float  rotZ = 0.f;
        // appearance
        bool   hasLooks = false;
        UInt8  female = 0;
        UInt32 race = 0;
        float  weights[3] = {};
        UInt32 skin = 0;          // RGBA
        UInt32 hairColor = 0;
        UInt32 faceTextures = 0;
        std::vector<UInt32> headParts;
        std::vector<float>  morphValues;
        std::vector<std::pair<UInt32, float>> morphSets;
        std::vector<RegionData> regions;
        std::vector<TintData>   tints;
    };

    // ── Text serialization (versioned key=value lines) ──────────────────────
    std::string Hex(UInt32 v) { char b[12]; std::snprintf(b, sizeof(b), "%08X", v); return b; }
    UInt32 ParseHex(const std::string& s) { return static_cast<UInt32>(std::strtoul(s.c_str(), nullptr, 16)); }
    std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::string cur;
        std::istringstream ss(s);
        while (std::getline(ss, cur, sep))
            if (!cur.empty()) out.push_back(cur);
        return out;
    }

    std::string Serialize(const Profile& p)
    {
        std::ostringstream o;
        o << "v=1\n";
        std::string name = p.name;
        for (char& c : name) if (c == '\n' || c == '\r' || c == '|') c = ' ';
        o << "name=" << name << "\n";
        std::string cname = p.charName;
        for (char& c : cname) if (c == '\n' || c == '\r' || c == '|') c = ' ';
        if (!cname.empty())
            o << "cname=" << cname << "\n";
        o << "level=" << p.level << "\n";
        o << "xp=" << p.xp << "\n";
        o << "special=";
        for (int i = 0; i < 7; ++i) o << (i ? "," : "") << p.special[i];
        o << "\nperks=";
        for (size_t i = 0; i < p.perks.size(); ++i) o << (i ? "," : "") << Hex(p.perks[i]);
        o << "\nitems=";
        for (size_t i = 0; i < p.items.size(); ++i) o << (i ? "," : "") << Hex(p.items[i].first) << ":" << p.items[i].second;
        o << "\nequip=";
        for (size_t i = 0; i < p.equipped.size(); ++i) o << (i ? "," : "") << Hex(p.equipped[i]);
        o << "\ncreated=" << (p.created ? 1 : 0) << "\n";
        o << "comps=";
        for (size_t i = 0; i < p.companions.size(); ++i) o << (i ? "," : "") << Hex(p.companions[i]);
        o << "\n";
        o << "mods=";
        for (size_t i = 0; i < p.modded.size(); ++i)
        {
            const ModdedItem& m = p.modded[i];
            o << (i ? "," : "") << Hex(m.base) << ":" << (m.equipped ? 1 : 0) << ":";
            for (size_t k = 0; k < m.mods.size(); ++k) o << (k ? "/" : "") << Hex(m.mods[k]);
        }
        o << "\n";
        if (p.hasPos)
            o << "pos=" << (p.posInterior ? "I" : "W") << ":" << Hex(p.posSpace) << ":"
              << p.pos[0] << "," << p.pos[1] << "," << p.pos[2] << "," << p.rotZ << "\n";
        if (p.hasLooks)
        {
            o << "looks=1\nfemale=" << (int)p.female << "\nrace=" << Hex(p.race) << "\n";
            o << "weights=" << p.weights[0] << "," << p.weights[1] << "," << p.weights[2] << "\n";
            o << "skin=" << Hex(p.skin) << "\nhair=" << Hex(p.hairColor) << "\nfacetex=" << Hex(p.faceTextures) << "\n";
            o << "heads=";
            for (size_t i = 0; i < p.headParts.size(); ++i) o << (i ? "," : "") << Hex(p.headParts[i]);
            o << "\nmorphs=";
            for (size_t i = 0; i < p.morphValues.size(); ++i) o << (i ? "," : "") << p.morphValues[i];
            o << "\nmsets=";
            for (size_t i = 0; i < p.morphSets.size(); ++i) o << (i ? "," : "") << Hex(p.morphSets[i].first) << ":" << p.morphSets[i].second;
            o << "\nregions=";
            for (size_t i = 0; i < p.regions.size(); ++i)
            {
                o << (i ? "," : "") << Hex(p.regions[i].index);
                for (float v : p.regions[i].v) o << "/" << v;
            }
            o << "\ntints=";
            for (size_t i = 0; i < p.tints.size(); ++i)
            {
                const TintData& t = p.tints[i];
                o << (i ? "," : "") << t.type << "/" << t.index << "/" << (int)t.percent << "/" << Hex(t.bgra) << "/" << t.colorID;
            }
            o << "\n";
        }
        return o.str();
    }

    bool Deserialize(const std::string& text, Profile& p)
    {
        std::map<std::string, std::string> kv;
        for (const std::string& line : Split(text, '\n'))
        {
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        if (kv["v"] != "1")
            return false;
        p.name  = kv["name"];
        p.charName = kv["cname"];
        p.level = static_cast<UInt32>(std::strtoul(kv["level"].c_str(), nullptr, 10));
        if (p.level < 1) p.level = 1;
        p.xp    = std::strtof(kv["xp"].c_str(), nullptr);
        auto sp = Split(kv["special"], ',');
        for (size_t i = 0; i < 7 && i < sp.size(); ++i) p.special[i] = std::strtof(sp[i].c_str(), nullptr);
        for (const auto& s : Split(kv["perks"], ',')) p.perks.push_back(ParseHex(s));
        for (const auto& s : Split(kv["items"], ','))
        {
            const size_t c = s.find(':');
            if (c != std::string::npos)
                p.items.push_back({ ParseHex(s.substr(0, c)), static_cast<SInt32>(std::strtol(s.c_str() + c + 1, nullptr, 10)) });
        }
        for (const auto& s : Split(kv["equip"], ',')) p.equipped.push_back(ParseHex(s));
        p.created = kv["created"] == "1";
        for (const auto& s : Split(kv["comps"], ',')) if (!s.empty()) p.companions.push_back(ParseHex(s));
        for (const auto& s : Split(kv["mods"], ','))
        {
            const auto f = Split(s, ':');
            if (f.size() != 3) continue;
            ModdedItem m;
            m.base = ParseHex(f[0]);
            m.equipped = f[1] == "1";
            for (const auto& id : Split(f[2], '/')) if (!id.empty()) m.mods.push_back(ParseHex(id));
            if (m.base && !m.mods.empty()) p.modded.push_back(m);
        }
        {
            const auto parts = Split(kv["pos"], ':');
            if (parts.size() == 3 && (parts[0] == "I" || parts[0] == "W"))
            {
                const auto v = Split(parts[2], ',');
                if (v.size() == 4)
                {
                    p.hasPos = true;
                    p.posInterior = parts[0] == "I";
                    p.posSpace = ParseHex(parts[1]);
                    for (int i = 0; i < 3; ++i) p.pos[i] = std::strtof(v[i].c_str(), nullptr);
                    p.rotZ = std::strtof(v[3].c_str(), nullptr);
                }
            }
        }

        p.hasLooks = kv["looks"] == "1";
        if (p.hasLooks)
        {
            p.female = static_cast<UInt8>(std::strtoul(kv["female"].c_str(), nullptr, 10));
            p.race = ParseHex(kv["race"]);
            auto w = Split(kv["weights"], ',');
            for (size_t i = 0; i < 3 && i < w.size(); ++i) p.weights[i] = std::strtof(w[i].c_str(), nullptr);
            p.skin = ParseHex(kv["skin"]);
            p.hairColor = ParseHex(kv["hair"]);
            p.faceTextures = ParseHex(kv["facetex"]);
            for (const auto& s : Split(kv["heads"], ',')) p.headParts.push_back(ParseHex(s));
            for (const auto& s : Split(kv["morphs"], ',')) p.morphValues.push_back(std::strtof(s.c_str(), nullptr));
            for (const auto& s : Split(kv["msets"], ','))
            {
                const size_t c = s.find(':');
                if (c != std::string::npos)
                    p.morphSets.push_back({ ParseHex(s.substr(0, c)), std::strtof(s.c_str() + c + 1, nullptr) });
            }
            for (const auto& s : Split(kv["regions"], ','))
            {
                auto parts = Split(s, '/');
                if (parts.size() != 9) continue;
                RegionData r;
                r.index = ParseHex(parts[0]);
                for (int i = 0; i < 8; ++i) r.v[i] = std::strtof(parts[i + 1].c_str(), nullptr);
                p.regions.push_back(r);
            }
            for (const auto& s : Split(kv["tints"], ','))
            {
                auto parts = Split(s, '/');
                if (parts.size() != 5) continue;
                TintData t;
                t.type    = static_cast<UInt32>(std::strtoul(parts[0].c_str(), nullptr, 10));
                t.index   = static_cast<UInt16>(std::strtoul(parts[1].c_str(), nullptr, 10));
                t.percent = static_cast<UInt8>(std::strtoul(parts[2].c_str(), nullptr, 10));
                t.bgra    = ParseHex(parts[3]);
                t.colorID = static_cast<SInt16>(std::strtol(parts[4].c_str(), nullptr, 10));
                p.tints.push_back(t);
            }
        }
        return true;
    }

    // ── State ───────────────────────────────────────────────────────────────
    std::mutex s_mx;
    std::unordered_map<std::string, std::string> s_profiles;   // key -> serialized (host: authoritative)
    std::unordered_map<uint32_t, std::string>    s_connKey;    // host: conn -> key

    enum class ApplyState { None, Pending, Applied };
    ApplyState  s_applyState = ApplyState::None;
    bool        s_freshStarterCheck = false;   // new player: re-check starting gear after the editor
    double      s_readySince = 0.0;
    double      s_lastUpload = 0.0;
    const char* s_status = "";
    bool        s_openLooksMenu = false;
    double      s_looksMenuAt = 0.0;
    // Client that loaded in wearing the HOST's Power Armor: eject first, then
    // hide that frame, then build the character (never inside the host's armor).
    uint32_t    s_inheritedFrame = 0;
    double      s_paExitStarted = 0.0;
    bool        s_characterCreated = false;   // this session's character went through the editor (or was restored)
    // Object mods to put back once the restored items have landed (AddItem is
    // queued on the Papyrus VM, so they are attached a moment later).
    std::vector<ModdedItem> s_modQueue;
    ULONGLONG               s_modAtMs = 0;

    // ── Character editor staging ────────────────────────────────────────────
    // The editor runs somewhere quiet, in third person, with the camera
    // swung round in front of the face and zoomed in (the LooksMenu itself
    // never moves the camera -- see FrameFaceCamera).
    // Vault 111's cryo room (Fallout4.esm cell Vault111Cryo): standing in the
    // aisle in front of the player's own cryopod (MQPlayerCryopodREF 0x4A011
    // at 640,-288,-72; the spouse's pod faces it at 640,-736), facing the pod.
    constexpr UInt32 kEditorCellID  = 0x000016D8;
    constexpr float  kEditorPos[3]  = { 640.f, -512.f, -60.f };
    constexpr float  kEditorRotZ    = 0.f;

    // Third-person camera (IDA 1.11.240, ThirdPersonState vtable 0x142522A88):
    // PlayerCamera::cameraStates[8] = ThirdPersonState at +0x120 (ForceThirdPerson
    // switches to it). In the state: +0xC0 free-rotation yaw/pitch (absolute
    // while +0x128 is set -- it is initialised to the actor's heading),
    // +0xE8/+0xEC target/current zoom (clamped to fMinCurrentZoom..1.0).
    constexpr uintptr_t kCam_CurrentState  = 0x28;
    constexpr uintptr_t kCam_ThirdPerson   = 0x120;
    constexpr uintptr_t kTPS_FreeYaw       = 0xC0;
    constexpr uintptr_t kTPS_TargetZoom    = 0xE8;
    constexpr uintptr_t kTPS_CurrentZoom   = 0xEC;
    constexpr uintptr_t kTPS_FreeRotation  = 0x128;
    // Over-the-shoulder offset (IDA 1.11.240): +0x60 = target (x,y,z) set from
    // fOverShoulderPosX/PosZ by the ctor 0x102C7B0 and slot 17 0x102DAD0 (combat
    // variants), rewritten by the update slot 11 0x102CD50 on the weapon edge;
    // +0x6C = current offset (slots 14-16 copy/interpolate +0x60 into it). The
    // offset is in CAMERA space, so with the camera swung round to face the
    // character, its X pushes the character off-centre (the FaceGen bug).
    constexpr uintptr_t kTPS_TargetOffset  = 0x60;
    constexpr uintptr_t kTPS_CurrentOffset = 0x6C;
    float s_savedOffsetX[2] = { 0.f, 0.f };
    bool  s_offsetSaved = false;
    // 0.5.0 (FaceGen camera still off-centre after 0.3.1): the shoulder offset
    // above is not the only sideways term. ThirdPersonState's translation
    // (IDA 1.11.240: update slot 11 sub_14102CD50 -> sub_141030BF0) also adds
    // the skeleton CAMERA node's animated local translate (x, y) -- BGSBodyPart
    // kCamera (21) = "Camera3rd [Cam3]", stored at ThirdPersonState+0xD0 by
    // sub_14102D9F0 -- rotated by the ACTOR's heading, and adds that node's
    // rotation (+pi/2 yaw) to the camera, whenever the INI setting
    // bApplyCameraNodeAnimations:Camera is set (default 1; its value byte at
    // 0x2F395A0 is read directly every frame). Behind the player that is the
    // over-the-shoulder framing; once the editor swings the camera round to
    // face the character it shifts the whole view sideways. The editor
    // suspends the setting while it owns the camera and puts the player's
    // value back the moment it lets go (RestoreCamera / Reset).
    constexpr uintptr_t kRVA_ApplyCameraNodeAnims = 0x02F395A0;
    int s_savedCamNodeAnims = -1;   // -1: not overridden
    constexpr uintptr_t kRVA_MinCurrentZoom = 0x02F393C0;   // fMinCurrentZoom:Camera value
    constexpr float kFaceExtraZoom = 0.25f;   // how far past the normal minimum the editor camera goes
    float s_savedZoom = -1.f;

    enum class EditorStage { None, MoveAway, Arriving, ThirdPerson, Opening, Open, Returning };
    EditorStage    s_editorStage = EditorStage::None;
    double         s_editorAt    = 0.0;   // next step time / stage deadline
    double         s_editorSince = 0.0;
    TESObjectCELL* s_originCell  = nullptr;
    TESWorldSpace* s_originWorld = nullptr;
    NiPoint3       s_originPos;
    NiPoint3       s_originRot;
    bool           s_wasFirstPerson = false;

    const F4SESerializationInterface* s_ser = nullptr;

    // ── Actor values ────────────────────────────────────────────────────────
    // Looked up by their fixed Fallout4.esm record IDs (F4 Update/esm_avif_result.txt).
    // NOT by name: F4SE's ActorValueInfo::avName (+0x68) is not a string
    // pointer on 1.11.240 -- reading it crashed in _stricmp (2026-09-25).
    constexpr UInt32 kAV_Strength   = 0x000002C2;   // ..0x2C8 = S.P.E.C.I.A.L. in order
    constexpr UInt32 kAV_Experience = 0x000002C9;
    constexpr UInt32 kAV_Health     = 0x000002D4;

    ActorValueInfo* AVById(UInt32 formID)
    {
        TESForm* f = LookupFormByID(formID);
        if (!f || f->formType != kFormType_AVIF)
            return nullptr;
        return static_cast<ActorValueInfo*>(f);
    }

    TESNPC* PlayerNPC(PlayerCharacter* pc)
    {
        if (!pc || !pc->baseForm || pc->baseForm->formType != kFormType_NPC_) return nullptr;
        return static_cast<TESNPC*>(pc->baseForm);
    }

    // ── Papyrus helpers (queued on the VM, like the rest of CoSync) ─────────
    void PapyrusRemoveItem(TESObjectREFR* ref, TESForm* item, SInt32 count)
    {
        static const BSFixedString kFn("RemoveItem");
        VMArray<VMVariable> args;
        VMVariable a; CoSyncVM::SetFormArg(a, item); args.Push(&a);
        VMVariable b; b.Set(&count); args.Push(&b);
        bool silent = true; VMVariable c; c.Set(&silent); args.Push(&c);
        VMVariable d; CoSyncVM::SetFormArg(d, nullptr); args.Push(&d);
        CallFunctionNoWait(ref, kFn, args);
    }
    void PapyrusAddItem(TESObjectREFR* ref, TESForm* item, SInt32 count)
    {
        static const BSFixedString kFn("AddItem");
        VMArray<VMVariable> args;
        VMVariable a; CoSyncVM::SetFormArg(a, item); args.Push(&a);
        VMVariable b; b.Set(&count); args.Push(&b);
        bool silent = true; VMVariable c; c.Set(&silent); args.Push(&c);
        CallFunctionNoWait(ref, kFn, args);
    }
    void PapyrusEquipItem(Actor* actor, TESForm* item)
    {
        static const BSFixedString kFn("EquipItem");
        VMArray<VMVariable> args;
        VMVariable a; CoSyncVM::SetFormArg(a, item); args.Push(&a);
        bool preventRemoval = false; VMVariable b; b.Set(&preventRemoval); args.Push(&b);
        bool silent = true; VMVariable c; c.Set(&silent); args.Push(&c);
        CallFunctionNoWait(actor, kFn, args);
    }
    void PapyrusAttachMod(TESObjectREFR* ref, TESForm* item, TESForm* mod)
    {
        static const BSFixedString kFn("AttachModToInventoryItem");
        VMArray<VMVariable> args;
        VMVariable a; CoSyncVM::SetFormArg(a, item); args.Push(&a);
        VMVariable b; CoSyncVM::SetFormArg(b, mod); args.Push(&b);
        CallFunctionNoWait(ref, kFn, args);
    }
    void PapyrusRestoreValue(Actor* actor, ActorValueInfo* av, float amount)
    {
        static const BSFixedString kFn("RestoreValue");
        VMArray<VMVariable> args;
        VMVariable a; CoSyncVM::SetFormArg(a, av); args.Push(&a);
        VMVariable b; b.Set(&amount); args.Push(&b);
        CallFunctionNoWait(actor, kFn, args);
    }

    // ── Capture ─────────────────────────────────────────────────────────────
    // Object mods on one inventory stack (its BGSObjectInstanceExtra). Runs
    // inside CaptureInventory's SEH guard; kept separate because it builds
    // C++ objects, which a __try function may not.
    __declspec(noinline) void CaptureStackMods(TESForm* form, BGSInventoryItem::Stack* s, bool equipped,
                                               std::vector<ModdedItem>& out)
    {
        BSExtraData* x = s->extraData ? s->extraData->GetByType(kExtraData_ObjectInstance) : nullptr;
        auto* oi = static_cast<BGSObjectInstanceExtra*>(x);
        if (!oi || !oi->data || !oi->data->forms)
            return;
        ModdedItem m;
        m.base = form->formID;
        m.equipped = equipped;
        const UInt32 n = oi->data->blockSize / sizeof(BGSObjectInstanceExtra::Data::Form);
        for (UInt32 k = 0; k < n && k < 64; ++k)
            if (oi->data->forms[k].formId)
                m.mods.push_back(oi->data->forms[k].formId);
        if (!m.mods.empty())
            out.push_back(std::move(m));
    }

    __declspec(noinline) void CaptureInventory(PlayerCharacter* pc, Profile& p)
    {
        __try
        {
            BGSInventoryList* inv = pc->inventoryList;
            if (!inv) return;
            for (UInt32 i = 0; i < inv->items.count; ++i)
            {
                BGSInventoryItem& item = inv->items[i];
                if (!item.form) continue;
                SInt32 total = 0;
                bool equipped = false;
                for (BGSInventoryItem::Stack* s = item.stack; s; s = s->next)
                {
                    total += s->count;
                    const bool stackEquipped = (s->flags & BGSInventoryItem::Stack::kFlagEquipped) != 0;
                    if (stackEquipped)
                        equipped = true;
                    CaptureStackMods(item.form, s, stackEquipped, p.modded);
                }
                if (total <= 0) continue;
                p.items.push_back({ item.form->formID, total });
                if (equipped)
                    p.equipped.push_back(item.form->formID);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[Profiles] Inventory read faulted -- partial inventory captured");
        }
    }

    __declspec(noinline) void CaptureLooks(TESNPC* npc, Profile& p)
    {
        __try
        {
            p.female = (npc->actorData.flags & TESActorBaseData::kFlagFemale) ? 1 : 0;
            p.race   = npc->race.race ? npc->race.race->formID : 0;
            p.weights[0] = npc->weightThin; p.weights[1] = npc->weightMuscular; p.weights[2] = npc->weightLarge;
            p.skin = (UInt32(npc->skinColor.red) << 24) | (UInt32(npc->skinColor.green) << 16) |
                     (UInt32(npc->skinColor.blue) << 8) | UInt32(npc->skinColor.alpha);
            if (npc->headData)
            {
                p.hairColor    = npc->headData->hairColor ? npc->headData->hairColor->formID : 0;
                p.faceTextures = npc->headData->faceTextures ? npc->headData->faceTextures->formID : 0;
            }
            if (npc->headParts)
                for (UInt32 i = 0; i < npc->numHeadParts; ++i)
                    if (npc->headParts[i]) p.headParts.push_back(npc->headParts[i]->formID);
            if (npc->morphSetValue)
                for (UInt32 i = 0; i < npc->morphSetValue->count; ++i)
                    p.morphValues.push_back((*npc->morphSetValue)[i]);
            if (npc->morphSetData)
                npc->morphSetData->ForEach([&p](TESNPC::MorphSetData* d) { p.morphSets.push_back({ d->key, d->value }); return true; });
            if (npc->morphRegionData)
                npc->morphRegionData->ForEach([&p](TESNPC::FaceMorphRegion* r)
                {
                    RegionData rd; rd.index = r->index;
                    for (int i = 0; i < 8; ++i) rd.v[i] = r->value[i];
                    p.regions.push_back(rd);
                    return true;
                });
            if (npc->tints)
                for (UInt32 i = 0; i < npc->tints->count; ++i)
                {
                    BGSCharacterTint::Entry* e = (*npc->tints)[i];
                    if (!e) continue;
                    TintData t;
                    t.type = e->GetType();
                    t.index = e->tintIndex;
                    t.percent = e->percent;
                    if (t.type == BGSCharacterTint::Entry::kTypePalette)
                    {
                        auto* pe = static_cast<BGSCharacterTint::PaletteEntry*>(e);
                        t.bgra = pe->color.bgra;
                        t.colorID = pe->colorID;
                    }
                    p.tints.push_back(t);
                }
            p.hasLooks = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            p.hasLooks = false;
            LOG_WARN("[Profiles] Appearance read faulted -- appearance not captured this time");
        }
    }

    __declspec(noinline) void CapturePosition(PlayerCharacter* pc, Profile& p)
    {
        // Not while the character editor has them in the Vault 111 room.
        if (s_editorStage != EditorStage::None)
            return;
        __try
        {
            TESObjectREFR* ref = reinterpret_cast<TESObjectREFR*>(pc);
            TESWorldSpace* ws = CALL_MEMBER_FN(ref, GetWorldspace)();
            TESObjectCELL* cell = ref->parentCell;
            const UInt32 space = ws ? reinterpret_cast<TESForm*>(ws)->formID : (cell ? cell->formID : 0);
            if (!space || (space >> 24) == 0xFF)
                return;
            p.hasPos = true;
            p.posInterior = (ws == nullptr);
            p.posSpace = space;
            p.pos[0] = ref->pos.x; p.pos[1] = ref->pos.y; p.pos[2] = ref->pos.z;
            p.rotZ = ref->rot.z;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            p.hasPos = false;
        }
    }

    // Puts a returning player back where they last stood (their co-op
    // position, not the host's). Returns false when the place isn't valid.
    bool RestorePosition(PlayerCharacter* pc, const Profile& p)
    {
        if (!p.hasPos)
            return false;
        TESForm* f = LookupFormByID(p.posSpace);
        TESObjectCELL* cell = nullptr;
        TESWorldSpace* ws = nullptr;
        if (p.posInterior && f && f->formType == kFormType_CELL)
            cell = static_cast<TESObjectCELL*>(f);
        else if (!p.posInterior && f && f->formType == kFormType_WRLD)
            ws = reinterpret_cast<TESWorldSpace*>(f);   // F4SE only forward-declares it; TESForm is its base
        if (!cell && !ws)
        {
            LOG_WARN("[Profiles] Saved position space 0x%08X is not a %s -- staying at the host's spot",
                p.posSpace, p.posInterior ? "cell" : "worldspace");
            return false;
        }
        NiPoint3 pos(p.pos[0], p.pos[1], p.pos[2]);
        NiPoint3 rot(0.f, 0.f, p.rotZ);
        UInt32 nullHandle = 0;
        LOG_INFO("[Profiles] Returning the player to where they left (%s 0x%08X at %.0f,%.0f,%.0f)",
            p.posInterior ? "cell" : "worldspace", p.posSpace, p.pos[0], p.pos[1], p.pos[2]);
        MoveRefrToPosition(reinterpret_cast<TESObjectREFR*>(pc), &nullHandle, cell, ws, &pos, &rot);
        return true;
    }

    __declspec(noinline) const char* ReadCharacterNameImpl(TESNPC* npc)
    {
        __try { return npc ? npc->fullName.name.c_str() : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    std::string ReadCharacterName(TESNPC* npc)
    {
        const char* n = ReadCharacterNameImpl(npc);
        return (n && n[0]) ? std::string(n) : std::string();
    }

    bool Capture(Profile& p)
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        TESNPC* npc = PlayerNPC(pc);
        if (!pc || !npc || !EngineOk())
            return false;

        p.name = CoSyncNet::GetMyName();
        p.charName = ReadCharacterName(npc);
        p.level = CALL_MEMBER_FN(&npc->actorData, GetLevel)();
        if (ActorValueInfo* xpAV = AVById(kAV_Experience))
            CoSyncAVSafety::SafeGetValue(pc, xpAV, p.xp);
        for (int i = 0; i < 7; ++i)
            if (ActorValueInfo* av = AVById(kAV_Strength + i))
                CoSyncAVSafety::SafeGetBase(pc, av, p.special[i]);

        DataHandler* dh = *g_dataHandler;
        const auto getRank = Fn<GetPerkRank_t>(kRVA_GetPerkRank);
        for (UInt32 i = 0; i < dh->arrPERK.count; ++i)
        {
            BGSPerk* perk = dh->arrPERK[i];
            if (perk && perk->playable && !perk->hidden && getRank(pc, perk) > 0)
                p.perks.push_back(perk->formID);
        }

        CaptureInventory(pc, p);
        CaptureLooks(npc, p);
        p.created = s_characterCreated;
        CapturePosition(pc, p);
        for (uint32_t b : CoSyncCompanions::GetMyCompanionBases()) p.companions.push_back(b);
        return true;
    }

    // ── Apply ───────────────────────────────────────────────────────────────
    // Writes a stored appearance onto an NPC base record (face, head parts,
    // morphs, tints, body weights, skin). Does not touch sex/race and does not
    // rebuild any 3D -- callers do that.
    __declspec(noinline) bool WriteLooks(TESNPC* npc, const Profile& p)
    {
        __try
        {
            npc->weightThin = p.weights[0]; npc->weightMuscular = p.weights[1]; npc->weightLarge = p.weights[2];
            npc->skinColor.red   = UInt8(p.skin >> 24);
            npc->skinColor.green = UInt8(p.skin >> 16);
            npc->skinColor.blue  = UInt8(p.skin >> 8);
            npc->skinColor.alpha = UInt8(p.skin);
            if (npc->headData)
            {
                if (TESForm* f = LookupFormByID(p.hairColor))    npc->headData->hairColor = static_cast<BGSColorForm*>(f);
                if (TESForm* f = LookupFormByID(p.faceTextures)) npc->headData->faceTextures = static_cast<BGSTextureSet*>(f);
            }
            for (UInt32 id : p.headParts)
                if (TESForm* f = LookupFormByID(id))
                    if (f->formType == kFormType_HDPT)
                        npc->ChangeHeadPart(static_cast<BGSHeadPart*>(f), false, false);

            if (npc->morphSetValue && npc->morphSetValue->count == p.morphValues.size())
                for (UInt32 i = 0; i < npc->morphSetValue->count; ++i)
                    npc->morphSetValue->entries[i] = p.morphValues[i];

            if (npc->morphSetData)
            {
                npc->morphSetData->Clear();
                for (const auto& ms : p.morphSets)
                {
                    TESNPC::MorphSetData d; d.key = ms.first; d.value = ms.second;
                    npc->morphSetData->Add(&d);
                }
            }
            if (npc->morphRegionData)
            {
                npc->morphRegionData->Clear();
                for (const RegionData& r : p.regions)
                {
                    TESNPC::FaceMorphRegion d; d.index = r.index;
                    for (int i = 0; i < 8; ++i) d.value[i] = r.v[i];
                    npc->morphRegionData->Add(&d);
                }
            }
            if (npc->tints)
            {
                Fn<ClearTints_t>(kRVA_ClearTints)(npc->tints);
                const auto create = Fn<CreateTintEntry_t>(kRVA_CreateTintEntry);
                for (const TintData& t : p.tints)
                {
                    BGSCharacterTint::Entry* e = create((UInt32(t.index) << 16) | (t.type & 0xFFFF));
                    if (!e) continue;
                    e->percent = t.percent;
                    if (t.type == BGSCharacterTint::Entry::kTypePalette)
                    {
                        auto* pe = static_cast<BGSCharacterTint::PaletteEntry*>(e);
                        pe->color.bgra = t.bgra;
                        pe->colorID = t.colorID;
                    }
                    npc->tints->Push(e);
                }
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Profiles] Appearance write faulted (npc 0x%08X)", npc ? npc->formID : 0);
            return false;
        }
    }

    __declspec(noinline) bool ApplyLooks(PlayerCharacter* pc, TESNPC* npc, const Profile& p)
    {
        __try
        {
            const UInt8 curFemale = (npc->actorData.flags & TESActorBaseData::kFlagFemale) ? 1 : 0;
            const UInt32 curRace = npc->race.race ? npc->race.race->formID : 0;
            if (curFemale != p.female || curRace != p.race)
            {
                // Sex/race are not safe to swap underneath a live character.
                // The editor rebuilds them properly -- ask the player to redo it.
                LOG_WARN("[Profiles] Stored appearance is a different sex/race than this world's player -- opening the editor");
                return false;
            }

            if (!WriteLooks(npc, p))
                return false;

            // Rebuild the head/face/skin from the new data.
            if (pc->middleProcess)
                Fn<DoUpdate3dModel_t>(kRVA_DoUpdate3dModel)(pc->middleProcess, pc,
                    Actor::AIProcess::RESET_HEAD | Actor::AIProcess::RESET_FACE | Actor::AIProcess::RESET_SKIN);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Profiles] Appearance apply faulted -- opening the editor instead");
            return false;
        }
    }

    // Replaces the inventory (the loaded world's player is the HOST's copy).
    // Gives a vanilla starting item back after the wipe with the engine's own
    // add + equip (the same path the Pip-Boy has always used), then checks it.
    void GiveStartingItem(PlayerCharacter* pc, UInt32 formID, const char* what)
    {
        TESObjectREFR* ref = reinterpret_cast<TESObjectREFR*>(pc);
        TESForm* form = LookupFormByID(formID);
        if (!form)
        {
            LOG_WARN("[Profiles] New Player Initialization: %s form 0x%08X not found", what, formID);
            return;
        }
        Profile before;
        CaptureInventory(pc, before);
        const bool had = std::find_if(before.items.begin(), before.items.end(),
            [formID](const std::pair<UInt32, SInt32>& it) { return it.first == formID; }) != before.items.end();
        if (!had && !CoSyncGameAPI::NativeAddItem(ref, form, 1))
            PapyrusAddItem(pc, form, 1);
        if (!CoSyncEquipHook::EquipItemDirect(reinterpret_cast<Actor*>(pc), form))
            PapyrusEquipItem(pc, form);
        Profile check;
        CaptureInventory(pc, check);
        const bool have = std::find_if(check.items.begin(), check.items.end(),
            [formID](const std::pair<UInt32, SInt32>& it) { return it.first == formID; }) != check.items.end();
        const bool worn = std::find(check.equipped.begin(), check.equipped.end(), formID) != check.equipped.end();
        if (have && worn)
            LOG_INFO("[Profiles] New Player Initialization: %s in the inventory and equipped", what);
        else
            LOG_WARN("[Profiles] New Player Initialization: %s check failed: in inventory=%d equipped=%d",
                what, have ? 1 : 0, worn ? 1 : 0);
    }

    void ReplaceInventory(PlayerCharacter* pc, const std::vector<std::pair<UInt32, SInt32>>& items,
                          const std::vector<UInt32>& equip, bool freshStart = false)
    {
        // Everything the loaded world gave this player (the HOST's gear) goes,
        // synchronously, in one engine call. Queued per-item Papyrus
        // RemoveItem calls (the old way) were partly lost across the
        // teleport into the editor room: 9 of the host's items survived.
        TESObjectREFR* ref = reinterpret_cast<TESObjectREFR*>(pc);
        // Mode 0, no target = the console "removeallitems" wipe
        // (0x14034DB50 on the inventory list): EVERYTHING goes, quest items
        // and keys included -- a joining player is a brand-new character, as
        // if starting a new save (user, 2026-09-26). Mode 3 kept those.
        Fn<RemoveAllItems_t>(kRVA_RemoveAllItems)(ref, nullptr, 0);
        Fn<PlayerInvChanged_t>(kRVA_PlayerInvChanged)(pc, ref);
        {
            Profile left;
            CaptureInventory(pc, left);
            bool hasPipBoy = false;
            for (const auto& it : left.items)
                if (it.first == kPipBoyFormID) hasPipBoy = true;
            LOG_INFO("[Profiles] Inventory Initialization: inventory wiped, %zu item(s) left%s",
                left.items.size(), hasPipBoy ? "" : " -- giving the Pip-Boy back");
            // RemoveAllItems mode 0 takes the Pip-Boy too. Give it back with
            // the engine's own add and equip: the queued Papyrus AddItem /
            // EquipItem form-argument calls used before silently never landed
            // (0.3.0 logs: every upload "0 items"; no Pip-Boy in game).
            if (TESForm* pip = LookupFormByID(kPipBoyFormID))
            {
                if (!hasPipBoy && !CoSyncGameAPI::NativeAddItem(ref, pip, 1))
                    PapyrusAddItem(pc, pip, 1);
                if (!CoSyncEquipHook::EquipItemDirect(reinterpret_cast<Actor*>(pc), pip))
                    PapyrusEquipItem(pc, pip);
                Profile check;
                CaptureInventory(pc, check);
                const bool have = std::find_if(check.items.begin(), check.items.end(),
                    [](const std::pair<UInt32, SInt32>& it) { return it.first == kPipBoyFormID; }) != check.items.end();
                const bool worn = std::find(check.equipped.begin(), check.equipped.end(), kPipBoyFormID) != check.equipped.end();
                if (have && worn)
                    LOG_INFO("[Profiles] Pip-Boy in the inventory and equipped");
                else
                    LOG_WARN("[Profiles] Pip-Boy check failed: in inventory=%d equipped=%d", have ? 1 : 0, worn ? 1 : 0);
            }
        }
        // 0.3.6: a brand-new character also starts in the Vault 111 jumpsuit,
        // like a vanilla new game. RemoveAllItems mode 0 took it with
        // everything else and only the Pip-Boy was given back, so every fresh
        // player walked out of the face editor in their underwear.
        if (freshStart)
            GiveStartingItem(pc, kVaultSuitFormID, "Vault 111 jumpsuit");
        for (const auto& it : items)
        {
            if (it.first == kPipBoyFormID) continue;
            if (TESForm* f = LookupFormByID(it.first))
                if (!CoSyncGameAPI::NativeAddItem(ref, f, static_cast<UInt32>(it.second)))   // synchronous
                    PapyrusAddItem(pc, f, it.second);
        }
        for (UInt32 id : equip)
            if (TESForm* f = LookupFormByID(id))
                PapyrusEquipItem(pc, f);
    }

    void SetPerks(PlayerCharacter* pc, const std::vector<UInt32>& wanted)
    {
        DataHandler* dh = *g_dataHandler;
        const auto getRank = Fn<GetPerkRank_t>(kRVA_GetPerkRank);
        const auto removePerk = Fn<RemovePerk_t>(kRVA_RemovePerk);
        const auto addPerk = Fn<AddPerk_t>(kRVA_AddPerk);
        for (UInt32 i = 0; i < dh->arrPERK.count; ++i)
        {
            BGSPerk* perk = dh->arrPERK[i];
            if (!perk || !perk->playable || perk->hidden) continue;   // never touch system perks
            const bool want = std::find(wanted.begin(), wanted.end(), perk->formID) != wanted.end();
            const bool have = getRank(pc, perk) > 0;
            if (have && !want) removePerk(pc, perk);
            if (!have && want) addPerk(pc, perk, 0);
        }
    }

    void SetStats(PlayerCharacter* pc, UInt32 level, float xp, const float special[7])
    {
        Fn<SetPlayerLevel_t>(kRVA_SetPlayerLevel)(pc, level);
        if (ActorValueInfo* xpAV = AVById(kAV_Experience))
            CoSyncAVSafety::SafeSetBase(pc, xpAV, xp);
        for (int i = 0; i < 7; ++i)
            if (ActorValueInfo* av = AVById(kAV_Strength + i))
                CoSyncAVSafety::SafeSetBase(pc, av, special[i]);
    }

    void HealFully(PlayerCharacter* pc)
    {
        if (ActorValueInfo* hp = AVById(kAV_Health))
            PapyrusRestoreValue(pc, hp, 100000.f);
    }

    void ApplyFresh(PlayerCharacter* pc)
    {
        LOG_INFO("[Profiles] No saved character in this world -- creating a fresh one");
        s_status = "CREATING";
        const float special[7] = { kFreshSpecial, kFreshSpecial, kFreshSpecial, kFreshSpecial,
                                   kFreshSpecial, kFreshSpecial, kFreshSpecial };
        SetStats(pc, 1, 0.f, special);
        SetPerks(pc, {});
        // A brand-new character owns nothing: the world is a copy of the
        // host's save, so this strips the host's gear from the copy. The
        // Pip-Boy is given back and equipped by ReplaceInventory -- without it
        // the game has no inventory, map or data screens at all.
        ReplaceInventory(pc, {}, {}, /*freshStart*/ true);
        s_freshStarterCheck = true;
        HealFully(pc);
        s_characterCreated = false;
        // Character creation: the vanilla face/body editor, full mode.
        s_openLooksMenu = true;
    }

    void ApplyProfile(PlayerCharacter* pc, const Profile& p)
    {
        LOG_INFO("[Profiles] Restoring '%s': level %u, %zu perks, %zu items, looks=%d",
            p.name.c_str(), p.level, p.perks.size(), p.items.size(), p.hasLooks ? 1 : 0);
        s_status = "RESTORING";
        SetStats(pc, p.level, p.xp, p.special);
        SetPerks(pc, p.perks);
        ReplaceInventory(pc, p.items, p.equipped);
        // Mods attach to the FIRST copy of an item (AttachModToInventoryItem),
        // so one modded instance per base item -- the equipped one if any.
        s_modQueue.clear();
        for (const ModdedItem& m : p.modded)
        {
            auto it = std::find_if(s_modQueue.begin(), s_modQueue.end(),
                [&m](const ModdedItem& q) { return q.base == m.base; });
            if (it == s_modQueue.end())
                s_modQueue.push_back(m);
            else if (m.equipped && !it->equipped)
                *it = m;
        }
        s_modAtMs = GetTickCount64() + 2500;
        HealFully(pc);
        s_characterCreated = true;
        // 0.5.1: the character keeps the name it was given (a display
        // property on the player's own record; identity stays the key).
        if (!p.charName.empty())
            if (TESNPC* named = PlayerNPC(pc))
            {
                named->fullName.name = BSFixedString(p.charName.c_str());
                LOG_INFO("[Profiles] Character name restored: '%s'", p.charName.c_str());
            }
        if (RestorePosition(pc, p))
            CoSyncReviveSync::SetRespawnPoint(p.posInterior, p.posSpace, p.pos, p.rotZ, "returned to the saved position");
        TESNPC* npc = PlayerNPC(pc);
        if (!p.hasLooks || !npc || !ApplyLooks(pc, npc, p))
            s_openLooksMenu = true;   // appearance could not be restored: let them redo it
    }

    // ── Character editor staging (see s_editorStage) ─────────────────────────
    bool IsFirstPerson()
    {
        const auto* cam = *reinterpret_cast<const uint8_t* const*>(Base() + kRVA_PlayerCamera);
        if (!cam)
            return false;
        const void* cur   = *reinterpret_cast<const void* const*>(cam + 0x28);
        const void* first = *reinterpret_cast<const void* const*>(cam + 0xE0);
        return cur && cur == first;
    }

    // The third-person camera state, only while it is the active one.
    uint8_t* ActiveThirdPersonState()
    {
        auto* cam = *reinterpret_cast<uint8_t* const*>(Base() + kRVA_PlayerCamera);
        if (!cam)
            return nullptr;
        auto* cur = *reinterpret_cast<uint8_t* const*>(cam + kCam_CurrentState);
        auto* tps = *reinterpret_cast<uint8_t* const*>(cam + kCam_ThirdPerson);
        return (tps && cur == tps) ? tps : nullptr;
    }

    void SuspendCameraNodeAnimations()
    {
        auto* v = reinterpret_cast<uint8_t*>(Base() + kRVA_ApplyCameraNodeAnims);
        if (s_savedCamNodeAnims < 0)
        {
            s_savedCamNodeAnims = *v;
            LOG_INFO("[Profiles] Editor camera: camera-node animation suspended (was %d)", s_savedCamNodeAnims);
        }
        *v = 0;
    }

    void RestoreCameraNodeAnimations()
    {
        if (s_savedCamNodeAnims < 0)
            return;
        *reinterpret_cast<uint8_t*>(Base() + kRVA_ApplyCameraNodeAnims) = static_cast<uint8_t>(s_savedCamNodeAnims);
        LOG_INFO("[Profiles] Editor camera: camera-node animation restored (%d)", s_savedCamNodeAnims);
        s_savedCamNodeAnims = -1;
    }

    // Holds the shoulder offset's sideways component at 0 and keeps the
    // skeleton camera node out of the camera position so the character is
    // centred (the engine may rewrite +0x60, so this runs every editor tick).
    // The originals are restored by RestoreCamera.
    void CentreCamera()
    {
        SuspendCameraNodeAnimations();
        uint8_t* tps = ActiveThirdPersonState();
        if (!tps)
            return;
        float* target  = reinterpret_cast<float*>(tps + kTPS_TargetOffset);
        float* current = reinterpret_cast<float*>(tps + kTPS_CurrentOffset);
        if (!s_offsetSaved)
        {
            s_savedOffsetX[0] = target[0];
            s_savedOffsetX[1] = current[0];
            s_offsetSaved = true;
        }
        target[0] = 0.f;
        current[0] = 0.f;
    }

    // Swings the third-person camera round to the front of the character and
    // zooms in to the head -- what a mirror would show. Free rotation keeps
    // the camera there instead of snapping back behind the player.
    void FrameFaceCamera(PlayerCharacter* pc)
    {
        uint8_t* tps = ActiveThirdPersonState();
        if (!tps)
            return;
        auto* zoom = reinterpret_cast<float*>(tps + kTPS_TargetZoom);
        if (s_savedZoom < 0.f)
            s_savedZoom = *zoom;
        float yaw = reinterpret_cast<TESObjectREFR*>(pc)->rot.z + 3.14159265f;
        if (yaw > 6.28318531f)
            yaw -= 6.28318531f;
        // Closer than the normal minimum (user test 2026-09-26: "zoom in a bit
        // more"). The engine only clamps the zoom inside its mouse-zoom input
        // branch (0x14102CD50), which the LooksMenu never runs, so a value
        // below fMinCurrentZoom holds while the editor is open.
        const float minZoom = *reinterpret_cast<const float*>(Base() + kRVA_MinCurrentZoom);
        const float faceZoom = minZoom - kFaceExtraZoom;
        *reinterpret_cast<uint8_t*>(tps + kTPS_FreeRotation) = 1;
        *reinterpret_cast<float*>(tps + kTPS_FreeYaw) = yaw;
        *zoom = faceZoom;
        *reinterpret_cast<float*>(tps + kTPS_CurrentZoom) = faceZoom;
        CentreCamera();
    }

    // Puts the camera back behind the player at their old zoom.
    void RestoreCamera(PlayerCharacter* pc)
    {
        uint8_t* tps = ActiveThirdPersonState();
        if (tps)
        {
            *reinterpret_cast<float*>(tps + kTPS_FreeYaw) = reinterpret_cast<TESObjectREFR*>(pc)->rot.z;
            if (s_savedZoom >= 0.f)
                *reinterpret_cast<float*>(tps + kTPS_TargetZoom) = s_savedZoom;
            if (s_offsetSaved)
            {
                reinterpret_cast<float*>(tps + kTPS_TargetOffset)[0]  = s_savedOffsetX[0];
                reinterpret_cast<float*>(tps + kTPS_CurrentOffset)[0] = s_savedOffsetX[1];
            }
        }
        s_savedZoom = -1.f;
        s_offsetSaved = false;
        RestoreCameraNodeAnimations();
    }

    // Where to put the player back: beside the host's proxy when it is in the
    // world, otherwise where they stood before (the host's spot in the save).
    void ReturnDestination(TESObjectCELL*& cell, TESWorldSpace*& ws, NiPoint3& pos, NiPoint3& rot)
    {
        cell = s_originCell; ws = s_originWorld; pos = s_originPos; rot = s_originRot;
        for (const CoSyncNet::RosterEntry& r : CoSyncNet::GetRoster())
        {
            if (!r.isHost || r.isLocal)
                continue;
            const CoSyncPlayer* host = g_CoSyncPlayerManager.GetPlayerByEntityID(r.entityID);
            TESObjectREFR* ref = host ? reinterpret_cast<TESObjectREFR*>(host->actorRef) : nullptr;
            if (!ref)
                break;
            // The host's REAL location from its WorldSync reports -- NOT the
            // proxy's own cell: WorldSync rebuilds proxies into the local
            // player's cell, so while we were in the editor room the host's
            // proxy lived "in Vault 111" too, and returning there stranded
            // the player in the vault (user test 2026-09-26).
            TESWorldSpace* hws = nullptr;
            TESObjectCELL* hcell = nullptr;
            if (host->lastRemoteWorldspaceID == 0xFFFFFFFF)
                break;   // no WorldSync report yet
            if (host->lastRemoteWorldspaceID)
            {
                TESForm* f = LookupFormByID(host->lastRemoteWorldspaceID);
                if (f && f->formType == kFormType_WRLD)
                    hws = reinterpret_cast<TESWorldSpace*>(f);
            }
            else if (host->lastRemoteCellFormID && host->lastRemoteCellFormID != kEditorCellID)
            {
                TESForm* f = LookupFormByID(host->lastRemoteCellFormID);
                if (f && f->formType == kFormType_CELL)
                    hcell = static_cast<TESObjectCELL*>(f);
            }
            if (!hws && !hcell)
                break;   // host location unknown yet: back to where we loaded in
            ws = hws; cell = hcell;
            pos = NiPoint3(ref->pos.x + 96.f, ref->pos.y, ref->pos.z);   // beside, not inside, the host
            rot = NiPoint3(0.f, 0.f, ref->rot.z);
            break;
        }
    }

    // One step of the editor flow. GAME THREAD.
    void EditorStep(PlayerCharacter* pc, double now)
    {
        TESObjectREFR* ref = reinterpret_cast<TESObjectREFR*>(pc);
        UInt32 nullHandle = 0;
        switch (s_editorStage)
        {
        case EditorStage::MoveAway:
        {
            if (now < s_editorAt)
                return;
            s_originWorld = CALL_MEMBER_FN(ref, GetWorldspace)();
            s_originCell  = s_originWorld ? nullptr : ref->parentCell;
            s_originPos   = NiPoint3(ref->pos.x, ref->pos.y, ref->pos.z);
            s_originRot   = NiPoint3(0.f, 0.f, ref->rot.z);
            s_wasFirstPerson = IsFirstPerson();

            TESForm* f = LookupFormByID(kEditorCellID);
            TESObjectCELL* cell = (f && f->formType == kFormType_CELL) ? static_cast<TESObjectCELL*>(f) : nullptr;
            if (!cell)
            {
                // Should not happen with Fallout4.esm; still let them customize in place.
                LOG_WARN("[Profiles] Editor room (cell 0x%08X) not found -- opening the editor in place", kEditorCellID);
                s_editorStage = EditorStage::ThirdPerson;
                s_editorAt = now;
                return;
            }
            NiPoint3 pos(kEditorPos[0], kEditorPos[1], kEditorPos[2]);
            NiPoint3 rot(0.f, 0.f, kEditorRotZ);
            LOG_INFO("[Profiles] Moving the new character to their Vault 111 cryopod for the editor");
            MoveRefrToPosition(ref, &nullHandle, cell, nullptr, &pos, &rot);
            s_editorStage = EditorStage::Arriving;
            s_editorAt = now + 30.0;   // stop waiting for the room after 30 s
            return;
        }
        case EditorStage::Arriving:
        {
            const bool there   = ref->parentCell && ref->parentCell->formID == kEditorCellID;
            const bool settled = there && !CoSyncRuntime_IsMenuOpen("LoadingMenu") && CoSyncWorld::IsWorldReady();
            if (!settled && now < s_editorAt)
                return;
            if (!settled)
                LOG_WARN("[Profiles] Editor room did not finish loading in 30 s -- opening the editor anyway");
            s_editorStage = EditorStage::ThirdPerson;
            s_editorAt = now + 1.0;       // let the room and the player's 3D settle
            return;
        }
        case EditorStage::ThirdPerson:
            if (now < s_editorAt)
                return;
            Fn<ForcePerson_t>(kRVA_ForceThirdPerson)();
            s_editorStage = EditorStage::Opening;
            s_editorAt = now + 1.0;       // camera transition
            return;
        case EditorStage::Opening:
            // Keep framing the face through the camera transition.
            FrameFaceCamera(pc);
            if (now < s_editorAt)
                return;
            LOG_INFO("[Profiles] Opening the character editor");
            Fn<ShowLooksMenu_t>(kRVA_ShowLooksMenu)(nullptr, 1, nullptr, nullptr, nullptr);
            s_editorStage = EditorStage::Open;
            s_editorSince = now;
            s_editorAt = 0.0;             // set once the menu is seen open
            return;
        case EditorStage::Open:
        {
            if (CoSyncRuntime_IsMenuOpen("LooksMenu"))
            {
                // Hold the front view for the menu's first seconds (its
                // opening can still settle the camera); after that the player
                // is standing still in a menu and it stays put.
                if (now - s_editorSince < 3.0)
                    FrameFaceCamera(pc);
                else
                    CentreCamera();
                s_editorAt = now;         // seen open
                return;
            }
            // Closed after being open, or it never appeared within 10 s.
            if (s_editorAt == 0.0 && now - s_editorSince < 10.0)
            {
                FrameFaceCamera(pc);
                return;
            }
            if (s_editorAt == 0.0)
                LOG_WARN("[Profiles] The character editor did not open -- returning the player");
            else
                s_characterCreated = true;   // finished the editor: from now on this is a saved character
            RestoreCamera(pc);
            TESObjectCELL* cell; TESWorldSpace* ws; NiPoint3 pos, rot;
            ReturnDestination(cell, ws, pos, rot);
            if (cell || ws)
            {
                LOG_INFO("[Profiles] Character editor closed -- returning the player to the session");
                MoveRefrToPosition(ref, &nullHandle, cell, ws, &pos, &rot);
            }
            s_editorStage = EditorStage::Returning;
            s_editorAt = now + 30.0;
            return;
        }
        case EditorStage::Returning:
        {
            // (If the session itself is in Vault 111 the timeout ends this.)
            const bool away    = !(ref->parentCell && ref->parentCell->formID == kEditorCellID);
            const bool settled = away && !CoSyncRuntime_IsMenuOpen("LoadingMenu") && CoSyncWorld::IsWorldReady();
            if (!settled && now < s_editorAt)
                return;
            if (s_wasFirstPerson)
                Fn<ForcePerson_t>(kRVA_ForceFirstPerson)();
            s_editorStage = EditorStage::None;
            s_status = "READY";
            // 0.3.6: a new player leaves the editor wearing the Pip-Boy and the
            // Vault 111 jumpsuit (re-checked here: the editor's sex/race
            // rebuild and the teleports back are the last steps before play).
            if (s_freshStarterCheck)
            {
                s_freshStarterCheck = false;
                if (PlayerCharacter* pc = CoSyncGetPlayer())
                {
                    GiveStartingItem(pc, kPipBoyFormID, "Pip-Boy");
                    GiveStartingItem(pc, kVaultSuitFormID, "Vault 111 jumpsuit");
                }
            }
            CoSyncProfiles::ClientUploadNow();   // the host keeps the new look right away
            CoSyncProxyLooks::SendMine();        // and everyone sees it
            return;
        }
        default:
            return;
        }
    }

    // Runs fn(ctx) under SEH: any access violation inside the apply / capture
    // path is logged and skipped instead of taking the game down.
    __declspec(noinline) bool RunGuarded(void (*fn)(void*), void* ctx, const char* what)
    {
        __try
        {
            fn(ctx);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERROR("[Profiles] %s faulted -- skipped (game kept running)", what);
            return false;
        }
    }

    struct ApplyCtx { PlayerCharacter* pc; const Profile* profile; };
    void StripThunk(void* p)
    {
        auto* c = static_cast<ApplyCtx*>(p);
        ReplaceInventory(c->pc, {}, {});
        LOG_INFO("[Profiles] The host's inventory was removed as soon as the world loaded");
    }

    void ApplyFreshThunk(void* c)   { ApplyFresh(static_cast<ApplyCtx*>(c)->pc); }
    void ApplyProfileThunk(void* c) { auto* a = static_cast<ApplyCtx*>(c); ApplyProfile(a->pc, *a->profile); }

    struct CaptureCtx { Profile* out; bool ok; };
    void CaptureThunk(void* c) { auto* a = static_cast<CaptureCtx*>(c); a->ok = Capture(*a->out); }

    // ── F4SE co-save ────────────────────────────────────────────────────────
    void OnSave(const F4SESerializationInterface* ser)
    {
        {
            std::lock_guard<std::mutex> lk(s_mx);
            std::string blob;
            for (const auto& kv : s_profiles)
            {
                const uint32_t klen = static_cast<uint32_t>(kv.first.size());
                const uint32_t vlen = static_cast<uint32_t>(kv.second.size());
                blob.append(reinterpret_cast<const char*>(&klen), 4);
                blob.append(kv.first);
                blob.append(reinterpret_cast<const char*>(&vlen), 4);
                blob.append(kv.second);
            }
            const uint32_t count = static_cast<uint32_t>(s_profiles.size());
            ser->OpenRecord(kRecordType, kRecordVersion);
            ser->WriteRecordData(&count, 4);
            if (!blob.empty())
                ser->WriteRecordData(blob.data(), static_cast<UInt32>(blob.size()));
            LOG_INFO("[Profiles] Co-save: wrote %u player profile(s)", count);
        }

        // Other world state kept in the same co-save ('CSYN' has one set of
        // callbacks per plugin). Outside s_mx: those modules call back into
        // Profiles (HostGetConnKey) under their own locks.
        CoSyncTeams::SaveRecord(ser);
        CoSyncWorkshopAuthority::SaveRecord(ser);
    }

    void OnLoad(const F4SESerializationInterface* ser)
    {
        std::unique_lock<std::mutex> lk(s_mx);
        s_profiles.clear();
        UInt32 type = 0, version = 0, length = 0;
        while (ser->GetNextRecordInfo(&type, &version, &length))
        {
            // Other modules' records are read without s_mx held (see OnSave).
            lk.unlock();
            const bool other = CoSyncTeams::LoadRecord(ser, type, version, length) ||
                               CoSyncWorkshopAuthority::LoadRecord(ser, type, version, length);
            lk.lock();
            if (other)
                continue;
            if (type != kRecordType || version != kRecordVersion || length < 4)
                continue;
            std::string buf(length, '\0');
            if (ser->ReadRecordData(&buf[0], length) != length)
                break;
            uint32_t count = 0;
            std::memcpy(&count, buf.data(), 4);
            size_t pos = 4;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t klen = 0, vlen = 0;
                if (pos + 4 > buf.size()) break;
                std::memcpy(&klen, buf.data() + pos, 4); pos += 4;
                if (klen > 256 || pos + klen + 4 > buf.size()) break;
                std::string key = buf.substr(pos, klen); pos += klen;
                std::memcpy(&vlen, buf.data() + pos, 4); pos += 4;
                if (vlen > kMaxBlobBytes || pos + vlen > buf.size()) break;
                s_profiles[key] = buf.substr(pos, vlen); pos += vlen;
            }
        }
        LOG_INFO("[Profiles] Co-save: loaded %zu player profile(s)", s_profiles.size());

        // A client loading the host's world: its character comes from here.
        if (CoSyncWorldTransfer::GetClientPhase() == CoSyncWorldTransfer::ClientPhase::Loading)
        {
            s_applyState = ApplyState::Pending;
            s_readySince = 0.0;
        }
    }

    void OnRevert(const F4SESerializationInterface*)
    {
        CoSyncTeams::Revert();
        CoSyncWorkshopAuthority::Revert();
        std::lock_guard<std::mutex> lk(s_mx);
        s_profiles.clear();
    }
}

namespace CoSyncProfiles
{
    void RegisterSerialization(const F4SEInterface* f4se, uint32_t pluginHandle)
    {
        LOG_REQUIREMENT("CoSyncProfiles::RegisterSerialization", "Plugin load only.");
        s_ser = static_cast<const F4SESerializationInterface*>(f4se->QueryInterface(kInterface_Serialization));
        if (!s_ser)
        {
            LOG_ERROR("[Profiles] F4SE serialization interface unavailable -- profiles won't persist");
            return;
        }
        s_ser->SetUniqueID(pluginHandle, 'CSYN');
        s_ser->SetSaveCallback(pluginHandle, OnSave);
        s_ser->SetLoadCallback(pluginHandle, OnLoad);
        s_ser->SetRevertCallback(pluginHandle, OnRevert);
        LOG_INFO("[Profiles] Co-save callbacks registered");
    }

    std::string GetMyProfileKey()
    {
        std::string key;
        const uint64_t sid = CoSyncSteamID::GetSteamID64();
        if (sid != 0)
            key = std::to_string(sid);
        else
        {
            key = "name:" + std::string(CoSyncNet::GetMyName());
            for (char& c : key) if (c == '|' || c == '\n') c = '_';
        }
        // Second instance on the same account (test rig): distinct character.
        if (HMODULE proxy = GetModuleHandleA("nvToolsExt64_1.dll"))
            if (auto idx = reinterpret_cast<int (*)()>(GetProcAddress(proxy, "CoSyncMultiInstance_Index")))
                if (int n = idx(); n > 0)
                    key += "-P" + std::to_string(n + 1);
        return key;
    }

    void HostOnPlayerKey(uint32_t conn, const std::string& key, const std::string& name)
    {
        if (key.empty() || key.size() > 128)
            return;
        std::lock_guard<std::mutex> lk(s_mx);
        s_connKey[conn] = key;
        LOG_INFO("[Profiles] conn=%u is '%s' (key %s, %s)", conn, name.c_str(), key.c_str(),
            s_profiles.count(key) ? "returning" : "new");
    }

    std::string HostGetConnKey(uint32_t conn)
    {
        std::lock_guard<std::mutex> lk(s_mx);
        auto it = s_connKey.find(conn);
        return it == s_connKey.end() ? std::string() : it->second;
    }

    bool HostHasFinishedCharacter(const std::string& key)
    {
        std::string blob;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            auto it = s_profiles.find(key);
            if (it == s_profiles.end())
                return false;
            blob = it->second;
        }
        Profile p;
        return Deserialize(blob, p) && p.created;
    }

    void HostBeforeWorldSave()
    {
        std::lock_guard<std::mutex> lk(s_mx);
        LOG_INFO("[Profiles] Saving world with %zu stored character(s)", s_profiles.size());
    }

    bool IsProfileMessage(const std::string& msg)
    {
        return msg.rfind("PROFILEUP|", 0) == 0;
    }

    void OnReceive(const std::string& msg, uint32_t conn, bool isHostRole)
    {
        LOG_REQUIREMENT("CoSyncProfiles::OnReceive", "Main thread. Untrusted input: key must match the sender's connection.");
        if (!isHostRole || conn == 0 || msg.size() > kMaxBlobBytes)
            return;
        // PROFILEUP|<key>|<blob>
        const size_t a = msg.find('|');
        const size_t b = msg.find('|', a + 1);
        if (b == std::string::npos)
            return;
        const std::string key = msg.substr(a + 1, b - a - 1);
        const std::string blob = msg.substr(b + 1);

        std::lock_guard<std::mutex> lk(s_mx);
        auto it = s_connKey.find(conn);
        if (it == s_connKey.end() || it->second != key)
        {
            LOG_WARN("[Profiles] PROFILEUP key '%.40s' does not match conn=%u -- ignored", key.c_str(), conn);
            return;
        }
        Profile check;
        if (!Deserialize(blob, check))
            return;
        s_profiles[key] = blob;
    }

    void Tick(double now)
    {
        if (CoSyncNet::IsHost())
            return;

        // Apply once the host's world is up and the player is in it.
        if (s_applyState == ApplyState::Pending && CoSyncWorldTransfer::IsClientWorldReady() && CoSyncWorld::IsWorldReady())
        {
            if (s_readySince == 0.0)
                s_readySince = now;
            // The host's save may have the player in Power Armor: get out of it
            // before anything else (the pieces go back onto the frame).
            bool waitForPA = false;
            if (now - s_readySince >= 1.5)
            {
                if (CoSyncPowerArmorSync::IsLocalPlayerInPowerArmor())
                {
                    if (s_paExitStarted == 0.0)
                    {
                        s_paExitStarted = now;
                        s_inheritedFrame = CoSyncPowerArmorSync::ExitLocalPlayerPowerArmor();
                    }
                    waitForPA = (now - s_paExitStarted) < 10.0;
                    if (!waitForPA)
                        LOG_WARN("[Profiles] Still in Power Armor after 10 s -- building the character anyway");
                }
                else if (s_inheritedFrame)
                {
                    // Out: that frame is the host's (its proxy wears it) -- hide our copy.
                    TESForm* f = LookupFormByID(s_inheritedFrame);
                    if (f && f->formType == kFormType_REFR)
                        CoSyncGameAPI::DisableWorldActor(reinterpret_cast<TESObjectREFR*>(f));
                    LOG_INFO("[Profiles] Hid the host's Power Armor frame ref=0x%08X", s_inheritedFrame);
                    s_inheritedFrame = 0;
                }
            }
            if (now - s_readySince >= 1.5 && !waitForPA)   // let the player's 3D finish loading
            {
                PlayerCharacter* pc = CoSyncGetPlayer();
                if (pc && EngineOk())
                {
                    std::string blob;
                    {
                        std::lock_guard<std::mutex> lk(s_mx);
                        auto it = s_profiles.find(GetMyProfileKey());
                        if (it != s_profiles.end()) blob = it->second;
                    }
                    Profile p;
                    ApplyCtx ctx{ pc, &p };
                    const bool haveSaved = !blob.empty() && Deserialize(blob, p);
                    LOG_INFO("[Profiles] Player Join: key=%s save %s", GetMyProfileKey().c_str(),
                        !haveSaved ? "NOT FOUND in the host world (new player -> face generation)"
                                   : (p.created ? "FOUND in the host world (restoring)"
                                                : "found but unfinished (new player -> face generation)"));
                    if (haveSaved && p.created)
                        RunGuarded(ApplyProfileThunk, &ctx, "Restoring the character");
                    else
                    {
                        if (haveSaved)
                            LOG_INFO("[Profiles] The stored character was never finished in the editor -- starting over");
                        p = Profile();
                        RunGuarded(ApplyFreshThunk, &ctx, "Creating the fresh character");
                    }
                    // Host's followers back to the host; this player's own
                    // companions come back with the character.
                    CoSyncCompanions::ClientOnCharacterApplied(std::vector<uint32_t>(p.companions.begin(), p.companions.end()));
                    if (s_openLooksMenu)
                        s_looksMenuAt = now + 1.0;
                }
                s_applyState = ApplyState::Applied;
                s_lastUpload = now;
            }
        }

        if (s_openLooksMenu && s_looksMenuAt > 0.0 && now >= s_looksMenuAt)
        {
            s_openLooksMenu = false;
            s_looksMenuAt = 0.0;
            s_status = "CUSTOMIZING";
            LOG_INFO("[Profiles] Face Generation: opening the character editor for the new player");
            s_editorStage = EditorStage::MoveAway;
            s_editorAt = now;
        }
        if (s_editorStage != EditorStage::None)
        {
            if (PlayerCharacter* pc = CoSyncGetPlayer())
                EditorStep(pc, now);
        }

        // Weapon/armor mods of the restored character.
        if (!s_modQueue.empty() && GetTickCount64() >= s_modAtMs)
        {
            if (PlayerCharacter* pc = CoSyncGetPlayer())
            {
                size_t attached = 0;
                for (const ModdedItem& m : s_modQueue)
                {
                    TESForm* base = LookupFormByID(m.base);
                    if (!base)
                        continue;
                    for (UInt32 id : m.mods)
                        if (TESForm* mod = LookupFormByID(id))
                        {
                            PapyrusAttachMod(pc, base, mod);
                            ++attached;
                        }
                }
                LOG_INFO("[Profiles] Re-attached %zu mods on %zu items", attached, s_modQueue.size());
            }
            s_modQueue.clear();
        }

        if (s_applyState == ApplyState::Applied)
        {
            if (s_editorStage == EditorStage::None && (!*s_status || (now - s_lastUpload) > 3.0))
                s_status = "READY";
            // Not mid-edit: the half-made face isn't the character yet.
            if (s_editorStage == EditorStage::None && CoSyncNet::IsConnected() && now - s_lastUpload >= 30.0)
            {
                s_lastUpload = now;
                ClientUploadNow();
            }
        }
    }

    void ClientUploadNow()
    {
        if (CoSyncNet::IsHost() || s_applyState != ApplyState::Applied || !CoSyncTransport::IsInitialized())
            return;
        Profile p;
        CaptureCtx ctx{ &p, false };
        if (!RunGuarded(CaptureThunk, &ctx, "Capturing the character") || !ctx.ok)
            return;
        const std::string msg = "PROFILEUP|" + GetMyProfileKey() + "|" + Serialize(p);
        CoSyncTransport::Send(msg);
        LOG_INFO("[Profiles] Uploaded character (%zu bytes, level %u, %zu items)", msg.size(), p.level, p.items.size());
    }

    void ClientUploadIntroCharacter()
    {
        // 0.5.1 (Bug #4): the character this player just made in Fallout 4's
        // own intro -- face, body and sex from the mirror, SPECIAL and name
        // from the Vault-Tec rep, what they carried out of Vault 111 and where
        // they stand -- becomes their profile in the host's world. Marked
        // finished, so joining restores it instead of opening CoSync's editor.
        if (CoSyncNet::IsHost() || !CoSyncTransport::IsInitialized())
            return;
        Profile p;
        CaptureCtx ctx{ &p, false };
        if (!RunGuarded(CaptureThunk, &ctx, "Capturing the intro character") || !ctx.ok)
        {
            LOG_WARN("[Profiles] Could not capture the intro character -- the editor will open in the host's world");
            return;
        }
        p.created = true;
        s_characterCreated = true;
        const std::string msg = "PROFILEUP|" + GetMyProfileKey() + "|" + Serialize(p);
        CoSyncTransport::Send(msg);
        LOG_INFO("[Profiles] Uploaded the character from my own intro (%zu bytes, level %u, %zu items, looks=%d)",
            msg.size(), p.level, p.items.size(), p.hasLooks ? 1 : 0);
    }

    const char* GetClientStatus() { return s_status; }

    bool IsEditorBusy() { return s_editorStage != EditorStage::None || s_openLooksMenu; }

    void ClientStripInheritedInventory()
    {
        LOG_REQUIREMENT("CoSyncProfiles::ClientStripInheritedInventory", "Client, main thread, right after the host's world loaded.");
        if (CoSyncNet::IsHost() || !EngineOk())
            return;
        // In the host's Power Armor: the Tick path ejects first and strips
        // once out (wiping worn pieces mid-armor breaks the frame).
        if (CoSyncPowerArmorSync::IsLocalPlayerInPowerArmor())
            return;
        PlayerCharacter* pc = CoSyncGetPlayer();
        if (!pc)
            return;
        ApplyCtx ctx{ pc, nullptr };
        RunGuarded(StripThunk, &ctx, "Removing the host's inventory");
    }

    bool IsCharacterApplied()
    {
        return CoSyncNet::IsHost() || s_applyState == ApplyState::Applied;
    }

    std::string CaptureMyLooks()
    {
        PlayerCharacter* pc = CoSyncGetPlayer();
        TESNPC* npc = PlayerNPC(pc);
        if (!pc || !npc)
            return std::string();
        Profile p;
        CaptureLooks(npc, p);
        if (!p.hasLooks)
            return std::string();
        p.name = CoSyncNet::GetMyName();
        return Serialize(p);
    }

    bool WriteLooksToBase(TESNPC* base, const std::string& blob, bool& sexChanged)
    {
        sexChanged = false;
        if (!base || !EngineOk())
            return false;
        Profile p;
        if (!Deserialize(blob, p) || !p.hasLooks)
            return false;
        const bool isFemale = (base->actorData.flags & TESActorBaseData::kFlagFemale) != 0;
        if (isFemale != (p.female != 0))
        {
            // A proxy base has no live "player" behind it: its sex can follow
            // the player's, then the proxy's 3D is reloaded (caller).
            uint32_t& fl = CoSyncGameAPI::ActorBaseFlags32(base);
            if (p.female) fl |= TESActorBaseData::kFlagFemale;
            else          fl &= ~static_cast<uint32_t>(TESActorBaseData::kFlagFemale);
            sexChanged = true;
        }
        return WriteLooks(base, p);
    }

    void RebuildFace(Actor* actor)
    {
        if (actor && actor->middleProcess && EngineOk())
            Fn<DoUpdate3dModel_t>(kRVA_DoUpdate3dModel)(actor->middleProcess, actor,
                Actor::AIProcess::RESET_HEAD | Actor::AIProcess::RESET_FACE | Actor::AIProcess::RESET_SKIN);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_connKey.clear();
        s_applyState = ApplyState::None;
        s_readySince = 0.0;
        s_status = "";
        s_openLooksMenu = false;
        s_looksMenuAt = 0.0;
        s_editorStage = EditorStage::None;
        s_editorAt = 0.0;
        s_savedZoom = -1.f;
        s_offsetSaved = false;
        RestoreCameraNodeAnimations();   // session ended inside the editor
        s_characterCreated = false;
        s_modQueue.clear();
        s_inheritedFrame = 0;
        s_paExitStarted = 0.0;
        s_originCell = nullptr;
        s_originWorld = nullptr;
    }
}
