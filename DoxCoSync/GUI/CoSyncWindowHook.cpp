#include "CoSyncWindowHook.h"
#include "CoSyncMenuInject.h"
#include "CoSyncPrismaUI.h"
#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"
#include "CoSyncThreadDiag.h"
#include "CoSyncTransport.h"
#include "CoSyncNet.h"
#include "CoSyncVoice.h"

#include <Windows.h>
#include <string>

extern bool CoSyncRuntime_IsMenuOpen(const char* menuName);

namespace
{
    HWND    g_hWnd = nullptr;

    // [Chat] iOpenKey (virtual-key code) from Data\F4SE\Plugins\CoSync.ini.
    int ChatKey()
    {
        static int s_key = -1;
        if (s_key >= 0)
            return s_key;
        char exe[MAX_PATH] = {};
        std::string ini;
        if (GetModuleFileNameA(nullptr, exe, MAX_PATH))
        {
            ini = exe;
            ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "Data\\F4SE\\Plugins\\CoSync.ini";
        }
        s_key = static_cast<int>(GetPrivateProfileIntA("Chat", "iOpenKey", 0x59, ini.c_str()));
        if (s_key <= 0 || s_key > 0xFE)
            s_key = 0x59;
        return s_key;
    }

    bool CanOpenChat()
    {
        if (!CoSyncTransport::IsInitialized() || !CoSyncNet::IsConnected())
            return false;
        if (CoSyncPrismaUI::IsMultiplayerOpen() || CoSyncPrismaUI::IsChatOpen())
            return false;
        static const char* const kBlocking[] = {
            "PipboyMenu", "PauseMenu", "Console", "LooksMenu", "WorkshopMenu", "ContainerMenu",
            "BarterMenu", "DialogueMenu", "MainMenu", "LoadingMenu", "TerminalMenu", "ExamineMenu",
            "FavoritesMenu", "LockpickingMenu", "BookMenu", "SleepWaitMenu", "VATSMenu", "CookingMenu",
            "LevelUpMenu", "MessageBoxMenu", "PowerArmorModMenu", "SPECIALMenu", "CreditsMenu",
        };
        for (const char* m : kBlocking)
            if (CoSyncRuntime_IsMenuOpen(m))
                return false;
        return true;
    }
    WNDPROC g_origWndProc = nullptr;

    LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        LOG_REQUIREMENT("(anonymous)::HookedWndProc", "Runs on the game window's thread. The original window procedure must already be saved.");
        CoSyncThreadDiag::NoteWindowEntry();

        // PauseMenu DISCONNECT: read GFx selectedIndex on the main thread.
        // WM_LBUTTONUP is intentional -- GFx processes the click on
        // LBUTTONDOWN, so selectedIndex is already updated by the time UP
        // fires. WM_KEYDOWN/VK_RETURN works because the item is already
        // selected before Enter. Not while the PrismaUI menu owns input.
        {
            const bool isActivate = (msg == WM_KEYDOWN && wParam == VK_RETURN)
                                 || (msg == WM_LBUTTONUP);
            if (isActivate && !CoSyncPrismaUI::IsMultiplayerOpen())
                CoSyncMenuInject_PauseMenuKeyActivate();
        }

        // F10: Multiplayer menu hotkey (anywhere). F10 is a Windows SYSTEM
        // key: it arrives as WM_SYSKEYDOWN/WM_SYSKEYUP, never WM_KEYDOWN. The
        // key-up is swallowed too, or DefWindowProc enters window menu mode on
        // release. Bit 30 of lParam = auto-repeat -- only the first press
        // toggles. (Escape is handled by the menu page itself: Ultralight
        // delivers it as keyCode 27 with key="Unidentified".)
        if (wParam == VK_F10)
        {
            if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && !(lParam & (1 << 30)))
            {
                CoSyncMenuInject_OnMultiplayerHotkey();
                return 0;
            }
            if (msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN || msg == WM_SYSKEYUP || msg == WM_KEYUP)
                return 0;
        }

        // Chat key (default Y, [Chat] iOpenKey in CoSync.ini): opens the chat
        // box during a session, in game, when no game menu or other panel has
        // the input. While the box is open it owns the keyboard (PrismaUI
        // focus), so typing Y there never lands here.
        if (msg == WM_KEYDOWN && !(lParam & (1 << 30)) && static_cast<int>(wParam) == ChatKey() &&
            CanOpenChat())
        {
            CoSyncPrismaUI::RequestOpenChat();
            return 0;
        }

        // Voice keys ([Voice] iModeKey default U, iMuteKey unbound): same
        // conditions as the chat key.
        if (msg == WM_KEYDOWN && !(lParam & (1 << 30)))
        {
            const int vk = static_cast<int>(wParam);
            const int modeKey = CoSyncVoice::ModeKey();
            const int muteKey = CoSyncVoice::MuteKey();
            if ((vk == modeKey || vk == muteKey) && vk != 0 && CanOpenChat())
            {
                if (vk == modeKey)
                    CoSyncVoice::CycleMode();
                else
                    CoSyncVoice::ToggleMute();
                return 0;
            }
        }

        // Window closing (Alt+F4 etc.) while the PrismaUI menu holds focus:
        // give the focus back first. 2026-09-25 crash: quitting with the view
        // focused corrupted the heap inside PrismaUI_F4.dll's own handler.
        if (msg == WM_CLOSE && CoSyncPrismaUI::IsMultiplayerOpen())
            CoSyncPrismaUI::CloseMultiplayerNow();

        return CallWindowProcA(g_origWndProc, hWnd, msg, wParam, lParam);
    }

    // The game window is the largest visible, unowned top-level window created
    // by THIS thread (the main thread owns it). Scoping to the thread rather
    // than FindWindow keeps a second game instance's window out of it.
    struct FindCtx { HWND best = nullptr; LONG bestArea = 0; };

    BOOL CALLBACK EnumThreadWndProc(HWND hwnd, LPARAM lp)
    {
        auto* ctx = reinterpret_cast<FindCtx*>(lp);
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc))
            return TRUE;
        const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > ctx->bestArea)
        {
            ctx->best = hwnd;
            ctx->bestArea = area;
        }
        return TRUE;
    }
}

void CoSyncWindowHook_EnsureInstalled()
{
    LOG_REQUIREMENT("CoSyncWindowHook_EnsureInstalled", "Game (window-owning) thread only. Cheap no-op once installed.");
    if (g_origWndProc)
        return;

    FindCtx ctx;
    EnumThreadWindows(GetCurrentThreadId(), EnumThreadWndProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.best)
        return;   // window not created yet -- try again next frame

    char cls[64] = {};
    GetClassNameA(ctx.best, cls, sizeof(cls));

    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(ctx.best, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc)));
    if (!g_origWndProc)
    {
        LOG_ERROR("[WindowHook] SetWindowLongPtr failed (hwnd=%p err=%lu)", (void*)ctx.best, GetLastError());
        return;
    }
    g_hWnd = ctx.best;
    LOG_INFO("[WindowHook] Installed on hwnd=%p class='%s' (thread %lu)", (void*)g_hWnd, cls, GetCurrentThreadId());
}

void CoSyncWindowHook_Shutdown()
{
    LOG_REQUIREMENT("CoSyncWindowHook_Shutdown", "Safe to call even if the hook was never installed.");
    if (g_origWndProc && g_hWnd)
        SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    g_origWndProc = nullptr;
    g_hWnd = nullptr;
}
