#pragma once

#include <string>
#include <unordered_map>

class IniReader
{
public:
    // Load INI file
    bool Load(const char* path);

    // Core getters
    std::string Get(const std::string& key, const std::string& defaultValue = "") const;
    std::string GetString(const std::string& key, const std::string& defaultValue = "") const;

    int   GetInt(const std::string& key, int defaultValue = 0) const;
    float GetFloat(const std::string& key, float defaultValue = 0.0f) const;

private:
    std::unordered_map<std::string, std::string> kv;
};
