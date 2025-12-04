#pragma once


#include <cstdint>

// ---------------------------------------------------------
// Fallout 4 Next-Gen runtime detector
// F4SE next-gen versions are:
//   1.10.980
//   1.10.984
//   1.10.985
// ---------------------------------------------------------

// Convert X.Y.Z ? integer XYYYZZZ (F4SE format)
#define F4MP_MAKE_RUNTIME(maj, min, pat) (((maj) * 10000) + ((min) * 100) + (pat))

// Known supported next-gen executables
static const uint32_t F4MP_RUNTIME_1_10_980 = F4MP_MAKE_RUNTIME(1, 10, 980);
static const uint32_t F4MP_RUNTIME_1_10_984 = F4MP_MAKE_RUNTIME(1, 10, 984);
static const uint32_t F4MP_RUNTIME_1_10_985 = F4MP_MAKE_RUNTIME(1, 10, 985);

// Helper: check if runtime matches any supported one
inline bool F4MP_IsRuntimeCompatible(uint32_t runtime)
{
    // TEMPORARY: accept ALL runtimes
    return true;
}
