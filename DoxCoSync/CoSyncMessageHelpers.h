#pragma once
#include <string>

inline bool IsHello(const std::string& msg)
{
    return msg.rfind("HELLO|", 0) == 0;
}

inline bool IsEntityCreate(const std::string& msg)
{
    return msg.rfind("EC|", 0) == 0;
}

inline bool IsEntityUpdate(const std::string& msg)
{
    return msg.rfind("EU|", 0) == 0;
}

inline bool IsEntityDestroy(const std::string& msg)
{
    return msg.rfind("ED|", 0) == 0;
}
