#pragma once

#include <cstdint>

#include "NiTypes.h"
#include "CoSyncAnimVarSystem.h"

struct EntityUpdatePacket
{

    uint32_t entityID = 0;

    enum UpdateFlags : uint32_t
    {
        None = 0,
        Teleport = 1 << 0,
        NoRotation = 1 << 1,
        NoVelocity = 1 << 2,
    };

    uint32_t flags = None;

    NiPoint3 pos{ 0.f, 0.f, 0.f };
    NiPoint3 rot{ 0.f, 0.f, 0.f };
    NiPoint3 vel{ 0.f, 0.f, 0.f };

    double timestamp = 0.0;

    bool            hasAnimVars = false;
    AnimVarSnapshot animVars;

    uint32_t equippedWeaponFormID = 0;

    // formID of the hand throwable (grenade / Molotov / mine) this entity has
    // equipped right now, or 0. Separate from equippedWeaponFormID because a
    // throw equips the grenade ALONGSIDE the primary weapon rather than
    // replacing it -- both slots are live at once, and the receiver needs the
    // throwable specifically to pick between grenadeThrowStart and
    // mineThrowStart when AnimVarSnapshot's bIsThrowing bit goes high.
    uint32_t equippedThrowableFormID = 0;

    // Read BY NAME off the sender's behavior graph, not decoded from
    // animVars.booleans bit 21. Bit 21 is produced by a raw indexed read of
    // graph slot 208 (TP::kbIsThrowing), and the 2026-08-04 live test showed
    // it never went high through a session with four grenades thrown while
    // every other anim event fired normally -- the same wrong-on-NG hardcoded
    // index problem already documented for the iSync* variables. The by-name
    // path re-resolves through the graph's own hash table per call, so it is
    // correct for whatever graph the actor actually has.
    bool     isThrowing = false;

    // Same story as isThrowing: TickAnimVars used to drive reloadStart/
    // reloadEnd off animVars.booleans bit 3 (raw indexed read of graph slot
    // 31, TP::kisReloading) and the reload animation never played on proxies.
    // Read by name ("isReloading", confirmed present in RaiderRootBehavior.hkx)
    // for the same reason.
    bool     isReloading = false;

    // Ranged fire, read by name ("isFiring", present in
    // RaiderRootBehavior.hkx). This CANNOT come from actorStateFlags1's
    // atkState: 2026-08-04 live log shows atkState only ever transitions
    // 0->1 and 3->0, and those transitions are the player BASHING, not
    // shooting. atkState is the melee/bash state machine; firing a gun never
    // touches it. Driving the shot off atkState is what made proxies play a
    // shot when the real player pressed melee.
    bool     isFiring = false;

    bool     isCombatActive      = false;
    uint32_t combatTargetEntityID = 0;
    bool     isInDialog           = false;
};
