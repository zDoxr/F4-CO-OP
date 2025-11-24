#pragma once
#include <cstdint>

// ---------------------------------------------------------
// Lightweight runtime versioning for Fallout 4
// Does NOT rely on F4SE's f4se_version.h
// ---------------------------------------------------------

// Choose a default supported version
#define F4MP_DEFAULT_RUNTIME_MAJOR 1
#define F4MP_DEFAULT_RUNTIME_MINOR 10
#define F4MP_DEFAULT_RUNTIME_PATCH 163

#define F4MP_MAKE_RUNTIME(maj, min, pat) \
    (((maj) * 10000) + ((min) * 100) + (pat))

// Expected game runtime (1.10.163 is most common)
static const uint32_t F4MP_SUPPORTED_RUNTIME =
F4MP_MAKE_RUNTIME(
    F4MP_DEFAULT_RUNTIME_MAJOR,
    F4MP_DEFAULT_RUNTIME_MINOR,
    F4MP_DEFAULT_RUNTIME_PATCH
);

// Runtime version struct
struct F4MPRuntimeInfo
{
    uint32_t version;
    uint32_t exeTimestamp;      // optional, for anti-cheat or debugging
    bool     isAE;              // true = Survivor’s Edition / “next-gen”
};

// Helper
inline bool F4MP_IsRuntimeCompatible(uint32_t runtime)
{
    return runtime == F4MP_SUPPORTED_RUNTIME;
}
