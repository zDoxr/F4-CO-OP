#pragma once

#include <string>

class HamachiUtil
{
public:
    // Returns the first Hamachi IPv4 address (always 25.x.x.x)
    // Returns "" if no Hamachi adapter is found or no IPv4 assigned.
    static std::string GetHamachiIPv4();

    // OPTIONAL HELPERS (uncomment if needed later)
    //
    // static bool IsHamachiInstalled();
    // static std::vector<std::string> GetAllHamachiIPv4();
};
