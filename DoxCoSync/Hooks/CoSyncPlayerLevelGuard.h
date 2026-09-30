#pragma once

// Repairs an impossible player level/XP record right before the engine's
// level-up check runs (sub_140D80110), so a save carrying one loads instead of
// looping forever. See CoSyncPlayerLevelGuard.cpp. Install at plugin load.
namespace CoSyncPlayerLevelGuard
{
    void Install();
}
