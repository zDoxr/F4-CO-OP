#pragma once

#include <cstdint>
#include <string>

// Mine sync (0.4.3). Evidence: docs/NPC_AUTHORITY_REPORT.md section "Mines".
//
// Fallout 4 (1.11.240, IDA):
//   A mine is a live Projectile ref (CK-placed: PGRE records; thrown: a PROJ
//   with explosionProximity > 0). Its state machine is sub_140E50A40:
//   Projectile+0x230 is the detonation countdown (FLT_MAX = not triggered),
//   +0x234 the blink/beep timer (fMinesBlink*), the beep task is queued
//   through sub_140C4C790.
//   The trigger test is sub_140E51300: every actor in the high-process list
//   (ProcessLists+0x40/+0x50) and the player, within the proximity radius
//   (x fMineExteriorRadiusMult outdoors), is asked
//       sub_140E48440(projectile, actor, shooter, ...)
//   (shooter = the handle at Projectile+0x1A8; the shooter itself only when
//   iProjectileMineShooterCanTrigger; dead/allied actors never) and the first
//   yes starts the countdown (fMinesDelayMin) and stores the trigger actor at
//   +0x1AC.
//   The HUD explosive indicator (MineIcon_mc) is computed by
//   sub_1409C4A20 from the player's position over the engine's own live
//   projectile lists.
//
// What was wrong (2026-09-30 21:40:23, logs 30332/34756): thrown/placed mines
// existed only on the machine that placed them (CoSyncThrowableSync is
// switched off; nothing else replicated them). The client had no mine: no
// beep, no blink, no HUD indicator, no disarm. On the host the CLIENT'S PROXY
// -- a high-process actor -- walked into the host's mine and triggered it; the
// host's explosion then damaged the proxy and the damage was relayed.
//
// Model:
//   - Trigger authority: a mine on a machine is triggered only by actors that
//     machine simulates. sub_140E48440 is hooked: player proxies and NPC
//     puppets never trigger. Each player's own machine decides with that
//     player's exact position; an NPC's owner decides for the NPC.
//   - Existence: every machine has a REAL mine. A runtime mine is announced
//     once it has come to rest (MINE|sender|owner|ref|proj|shooter|x|y|z|rz|ws|cell)
//     and re-announced every 15 s; peers place the same PROJ (the engine then
//     creates the live projectile) and give it the same shooter. A mine that
//     already exists here (a CK mine every save has) is linked, not doubled.
//   - End: when this machine's copy detonates, is disarmed or picked up, it
//     says so (MINEX|sender|owner|ref); every other machine removes its copy
//     without detonating it. The explosion itself arrives as EXP
//     (CoSyncExplosionSync) and its damage is applied on each victim's own
//     machine (CoSyncCombatAuthority: aggressor-less hits on proxies refused).
namespace CoSyncMineSync
{
    void Install();
    void Reset();

    // Game thread, every CoSync tick.
    void Tick(double now);

    bool IsMineMsg(const std::string& msg);
    void OnReceive(const std::string& msg);   // game thread (CoSyncNet::OnReceive)
}
