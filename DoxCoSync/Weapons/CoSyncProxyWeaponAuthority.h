#pragma once

#include <cstdint>

class Actor;

// Weapon drawn/holstered state of remote-controlled actors (player proxies
// and NPC proxies) is REPLICATED state: the owner's machine decides it, every
// other machine only shows it.
//
// Root cause this exists for (0.3.3 report: "a player holds a weapon, the
// model appears for a few seconds, then becomes invisible"): a proxy is a
// real engine Actor with its AI running, and the engine's middle-high AI
// process update (sub_140D12010, 1.11.240) holsters the weapon of any
// non-player actor that has it drawn with no combat target and no alert --
// it calls Actor::DrawWeaponMagicHands(false) (Actor vtable slot 0xC9,
// sub_140C5D5A0) on its own. Fallout 4 draws no model for a holstered weapon,
// and CoSync only re-drew on an EDGE of the owner's state, so the proxy stayed
// empty-handed until the owner holstered and drew again.
//
// The fix is authority, not visibility: DrawWeaponMagicHands is hooked, and a
// call from that AI update that CONTRADICTS the replicated state of a
// registered proxy is refused. CoSync's own calls (CoSyncEquipHook::
// DrawWeaponEngine) always pass. Calls from any other engine path are allowed
// and counted, with their return address, so a future log shows whether some
// other system also fights the replicated state.
namespace CoSyncProxyWeaponAuthority
{
    // MinHook on sub_140C5D5A0. Idempotent; safe at plugin load.
    void Install();

    // Game thread. The owner's current drawn state for this proxy actor.
    void SetReplicatedDrawn(Actor* actor, bool drawn);

    // Game thread. Stop tracking (proxy despawned / freed / released).
    void Forget(Actor* actor);

    // Game thread. Session start/end.
    void Reset();

    // True when the engine has this actor's weapon fully holstered (weapon
    // state 0 in actor+0x134 bits 1-3, the field DrawWeaponMagicHands reads).
    // false for any other state or an unreadable actor.
    bool IsEngineHolstered(Actor* actor);

    // Wraps CoSync's own draw/holster calls so the hook lets them through.
    struct ScopedOwnCall
    {
        ScopedOwnCall();
        ~ScopedOwnCall();
    };

    // Game thread, called every CoSync tick: a 10 s summary when anything was
    // refused or observed.
    void TickDiagnostics(double now);
}
