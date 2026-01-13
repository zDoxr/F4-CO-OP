// DX11Hook.cpp
#include "DX11Hook.h"
#include "CoSyncOverlay.h"
#include "ConsoleLogger.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include "TickHook.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "CoSyncRuntime.h"

#include "GNS_Session.h"  // <<< NEW: so we can tick networking every frame

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

// ImGui Win32 handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------
static PresentFn                 g_originalPresent = nullptr;
static bool                      g_presentHookInstalled = false;

static HWND                      g_dummyHwnd = nullptr;
static HWND                      g_gameHwnd = nullptr;
static WNDPROC                   g_originalGameWndProc = nullptr;

static ID3D11Device* g_d3dDevice = nullptr;
static ID3D11DeviceContext* g_d3dContext = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

static bool                      g_imguiInitialized = false;

// ---------------------------------------------------------------------
// Dummy wndproc (for dummy device creation only)
// ---------------------------------------------------------------------
static LRESULT CALLBACK DummyWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------
// Game WndProc hook — routes input to ImGui when overlay is visible
// ---------------------------------------------------------------------
static LRESULT CALLBACK GameWndProc_Hook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Only let ImGui process when our overlay is visible
    if (CoSyncOverlay_IsVisible())
    {
        // If ImGui handles the message, eat it so the game doesn't see it
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return TRUE;
    }

    // Otherwise, pass through to the original game wndproc
    return CallWindowProc(g_originalGameWndProc, hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------
// Create / recreate render target view from swap chain
// ---------------------------------------------------------------------
static void CreateRenderTarget(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain || !g_d3dDevice)
        return;

    if (g_mainRTV)
    {
        g_mainRTV->Release();
        g_mainRTV = nullptr;
    }

    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer)))
    {
        g_d3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRTV);
        pBackBuffer->Release();
    }
}

// ---------------------------------------------------------------------
// Hooked Present
// ---------------------------------------------------------------------
static HRESULT __stdcall HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    // One–time ImGui + device init based on the REAL game swapchain
    if (!g_imguiInitialized)
    {
        // Get device + context from the game swapchain
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_d3dDevice)) &&
            g_d3dDevice)
        {
            g_d3dDevice->GetImmediateContext(&g_d3dContext);
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        pSwapChain->GetDesc(&desc);
        g_gameHwnd = desc.OutputWindow;

        CreateRenderTarget(pSwapChain);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(g_gameHwnd);
        ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);

        g_imguiInitialized = true;

        // Hook the game window's WndProc so ImGui can see messages
        if (g_gameHwnd && !g_originalGameWndProc)
        {
            g_originalGameWndProc = (WNDPROC)SetWindowLongPtr(
                g_gameHwnd,
                GWLP_WNDPROC,
                (LONG_PTR)GameWndProc_Hook
            );

            if (!g_originalGameWndProc)
            {
                LOG_ERROR("[DX11Hook] Failed to hook game WndProc");
            }
            else
            {
                LOG_INFO("[DX11Hook] Game WndProc hooked successfully");
            }
        }

        // Now it’s safe to init our overlay (it will see a valid ImGui context)
        CoSyncOverlay_Init(pSwapChain);

        LOG_INFO("[DX11Hook] ImGui + CoSyncOverlay fully initialized");
    }

    // If ImGui is ready, build a frame
    if (g_imguiInitialized && g_d3dContext)
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // TOGGLE OVERLAY VISIBILITY
        if (GetAsyncKeyState(VK_INSERT) & 1)
        {
            CoSyncOverlay_ToggleVisible();
        }

        // INPUT & CURSOR HANDLING
        ImGuiIO& io = ImGui::GetIO();
        const bool visible = CoSyncOverlay_IsVisible();

        io.WantCaptureMouse = visible;
        io.WantCaptureKeyboard = visible;

        // Fallout 4 hides the cursor in gameplay; we force it visible when overlay is open
        if (visible)
        {
            ShowCursor(TRUE);
        }
        else
        {
            ShowCursor(FALSE);
        }

        // Draw our overlay
        CoSyncOverlay_Render();

        ImGui::EndFrame();
        ImGui::Render();

        // Make sure we render to the correct RT
        if (g_mainRTV)
        {
            g_d3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        }

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    CoSyncRuntime_TickGameThread();

    GNS_Session::Get().Tick();
    
    // Always call original Present at the end
    return g_originalPresent
        ? g_originalPresent(pSwapChain, SyncInterval, Flags)
        : S_OK;
}

// ---------------------------------------------------------------------
// Install hook
// ---------------------------------------------------------------------
bool InitDX11Hook()
{
    if (g_presentHookInstalled)
        return true;

    LOG_INFO("[DX11Hook] Initializing DX11 Present hook");

    HINSTANCE hInst = GetModuleHandle(nullptr);

    // 1) Register dummy window class
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

    // 2) Create dummy window
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

    // 3) Create dummy device/swapchain just to grab vtable
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

    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &scd,
        &pSwapChain,
        &pDevice,
        nullptr,
        &pContext
    );

    if (FAILED(hr) || !pSwapChain)
    {
        LOG_ERROR("[DX11Hook] D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        if (g_dummyHwnd)
        {
            DestroyWindow(g_dummyHwnd);
            g_dummyHwnd = nullptr;
        }
        return false;
    }

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

    LOG_INFO("[DX11Hook] Present hook installed successfully.");

    pSwapChain->Release();
    pDevice->Release();
    pContext->Release();
    DestroyWindow(g_dummyHwnd);
    g_dummyHwnd = nullptr;

    g_presentHookInstalled = true;
    return true;
}

// ---------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------
void ShutdownDX11Hook()
{
    LOG_INFO("[DX11Hook] ShutdownDX11Hook");

    // Restore original WndProc if we hooked it
    if (g_gameHwnd && g_originalGameWndProc)
    {
        SetWindowLongPtr(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_originalGameWndProc);
        g_originalGameWndProc = nullptr;
        g_gameHwnd = nullptr;
    }

    CoSyncOverlay_Shutdown();

    if (g_mainRTV)
    {
        g_mainRTV->Release();
        g_mainRTV = nullptr;
    }

    if (g_d3dContext)
    {
        g_d3dContext->Release();
        g_d3dContext = nullptr;
    }

    if (g_d3dDevice)
    {
        g_d3dDevice->Release();
        g_d3dDevice = nullptr;
    }

    if (g_imguiInitialized)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }
}
