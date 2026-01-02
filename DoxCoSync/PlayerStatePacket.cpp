#include "PlayerStatePacket.h"
#include "ConsoleLogger.h"

#include <sstream>
#include <vector>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

static double GetNowSeconds()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static bool Split(const std::string& s, char delim, std::vector<std::string>& out)
{
    out.clear();
    std::string cur;
    for (char ch : s)
    {
        if (ch == delim)
        {
            out.push_back(cur);
            cur.clear();
        }
        else cur.push_back(ch);
    }
    out.push_back(cur);
    return true;
}

static bool Parse3f(const std::string& s, float& a, float& b, float& c)
{
    std::vector<std::string> p;
    Split(s, ',', p);
    if (p.size() != 3) return false;
    a = (float)std::atof(p[0].c_str());
    b = (float)std::atof(p[1].c_str());
    c = (float)std::atof(p[2].c_str());
    return true;
}

static bool Parse2i(const std::string& s, int& a, int& b)
{
    std::vector<std::string> p;
    Split(s, ',', p);
    if (p.size() != 2) return false;
    a = std::atoi(p[0].c_str());
    b = std::atoi(p[1].c_str());
    return true;
}

static bool Parse4f(const std::string& s, float& a, float& b, float& c, float& d)
{
    std::vector<std::string> p;
    Split(s, ',', p);
    if (p.size() != 4) return false;
    a = (float)std::atof(p[0].c_str());
    b = (float)std::atof(p[1].c_str());
    c = (float)std::atof(p[2].c_str());
    d = (float)std::atof(p[3].c_str());
    return true;
}

bool IsPlayerStateString(const std::string& msg)
{
    return msg.rfind("STATE:", 0) == 0;
}

bool SerializePlayerStateToString(const LocalPlayerState& st, std::string& out)
{
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(2);

    const double ts = (st.timestamp > 0.0) ? st.timestamp : GetNowSeconds();

    ss << "STATE:"
        << st.position.x << "," << st.position.y << "," << st.position.z << "|"
        << st.rotation.x << "," << st.rotation.y << "," << st.rotation.z << "|"
        << st.velocity.x << "," << st.velocity.y << "," << st.velocity.z << "|"
        << (st.isMoving ? 1 : 0) << "," << (st.isSprinting ? 1 : 0) << "|"
        << (st.isCrouching ? 1 : 0) << "," << (st.isJumping ? 1 : 0) << "|"
        << st.health << "," << st.maxHealth << "," << st.ap << "," << st.maxActionPoints << "|"
        << ts;

    out = ss.str();
    return true;
}

bool DeserializePlayerStateFromString(const std::string& msg, LocalPlayerState& out)
{
    if (!IsPlayerStateString(msg))
        return false;

    const std::string payload = msg.substr(6);
    std::vector<std::string> parts;
    Split(payload, '|', parts);

    if (parts.size() < 6)
        return false;

    if (!Parse3f(parts[0], out.position.x, out.position.y, out.position.z)) return false;
    if (!Parse3f(parts[1], out.rotation.x, out.rotation.y, out.rotation.z)) return false;
    if (!Parse3f(parts[2], out.velocity.x, out.velocity.y, out.velocity.z)) return false;

    {
        int a = 0, b = 0;
        if (!Parse2i(parts[3], a, b)) return false;
        out.isMoving = (a != 0);
        out.isSprinting = (b != 0);
    }

    {
        int a = 0, b = 0;
        if (!Parse2i(parts[4], a, b)) return false;
        out.isCrouching = (a != 0);
        out.isJumping = (b != 0);
    }

    if (!Parse4f(parts[5], out.health, out.maxHealth, out.ap, out.maxActionPoints))
        return false;

    out.timestamp = (parts.size() >= 7) ? std::atof(parts[6].c_str()) : GetNowSeconds();
    return true;
}
