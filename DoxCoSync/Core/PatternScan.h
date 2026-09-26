#pragma once
#include <Windows.h>
#include <vector>
#include <string>

inline uintptr_t PatternScan(const uint8_t* base, size_t size, const char* pattern)
{
    auto patternToBytes = [](const char* pat)
        {
            std::vector<int> bytes;
            const char* end = pat + strlen(pat);

            for (const char* p = pat; p < end; )
            {
                if (*p == ' ')
                {
                    p++;
                    continue;
                }

                if (*p == '?')
                {
                    bytes.push_back(-1);
                    if (*(p + 1) == '?')
                        p += 2;
                    else
                        p += 1;
                }
                else
                {
                    bytes.push_back(strtoul(p, NULL, 16));
                    p += 2;
                }
            }
            return bytes;
        };

    std::vector<int> patBytes = patternToBytes(pattern);
    size_t patSize = patBytes.size();

    for (size_t i = 0; i < size - patSize; i++)
    {
        bool matched = true;

        for (size_t j = 0; j < patSize; j++)
        {
            if (patBytes[j] != -1 && base[i + j] != patBytes[j])
            {
                matched = false;
                break;
            }
        }

        if (matched)
            return (uintptr_t)(base + i);
    }

    return 0;
}
