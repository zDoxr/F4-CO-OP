#include "PlayerStatePacket.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"  // for GetMyName (optional fallback)

#include <sstream>
#include <vector>
#include <cstdlib>

// --------------------------------------------------------
// Helper: split string by a single character delimiter
// --------------------------------------------------------
static std::vector<std::string> Split(const std::string& s, char delim)
{
    std::vector<std::string> parts;
    std::string              tmp;
    std::istringstream       iss(s);

    while (std::getline(iss, tmp, delim))
        parts.push_back(tmp);

    return parts;
}

// --------------------------------------------------------
// IsPlayerStateString
// --------------------------------------------------------
bool IsPlayerStateString(const std::string& in)
{
    // Very simple: must start with "STATE:"
    if (in.size() < 6)
        return false;

    if (in.compare(0, 6, "STATE:") == 0)
        return true;

    // Some callers may prepend other tags; be strict for now
    return false;
}

// --------------------------------------------------------
// SerializePlayerStateToString
// --------------------------------------------------------
bool SerializePlayerStateToString(const LocalPlayerState& state, std::string& out)
{
    // Use username from state if present; otherwise fallback to CoSyncNet identity.
    std::string username = state.username;
    if (username.empty())
        username = CoSyncNet::GetMyName();

    std::ostringstream ss;

    // 0: header: STATE:formID,cellFormID
    ss << "STATE:"
        << state.formID << "," << state.cellFormID << "|";

    // 1: position
    ss << state.position.x << ","
        << state.position.y << ","
        << state.position.z << "|";

    // 2: rotation
    ss << state.rotation.x << ","
        << state.rotation.y << ","
        << state.rotation.z << "|";

    // 3: velocity
    ss << state.velocity.x << ","
        << state.velocity.y << ","
        << state.velocity.z << "|";

    // 4: health/maxHealth
    ss << state.health << ","
        << state.maxHealth << "|";

    // 5: AP / maxAP
    ss << state.ap << ","
        << state.maxActionPoints << "|";

    // 6: movement flags
    ss << (state.isMoving ? 1 : 0) << ","
        << (state.isSprinting ? 1 : 0) << ","
        << (state.isCrouching ? 1 : 0) << ","
        << (state.isJumping ? 1 : 0) << "|";

    // 7: equipped weapon
    ss << state.equippedWeaponFormID << "|";

    // 8: username (NEW 9th field)
    ss << username;

    out = ss.str();
    return true;
}

// --------------------------------------------------------
// DeserializePlayerStateFromString
// --------------------------------------------------------
bool DeserializePlayerStateFromString(const std::string& in, LocalPlayerState& out)
{
    if (!IsPlayerStateString(in))
        return false;

    // Split on '|'
    std::vector<std::string> fields = Split(in, '|');

    // Expect at least 8 fields (old) or 9 fields (new with username).
    if (fields.size() < 8)
    {
        LOG_ERROR("[PlayerState] BAD PACKET: expected >=8 fields, got %zu", fields.size());
        return false;
    }

    // ----------------------------------------------------
    // 0: "STATE:form,cell"
    // ----------------------------------------------------
    const std::string& header = fields[0];
    std::string        formCell = header.substr(6); // skip "STATE:"

    std::vector<std::string> fc = Split(formCell, ',');
    if (fc.size() != 2)
    {
        LOG_ERROR("[PlayerState] BAD HEADER: '%s'", header.c_str());
        return false;
    }

    out.formID = static_cast<UInt32>(std::strtoul(fc[0].c_str(), nullptr, 10));
    out.cellFormID = static_cast<UInt32>(std::strtoul(fc[1].c_str(), nullptr, 10));

    // ----------------------------------------------------
    // 1: position x,y,z
    // ----------------------------------------------------
    {
        std::vector<std::string> p = Split(fields[1], ',');
        if (p.size() != 3)
        {
            LOG_ERROR("[PlayerState] BAD POS: '%s'", fields[1].c_str());
            return false;
        }
        out.position.x = std::strtof(p[0].c_str(), nullptr);
        out.position.y = std::strtof(p[1].c_str(), nullptr);
        out.position.z = std::strtof(p[2].c_str(), nullptr);
    }

    // ----------------------------------------------------
    // 2: rotation x,y,z
    // ----------------------------------------------------
    {
        std::vector<std::string> r = Split(fields[2], ',');
        if (r.size() != 3)
        {
            LOG_ERROR("[PlayerState] BAD ROT: '%s'", fields[2].c_str());
            return false;
        }
        out.rotation.x = std::strtof(r[0].c_str(), nullptr);
        out.rotation.y = std::strtof(r[1].c_str(), nullptr);
        out.rotation.z = std::strtof(r[2].c_str(), nullptr);
    }

    // ----------------------------------------------------
    // 3: velocity x,y,z
    // ----------------------------------------------------
    {
        std::vector<std::string> v = Split(fields[3], ',');
        if (v.size() != 3)
        {
            LOG_ERROR("[PlayerState] BAD VEL: '%s'", fields[3].c_str());
            return false;
        }
        out.velocity.x = std::strtof(v[0].c_str(), nullptr);
        out.velocity.y = std::strtof(v[1].c_str(), nullptr);
        out.velocity.z = std::strtof(v[2].c_str(), nullptr);
    }

    // ----------------------------------------------------
    // 4: health,maxHealth
    // ----------------------------------------------------
    {
        std::vector<std::string> hp = Split(fields[4], ',');
        if (hp.size() == 2)
        {
            out.health = std::strtof(hp[0].c_str(), nullptr);
            out.maxHealth = std::strtof(hp[1].c_str(), nullptr);
        }
        else
        {
            out.health = out.maxHealth = 0.0f;
        }
    }

    // ----------------------------------------------------
    // 5: ap,maxAP
    // ----------------------------------------------------
    {
        std::vector<std::string> ap = Split(fields[5], ',');
        if (ap.size() == 2)
        {
            out.ap = std::strtof(ap[0].c_str(), nullptr);
            out.maxActionPoints = std::strtof(ap[1].c_str(), nullptr);
        }
        else
        {
            out.ap = out.maxActionPoints = 0.0f;
        }
    }

    // ----------------------------------------------------
    // 6: flags: isMoving,isSprinting,isCrouching,isJumping
    // ----------------------------------------------------
    {
        std::vector<std::string> f = Split(fields[6], ',');
        if (f.size() == 4)
        {
            out.isMoving = (std::strtol(f[0].c_str(), nullptr, 10) != 0);
            out.isSprinting = (std::strtol(f[1].c_str(), nullptr, 10) != 0);
            out.isCrouching = (std::strtol(f[2].c_str(), nullptr, 10) != 0);
            out.isJumping = (std::strtol(f[3].c_str(), nullptr, 10) != 0);
        }
        else
        {
            out.isMoving = out.isSprinting = out.isCrouching = out.isJumping = false;
        }
    }

    // ----------------------------------------------------
    // 7: equipped weapon formID
    // ----------------------------------------------------
    {
        out.equippedWeaponFormID =
            static_cast<UInt32>(std::strtoul(fields[7].c_str(), nullptr, 10));
    }

    // ----------------------------------------------------
    // 8: username (optional; may be missing on old packets)
    // ----------------------------------------------------
    if (fields.size() >= 9)
    {
        out.username = fields[8];
    }
    else
    {
        // Old packet: leave username unchanged
        if (out.username.empty())
            out.username = "Remote";
    }

    // timestamp is local-only; set to "now-ish" (monotonic-ish)
    out.timestamp = GetTickCount64() / 1000.0; // seconds

    return true;
}
