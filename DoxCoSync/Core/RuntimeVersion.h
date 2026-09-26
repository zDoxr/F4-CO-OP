#pragma once


#include <cstdint>

#define COSYNC_MAKE_RUNTIME(maj, min, pat) (((maj) * 10000) + ((min) * 100) + (pat))


static const uint32_t COSYNC_RUNTIME_1_10_980 = COSYNC_MAKE_RUNTIME(1, 10, 980);
static const uint32_t COSYNC_RUNTIME_1_10_984 = COSYNC_MAKE_RUNTIME(1, 10, 984);
static const uint32_t COSYNC_RUNTIME_1_10_985 = COSYNC_MAKE_RUNTIME(1, 10, 985);


inline bool CoSync_IsRuntimeCompatible(uint32_t runtime)
{

    return true;
}
