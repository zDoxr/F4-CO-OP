#pragma once

#include <string>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "Packets_EntityCreate.h"
#include "Packets_EntityUpdate.h"
#include "Packets_EntityDestroy.h"
#include "CoSyncMessageHelpers.h"



inline std::string SerializeEntityCreate(const EntityCreatePacket& p)
{
    std::ostringstream ss;
    ss << "EC|"
        << p.entityID << "|"
        << static_cast<uint32_t>(p.type) << "|"
        << p.baseFormID << "|"
        << p.ownerEntityID << "|"
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

    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;

    if (!std::getline(ss, tok, '|')) return false;
    out.type = static_cast<CoSyncEntityType>(std::strtoul(tok.c_str(), nullptr, 10));

    if (!std::getline(ss, tok, '|')) return false;
    out.baseFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.baseFormID == 0) return false;

    if (!std::getline(ss, tok, '|')) return false;
    out.ownerEntityID = tok.empty()
        ? 0
        : static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

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

    return true;
}

// ============================================================================
// ENTITY UPDATE
//
// Format:
// EU|entityID|flags|px,py,pz|rx,ry,rz|vx,vy,vz|timestamp
//
// Rules:
//  • UPDATE never spawns
//  • UPDATE may arrive before CREATE
//  • UPDATE must be extremely lightweight
// ============================================================================

inline std::string SerializeEntityUpdate(const EntityUpdatePacket& p)
{
    std::ostringstream ss;
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

    if (!std::getline(ss, tok, '|')) return false;
    out.entityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (out.entityID == 0) return false;

    if (std::getline(ss, tok, '|') && !tok.empty())
        out.reasonFlags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    else
        out.reasonFlags = 0;

    return true;
}
