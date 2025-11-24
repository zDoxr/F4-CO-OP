#pragma once

// ---------------------------------------------------------
// Basic plugin versioning for F4MP Steam Multiplayer System
// ---------------------------------------------------------

#define F4MP_VERSION_MAJOR 1
#define F4MP_VERSION_MINOR 0
#define F4MP_VERSION_PATCH 0

#define F4MP_MAKE_VERSION(maj, min, pat) \
    (((maj) * 10000) + ((min) * 100) + (pat))

#define F4MP_VERSION \
    F4MP_MAKE_VERSION(F4MP_VERSION_MAJOR, F4MP_VERSION_MINOR, F4MP_VERSION_PATCH)

// Human-readable form
#define F4MP_VERSION_STR "1.0.0"
