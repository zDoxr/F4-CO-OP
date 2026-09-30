#pragma once

#include <cstdint>
#include <string>

// NPC idles (smoking, drinking, leaning idles, contextual/sandbox idles,
// scripted and console PlayIdle) shown on the other machines. 0.3.6.
//
// Choke point (IDA 1.11.240, 2026-09-29): sub_140CFB0D0
//   char (AIProcess*, Actor*, uint32 idleType, TESIdleForm* idle, bool, TESObjectREFR* target)
// -- FO4's special-idle start. Console PlayIdle (sub_1405CB340) calls it as
// (process, actor, 53, idle, 1, target); ~38 callers, most of them AI package
// procedures (0x140D0xxxx-0x140D4xxxx), plus Papyrus and dialogue. A bare
// graph-event name cannot carry a dynamic idle's animation (dyn_Activation
// events take it from the TESIdleForm), so the idle FORM is replicated: a
// form ID from the load order is identical on every machine.
//
// The machine that SIMULATES an NPC (no network transforms arrive for it)
// sends every explicit idle it successfully starts:
//   NIDL|entity|type|idleFormID|targetFormID
// The puppet's machine starts the same idle through the same function. The
// puppet's own idle calls are not blocked (furniture entry/exit may use this
// path; blocking it would break the furniture sync).
namespace CoSyncNpcIdleSync
{
    void Install();
    void Tick(double now);
    bool IsIdleMsg(const std::string& msg);
    void OnReceive(const std::string& msg, bool isHostRole);
    void Reset();
}
