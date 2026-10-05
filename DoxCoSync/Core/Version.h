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
// 0.3.0 (2026-09-28): factions, territory and authoritative workshop sync
//       (FQ/FS/FR/FN and WKQ/WKA/WKV/WKX/WKR/WKC/WKN messages).
// 0.3.1 (2026-09-28): location identity -- CREATE and NPCSEEN carry
//       worldspace/cell; clients send WS| too.
// 0.3.3 (2026-09-28): sync-stability directive -- proxies positioned through
//       the engine's own SetPosition, sender-time interpolation. Behaviour
//       only; same wire format.
// 0.3.4 (2026-09-29): sync rework directive -- proxy bone-cache validity,
//       replicated weapon authority, Power Armor / NPC authority fixes.
// 0.4.0 (2026-09-30): pre-join content sync -- the host's plugins, their
//       archives and deployed loose files are checked (SHA-256 + possession
//       proof) and transferred before the world (CS* messages; WORLDREQ and
//       every session message are refused until the content is verified).
// 0.4.3 (2026-10-01): NPC authority directive -- puppets no longer run their
//       own fights or locomotion intention (combat-entry gate, the owner's
//       Speed/Direction channels, graph-input gate, owner-only puppet health),
//       corpses stay corpses (tombstones, Dead CREATE flag, contact-based
//       timeouts), player flashlights (FL), real replicated mines (MINE/MINEX)
//       that proxies cannot trigger, aggressor-less hits on proxies refused.
// 0.4.4 (2026-10-01): NPC update scheduler (per-NPC distance-based rate,
//       change-driven, no animation snapshot, unreliable stream) and
//       in-place ownership transfer (OWN/OWNNAK) instead of destroy +
//       re-create when the host leaves or comes back.
// 0.5.0 (2026-10-05): factions, ranks, territory and banners removed;
//       teams (TQ/TS/TR/TN), team and personal settlements with host-
//       authoritative settlement state (SSR/SSI/SSD), team chat / voice
//       ('T' scope), plus the teams + bug-fix directive's other changes.
#define COSYNC_VERSION_MAJOR 0
#define COSYNC_VERSION_MINOR 5
#define COSYNC_VERSION_PATCH 0
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
