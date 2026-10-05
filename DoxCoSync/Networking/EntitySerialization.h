#pragma once

#include <string>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iomanip>

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "Packets_EntityDestroy.h"
#include "CoSyncMessageHelpers.h"

// P7 (2026-09-26): "%f" parsing accepts "nan"/"inf", and the engine stores
// positions with no validation of its own (IDA: TESObjectREFR::SetPosition
// 0x513DB0 compares and writes the floats as given). A non-finite transform
// from any peer would be written straight into an actor. Reject it here.
inline bool CoSyncIsFinite3(const NiPoint3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline std::string SerializeEntityCreate(const EntityCreatePacket& p)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3);

    ss << "EC|"
        << p.entityID << "|"
        << static_cast<uint32_t>(p.type) << "|"
        << p.baseFormID << "|"
        << p.ownerEntityID << "|"
        << p.spawnFlags << "|"
        << p.spawnPos.x << "," << p.spawnPos.y << "," << p.spawnPos.z << "|"
        << p.spawnRot.x << "," << p.spawnRot.y << "," << p.spawnRot.z << "|"
        << p.reserved0 << "|"
        << p.locWS << "|"
        << p.locCell << "|"
        << p.username;

    return ss.str();
}

inline bool DeserializeEntityCreate(const std::string& msg, EntityCreatePacket& out)
{
    if (!IsEntityCreate(msg))
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;


    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;


    if (!std::getline(ss, tok, '|')) return false;
    out.type = static_cast<CoSyncEntityType>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.type != CoSyncEntityType::Player &&
        out.type != CoSyncEntityType::NPC &&
        out.type != CoSyncEntityType::Companion)
        return false;


    if (!std::getline(ss, tok, '|')) return false;
    out.baseFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.baseFormID == 0 && out.type != CoSyncEntityType::Player) return false;


    if (!std::getline(ss, tok, '|')) return false;
    out.ownerEntityID = tok.empty()
        ? 0
        : static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));


    if (!std::getline(ss, tok, '|')) return false;
    out.spawnFlags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));


    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.spawnPos.x,
        &out.spawnPos.y,
        &out.spawnPos.z) != 3)
        return false;


    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.spawnRot.x,
        &out.spawnRot.y,
        &out.spawnRot.z) != 3)
        return false;


    if (!CoSyncIsFinite3(out.spawnPos) || !CoSyncIsFinite3(out.spawnRot))
        return false;

    // reserved0 — world NPC reference formID (or companion refFormID); optional
    // field added after rot. Old packets that don't include this field leave
    // reserved0 at its zero-init default, which the bind path treats as
    // "no direct-lookup hint available".
    {
        std::string r0tok;
        if (std::getline(ss, r0tok, '|') && !r0tok.empty())
            out.reserved0 = static_cast<uint32_t>(std::strtoul(r0tok.c_str(), nullptr, 10));
    }

    // Location (0.3.1). Always present from 0.3.1 peers (mismatched versions
    // refuse each other through VER/WORLDREQ).
    {
        std::string wtok, ctok;
        if (std::getline(ss, wtok, '|') && !wtok.empty())
            out.locWS = static_cast<uint32_t>(std::strtoul(wtok.c_str(), nullptr, 10));
        if (std::getline(ss, ctok, '|') && !ctok.empty())
            out.locCell = static_cast<uint32_t>(std::strtoul(ctok.c_str(), nullptr, 10));
    }

    {
        std::string uname;
        if (std::getline(ss, uname) && !uname.empty())
            out.username = uname;
    }

    return true;
}

inline std::string SerializeEntityUpdate(const EntityUpdatePacket& p)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3);

    ss << "EU|"
        << p.entityID << "|"
        << p.flags << "|"
        << p.pos.x << "," << p.pos.y << "," << p.pos.z << "|"
        << p.rot.x << "," << p.rot.y << "," << p.rot.z << "|"
        << p.vel.x << "," << p.vel.y << "," << p.vel.z << "|"
        << p.timestamp << "|"
        << (p.hasAnimVars ? 1 : 0);

    if (p.hasAnimVars)
    {
        const auto& av = p.animVars;


        ss << "|" << av.booleans;


        ss << "|";
        for (int i = 0; i < AnimVarSnapshot::NUM_FLOATS; ++i)
        {
            if (i > 0) ss << ",";
            ss << av.floats[i];
        }


        ss << "|";
        for (int i = 0; i < AnimVarSnapshot::NUM_INTS; ++i)
        {
            if (i > 0) ss << ",";
            ss << av.integers[i];
        }


        ss << "|" << av.actorStateFlags1
           << "|" << av.actorStateFlags2
           << "|" << av.direction;
    }


    ss << "|" << p.equippedWeaponFormID;


    ss << "|" << (p.isCombatActive ? 1 : 0)
       << "|" << p.combatTargetEntityID
       << "|" << (p.isInDialog ? 1 : 0)
       << "|" << p.equippedThrowableFormID
       << "|" << (p.isThrowing ? 1 : 0)
       << "|" << (p.isReloading ? 1 : 0)
       << "|" << (p.isFiring ? 1 : 0)
       << "|" << (p.hasMoveChannels ? 1 : 0)
       << "|" << p.moveSpeed
       << "|" << p.moveDirection
       << "|" << static_cast<int>(p.weaponDrawnState);

    return ss.str();
}

