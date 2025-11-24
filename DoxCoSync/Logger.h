#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdarg>


namespace f4mp
{
    inline void Log(const char* fmt, ...)
    {
        FILE* f = nullptr;
        errno_t err = fopen_s(&f, "f4mp_log.txt", "a");

        if (err != 0 || !f)
            return;

        va_list args;
        va_start(args, fmt);

        vfprintf(f, fmt, args);
        fprintf(f, "\n");

        va_end(args);
        fclose(f);
    }
}

#define F4MP_LOG(fmt, ...) f4mp::Log("[F4MP] " fmt, ##__VA_ARGS__)
