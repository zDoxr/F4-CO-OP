#pragma once

#include <string>
#include <cstdint>

// Event-driven equip/unequip sync. Hooks ActorEquipManager::EquipObject and
// UnequipObject at known RVAs (FO4 1.11.191 next-gen).
//
// Packet format: "EQUIP|entityID|formID|kind"
//   kind=1 → equip, kind=0 → unequip
//
// TLS guard (tls_cs_equip_applying) prevents echo when the receiver applies
// the op to the proxy actor and re-enters the detour.
//
// EquipItemDirect / UnequipItemDirect call ActorEquipManager::EquipObject
// synchronously on the game thread, firing the full visual attach chain
// (HandleItemEquip → AttachWeapon → DoReparentWeapon → DrawWeaponMagicHands
// → Update3DPosition → RequestLoadAnimationsForWeaponChange).
// Works for weapons, armor, and clothing. The singleton pointer is cached
// automatically from the first Detour_EquipObject call.

class Actor;
class TESForm;

void InstallEquipHook();
void UninstallEquipHook();

namespace CoSyncEquipHook
{
    bool IsEquip(const std::string& msg);
    void OnReceiveEquip(const std::string& msg);
    void FlushPendingEquipOps();

    // Synchronous engine equip/unequip — call on game thread only.
    // Returns false if the singleton hasn't been cached yet (fall back to Papyrus).
    bool EquipItemDirect(Actor* actor, TESForm* form);
    bool UnequipItemDirect(Actor* actor, TESForm* form);

    // Engine-level weapon draw/sheathe — Actor::DrawWeaponMagicHands, vtable
    // slot 201 (byte offset 0x648). IDA-verified 2026-07-15 (Fallout4_disk.i64):
    // Papyrus Actor.DrawWeapon native (sub_1410FC8F0) is exactly
    // `(*vtbl[201])(actor, 1)`; Actor's implementation (0xC5D210) executes the
    // BGSAction draw via TESActionData + ActionExecute and updates ActorState
    // (Actor+0x128) weapon-state bits. This is the ONLY path that makes the
    // engine attach the weapon 3D to the hand — injecting the "weaponDraw"
    // behavior-graph event animates the arms but never parents the mesh, and
    // FO4 does not render holstered NPC weapons at all.
    // Call on game thread only.
    bool DrawWeaponEngine(Actor* actor, bool draw);
}
