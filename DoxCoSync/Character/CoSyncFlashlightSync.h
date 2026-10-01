#pragma once

#include <cstdint>
#include <string>

// Player flashlight sync (0.4.3). Evidence: docs/NPC_AUTHORITY_REPORT.md
// section "Flashlight".
//
// What Fallout 4 uses (Fallout4.exe 1.11.240, IDA):
//   PlayerCharacter::TogglePipboyLight  sub_140D76930
//     plays UIPipBoyLightOn/Off and calls
//   SetPipboyLight(player, on, silent)  sub_140D769C0
//     ON:  form = the light marker found in the actor's third-person 3D
//               (sub_140D86850), else PipboyLightOverride_DO (the Power Armor
//               headlamp, defaultPowerArmorHeadlampLight 0xB48A0, a 70 deg
//               spot light) when the actor has PipboyLightOverrideKeyword_DO
//               (KYWD PowerArmorHelmetLightOverride 0xB34A6, on every PA
//               helmet OMOD), else DefaultObject 374 (defaultPipboyLight
//               0x1ED2D, a 90 deg light with a gobo);
//          parent = that marker node, else the third-person root
//               (Actor vfunc 0x8B GetActorRootNode(false));
//          light = sub_140454880(form, 0, parent, 1, 1, 0, &light, 8.0, 0)
//               (the engine's generic LIGH -> scene light builder, 19 callers)
//          stored at PlayerCharacter+0xC38 (light) / +0xC40 (holder).
//     OFF: ShadowSceneNode remove (sub_1421B9E50(qword_143E5DF60, &light)),
//          holder+0x28 vfunc 0x1E8, release both.
//   The light is ON exactly while PlayerCharacter+0xC38 is non-null; Power
//   Armor enter/exit (sub_140989D90 / sub_14098B580) re-run it, which is how
//   the PA headlamp replaces the Pip-Boy light.
//
// Sync: the owner sends only the STATE ("FL|eid|on") on change and every 5 s.
// Every other machine runs the same engine builder on that player's proxy,
// with the same form choice and parent, so the light rides the proxy's own
// 3D: its position and direction follow the replicated body/head pose with
// the proxy's interpolation -- no light transform is streamed, nothing to
// jitter. A Power Armor change on the proxy changes the chosen form and the
// light is rebuilt.
namespace CoSyncFlashlightSync
{
    void Install();
    void Reset();

    // Game thread, every CoSync tick.
    void Tick(double now);

    bool IsFlashlightMsg(const std::string& msg);
    void OnReceive(const std::string& msg);   // game thread (CoSyncNet::OnReceive)

    // A player proxy is being removed: its light goes with it.
    void OnProxyRemoved(uint32_t entityID);
}
