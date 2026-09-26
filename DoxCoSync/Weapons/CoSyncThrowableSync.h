#pragma once

#include <string>
#include <cstdint>

class Actor;
class TESObjectWEAP;
class BGSProjectile;

// Throwable / explosive-projectile sync (companion to CoSyncExplosionSync).
//
// CoSyncExplosionSync replays the DETONATION -- the EXPL ref that appears at
// the moment of impact. That is why a peer's grenade currently reads as "a
// bang out of nowhere": the ~1-2 seconds of the grenade arcing through the
// air, and the throw animation that launched it, were never on the wire at
// all. This module covers both halves of that gap:
//
//   1. THE FLYING OBJECT. Polls the loaded cell for refs whose base is a
//      BGSProjectile (form type 62 / PROJ) and streams the ones that matter
//      to peers, which spawn a cosmetic stand-in and fly it along the same
//      path. Covers hand throwables (frag/plasma/pulse grenades, Molotovs,
//      mines) AND weapon-launched explosives (missile launcher, Fat Man,
//      Broadsider, 40mm grenade launcher) -- both are PROJ refs and both are
//      selected by the same explosion-carrying filter, so DLC and modded
//      explosives are covered without a hand-written list.
//
//      The stand-in is purely visual. Damage stays with the EXPL replay in
//      CoSyncExplosionSync, so nothing here can double-apply it.
//
//   2. THE THROW ANIMATION. The behavior graph's bIsThrowing bool is already
//      captured into AnimVarSnapshot (bit 21) and shipped for players and
//      host-owned NPCs alike -- CoSyncPlayer::TickAnimVars just never read
//      it. The helpers below let the receiving side turn that bit into the
//      right graph event by inspecting what the thrower had equipped
//      (grenade -> grenadeThrowStart, mine -> mineThrowStart).
//
// THE STAND-IN MUST NEVER BE PROJ-BASED. Two separate failures came from
// getting this wrong, and both are worth knowing before touching SpawnStandIn:
//
//   1. A PROJ-based ref IS what the detector scans for, so each peer
//      re-detected its own stand-ins and re-broadcast them. One grenade became
//      20+ objects across three peers and nearly crashed all of them.
//   2. Worse, placing a ref on a PROJ base does not even give an inert prop --
//      the ENGINE instantiates a real, live projectile from it as a separate
//      ref we never created. It flies under its own physics with no launch
//      velocity, detonates on its own, and no exemption table can suppress it
//      because we never see its formID come into existence.
//
// Stand-ins are therefore plain inert item forms (the grenade WEAP, or the
// launcher's AMMO round), which the detector structurally cannot see and the
// engine cannot animate. s_ourStandIns in the .cpp is kept anyway as defence
// in depth -- this comment has been wrong before.
namespace CoSyncThrowableSync
{
    // ── Shared classification helpers (also used by the anim side) ───────────

    // True for a projectile worth streaming: it produces an explosion and it
    // is slow enough to actually be seen in flight. The speed cap is what
    // keeps explosive legendary rounds (bullet-speed projectiles that carry
    // an explosion) from flooding the wire on every trigger pull -- they
    // cross the visible distance in a fraction of a frame, so the EXPL replay
    // alone already looks correct for them.
    bool IsSyncableProjectile(BGSProjectile* proj);

    // The projectile a weapon launches, or nullptr. Reads
    // weapData.firingData->projectileOverride.
    BGSProjectile* GetWeaponProjectile(TESObjectWEAP* weap);

    // True when `weap` is a HAND throwable (grenade/Molotov/mine) rather than
    // a launcher: its projectile arcs under gravity. A missile launcher's
    // projectile has gravity 0, so the launcher never matches.
    bool IsHandThrowable(TESObjectWEAP* weap);

    // formID of the hand throwable `actor` currently has equipped, or 0.
    // Fills EntityUpdatePacket::equippedThrowableFormID on the sending side so
    // the receiver can pick the right throw animation.
    uint32_t GetEquippedThrowableFormID(Actor* actor);

    // True when `weapFormID` is a MINE rather than a grenade, so the receiver
    // sends mineThrowStart instead of grenadeThrowStart. Discriminator is the
    // projectile's explosionProximity: mines arm and detonate on proximity,
    // grenades and Molotovs run on explosionTimer with proximity 0.
    bool IsMineThrowable(uint32_t weapFormID);

    // ── Wire ────────────────────────────────────────────────────────────────

    // "THRP|ownerEID|srcRefID|projBaseFormID|x|y|z|lx|ly|lz"  launch
    //     x/y/z  = absolute world launch point
    //     lx/ly/lz = that point expressed in the THROWER's local frame, so the
    //     receiver can re-anchor it onto the thrower's proxy (a first-person
    //     thrower's grenade is born at their camera, which does not line up
    //     with the third-person proxy the receiver sees)
    // "THRM|ownerEID|srcRefID|x|y|z|vx|vy|vz"        position + velocity
    // "THRX|ownerEID|srcRefID"                       gone (detonated/expired)
    //
    // ownerEID is not decoration. The host relays these by calling
    // CoSyncTransport::Send, and GNS_Session::SendText broadcasts to EVERY
    // client connection -- including the one the message just arrived on. A
    // client's own throw therefore comes straight back to it, and without
    // this tag the thrower would spawn a stand-in sitting on top of its own
    // real grenade. It also keys the receiver's table, since two peers can
    // independently mint the same 0xFF-space ref formID.
    bool IsThrowableMsg(const std::string& msg);
    void OnReceiveThrowableMsg(const std::string& msg);

    // Detects and streams locally-spawned projectiles. Runs on BOTH host and
    // client -- a projectile belongs to whoever launched it, not to the host,
    // exactly like explosions and debris. Call once per tick.
    void Tick(double now);

    // Spawns/moves/deletes the remote stand-ins. Must run on the game thread
    // (spawns via CreateReferenceAtLocation, writes the scene graph). Call
    // once per tick, unthrottled -- the per-frame extrapolation between the
    // 20Hz THRM updates is what makes the flight look smooth instead of
    // stepping.
    void FlushPendingThrowables(double now);

    // True if `refFormID` is a cosmetic in-flight stand-in this machine
    // spawned. The activation hook consults this to make stand-ins
    // NON-LOOTABLE: they are ordinary WEAP/AMMO item refs (that is what gives
    // them the right model), so without this a peer could walk up and pick a
    // grenade out of the air mid-flight, or loot one that outlived its delete.
    bool IsStandInRef(uint32_t refFormID);

    // Drops all tracking and deletes any stand-ins still in the world. Call
    // on disconnect so a dropped THRX can't strand a grenade model in the
    // player's game.
    void Reset();
}
