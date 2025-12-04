#include "PlayerStatePacket.h"
#include "ConsoleLogger.h"
#include <sstream>

// ------------------------------------------------------------
// Utility: return true if text begins with "STATE:"
// ------------------------------------------------------------
bool IsPlayerStateString(const std::string& msg)
{
    return msg.rfind(kPlayerStatePrefix, 0) == 0;
}

// ------------------------------------------------------------
// Serialize player state into a single text line:
//
// Example:
// STATE:posX,posY,posZ|rotX,rotY,rotZ|velX,velY,velZ|hp,maxHp|ap,maxAp|moving
//
// ------------------------------------------------------------
bool SerializePlayerStateToString(const LocalPlayerState& st, std::string& out)
{
    std::ostringstream ss;

    ss << kPlayerStatePrefix
        << st.position.x << "," << st.position.y << "," << st.position.z << "|"
        << st.rotation.x << "," << st.rotation.y << "," << st.rotation.z << "|"
        << st.velocity.x << "," << st.velocity.y << "," << st.velocity.z << "|"
        << st.health << "," << st.maxHealth << "|"
        << st.ap << "," << st.maxActionPoints << "|"
        << (st.isMoving ? 1 : 0);

    out = ss.str();
    return true;
}

// ------------------------------------------------------------
// Deserialize the string format created above
// ------------------------------------------------------------
bool DeserializePlayerStateFromString(const std::string& text, LocalPlayerState& out)
{
    if (!IsPlayerStateString(text))
        return false;

    // Remove "STATE:"
    std::string payload = text.substr(strlen(kPlayerStatePrefix));

    // Split by '|'
    std::vector<std::string> fields;
    {
        size_t start = 0;
        size_t pos;
        while ((pos = payload.find('|', start)) != std::string::npos)
        {
            fields.push_back(payload.substr(start, pos - start));
            start = pos + 1;
        }
        fields.push_back(payload.substr(start));
    }

    if (fields.size() < 6)
    {
        LOG_ERROR("[PlayerState] Invalid packet: wrong field count");
        return false;
    }

    auto parseVec3 = [](const std::string& s, float v[3]) -> bool
        {
            size_t p1 = s.find(',');
            if (p1 == std::string::npos) return false;

            size_t p2 = s.find(',', p1 + 1);
            if (p2 == std::string::npos) return false;

            try {
                v[0] = std::stof(s.substr(0, p1));
                v[1] = std::stof(s.substr(p1 + 1, p2 - p1 - 1));
                v[2] = std::stof(s.substr(p2 + 1));
            }
            catch (...) { return false; }

            return true;
        };

    float pos[3], rot[3], vel[3];

    if (!parseVec3(fields[0], pos)) return false;
    if (!parseVec3(fields[1], rot)) return false;
    if (!parseVec3(fields[2], vel)) return false;

    out.position = { pos[0], pos[1], pos[2] };
    out.rotation = { rot[0], rot[1], rot[2] };
    out.velocity = { vel[0], vel[1], vel[2] };

    // HP
    {
        size_t comma = fields[3].find(',');
        if (comma == std::string::npos) return false;

        out.health = std::stof(fields[3].substr(0, comma));
        out.maxHealth = std::stof(fields[3].substr(comma + 1));
    }

    // AP
    {
        size_t comma = fields[4].find(',');
        if (comma == std::string::npos) return false;

        out.ap = std::stof(fields[4].substr(0, comma));
        out.maxActionPoints = std::stof(fields[4].substr(comma + 1));
    }

    // Moving
    out.isMoving = (fields[5] == "1");

    return true;
}


// ------------------------------------------------------------
// Legacy binary packet helpers (unused for GNS)
// ------------------------------------------------------------

bool SerializePlayerState(const LocalPlayerState&, std::vector<UInt8>&)
{
    return false; // Not used anymore
}

bool DeserializePlayerState(const UInt8*, size_t, LocalPlayerState&)
{
    return false; // Not used anymore
}

bool IsPlayerStatePacket(const UInt8*, size_t)
{
    return false; // Not used anymore
}
