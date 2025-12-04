#include "DX11Hook.h"
#include "CoSyncOverlay.h"
#include "ConsoleLogger.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

static PresentFn       g_originalPresent = nullptr;
static bool            g_presentHookInstalled = false;

// Dummy window handle (only for creating dummy device)
static HWND            g_dummyHwnd = nullptr;

// Very small Win32 dummy proc
static LRESULT CALLBACK DummyWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// Our hooked Present
static HRESULT __stdcall HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    // Initialize overlay once we see a real swapchain from the game
    static bool overlayInitialized = false;
    if (!overlayInitialized)
    {
        if (CoSyncOverlay::Get().Init(pSwapChain))
        {
            LOG_INFO("[DX11Hook] CoSyncOverlay initialized from Present hook");
            overlayInitialized = true;
        }
        else
        {
            LOG_ERROR("[DX11Hook] CoSyncOverlay init failed");
        }
    }

    // Render overlay every frame (if initialized)
    if (overlayInitialized)
    {
        CoSyncOverlay::Get().Render();
    }

    // Call original Present
    return g_originalPresent(pSwapChain, SyncInterval, Flags);
}

bool InitDX11Hook()
{
    if (g_presentHookInstalled)
        return true;

    LOG_INFO("[DX11Hook] Initializing DX11 Present hook");

    // --------------------------------------------------------
    // 1) Create dummy hidden window
    // --------------------------------------------------------
    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "CoSyncDummyDX11Class";

    if (!RegisterClassExA(&wc))
    {
        LOG_ERROR("[DX11Hook] RegisterClassExA failed");
        return false;
    }

    g_dummyHwnd = CreateWindowExA(
        0,
        wc.lpszClassName,
        "CoSyncDX11Window",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        100, 100,
        nullptr, nullptr,
        hInst,
        nullptr
    );

    if (!g_dummyHwnd)
    {
        LOG_ERROR("[DX11Hook] CreateWindowExA failed");
        return false;
    }

    // --------------------------------------------------------
    // 2) Create dummy D3D11 device + swapchain
    // --------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 100;
    scd.BufferDesc.Height = 100;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_dummyHwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelsRequested[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelsRequested,
        _countof(featureLevelsRequested),
        D3D11_SDK_VERSION,
        &scd,
        &pSwapChain,
        &pDevice,
        &featureLevel,
        &pContext
    );

    if (FAILED(hr) || !pSwapChain)
    {
        LOG_ERROR("[DX11Hook] D3D11CreateDeviceAndSwapChain failed (hr=0x%08X)", hr);
        if (g_dummyHwnd)
        {
            DestroyWindow(g_dummyHwnd);
            g_dummyHwnd = nullptr;
            UnregisterClassA(wc.lpszClassName, hInst);

        }
        return false;
    }

    // --------------------------------------------------------
    // 3) Grab vtable and hook Present (index 8)
    // --------------------------------------------------------
    void** vtbl = *reinterpret_cast<void***>(pSwapChain);
    if (!vtbl)
    {
        LOG_ERROR("[DX11Hook] Failed to get swapchain vtable");
        pSwapChain->Release();
        pDevice->Release();
        pContext->Release();
        DestroyWindow(g_dummyHwnd);
        g_dummyHwnd = nullptr;
        return false;
    }

    g_originalPresent = reinterpret_cast<PresentFn>(vtbl[8]);

    LOG_INFO("[DX11Hook] Original Present = %p", g_originalPresent);

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtbl[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        LOG_ERROR("[DX11Hook] VirtualProtect on vtable[8] failed");
        pSwapChain->Release();
        pDevice->Release();
        pContext->Release();
        DestroyWindow(g_dummyHwnd);
        g_dummyHwnd = nullptr;
        return false;
    }

    vtbl[8] = reinterpret_cast<void*>(&HookedPresent);

    DWORD dummy = 0;
    VirtualProtect(&vtbl[8], sizeof(void*), oldProtect, &dummy);

    LOG_INFO("[DX11Hook] Present hook installed");

    // Cleanup dummy objects (hook remains valid)
    pSwapChain->Release();
    pDevice->Release();
    pContext->Release();

    DestroyWindow(g_dummyHwnd);
    g_dummyHwnd = nullptr;

    g_presentHookInstalled = true;
    return true;
}

void ShutdownDX11Hook()
{
    if (!g_presentHookInstalled)
        return;

    LOG_INFO("[DX11Hook] ShutdownDX11Hook called (no unhook implemented yet)");

    CoSyncOverlay::Get().Shutdown();

    // If we want to unhook later, we would need to keep the vtable pointer
    // and original function pointer, and restore vtbl[8].
    // For now we assume process exit will clean up everything.
}
