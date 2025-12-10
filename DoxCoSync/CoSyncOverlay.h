#pragma once

#include <d3d11.h>

// Lifecycle
void CoSyncOverlay_Init(IDXGISwapChain* pSwapChain);
void CoSyncOverlay_Shutdown();

// Rendering
void CoSyncOverlay_Render();

// Visibility
void CoSyncOverlay_ToggleVisible();
void CoSyncOverlay_Show();
void CoSyncOverlay_Hide();
bool CoSyncOverlay_IsVisible();

// Utility: return last-used host IP (Hamachi or manual)
const char* CoSyncOverlay_GetHostIP();
