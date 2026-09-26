#include "IniReader.h"
#include "CoSyncRequirementsLog.h"
#include <fstream>
#include <algorithm>

bool IniReader::Load(const char* path)
{
    LOG_REQUIREMENT("IniReader::Load", "Doesn't touch the game -- just needs a valid file path. Returns false if the file can't be opened.");
    kv.clear();

    std::ifstream file(path);
    if (!file.is_open())
        return false;

    std::string line;
    while (std::getline(file, line))
    {

        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;

        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);


        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);

        val.erase(0, val.find_first_not_of(" \t\r\n"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);

        kv[key] = val;
    }

    return true;
}

std::string IniReader::Get(const std::string& key, const std::string& def) const
{
    LOG_REQUIREMENT("IniReader::Get", "Doesn't touch the game -- just needs a valid key name. Returns the default value if that key was never loaded.");
    auto it = kv.find(key);
    if (it != kv.end())
        return it->second;

    return def;
}

std::string IniReader::GetString(const std::string& key, const std::string& def) const
{
    LOG_REQUIREMENT("IniReader::GetString", "Doesn't touch the game -- just a thin wrapper around Get() with the same requirements.");
    return Get(key, def);
}

int IniReader::GetInt(const std::string& key, int defaultValue) const
{
    LOG_REQUIREMENT("IniReader::GetInt", "Doesn't touch the game -- just needs a valid key name. A non-numeric stored value safely falls back to the default.");
    auto it = kv.find(key);
    if (it == kv.end())
        return defaultValue;

    try {
        return std::stoi(it->second);
    }
    catch (...) {
        return defaultValue;
    }
}

float IniReader::GetFloat(const std::string& key, float defaultValue) const
{
    LOG_REQUIREMENT("IniReader::GetFloat", "Doesn't touch the game -- just needs a valid key name. A non-numeric stored value safely falls back to the default.");
    auto it = kv.find(key);
    if (it == kv.end())
        return defaultValue;

    try {
        return std::stof(it->second);
    }
    catch (...) {
        return defaultValue;
    }
}
