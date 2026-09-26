#pragma once

#define COSYNC_VERSION_MAJOR 0
#define COSYNC_VERSION_MINOR 0
#define COSYNC_VERSION_PATCH 320
#define COSYNC_VERSION_TAG   ""

#define COSYNC_MAKE_VERSION(maj, min, pat) \
    (((maj) * 10000) + ((min) * 100) + (pat))

#define COSYNC_VERSION \
    COSYNC_MAKE_VERSION(COSYNC_VERSION_MAJOR, COSYNC_VERSION_MINOR, COSYNC_VERSION_PATCH)


// Bumped 0.007 -> 0.320 on 2026-08-04. The running version is now logged at
// plugin load: until today it lived only in source comments and CHANGELOG prose
// ("build 227", "build 145", ...), so a log a user sent back could not tell you
// which binary produced it.
#define COSYNC_VERSION_STR "0.320"


