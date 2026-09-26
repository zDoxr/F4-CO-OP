#pragma once

#include <string>
#include <cstdint>

namespace CoSyncQuestSync
{
    // ── Stage ────────────────────────────────────────────────────────────────
    bool IsQuestStage(const std::string& msg);
    std::string SerializeQuestStage(uint32_t senderEntityID, uint32_t questFormID, int stage);
    void NotifyQuestStage(uint32_t questFormID, int stage, uint32_t senderEntityID);
    void OnReceiveQuestStage(const std::string& msg);

    // ── Start ────────────────────────────────────────────────────────────────
    bool        IsQuestStart(const std::string& msg);
    std::string SerializeQuestStart(uint32_t senderEntityID, uint32_t questFormID);
    void        NotifyQuestStart(uint32_t questFormID, uint32_t senderEntityID);
    void        OnReceiveQuestStart(const std::string& msg);

    // ── Done ─────────────────────────────────────────────────────────────────
    bool        IsQuestDone(const std::string& msg);
    std::string SerializeQuestDone(uint32_t senderEntityID, uint32_t questFormID);
    void        NotifyQuestDone(uint32_t questFormID, uint32_t senderEntityID);
    void        OnReceiveQuestDone(const std::string& msg);

    // ── Objective: CompleteAll (QS_OBJ_ALL|sender|formID) ───────────────────
    bool        IsQuestObjAll(const std::string& msg);
    std::string SerializeObjAll(uint32_t questFormID, uint32_t senderEntityID);
    void        NotifyObjAll(uint32_t questFormID, uint32_t senderEntityID);
    void        OnReceiveObjAll(const std::string& msg);

    // ── Objective: SetCompleted (QS_OBJ_SET|sender|formID|idx|state) ────────
    bool        IsQuestObjSet(const std::string& msg);
    std::string SerializeObjSet(uint32_t questFormID, int32_t idx, bool state, uint32_t senderEntityID);
    void        NotifyObjSet(uint32_t questFormID, int32_t idx, bool state, uint32_t senderEntityID);
    void        OnReceiveObjSet(const std::string& msg);

    // ── Objective: SetDisplayed (QS_OBJ_DISP|sender|formID|idx|state) ───────
    bool        IsQuestObjDisp(const std::string& msg);
    std::string SerializeObjDisp(uint32_t questFormID, int32_t idx, bool displayed, uint32_t senderEntityID);
    void        NotifyObjDisp(uint32_t questFormID, int32_t idx, bool displayed, uint32_t senderEntityID);
    void        OnReceiveObjDisp(const std::string& msg);

    // ── Tracked quest (Pip-Boy selection) ────────────────────────────────────
    // "QS_TRK|senderEntityID|questFormID". Starting a quest is NOT the same as
    // tracking it -- without this, switching quests on the host never changed
    // what the client's Pip-Boy pointed at, even though the QS_ST packet
    // arrived and applied.
    bool IsQuestTrack(const std::string& msg);
    void OnReceiveQuestTrack(const std::string& msg);

    // ── Join catch-up ────────────────────────────────────────────────────────
    // Host-only. Pushes every active quest (with its current stage) plus the
    // completed set, so a joining client's journal matches the host's without
    // waiting on the 15s safety-net rebroadcast.
    void BroadcastFullState();

    // Call when a client joins (HELLO). Cannot broadcast immediately: before
    // any client connects TickLocalQuests bails on !IsConnected, so the
    // active/completed tables are still empty at HELLO time. This flags a
    // forced scan-then-broadcast on the next tick instead.
    void RequestFullStateBroadcast();

    // ── Flush / Tick ─────────────────────────────────────────────────────────
    void FlushPendingQuestOps();
    void FlushPendingQuestObjectiveOps();
    void TickLocalQuests(double now);

    // Returns true while FlushPendingQuestOps / FlushPendingQuestObjectiveOps
    // is applying remote state — used by objective hooks to suppress echo.
    bool IsRemoteApplying();
}
