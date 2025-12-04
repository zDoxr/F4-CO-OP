#pragma once
#include <d3d11.h>
#include <dxgi.h>

class CoSyncOverlay
{
public:
    static CoSyncOverlay& Get();

    bool Init(IDXGISwapChain* chain);
    void Render();
    void Shutdown();

    void ToggleVisible();
    bool IsVisible() const { return visible; }

private:
    CoSyncOverlay() = default;

    void ShowMainWindow();

    bool initialized = false;
    bool visible     = true;

    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
};
