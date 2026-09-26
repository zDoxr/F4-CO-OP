#pragma once
#include <cstdint>
#include <string>

namespace CoSyncContainerSync
{
    // Called from WorldSync::Tick (game thread) to cache the nearest static
    // CONT-type container formID within activation range of the player.
    void UpdateNearestContainer(double now);

    // Called from InvAddFromWorld hook detour (game thread) when srcREFR has
    // null parentCell — indicating a container withdrawal, not a world pickup.
    void OnContainerWithdrawal(uint32_t baseFormID, uint32_t count);

    // Packet routing — prefix "CTR|containerFID|baseFID|count".
    bool IsContainerSync(const std::string& msg);
    void OnReceiveContainerSync(const std::string& msg);

    // Drain queued RemoveItem operations on the game thread.
    void FlushPendingContainerOps();

    // Deposit half of item #8: samples the nearby container and reports any
    // item whose count grew. Call once per game tick.
    void TickDeposits(double now);

    // Packet routing — prefix "CTRD|containerFID|baseFID|count" (deposit).
    bool IsContainerDeposit(const std::string& msg);
    void OnReceiveContainerDeposit(const std::string& msg);
}
