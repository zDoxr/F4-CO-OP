#pragma once

// Initializes the standalone SteamNetworkingSockets runtime.
// Safe to call multiple times (only initializes once).
bool GNS_Init();

// Shuts down the networking runtime.
// Safe to call even if networking wasn't active.
void GNS_Shutdown();