inline bool DeserializeEntityUpdate(const std::string& msg, EntityUpdatePacket& out)
{
    if (!IsEntityUpdate(msg))
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;


    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;


    if (!std::getline(ss, tok, '|')) return false;
    out.flags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));


    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.pos.x, &out.pos.y, &out.pos.z) != 3)
        return false;


    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.rot.x, &out.rot.y, &out.rot.z) != 3)
        return false;


    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.vel.x, &out.vel.y, &out.vel.z) != 3)
        return false;


    if (!CoSyncIsFinite3(out.pos) || !CoSyncIsFinite3(out.rot) || !CoSyncIsFinite3(out.vel))
        return false;

    if (std::getline(ss, tok, '|') && !tok.empty())
        out.timestamp = std::strtod(tok.c_str(), nullptr);
    else
        out.timestamp = 0.0;

    if (!std::isfinite(out.timestamp) || out.timestamp < 0.0)
        out.timestamp = 0.0;


    out.hasAnimVars = false;
    std::string hasAnimTok;
    if (std::getline(ss, hasAnimTok, '|') && hasAnimTok == "1")
    {

        if (!std::getline(ss, tok, '|')) return true;
        out.animVars.booleans = (uint64_t)std::strtoull(tok.c_str(), nullptr, 10);


        if (!std::getline(ss, tok, '|')) return true;
        {
            std::stringstream fss(tok);
            std::string ftok;
            int fi = 0;
            while (fi < AnimVarSnapshot::NUM_FLOATS && std::getline(fss, ftok, ','))
                out.animVars.floats[fi++] = std::strtof(ftok.c_str(), nullptr);
        }


        if (!std::getline(ss, tok, '|')) return true;
        {
            std::stringstream iss(tok);
            std::string itok;
            int ii = 0;
            while (ii < AnimVarSnapshot::NUM_INTS && std::getline(iss, itok, ','))
                out.animVars.integers[ii++] = (uint32_t)std::strtoul(itok.c_str(), nullptr, 10);
        }


        if (!std::getline(ss, tok, '|')) return true;
        out.animVars.actorStateFlags1 = (uint32_t)std::strtoul(tok.c_str(), nullptr, 10);


        if (!std::getline(ss, tok, '|')) return true;
        out.animVars.actorStateFlags2 = (uint32_t)std::strtoul(tok.c_str(), nullptr, 10);


        if (!std::getline(ss, tok, '|')) return true;
        out.animVars.direction = std::strtof(tok.c_str(), nullptr);

        out.animVars.isValid = true;
        out.hasAnimVars = true;
    }


    {
        std::string wtok;
        if (!std::getline(ss, wtok, '|')) return true;
        if (!wtok.empty())
            out.equippedWeaponFormID = (uint32_t)std::strtoul(wtok.c_str(), nullptr, 10);
    }


    {
        std::string ctok;
        if (!std::getline(ss, ctok, '|')) return true;
        out.isCombatActive = (ctok == "1");
    }


    {
        std::string ctok;
        if (!std::getline(ss, ctok, '|')) return true;
        out.combatTargetEntityID = (uint32_t)std::strtoul(ctok.c_str(), nullptr, 10);
    }


    // NOTE: this used to be a delimiter-less getline (i.e. "read the rest of
    // the line"), which silently swallowed any field appended after it. It
    // takes '|' now so isInDialog stops at its own field and the throwable
    // below can be parsed -- keep it that way when adding the next field.
    {
        std::string dtok;
        if (!std::getline(ss, dtok, '|')) return true;
        out.isInDialog = (dtok == "1");
    }

    {
        std::string ttok;
        if (!std::getline(ss, ttok, '|')) return true;
        if (!ttok.empty())
            out.equippedThrowableFormID = (uint32_t)std::strtoul(ttok.c_str(), nullptr, 10);
    }

    {
        std::string thtok;
        if (!std::getline(ss, thtok, '|')) return true;
        out.isThrowing = (thtok == "1");
    }

    {
        std::string rtok;
        if (!std::getline(ss, rtok, '|')) return true;
        out.isReloading = (rtok == "1");
    }

    {
        std::string ftok;
        if (!std::getline(ss, ftok, '|')) return true;
        out.isFiring = (ftok == "1");
    }

    // 0.4.3 locomotion channels (world NPCs). Optional: a field that is missing
    // or not finite leaves hasMoveChannels false. The three fields are always
    // present on the wire (hasMoveChannels 0 included), so the next field
    // follows them.
    {
        std::string htok, stok, dtok;
        if (!std::getline(ss, htok, '|')) return true;
        if (!std::getline(ss, stok, '|') || !std::getline(ss, dtok, '|')) return true;
        const float sp = std::strtof(stok.c_str(), nullptr);
        const float dr = std::strtof(dtok.c_str(), nullptr);
        if (htok == "1" && std::isfinite(sp) && std::isfinite(dr) && sp >= 0.f && sp < 100000.f)
        {
            out.hasMoveChannels = true;
            out.moveSpeed       = sp;
            out.moveDirection   = dr;
        }
    }

    // 0.5.0 weapon drawn state (-1 / 0 / 1). Missing = -1.
    {
        std::string wtok;
        if (!std::getline(ss, wtok, '|') || wtok.empty()) return true;
        const long v = std::strtol(wtok.c_str(), nullptr, 10);
        out.weaponDrawnState = (v == 0 || v == 1) ? static_cast<int8_t>(v) : static_cast<int8_t>(-1);
    }

    return true;
}

inline std::string SerializeEntityDestroy(const EntityDestroyPacket& p)
{
    std::ostringstream ss;
    ss << "ED|"
        << p.entityID << "|"
        << p.reasonFlags;
    return ss.str();
}

inline bool DeserializeEntityDestroy(const std::string& msg, EntityDestroyPacket& out)
{
    if (!IsEntityDestroy(msg))
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;


    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;


    if (std::getline(ss, tok, '|') && !tok.empty())
        out.reasonFlags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    else
        out.reasonFlags = 0;

    return true;
}
