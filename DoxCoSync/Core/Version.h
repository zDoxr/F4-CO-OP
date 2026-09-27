#pragma once

// CoSync version: MAJOR.MINOR.PATCH (e.g. 0.2.3). This is the ONE place the
// version lives. It is logged at plugin load, shown by the UI/discovery, and
// COSYNC_VERSION is also the network build number exchanged in VER|/WORLDREQ|
// and advertised by LAN discovery -- peers with different versions refuse each
// other. Bump it whenever a build changes behaviour or the wire format.
//
// History: "0.007" -> "0.320" (2026-08-04), then switched to semantic-style
// 0.2.3 on 2026-09-26 at the user's request; the separate hand-kept network
// build number (229/230 in CoSyncNet.cpp) was folded into COSYNC_VERSION.
// 0.2.4 (2026-09-26): compact BONES wire format (N4).
#define COSYNC_VERSION_MAJOR 0
#define COSYNC_VERSION_MINOR 2
#define COSYNC_VERSION_PATCH 4
#define COSYNC_VERSION_TAG   ""

// Numeric form used on the wire. MINOR and PATCH must stay below 100 for the
// encoding to be unique.
#define COSYNC_MAKE_VERSION(maj, min, pat) \
    (((maj) * 10000) + ((min) * 100) + (pat))

#define COSYNC_VERSION \
    COSYNC_MAKE_VERSION(COSYNC_VERSION_MAJOR, COSYNC_VERSION_MINOR, COSYNC_VERSION_PATCH)

static_assert(COSYNC_VERSION_MINOR < 100 && COSYNC_VERSION_PATCH < 100,
    "COSYNC_MAKE_VERSION encoding needs MINOR and PATCH below 100");

#define COSYNC_VERSION_STRINGIFY_(x) #x
#define COSYNC_VERSION_STRINGIFY(x)  COSYNC_VERSION_STRINGIFY_(x)

// "0.2.4" -- built from the numbers above so the two can never disagree.
#define COSYNC_VERSION_STR \
    COSYNC_VERSION_STRINGIFY(COSYNC_VERSION_MAJOR) "." \
    COSYNC_VERSION_STRINGIFY(COSYNC_VERSION_MINOR) "." \
    COSYNC_VERSION_STRINGIFY(COSYNC_VERSION_PATCH) COSYNC_VERSION_TAG
