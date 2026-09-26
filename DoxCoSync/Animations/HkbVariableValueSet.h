#pragma once
#include <cstdint>

struct hkbVariableValueSet
{
    virtual ~hkbVariableValueSet();

    uint8_t pad8[0x8];
    uint32_t* data;
    uint32_t size;
};

static_assert(offsetof(hkbVariableValueSet, data) == 0x10);
static_assert(offsetof(hkbVariableValueSet, size) == 0x18);
