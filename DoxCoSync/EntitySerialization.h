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

// ============================================================================
// ENTITY CREATE
//
// Format:
// EC|entityID|type|baseFormID|ownerEntityID|spawnFlags|px,py,pz|rx,ry,rz
//
// Rules:
//  • CREATE is the ONLY spawn trigger
//  • CREATE may arrive after UPDATE
//  • spawnFlags MUST be preserved exactly
// ============================================================================

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
        << p.spawnRot.x << "," << p.spawnRot.y << "," << p.spawnRot.z;

    return ss.str();
}

inline bool DeserializeEntityCreate(const std::string& msg, EntityCreatePacket& out)
{
    if (!IsEntityCreate(msg))
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;

    // entityID
    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;

    // type
    if (!std::getline(ss, tok, '|')) return false;
    out.type = static_cast<CoSyncEntityType>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.type != CoSyncEntityType::Player &&
        out.type != CoSyncEntityType::NPC)
        return false;

    // baseFormID
    if (!std::getline(ss, tok, '|')) return false;
    out.baseFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.baseFormID == 0) return false;

    // ownerEntityID
    if (!std::getline(ss, tok, '|')) return false;
    out.ownerEntityID = tok.empty()
        ? 0
        : static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    // spawnFlags
    if (!std::getline(ss, tok, '|')) return false;
    out.spawnFlags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    // spawnPos
    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.spawnPos.x,
        &out.spawnPos.y,
        &out.spawnPos.z) != 3)
        return false;

    // spawnRot
    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.spawnRot.x,
        &out.spawnRot.y,
        &out.spawnRot.z) != 3)
        return false;

    return true;
}

// ============================================================================
// ENTITY UPDATE
//
// Format:
// EU|entityID|flags|px,py,pz|rx,ry,rz|vx,vy,vz|timestamp
//
// Rules:
//  • UPDATE NEVER spawns
//  • UPDATE may arrive before CREATE
//  • timestamp is ALWAYS host-time
//    (client timestamps are ignored by host)
// ============================================================================

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
        << p.timestamp;

    return ss.str();
}

inline bool DeserializeEntityUpdate(const std::string& msg, EntityUpdatePacket& out)
{
    if (!IsEntityUpdate(msg))
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;

    // entityID
    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;

    // flags
    if (!std::getline(ss, tok, '|')) return false;
    out.flags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    // pos
    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.pos.x, &out.pos.y, &out.pos.z) != 3)
        return false;

    // rot
    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.rot.x, &out.rot.y, &out.rot.z) != 3)
        return false;

    // vel
    if (!std::getline(ss, tok, '|')) return false;
    if (std::sscanf(tok.c_str(), "%f,%f,%f",
        &out.vel.x, &out.vel.y, &out.vel.z) != 3)
        return false;

    // timestamp (host-time)
    if (std::getline(ss, tok, '|') && !tok.empty())
        out.timestamp = std::strtod(tok.c_str(), nullptr);
    else
        out.timestamp = 0.0;

    if (!std::isfinite(out.timestamp) || out.timestamp < 0.0)
        out.timestamp = 0.0;

    return true;
}

// ============================================================================
// ENTITY DESTROY
//
// Format:
// ED|entityID|reasonFlags
// ============================================================================

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

    // entityID
    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;

    // reasonFlags
    if (std::getline(ss, tok, '|') && !tok.empty())
        out.reasonFlags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    else
        out.reasonFlags = 0;

    return true;
}
