#pragma once

#include <cstdint>
#include <string>

// Phase 8 -- proxy fidelity: other players see your weapons and armor exactly
// as they are, mods included (receivers, scopes, magazines, armor linings,
// legendary effects, Power Armor paint -- paint is a mod on each piece, and
// in Power Armor the pieces are equipped on the player).
//
//   * Every player reads the object mods on its EQUIPPED items once a second
//     and sends IMODS|<entityID>|<item>:<mod>/<mod>,<item>:... when that set
//     changes (and every 20 s for late joiners). The host relays it.
//   * Receivers keep the latest set per player and, once a second, compare it
//     with the mods on that player's proxy items; anything missing is attached
//     with the engine's own AttachModToInventoryItem (NativeAttachMod). This
//     also re-applies after a proxy respawns or re-equips.
//
// Threading: main thread only (runtime tick + transport inbox).
class TESObjectREFR;

namespace CoSyncItemMods
{
    // "ITEM:MOD/MOD,..." (hex) for the weapons/armor ref carries -- only the
    // equipped copies when equippedOnly (a player), every piece otherwise (a
    // Power Armor frame). "" if none carry mods. MAIN THREAD.
    std::string DescribeMods(TESObjectREFR* ref, bool equippedOnly);

    // Attaches what `text` lists and ref's single copy of each item lacks.
    // Returns how many listed mods are still missing afterwards (0 = done;
    // items ref doesn't carry yet count as missing). MAIN THREAD.
    int ApplyMods(TESObjectREFR* ref, const std::string& text, uint32_t logTag);

    bool IsItemModsMessage(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);

    void Tick(double now);
    void Reset();
}
