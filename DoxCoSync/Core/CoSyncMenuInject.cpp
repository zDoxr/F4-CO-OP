#include "CoSyncMenuInject.h"

#include <Windows.h>
#include <string>
#include <utility>

#include "PluginAPI.h"
#include "GameMenus.h"
#include "GameEvents.h"
#include "ScaleformCallbacks.h"
#include "ScaleformMovie.h"
#include "ScaleformValue.h"
#include "ConsoleLogger.h"
#include "CoSyncOverlay.h"
#include "CoSyncTransport.h"
#include "CoSyncGameAPI.h"
#include "CoSyncPrismaUI.h"
#include "CoSyncWorld.h"
#include "CoSyncLoadingScreen.h"

#include <atomic>
#include "CoSync_Main.h"
#include "CoSyncRequirementsLog.h"

// f4se-0.7.7's documented g_ui RVA (0x030DD8B0) is STALE for FO4 NG 1.11.221 --
// verified via IDA: every UI::IsMenuOpen / Register*Menu-family caller
// (sub_140A5DA80, sub_140A491F0, sub_140AC24F0, sub_140A793E0, etc.) loads the
// UI* singleton from a DIFFERENT global, qword_1430DD830 (RVA 0x030DD830, 0x80
// bytes earlier), and passes it directly as `a1` to sub_141A7FD40(a1, name),
// which itself calls the menuTable Find helper with `a1 + 424` (= a1 + 0x1A8).
// f4se's compiled UI::menuTable offset (0x1A8) and the real menuTable lock RVA
// (0x03DA6A70, see below) were both correct all along -- only g_ui's RVA was
// wrong, which made every (*g_ui)+offset read land in unrelated memory
// (hence menuTable size=0 / garbage fill seen at runtime, with no crash
// because it was just reading/append-ing into some other live object).
// 2026-09-26: CORRECTED to 0x030E8930 -- 0x030E88B0 was BSInputEnableManager
// (every CoSync_GetMenu logged "not found (menuTable size=0)"). See GameGlobals.cpp.
RelocPtr<UI*> g_ui_fixed(0x030E8930);

// g_menuTableLock's f4se-documented RVA (0x03DA6AF0) is dead padding (no xrefs);
// the real menuTable lock is at RVA 0x03DA6A70 (used by UI::Register*Menu
// and UI::IsMenuOpen-family functions to guard menuTable, confirmed via IDA:
// sub_141A7FD40 calls sub_141658FE0(&unk_143DA6A70) before the Find helper).
RelocPtr<BSReadWriteLock> g_menuTableLock(0x03DBD370);

static IMenu* CoSync_GetMenu(UI* ui, BSFixedString& name)
{
    LOG_REQUIREMENT("(global)::CoSync_GetMenu", "Needs a valid UI object and a real menu name to look up.");
    if (!name.data->Get<char>())
        return nullptr;

    BSReadLocker locker(g_menuTableLock);

    UI::MenuTable* menuTable = (UI::MenuTable*)((UInt8*)ui + 0x1A8);
    MenuTableItem* item = menuTable->Find(&name);
    if (!item)
    {
        LOG_DEBUG("[CoSyncMenuInject] CoSync_GetMenu: '%s' not found (menuTable size=%u, fill=%u)",
            name.c_str(), menuTable->Size(), menuTable->FillCount());
        return nullptr;
    }

    LOG_DEBUG("[CoSyncMenuInject] CoSync_GetMenu: '%s' found, menuInstance=%p, movie=%p",
        name.c_str(), item->menuInstance, item->menuInstance ? item->menuInstance->movie : nullptr);
    return item->menuInstance;
}

namespace
{
    constexpr double kMultiplayerIndex = 15.0;

    // GFxValue::IsNumber() only matches kType_Number (5), but values coming
    // from the GFx runtime (e.g. BSScrollingList.selectedIndex) are commonly
    // kType_Int (3). GetNumber() itself already handles Int/UInt/Number/Bool
    // correctly (see ScaleformValue.cpp) -- IsNumber() is just too strict.
    bool IsNumericType(const GFxValue& v)
    {
        LOG_REQUIREMENT("(global)::IsNumericType", "Doesn't touch the game. Safe to call with any value, even an empty one.");
        UInt32 t = v.GetType();
        return t == GFxValue::kType_Int || t == GFxValue::kType_UInt || t == GFxValue::kType_Number;
    }

    // Diagnostic helper: dumps DisplayObject geometry/visibility. Avoids
    // GetNumber()/GetBool() on mistyped values -- ScaleformValue.cpp's
    // default case calls HALT() on type mismatch, which we don't want to
    // risk triggering from a logging helper.
    void LogDisplayObjectInfo(const char* label, GFxValue& obj)
    {
        LOG_REQUIREMENT("(global)::LogDisplayObjectInfo", "Needs a real label for the log line. Safe to call on any menu object -- missing info just shows as a placeholder.");
        GFxValue x, y, w, h, a, vis;
        obj.GetMember("x", &x);
        obj.GetMember("y", &y);
        obj.GetMember("width", &w);
        obj.GetMember("height", &h);
        obj.GetMember("alpha", &a);
        obj.GetMember("visible", &vis);

        double xn = IsNumericType(x) ? x.GetNumber() : -999.0;
        double yn = IsNumericType(y) ? y.GetNumber() : -999.0;
        double wn = IsNumericType(w) ? w.GetNumber() : -999.0;
        double hn = IsNumericType(h) ? h.GetNumber() : -999.0;
        double an = IsNumericType(a) ? a.GetNumber() : -999.0;
        const char* visStr = (vis.GetType() == GFxValue::kType_Bool) ? (vis.GetBool() ? "true" : "false") : "?";

        LOG_INFO("[CoSyncMenuInject] %s: x=%.1f y=%.1f w=%.1f h=%.1f alpha=%.1f visible=%s",
            label, xn, yn, wn, hn, an, visStr);
    }

    // Diagnostic helper: checks for a `mask` (a DisplayObject used as a clip
    // shape -- masking is active regardless of the mask object's OWN
    // `visible` property) or `scrollRect` (a Rectangle viewport clip), plus
    // `cacheAsBitmap`/`blendMode`. Any of these could clip/hide rendering to
    // a tiny region (e.g. a thin bracket-shaped mask producing exactly a
    // "thin white line") without affecting any of the x/y/w/h/visible/alpha
    // properties LogDisplayObjectInfo already checks. Pure GetMember chains,
    // no Invoke -- zero crash risk.
    void LogMaskAndScrollRect(const char* label, GFxValue& obj)
    {
        LOG_REQUIREMENT("(global)::LogMaskAndScrollRect", "Needs a real label for the log line. Safe to call on any menu object -- missing info just shows as 'none' or '?'.");
        GFxValue maskVal;
        if (obj.GetMember("mask", &maskVal) && maskVal.IsObject())
            LogDisplayObjectInfo((std::string(label) + ".mask").c_str(), maskVal);
        else
            LOG_INFO("[CoSyncMenuInject] %s.mask: none", label);

        GFxValue rectVal;
        if (obj.GetMember("scrollRect", &rectVal) && rectVal.IsObject())
        {
            GFxValue rx, ry, rw, rh;
            rectVal.GetMember("x", &rx);
            rectVal.GetMember("y", &ry);
            rectVal.GetMember("width", &rw);
            rectVal.GetMember("height", &rh);
            LOG_INFO("[CoSyncMenuInject] %s.scrollRect: x=%.1f y=%.1f w=%.1f h=%.1f",
                label,
                IsNumericType(rx) ? rx.GetNumber() : -999.0,
                IsNumericType(ry) ? ry.GetNumber() : -999.0,
                IsNumericType(rw) ? rw.GetNumber() : -999.0,
                IsNumericType(rh) ? rh.GetNumber() : -999.0);
        }
        else
        {
            LOG_INFO("[CoSyncMenuInject] %s.scrollRect: none", label);
        }

        GFxValue cabVal, bmVal;
        obj.GetMember("cacheAsBitmap", &cabVal);
        obj.GetMember("blendMode", &bmVal);
        LOG_INFO("[CoSyncMenuInject] %s: cacheAsBitmap=%s blendMode='%s'",
            label,
            (cabVal.GetType() == GFxValue::kType_Bool) ? (cabVal.GetBool() ? "true" : "false") : "?",
            bmVal.IsString() ? bmVal.GetString() : "?");
    }

    // Diagnostic helper: scaleX/scaleY/rotation. A non-1.0 scale or non-zero
    // rotation on an object OR ANY OF ITS ANCESTORS changes how its already-
    // confirmed-correct local x/y/width/height map onto the stage -- e.g.
    // scaleX=0 on a parent would zero out a child's effective on-screen size
    // even though the child's OWN .width reports its un-scaled-by-parent
    // value. Pure GetMember, zero risk.
    void LogTransform(const char* label, GFxValue& obj)
    {
        LOG_REQUIREMENT("(global)::LogTransform", "Needs a real label for the log line. Safe to call on any menu object -- missing info just shows as a placeholder.");
        GFxValue sx, sy, rot;
        obj.GetMember("scaleX", &sx);
        obj.GetMember("scaleY", &sy);
        obj.GetMember("rotation", &rot);
        LOG_INFO("[CoSyncMenuInject] %s: scaleX=%.2f scaleY=%.2f rotation=%.2f",
            label,
            IsNumericType(sx) ? sx.GetNumber() : -999.0,
            IsNumericType(sy) ? sy.GetNumber() : -999.0,
            IsNumericType(rot) ? rot.GetNumber() : -999.0);
    }

    // True while the PrismaUI Multiplayer menu opened from the main menu is up
    // (the vanilla list is hidden + frozen for exactly that long).
    bool g_multiplayerPanelOpen = false;
    void OpenMultiplayerPanel(GFxMovieRoot* root);
    void SetMainPanelFrozen(GFxMovieRoot* root, bool frozen);

    // Set by the F10 hotkey on the main menu (window thread), consumed by the
    // MainMenu per-frame poll (the only place holding a live GFx root).
    std::atomic<bool> g_multiplayerToggleRequested{ false };

    // CACHE (2026-06-12): "root.Menu_mc" resolves fine on the InitMainList
    // hook's first 1-2 calls (right when MainMenu's title screen first
    // builds), but once GameLoaded fires, "_root"/"root.Menu_mc" stop
    // resolving entirely on every later call of the SAME hook with the SAME
    // movie/movieRoot pointer -- "List_mc not found" on every Activate
    // press. Cache the first successful Menu_mc reference (a refcounted
    // GFxValue handle) so later calls can fall back to it instead of
    // re-walking "root.Menu_mc", which appears to require the root timeline
    // to still be in its just-loaded state.
    // ROUND 146 (2026-07-09): deliberately leaked. As a plain global, this
    // object's static destructor ran from _execute_onexit_table during game
    // exit and called GFxValue::~GFxValue -> Scaleform release against the
    // engine's already-destroyed GFx heap -> READ AV that made BOTH machines
    // "crash on quit" every session (PDB-resolved from the 20:52:16/20:52:18
    // fatal VEH reports, build 145). A refcounted engine handle held in a
    // static must never be destructed at CRT exit; leaking it is correct --
    // the process is dying and the engine reclaims everything.
    GFxValue& g_cachedMenuMc = *(new GFxValue());
    bool g_cachedMenuMcValid = false;


    bool ResolveMenuMc(GFxMovieRoot* root, GFxValue& outMenuMc)
    {
        LOG_REQUIREMENT("(global)::ResolveMenuMc", "Safe to call with a missing menu root. Falls back to the last known-good menu if the current lookup fails.");
        if (root && root->GetVariable(&outMenuMc, "root.Menu_mc") && outMenuMc.IsObject())
        {
            if (!g_cachedMenuMcValid)
            {
                g_cachedMenuMc = outMenuMc;
                g_cachedMenuMcValid = true;
                LOG_INFO("[CoSyncMenuInject] ResolveMenuMc: cached Menu_mc from fresh lookup");
            }
            return true;
        }

        if (g_cachedMenuMcValid)
        {
            outMenuMc = g_cachedMenuMc;
            return true;
        }

        return false;
    }


    // Dumps Menu_mc's full child list (depth index, instance name, visible)
    // via numChildren/getChildAt/GetMember -- the exact pattern f4se's own
    // OnScaleformRegister runs on every movie every session
    // (Hooks_Scaleform.cpp), so getChildAt-via-Invoke is verified safe even
    // though setChildIndex/swapDepths/addEventListener are not.
    void DumpMenuMcChildren(GFxValue& menuMc)
    {
        LOG_REQUIREMENT("(global)::DumpMenuMcChildren", "Needs a valid menu object with a child list. Exits quietly if that's missing.");
        GFxValue numChildrenVal;
        if (!menuMc.GetMember("numChildren", &numChildrenVal))
        {
            LOG_WARN("[CoSyncMenuInject] DumpMenuMcChildren: numChildren missing");
            return;
        }

        SInt32 n = numChildrenVal.GetInt();
        LOG_INFO("[CoSyncMenuInject] Menu_mc.numChildren=%d", n);

        for (SInt32 i = 0; i < n; i++)
        {
            GFxValue index(i);
            GFxValue child;
            if (!menuMc.Invoke("getChildAt", &child, &index, 1) || !child.IsObject())
                continue;

            GFxValue nameVal, visVal;
            child.GetMember("name", &nameVal);
            child.GetMember("visible", &visVal);

            const char* name = nameVal.IsString() ? nameVal.GetString() : "?";
            bool isVisible = (visVal.GetType() == GFxValue::kType_Bool) && visVal.GetBool();
            const char* vis = (visVal.GetType() == GFxValue::kType_Bool) ? (isVisible ? "true" : "false") : "?";

            LOG_INFO("[CoSyncMenuInject]   Menu_mc[%d] name='%s' visible=%s", i, name, vis);

            // HelpPanel_mc sits at index 17 (confirmed). Anything above it
            // that's visible=true is a candidate for occluding it -- pull
            // geometry/alpha via the proven-safe LogDisplayObjectInfo
            // (GetMember-only, no new Invoke) to see if it overlaps
            // HelpPanel_mc's region (x=292 y=204 w=369.8 h=396.1).
            if (i > 17 && isVisible)
                LogDisplayObjectInfo(name, child);
        }
    }

    // Generic version of the above for any container -- dumps numChildren,
    // then index/name/visible + full geometry (via LogDisplayObjectInfo) for
    // EVERY child. Same proven-safe numChildren/getChildAt/GetMember
    // primitives, just without the Menu_mc-specific i>17 filter (these
    // containers are small, so dumping everything is cheap).
    void DumpChildrenOf(GFxValue& obj, const char* label)
    {
        LOG_REQUIREMENT("(global)::DumpChildrenOf", "Needs a valid menu object with a child list and a real label. Exits quietly if the child list is missing.");
        GFxValue numChildrenVal;
        if (!obj.GetMember("numChildren", &numChildrenVal))
        {
            LOG_WARN("[CoSyncMenuInject] DumpChildrenOf(%s): numChildren missing", label);
            return;
        }

        SInt32 n = numChildrenVal.GetInt();
        LOG_INFO("[CoSyncMenuInject] %s.numChildren=%d", label, n);

        for (SInt32 i = 0; i < n; i++)
        {
            GFxValue index(i);
            GFxValue child;
            if (!obj.Invoke("getChildAt", &child, &index, 1) || !child.IsObject())
                continue;

            GFxValue nameVal, visVal;
            child.GetMember("name", &nameVal);
            child.GetMember("visible", &visVal);

            const char* name = nameVal.IsString() ? nameVal.GetString() : "?";
            bool isVisible = (visVal.GetType() == GFxValue::kType_Bool) && visVal.GetBool();
            const char* vis = (visVal.GetType() == GFxValue::kType_Bool) ? (isVisible ? "true" : "false") : "?";

            LOG_INFO("[CoSyncMenuInject]   %s[%d] name='%s' visible=%s", label, i, name, vis);
            LogDisplayObjectInfo(name, child);
        }
    }

    // Logs an object's bounding box in another object's coordinate space
    // via DisplayObject.getBounds(targetCoordinateSpace) -- used with
    // _root as the target to get absolute stage pixel coordinates,
    // independent of any local transform/scale on intermediate parents.
    void LogGlobalBounds(GFxValue& obj, const char* label, GFxValue& targetSpace)
    {
        LOG_REQUIREMENT("(global)::LogGlobalBounds", "Needs a menu object that supports getBounds, and a real label for the log line.");
        GFxValue boundsArgs[1] = { targetSpace };
        GFxValue bounds;
        bool gotBounds = obj.Invoke("getBounds", &bounds, boundsArgs, 1);
        if (!gotBounds || !bounds.IsObject())
        {
            LOG_WARN("[CoSyncMenuInject] LogGlobalBounds(%s): getBounds failed ok=%d isObj=%d", label, gotBounds ? 1 : 0, bounds.IsObject() ? 1 : 0);
            return;
        }

        GFxValue bx, by, bw, bh;
        bounds.GetMember("x", &bx);
        bounds.GetMember("y", &by);
        bounds.GetMember("width", &bw);
        bounds.GetMember("height", &bh);
        LOG_INFO("[CoSyncMenuInject] %s globalBounds(_root): x=%.1f y=%.1f w=%.1f h=%.1f", label,
            IsNumericType(bx) ? bx.GetNumber() : -999.0,
            IsNumericType(by) ? by.GetNumber() : -999.0,
            IsNumericType(bw) ? bw.GetNumber() : -999.0,
            IsNumericType(bh) ? bh.GetNumber() : -999.0);
    }

    // Diagnostic helper: HelpList_mc's child[1] (reflected as "instance187")
    // is almost certainly EntryHolder_mc -- the anonymous MovieClip
    // BSScrollingList's constructor creates via
    // addChildAt(new MovieClip(), getChildIndex(border)+1) to hold
    // dynamically-spawned BSScrollingListEntry item-renderer clips. Its
    // non-zero bbox (134x112.8, ~3 rows tall) suggests UpdateList() DID
    // create+position entry clips for our 3 pushed categories -- this dumps
    // each entry clip's geometry plus its public textField/border members
    // (per BSScrollingListEntry.as) to see whether SetEntryText/
    // GlobalFunc.SetText actually wrote NETWORKING/SERVER/MOD CONFIG into
    // them, and whether THEY render any differently than HelpText/
    // HelpTitleText. Pure GetMember + getChildAt -- same proven-safe
    // primitives as DumpChildrenOf, zero new Invoke targets.
    void DumpEntryHolderChildren(GFxValue& entryHolder)
    {
        LOG_REQUIREMENT("(global)::DumpEntryHolderChildren", "Needs a valid entry-holder menu object with a child list.");
        LogDisplayObjectInfo("EntryHolder_mc", entryHolder);

        GFxValue numChildrenVal;
        if (!entryHolder.GetMember("numChildren", &numChildrenVal))
        {
            LOG_WARN("[CoSyncMenuInject] DumpEntryHolderChildren: numChildren missing");
            return;
        }

        SInt32 n = numChildrenVal.GetInt();
        LOG_INFO("[CoSyncMenuInject] EntryHolder_mc.numChildren=%d", n);

        for (SInt32 i = 0; i < n; i++)
        {
            GFxValue index(i);
            GFxValue entryClip;
            if (!entryHolder.Invoke("getChildAt", &entryClip, &index, 1) || !entryClip.IsObject())
                continue;

            GFxValue nameVal;
            entryClip.GetMember("name", &nameVal);
            const char* name = nameVal.IsString() ? nameVal.GetString() : "?";

            std::string label = "EntryHolder_mc[" + std::to_string(i) + "] " + name;
            LogDisplayObjectInfo(label.c_str(), entryClip);

            GFxValue textField;
            if (entryClip.GetMember("textField", &textField) && textField.IsObject())
            {
                LogDisplayObjectInfo((label + ".textField").c_str(), textField);

                GFxValue textVal, htmlVal;
                textField.GetMember("text", &textVal);
                textField.GetMember("htmlText", &htmlVal);
                LOG_INFO("[CoSyncMenuInject] %s.textField: text='%s' htmlText='%s'",
                    label.c_str(),
                    textVal.IsString() ? textVal.GetString() : "?",
                    htmlVal.IsString() ? htmlVal.GetString() : "?");
            }
            else
            {
                LOG_INFO("[CoSyncMenuInject] %s.textField: not found/object", label.c_str());
            }

            GFxValue borderVal;
            if (entryClip.GetMember("border", &borderVal) && borderVal.IsObject())
                LogDisplayObjectInfo((label + ".border").c_str(), borderVal);
        }
    }

    void InjectMultiplayerEntry(GFxMovieView* view)
    {
        LOG_REQUIREMENT("(global)::InjectMultiplayerEntry", "Safe to call on any menu, even one that isn't the main menu -- does nothing if there's no menu list to add an entry to.");
        if (!view || !view->movieRoot)
            return;

        GFxMovieRoot* root = view->movieRoot;

        GFxValue entryList;
        if (!root->GetVariable(&entryList, "root.Menu_mc.MainPanel_mc.List_mc.entryList") || !entryList.IsArray())
        {
            // Expected for every non-MainMenu movie when called from
            // CoSyncMenuInject_OnScaleformView -- not an error.
            return;
        }

        // Idempotency: InitMainList fires continuously (~every frame) while
        // the title screen is up, so this scan runs at high frequency --
        // keep it cheap (no per-element logging) and only log the
        // "already present" outcome once.
        UInt32 count = entryList.GetArraySize();
        for (UInt32 i = 0; i < count; i++)
        {
            GFxValue elem;
            if (!entryList.GetElement(i, &elem) || !elem.IsObject())
                continue;

            GFxValue idx;
            if (!elem.GetMember("index", &idx))
                continue;

            if (idx.GetNumber() == kMultiplayerIndex)
            {
                static bool s_loggedAlreadyPresent = false;
                if (!s_loggedAlreadyPresent)
                {
                    LOG_INFO("[CoSyncMenuInject] MULTIPLAYER row already present, skipping (further occurrences not logged)");
                    s_loggedAlreadyPresent = true;
                }
                return;
            }
        }

        GFxValue entry;
        root->CreateObject(&entry);

        GFxValue text;
        root->CreateString(&text, "MULTIPLAYER");
        entry.SetMember("text", &text);

        GFxValue indexVal(kMultiplayerIndex);
        entry.SetMember("index", &indexVal);

        if (!entryList.PushBack(&entry))
        {
            LOG_ERROR("[CoSyncMenuInject] failed to push MULTIPLAYER entry");
            return;
        }

        root->Invoke("root.Menu_mc.MainPanel_mc.List_mc.InvalidateData", nullptr, nullptr, 0);

        LOG_INFO("[CoSyncMenuInject] MULTIPLAYER row injected into MainMenu");
    }

    // Edge-detects an "Activate" press (Enter / Space / left mouse button)
    // without registering any GFx event listener -- addEventListener +
    // CreateFunction<NewHandlerType> is confirmed broken (5/5 crashes this
    // session, see project_fo4_ng_menu_inject.md CRASH #1-5).
    bool IsActivatePressed()
    {
        LOG_REQUIREMENT("(global)::IsActivatePressed", "Doesn't touch the game. Just checks if the Activate key was pressed since the last check -- needs to be polled regularly to catch every press.");
        static bool s_wasDown = false;

        bool isDown = (GetAsyncKeyState(VK_RETURN) & 0x8000) ||
                       (GetAsyncKeyState(VK_SPACE) & 0x8000) ||
                       (GetAsyncKeyState(VK_LBUTTON) & 0x8000);

        bool pressed = isDown && !s_wasDown;
        s_wasDown = isDown;
        return pressed;
    }

    // Deliberately keeps its own edge state, separate from
    // IsActivatePressed(). MainMenu's per-frame update keeps running after the
    // title screen is gone -- its movie/movieRoot stay live, which is the same
    // behaviour the ResolveMenuMc cache note above describes -- so the
    // main-menu poll and PollPauseMenuDisconnect both sample the keyboard on
    // the same frames. Sharing one s_wasDown would make whichever ran first
    // consume the press edge and leave the other seeing nothing.
    bool IsMainMenuActivatePressed()
    {
        LOG_REQUIREMENT("(global)::IsMainMenuActivatePressed", "Doesn't touch the game. Just checks if the Activate key was pressed since the last check, tracked separately for the main menu -- needs to be polled every frame.");
        static bool s_wasDown = false;

        bool isDown = (GetAsyncKeyState(VK_RETURN) & 0x8000) ||
                       (GetAsyncKeyState(VK_SPACE) & 0x8000) ||
                       (GetAsyncKeyState(VK_LBUTTON) & 0x8000);

        bool pressed = isDown && !s_wasDown;
        s_wasDown = isDown;
        return pressed;
    }

    // Same edge-detect pattern as IsActivatePressed, for the dedicated
    // close key. Vanilla onCancelPress(MAIN_STATE) with PauseMode==false
    // (title screen) is a no-op (just sets a local bool and returns), so
    // ESC reaching the vanilla handler too causes no conflicting behavior
    // (e.g. no quit-confirm dialog) -- see MainMenu.as ~1119-1127.
    bool IsEscapePressed()
    {
        LOG_REQUIREMENT("(global)::IsEscapePressed", "Doesn't touch the game. Just checks if Escape was pressed since the last check -- needs to be polled regularly to catch every press.");
        static bool s_wasDown = false;

        bool isDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

        bool pressed = isDown && !s_wasDown;
        s_wasDown = isDown;
        return pressed;
    }

    // Driven per-frame from MainMenu_Update_Hook (vtable index 0x10), NOT from
    // the InitMainList hook. InitMainList only runs when the list is rebuilt --
    // a handful of times per session -- which is far too rare to catch a
    // GetAsyncKeyState press edge. The per-frame cost here is two
    // GetAsyncKeyState reads plus the ResolveMenuMc cache prime; everything
    // past the edge check only runs on an actual press.
    //
    // On an Activate edge, checks whether the currently-highlighted entry is
    // the injected MULTIPLAYER row (via its custom "index" field, not
    // BSScrollingList's array-position selectedIndex) and toggles the panel.
    void PollMultiplayerRowActivation(GFxMovieRoot* root)
    {
        LOG_REQUIREMENT("(global)::PollMultiplayerRowActivation", "Needs a valid menu root. Safe if any of the expected menu pieces are missing -- just exits early.");
        // CACHE PRIMING (2026-06-13): ResolveMenuMc's cached fallback (see
        // its definition) was only ever invoked AFTER an Activate press --
        // by which point "root.Menu_mc" no longer resolves fresh and the
        // cache was still empty ("fresh+cached both failed"). Call it
        // unconditionally every tick so the cache gets populated on an
        // early tick (when "root.Menu_mc" is known to resolve, per the
        // splash-screen diagnostics that used to run here) and is available
        // as a fallback by the time the user presses Enter on MULTIPLAYER.
        // Pure GetVariable/GetMember, zero new Invoke risk.
        {
            GFxValue primeMenuMc;
            ResolveMenuMc(root, primeMenuMc);
        }

        bool activatePressed = IsMainMenuActivatePressed();
        const bool toggleRequested = g_multiplayerToggleRequested.exchange(false);

        if (toggleRequested)
        {
            if (!g_multiplayerPanelOpen)
            {
                LOG_INFO("[CoSyncMenuInject] Multiplayer hotkey -- opening from main menu");
                OpenMultiplayerPanel(root);
                return;
            }
            CoSyncPrismaUI::RequestToggleMultiplayer();   // closes; restore happens below
        }

        if (g_multiplayerPanelOpen)
        {
            // The PrismaUI menu owns Enter/Escape while open (Escape = BACK
            // inside it). Only react once it has closed.
            if (!CoSyncPrismaUI::IsMultiplayerOpen())
            {
                g_multiplayerPanelOpen = false;
                SetMainPanelFrozen(root, false);
                LOG_INFO("[CoSyncMenuInject] Multiplayer menu closed -- main menu restored");
            }
            return;
        }

        if (!activatePressed)
            return;

        LOG_INFO("[CoSyncMenuInject] Activate edge detected");

        // ROOT CAUSE (2026-06-12): stepwise bisect proved "_root" and
        // "root.Menu_mc" both fail (got=0) on every InitMainList hook call
        // once GameLoaded fires, even though a1/movie/movieRoot are the
        // EXACT SAME pointers as the first (successful) call. The first
        // call's root.Menu_mc reference is cached via ResolveMenuMc and
        // reused here as a fallback.
        GFxValue menuMc;
        if (!ResolveMenuMc(root, menuMc))
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: Menu_mc not found (fresh+cached both failed)");
            return;
        }

        // Only while MainMenu shows its main list. In QUIT_CONFIRM_STATE the
        // list keeps its selection (and follows the mouse), so the click that
        // confirmed QUIT also read as "MULTIPLAYER row activated": the PrismaUI
        // menu opened and took focus as the game was shutting down. PrismaUI
        // then released its input layer after the engine had destroyed
        // BSInputEnableManager (0x30E88B0 nulled by 0x140C3CF20) and
        // CRT-freed the game-owned layer -- 0xC0000374 on both host and
        // client, 2026-09-26. currentState is MainMenu.as's public getter for
        // strCurrentState. Unreadable => don't open (fail closed).
        {
            GFxValue state;
            if (!menuMc.GetMember("currentState", &state) || !state.IsString())
            {
                LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: MainMenu currentState unreadable -- ignoring press");
                return;
            }
            const char* s = state.GetString();
            if (!s || strcmp(s, "MAIN_STATE") != 0)
            {
                LOG_INFO("[CoSyncMenuInject] Activate ignored -- MainMenu state is '%s', not MAIN_STATE", s ? s : "(null)");
                return;
            }
        }

        GFxValue mainPanel;
        if (!menuMc.GetMember("MainPanel_mc", &mainPanel) || !mainPanel.IsObject())
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: MainPanel_mc not found");
            return;
        }

        GFxValue listMc;
        if (!mainPanel.GetMember("List_mc", &listMc) || !listMc.IsObject())
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: List_mc not found");
            return;
        }

        GFxValue selIdx;
        if (!listMc.GetMember("selectedIndex", &selIdx))
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: selectedIndex member missing");
            return;
        }
        if (!IsNumericType(selIdx))
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: selectedIndex not a number (type=%u)", selIdx.GetType());
            return;
        }

        double selIdxNum = selIdx.GetNumber();
        LOG_INFO("[CoSyncMenuInject] selectedIndex=%.0f", selIdxNum);

        if (selIdxNum < 0)
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: no row selected (selectedIndex=%.0f)", selIdxNum);
            return;
        }

        GFxValue entryList;
        if (!listMc.GetMember("entryList", &entryList) || !entryList.IsArray())
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: entryList not array");
            return;
        }

        GFxValue elem;
        if (!entryList.GetElement((UInt32)selIdxNum, &elem) || !elem.IsObject())
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: GetElement(%d) failed", (int)selIdxNum);
            return;
        }

        GFxValue idx;
        if (!elem.GetMember("index", &idx))
        {
            LOG_WARN("[CoSyncMenuInject] PollMultiplayerRowActivation: highlighted entry has no 'index' member");
            return;
        }

        LOG_INFO("[CoSyncMenuInject] highlighted entry index=%.0f", idx.GetNumber());

        if (idx.GetNumber() != kMultiplayerIndex)
            return;

        LOG_INFO("[CoSyncMenuInject] MULTIPLAYER row activated (native input)");
        OpenMultiplayerPanel(root);
    }

    // Opens the PrismaUI Multiplayer menu and hides + freezes the vanilla
    // main-menu list behind it (restored by the poll when the menu closes).
    // PrismaUI is required: there is no other multiplayer UI any more.
    void OpenMultiplayerPanel(GFxMovieRoot* root)
    {
        LOG_REQUIREMENT("(global)::OpenMultiplayerPanel", "Safe to call with a missing menu root -- exits before changing any state.");
        if (!root || g_multiplayerPanelOpen)
            return;

        if (!CoSyncPrismaUI::IsAvailable())
        {
            LOG_ERROR("[CoSyncMenuInject] MULTIPLAYER needs PrismaUI (PrismaUI_F4.dll) -- it is not loaded");
            CoSyncGameAPI::QueueNotification("CoSync needs PrismaUI for its menu.");
            return;
        }

        g_multiplayerPanelOpen = true;
        SetMainPanelFrozen(root, true);
        CoSyncPrismaUI::RequestOpenMultiplayer(true);
        LOG_INFO("[CoSyncMenuInject] Multiplayer menu opened");
    }

    // Hides the vanilla main-menu list AND stops it reacting to input while a
    // multiplayer UI is on top. Hiding alone is not enough: PrismaUI's focus
    // does not stop the MainMenu movie receiving input (2026-09-25: the list
    // moved and QUIT fired underneath the PrismaUI menu, and PrismaUI then
    // crashed during the shutdown). BSScrollingList's own disableInput /
    // disableSelection flags gate its keyboard nav, mouse hover, Enter and
    // item press (BSScrollingList.as), and MainMenu.as uses them the same way
    // to lock its lists. Pure SetMember -- no Invoke.
    void SetMainPanelFrozen(GFxMovieRoot* root, bool frozen)
    {
        LOG_REQUIREMENT("(global)::SetMainPanelFrozen", "Needs a valid menu root. Safe if the menu pieces can't be found -- just does nothing.");
        if (!root)
            return;
        GFxValue menuMc;
        if (!ResolveMenuMc(root, menuMc))
            return;

        GFxValue mainPanel;
        if (!menuMc.GetMember("MainPanel_mc", &mainPanel) || !mainPanel.IsObject())
            return;

        GFxValue visVal(!frozen);
        mainPanel.SetMember("visible", &visVal);

        GFxValue listMc;
        if (mainPanel.GetMember("List_mc", &listMc) && listMc.IsObject())
        {
            GFxValue lock(frozen);
            listMc.SetMember("disableInput", &lock);
            listMc.SetMember("disableSelection", &lock);
        }
        LOG_INFO("[CoSyncMenuInject] Vanilla main-menu list %s", frozen ? "hidden + frozen" : "restored");
    }


    // MainMenu::vftable (primary), confirmed via IDA/RTTI on 1.11.221: the
    // Complete Object Locator pointer for .?AVMainMenu@@ (typeDescRVA
    // 0x03069D38, offset=0 => primary vtable) sits at 0x1425A9160, so the
    // vtable itself starts at 0x1425A9168 (RVA 0x025A9168). The qword at
    // 0x1425A9160 is that COL pointer, not a virtual function.
    //
    // Slot index 0x15 (RVA 0x025A9210 -> sub_141071290) is MainMenu's
    // InitMainList override: it is one of only four functions in the image
    // referencing the "InitList" string (0x142531220), which it passes to
    // Invoke(..., "InitList", entries, 10) to populate
    // root.Menu_mc.MainPanel_mc.List_mc.entryList. Its real signature is
    // __int64 __fastcall(__int64 a1) -- a single argument -- which is what
    // _InitMainList_t below declares.
    //
    // The base constant was previously 0x025A9158, which is 0x10 (two slots)
    // too low: it counted the COL pointer and the qword ahead of it as
    // virtual functions. Combined with index 0x15 that patched RVA
    // 0x025A9200 = real index 0x13 = sub_140A72880, a THREE-argument
    // listener-notify helper rather than InitMainList. The one-argument thunk
    // called it with only RCX set, so RDX/R8 still held whatever the
    // preceding call had left in them; sub_140A72880 forwards both registers
    // verbatim into every registered listener's vfunc #1, which dereferences
    // the second as a 0x50-stride array. That produced a read AV at 0xB on
    // any machine where at least one listener was registered by that point
    // (with zero listeners it returns before touching either argument, which
    // is why the fault was load-order dependent).
    //
    // OnScaleformView probes see entryList.size==0 because that registration
    // callback fires BEFORE InitMainList runs; hooking this slot injects
    // immediately AFTER vanilla populates the list, every time MainMenu
    // rebuilds it (the idempotency check in InjectMultiplayerEntry prevents
    // double-injection on repeat calls).
    constexpr uintptr_t kMainMenuVtableRVA = 0x025B1258;
    constexpr UInt32 kInitMainListVtableIndex = 0x15;

    // Slot index 0x10 (RVA 0x025A91E8 -> sub_140A72730) is the shared IMenu
    // per-frame update: it takes the menu's lock, clears the two dirty flags at
    // +0xA8/+0xC0, dispatches vfunc index 0x13, and reports whether either flag
    // was re-set. Signature is _BOOL8 __fastcall(__int64 a1) -- a single
    // argument, so a thunk over it cannot drop or corrupt parameters the way a
    // mismatched signature over index 0x13 would.
    //
    // This is the per-frame driver for PollMultiplayerRowActivation. Index 0x15
    // (InitMainList) only fires when the list is rebuilt, which is far too
    // infrequent to edge-detect a keypress; index 0x10 runs every frame and,
    // unlike index 0x13, executes its body unconditionally rather than
    // returning early when no listeners are registered.
    //
    // Patching this slot only affects MainMenu instances -- sub_140A72730 is
    // the base implementation shared by ~40 menu vtables, but only MainMenu's
    // copy of the pointer is overwritten.
    constexpr UInt32 kMenuUpdateVtableIndex = 0x10;

    using _InitMainList_t = __int64(__fastcall*)(void* a1);
    _InitMainList_t g_origInitMainList = nullptr;

    __int64 __fastcall MainMenu_InitMainList_Hook(void* a1)
    {
        LOG_REQUIREMENT("(global)::MainMenu_InitMainList_Hook", "Needs a valid main-menu pointer. Safe if there's no movie loaded yet, or if the original function was never resolved.");
        __int64 result = g_origInitMainList ? g_origInitMainList(a1) : 0;

        // IMenu::movie is at offset 0x40 (STATIC_ASSERT'd in GameMenus.h)
        GFxMovieView* movie = *(GFxMovieView**)((UInt8*)a1 + 0x40);

        InjectMultiplayerEntry(movie);

        return result;
    }

    // Per-frame MainMenu update (vtable index 0x10). Exists purely to give
    // PollMultiplayerRowActivation a frame-rate driver -- the injection itself
    // stays on the InitMainList hook above, which is the correct place for it.
    using _MenuUpdate_t = bool(__fastcall*)(void* a1);
    _MenuUpdate_t g_origMainMenuUpdate = nullptr;

    bool __fastcall MainMenu_Update_Hook(void* a1)
    {
        LOG_REQUIREMENT("(global)::MainMenu_Update_Hook", "Needs a valid main-menu pointer. Safe if there's no movie loaded yet, or if the original function was never resolved.");
        bool result = g_origMainMenuUpdate ? g_origMainMenuUpdate(a1) : false;

        // IMenu::movie is at offset 0x40 (STATIC_ASSERT'd in GameMenus.h)
        GFxMovieView* movie = *(GFxMovieView**)((UInt8*)a1 + 0x40);

        if (movie && movie->movieRoot)
        {
            PollMultiplayerRowActivation(movie->movieRoot);
            // At the main menu CoSync's tick may not run: keep the loading
            // screen's state current from here too (join download).
            CoSyncLoading::Tick();
        }

        return result;
    }

    void InstallMainMenuUpdateHook()
    {
        LOG_REQUIREMENT("(global)::InstallMainMenuUpdateHook", "Needs the game's main-menu update function to be at the address this build expects. Only meant to be called once.");
        uintptr_t* slot = (uintptr_t*)(RelocationManager::s_baseAddr + kMainMenuVtableRVA
            + kMenuUpdateVtableIndex * sizeof(uintptr_t));

        uintptr_t origFn = *slot;
        if (!origFn)
        {
            LOG_ERROR("[CoSyncMenuInject] MainMenu update vtable slot is NULL, cannot hook");
            return;
        }

        if (origFn == reinterpret_cast<uintptr_t>(&MainMenu_Update_Hook))
        {
            LOG_WARN("[CoSyncMenuInject] MainMenu update vtable already patched");
            return;
        }

        g_origMainMenuUpdate = reinterpret_cast<_MenuUpdate_t>(origFn);

        DWORD oldProt{};
        VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
        *slot = reinterpret_cast<uintptr_t>(&MainMenu_Update_Hook);
        VirtualProtect(slot, sizeof(uintptr_t), oldProt, &oldProt);

        LOG_INFO("[CoSyncMenuInject] MainMenu per-frame update vtable hook installed (slot=%p orig=%p)",
            (void*)slot, (void*)origFn);
    }

    void InstallInitMainListHook()
    {
        LOG_REQUIREMENT("(global)::InstallInitMainListHook", "Needs the game's main-menu setup function to be at the address this build expects. Only meant to be called once.");
        uintptr_t* slot = (uintptr_t*)(RelocationManager::s_baseAddr + kMainMenuVtableRVA
            + kInitMainListVtableIndex * sizeof(uintptr_t));

        uintptr_t origFn = *slot;
        if (!origFn)
        {
            LOG_ERROR("[CoSyncMenuInject] InitMainList vtable slot is NULL, cannot hook");
            return;
        }

        if (origFn == reinterpret_cast<uintptr_t>(&MainMenu_InitMainList_Hook))
        {
            LOG_WARN("[CoSyncMenuInject] InitMainList vtable already patched");
            return;
        }

        g_origInitMainList = reinterpret_cast<_InitMainList_t>(origFn);

        DWORD oldProt{};
        VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
        *slot = reinterpret_cast<uintptr_t>(&MainMenu_InitMainList_Hook);
        VirtualProtect(slot, sizeof(uintptr_t), oldProt, &oldProt);

        LOG_INFO("[CoSyncMenuInject] MainMenu::InitMainList vtable hook installed (slot=%p orig=%p)",
            (void*)slot, (void*)origFn);
    }

    // Looks up "MainMenu" in the menu table right now and injects if it's
    // already open and the row isn't there yet. Used both as a one-shot
    // catch-up at Init() time (the game's title-screen MainMenu opens long
    // before our GameLoaded-time Init() runs, so we'd otherwise miss its
    // MenuOpenCloseEvent entirely) and from the sink for any later opens.
    void TryInjectIntoMainMenu()
    {
        LOG_REQUIREMENT("(global)::TryInjectIntoMainMenu", "Needs the game's UI system to be up. Safe if the main menu isn't open yet -- just logs it and exits.");
        if (!g_ui_fixed || !*g_ui_fixed)
        {
            LOG_WARN("[CoSyncMenuInject] TryInjectIntoMainMenu: g_ui_fixed not ready");
            return;
        }

        LOG_DEBUG("[CoSyncMenuInject] TryInjectIntoMainMenu: g_ui_fixed slot=0x%llX baseAddr=0x%llX *g_ui_fixed=%p",
            (unsigned long long)g_ui_fixed.GetUIntPtr(),
            (unsigned long long)RelocationManager::s_baseAddr,
            (void*)*g_ui_fixed);

        BSFixedString mainMenuName("MainMenu");
        IMenu* menu = CoSync_GetMenu(*g_ui_fixed, mainMenuName);
        if (!menu)
        {
            LOG_WARN("[CoSyncMenuInject] TryInjectIntoMainMenu: MainMenu not in menuTable");
            return;
        }
        if (!menu->movie)
        {
            LOG_WARN("[CoSyncMenuInject] TryInjectIntoMainMenu: MainMenu has no movie");
            return;
        }

        InjectMultiplayerEntry(menu->movie);
    }

    class CoSyncMainMenuOpenSink : public BSTEventSink<MenuOpenCloseEvent>
    {
    public:
        EventResult ReceiveEvent(MenuOpenCloseEvent* evn, void* dispatcher) override
        {
            LOG_REQUIREMENT("CoSyncMainMenuOpenSink::ReceiveEvent", "Safe to call with an empty event -- just logs it and continues.");
            if (!evn)
            {
                LOG_WARN("[CoSyncMenuInject] ReceiveEvent: null event");
                return kEvent_Continue;
            }

            LOG_DEBUG("[CoSyncMenuInject] ReceiveEvent: menu='%s' isOpen=%d",
                evn->menuName.c_str(), (int)evn->isOpen);

            if (!evn->isOpen)
                return kEvent_Continue;

            if (!(evn->menuName == "MainMenu"))
                return kEvent_Continue;

            TryInjectIntoMainMenu();
            return kEvent_Continue;
        }
    };

    CoSyncMainMenuOpenSink g_mainMenuOpenSink;

    // f4se's BSTEventDispatcher<T>::AddEventSink reimplements tArray::Insert,
    // which on growth calls Heap_Allocate/Heap_Free through f4se's g_mainHeap
    // (RVA 0x03E5E170). That RVA is stale for FO4 NG 1.11.221 -- verified via
    // IDA: it has zero xrefs and sits in 0xFF-filled padding, not a real
    // global. Calling through it corrupts the engine's real heap
    // (STATUS_HEAP_CORRUPTION, 0xC0000374) the moment the sink array grows.
    //
    // Instead, call the game's own RegisterSink<MenuOpenCloseEvent> directly
    // (RVA 0x00665EF0, confirmed via IDA -- every built-in menu class calls
    // this same function on construction with dispatcher = (*g_ui_fixed)+0x18,
    // which f4se's "// 70" comment for menuOpenCloseEventSource notwithstanding,
    // is the correct compiled offset -- vtable(0x00) + unk08(0x08) + unk10(0x10)).
    // This uses the engine's own allocator bookkeeping, sidestepping the bad
    // g_mainHeap RVA entirely.
    typedef void(*_RegisterMenuOpenCloseSink_t)(void* dispatcher, void* sink);
    RelocAddr<_RegisterMenuOpenCloseSink_t> UI_RegisterMenuOpenCloseSink(0x00666210);

    // ── PauseMenu DISCONNECT Injection ───────────────────────────────────────
    //
    // Runtime RTTI scan for PauseMenu's primary vtable.
    //
    // MSVC x64 RTTI layout:
    //   TypeDescriptor  (.data)  — [vftable*][spare*][mangled name string]
    //   COL             (.rdata) — [sig=1][off][cdOff][typeDescRVA][hierRVA][selfRVA]
    //   vtable          (.rdata) — [... &COL ...][fn0][fn1]...
    //
    // TypeDescriptor lives in .data (not .rdata) because its vftable pointer field
    // is patched by the CRT at load time, making the struct non-const. The COL and
    // vtable are fully const and live in .rdata.  Step 1 must therefore scan the
    // full image, not just .rdata.
    static uintptr_t* FindVtableByRTTI(const char* mangledName)
    {
        LOG_REQUIREMENT("(global)::FindVtableByRTTI", "Needs a real class name to search for. Returns nothing if that name can't be found in the game's own executable.");
        uintptr_t base = RelocationManager::s_baseAddr;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

        uintptr_t imageEnd  = base + nt->OptionalHeader.SizeOfImage;
        uintptr_t rdataBase = 0, rdataEnd = 0;

        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        {
            char sname[9] = {};
            memcpy(sname, sec->Name, 8);
            if (!strcmp(sname, ".rdata"))
            {
                rdataBase = base + sec->VirtualAddress;
                rdataEnd  = rdataBase + sec->Misc.VirtualSize;
                break;
            }
        }
        if (!rdataBase) return nullptr;

        // Step 1 — find the decorated type name string anywhere in the image.
        // TypeDescriptor is in .data (CRT patches its vftable ptr at startup),
        // so restricting to .rdata misses it.
        size_t    nameLen  = strlen(mangledName);
        uintptr_t nameAddr = 0;
        for (uintptr_t p = base; p + nameLen < imageEnd; ++p)
        {
            if (*reinterpret_cast<const char*>(p) != mangledName[0]) continue;
            if (memcmp(reinterpret_cast<void*>(p), mangledName, nameLen + 1) == 0)
            {
                nameAddr = p;
                break;
            }
        }
        if (!nameAddr)
        {
            LOG_ERROR("[CoSyncMenuInject] RTTI: '%s' not found in image", mangledName);
            return nullptr;
        }

        // Step 2 — TypeDescriptor starts 0x10 bytes before the name string.
        uintptr_t typeDesc    = nameAddr - 0x10;
        DWORD     typeDescRVA = static_cast<DWORD>(typeDesc - base);

        // Step 3 — scan .rdata for the COL that references this TypeDescriptor.
        // x64 COL: DWORD[0]=1 (sig), DWORD[3]=typeDescRVA.
        uintptr_t colAddr = 0;
        for (uintptr_t p = rdataBase; p + 24 <= rdataEnd; p += 4)
        {
            auto* d = reinterpret_cast<DWORD*>(p);
            if (d[0] == 1 && d[3] == typeDescRVA)
            {
                colAddr = p;
                break;
            }
        }
        if (!colAddr)
        {
            LOG_ERROR("[CoSyncMenuInject] RTTI: COL for '%s' not found (typeDescRVA=0x%08X)",
                mangledName, typeDescRVA);
            return nullptr;
        }

        // Step 4 — the vtable immediately follows &COL in .rdata.
        for (uintptr_t p = rdataBase; p + 8 <= rdataEnd; p += 8)
        {
            if (*reinterpret_cast<uintptr_t*>(p) == colAddr)
                return reinterpret_cast<uintptr_t*>(p + 8);
        }

        LOG_ERROR("[CoSyncMenuInject] RTTI: vtable for '%s' not found (COL=%p)",
            mangledName, reinterpret_cast<void*>(colAddr));
        return nullptr;
    }

    constexpr double kDisconnectIndex = 99.0;
    // 0.3.3 (Stage 8): the Multiplayer menu lives in the Pause menu. Index 98
    // is unused by vanilla's PauseMenu list, like DISCONNECT's 99.
    constexpr double kPauseMultiplayerIndex = 98.0;

    void InjectPauseMultiplayerEntry(GFxMovieView* view)
    {
        if (!view || !view->movieRoot) return;
        GFxMovieRoot* root = view->movieRoot;

        GFxValue entryList;
        if (!root->GetVariable(&entryList, "root.Menu_mc.MainPanel_mc.List_mc.entryList")
            || !entryList.IsArray())
            return;

        const UInt32 count = entryList.GetArraySize();
        if (count == 0)
            return;   // vanilla has not filled the list yet (same rule as MainMenu)
        for (UInt32 i = 0; i < count; ++i)
        {
            GFxValue elem;
            if (!entryList.GetElement(i, &elem) || !elem.IsObject()) continue;
            GFxValue idx;
            if (!elem.GetMember("index", &idx)) continue;
            if (idx.GetNumber() == kPauseMultiplayerIndex) return;   // already there
        }

        GFxValue entry;
        root->CreateObject(&entry);
        GFxValue text; root->CreateString(&text, "MULTIPLAYER");
        entry.SetMember("text", &text);
        GFxValue indexVal(kPauseMultiplayerIndex);
        entry.SetMember("index", &indexVal);
        if (!entryList.PushBack(&entry))
        {
            LOG_ERROR("[CoSyncMenuInject] PauseMenu: PushBack MULTIPLAYER failed");
            return;
        }
        root->Invoke("root.Menu_mc.MainPanel_mc.List_mc.InvalidateData", nullptr, nullptr, 0);
        LOG_INFO("[CoSyncMenuInject] MULTIPLAYER row injected into PauseMenu");
    }

    void InjectDisconnectEntry(GFxMovieView* view)
    {
        LOG_REQUIREMENT("(global)::InjectDisconnectEntry", "Only does anything while actually connected to a session. Safe if the menu list isn't available.");
        if (!view || !view->movieRoot) return;
        if (!CoSyncTransport::IsInitialized())  return;   // only when in a session

        GFxMovieRoot* root = view->movieRoot;

        GFxValue entryList;
        if (!root->GetVariable(&entryList, "root.Menu_mc.MainPanel_mc.List_mc.entryList")
            || !entryList.IsArray())
            return;

        // Idempotency — skip if already injected
        UInt32 count = entryList.GetArraySize();
        for (UInt32 i = 0; i < count; ++i)
        {
            GFxValue elem;
            if (!entryList.GetElement(i, &elem) || !elem.IsObject()) continue;
            GFxValue idx;
            if (!elem.GetMember("index", &idx)) continue;
            if (idx.GetNumber() == kDisconnectIndex) return;
        }

        GFxValue entry;
        root->CreateObject(&entry);
        GFxValue text; root->CreateString(&text, "DISCONNECT");
        entry.SetMember("text", &text);
        GFxValue indexVal(kDisconnectIndex);
        entry.SetMember("index", &indexVal);

        if (!entryList.PushBack(&entry))
        {
            LOG_ERROR("[CoSyncMenuInject] PauseMenu: PushBack DISCONNECT failed");
            return;
        }

        root->Invoke("root.Menu_mc.MainPanel_mc.List_mc.InvalidateData",
            nullptr, nullptr, 0);
        LOG_INFO("[CoSyncMenuInject] DISCONNECT row injected into PauseMenu");
    }

    void PollPauseMenuDisconnect(GFxMovieRoot* root)
    {
        LOG_REQUIREMENT("(global)::PollPauseMenuDisconnect", "Safe with a missing menu root or missing menu pieces -- just exits early. Picking Disconnect always shuts down the session.");
        if (!root) return;
        if (!IsActivatePressed()) return;

        GFxValue listMc;
        if (!root->GetVariable(&listMc, "root.Menu_mc.MainPanel_mc.List_mc")
            || !listMc.IsObject())
            return;

        GFxValue selIdx;
        if (!listMc.GetMember("selectedIndex", &selIdx) || !IsNumericType(selIdx))
            return;

        double selIdxNum = selIdx.GetNumber();
        if (selIdxNum < 0) return;

        GFxValue entryList;
        if (!listMc.GetMember("entryList", &entryList) || !entryList.IsArray())
            return;

        GFxValue elem;
        if (!entryList.GetElement(static_cast<UInt32>(selIdxNum), &elem)
            || !elem.IsObject())
            return;

        GFxValue idx;
        if (!elem.GetMember("index", &idx)) return;
        if (idx.GetNumber() == kPauseMultiplayerIndex)
        {
            LOG_INFO("[CoSyncMenuInject] MULTIPLAYER activated from PauseMenu");
            CoSyncPrismaUI::RequestOpenMultiplayer(false);
            return;
        }
        if (idx.GetNumber() != kDisconnectIndex) return;

        // The Enter key path (CoSyncMenuInject_PauseMenuKeyActivate) can fire
        // for the same press: whichever runs first ends the session.
        if (!CoSyncTransport::IsInitialized())
            return;
        LOG_INFO("[CoSyncMenuInject] DISCONNECT activated from PauseMenu");
        cosync::CoSync_Main::Get().Shutdown();
        CoSyncOverlay_Disconnect();
    }

    _InitMainList_t g_origPauseMenuInitMainList = nullptr;

    __int64 __fastcall PauseMenu_InitMainList_Hook(void* a1)
    {
        LOG_REQUIREMENT("(global)::PauseMenu_InitMainList_Hook", "Needs a valid pause-menu pointer. Safe if there's no movie loaded yet, or if the original function was never resolved.");
        __int64 result = g_origPauseMenuInitMainList
            ? g_origPauseMenuInitMainList(a1) : 0;

        GFxMovieView* movie = *reinterpret_cast<GFxMovieView**>(
            reinterpret_cast<UInt8*>(a1) + 0x40);

        InjectPauseMultiplayerEntry(movie);
        InjectDisconnectEntry(movie);

        // Per-frame path: also poll here in case slot 0x15 fires continuously
        // for PauseMenu (as it does for MainMenu). If it only fires on open,
        // the WndProc path in CoSyncMenuInject_PauseMenuKeyActivate() picks up
        // the Enter press instead.
        if (movie && movie->movieRoot)
            PollPauseMenuDisconnect(movie->movieRoot);

        return result;
    }

    void InstallPauseMenuHook()
    {
        LOG_REQUIREMENT("(global)::InstallPauseMenuHook", "Needs the pause menu's setup function to be found in the game's own executable. Only meant to be called once.");
        uintptr_t* vtable = FindVtableByRTTI(".?AVPauseMenu@@");
        if (!vtable)
        {
            LOG_ERROR("[CoSyncMenuInject] PauseMenu vtable not found via RTTI scan");
            return;
        }

        uintptr_t* slot   = vtable + kInitMainListVtableIndex;
        uintptr_t  origFn = *slot;

        if (!origFn)
        {
            LOG_ERROR("[CoSyncMenuInject] PauseMenu InitMainList slot is null");
            return;
        }
        if (origFn == reinterpret_cast<uintptr_t>(&PauseMenu_InitMainList_Hook))
        {
            LOG_WARN("[CoSyncMenuInject] PauseMenu InitMainList already hooked");
            return;
        }

        g_origPauseMenuInitMainList = reinterpret_cast<_InitMainList_t>(origFn);

        DWORD oldProt{};
        VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
        *slot = reinterpret_cast<uintptr_t>(&PauseMenu_InitMainList_Hook);
        VirtualProtect(slot, sizeof(uintptr_t), oldProt, &oldProt);

        LOG_INFO("[CoSyncMenuInject] PauseMenu::InitMainList hooked "
            "(vtable=%p slot=%p orig=%p)",
            static_cast<void*>(vtable),
            static_cast<void*>(slot),
            reinterpret_cast<void*>(origFn));
    }
}

// Called from HookedWndProc (main thread) on every WM_KEYDOWN / VK_RETURN
// while the game is running. Reads the currently-selected PauseMenu list
// entry directly from the cached GFxMovieRoot (safe: same thread as GFx).
// Fires disconnect if the DISCONNECT row (index 99) is highlighted.
void CoSyncMenuInject_OnMultiplayerHotkey()
{
    LOG_REQUIREMENT("(global)::CoSyncMenuInject_OnMultiplayerHotkey", "Safe from any thread: sets an atomic flag or hands off to the PrismaUI dispatcher.");
    if (!CoSyncWorld::IsWorldReady())
        g_multiplayerToggleRequested.store(true);   // main menu: handled by the MainMenu poll
    else
        CoSyncPrismaUI::RequestToggleMultiplayer();
}

void CoSyncMenuInject_PauseMenuKeyActivate()
{
    LOG_REQUIREMENT("(global)::CoSyncMenuInject_PauseMenuKeyActivate", "Needs the game's UI system to be up. MULTIPLAYER works any time; DISCONNECT only while connected. Safe if the pause menu isn't open or its pieces are missing.");
    if (!g_ui_fixed || !*g_ui_fixed)   return;

    // Look the menu up live from the engine's menu table instead of trusting
    // a cached GFxMovieRoot*: the cache went stale (and crashed) whenever
    // PauseMenu closed/reopened/reinitialized without us observing it. The
    // menu table is the engine's own source of truth, so this can't go stale.
    BSFixedString pauseMenuName("PauseMenu");
    IMenu* menu = CoSync_GetMenu(*g_ui_fixed, pauseMenuName);
    if (!menu || !menu->movie || !menu->movie->movieRoot)
        return;

    GFxMovieRoot* root = menu->movie->movieRoot;

    GFxValue listMc;
    if (!root->GetVariable(&listMc, "root.Menu_mc.MainPanel_mc.List_mc")
        || !listMc.IsObject())
        return;

    GFxValue selIdx;
    if (!listMc.GetMember("selectedIndex", &selIdx))
        return;
    if (selIdx.GetType() != GFxValue::kType_Number
        && selIdx.GetType() != GFxValue::kType_Int)
        return;

    double selIdxNum = selIdx.GetNumber();
    if (selIdxNum < 0) return;

    GFxValue entryList;
    if (!listMc.GetMember("entryList", &entryList) || !entryList.IsArray())
        return;

    GFxValue elem;
    if (!entryList.GetElement(static_cast<UInt32>(selIdxNum), &elem)
        || !elem.IsObject())
        return;

    GFxValue idx;
    if (!elem.GetMember("index", &idx)) return;
    if (idx.GetNumber() == 98.0)
    {
        LOG_INFO("[CoSyncMenuInject] MULTIPLAYER activated via WndProc (selIdx=%.0f)", selIdxNum);
        CoSyncPrismaUI::RequestOpenMultiplayer(false);
        return;
    }
    if (idx.GetNumber() != 99.0) return;
    if (!CoSyncTransport::IsInitialized())  return;

    LOG_INFO("[CoSyncMenuInject] DISCONNECT activated via WndProc (selIdx=%.0f)", selIdxNum);
    cosync::CoSync_Main::Get().Shutdown();
    CoSyncOverlay_Disconnect();
}

void CoSyncMenuInject_InstallHooks()
{
    LOG_REQUIREMENT("(global)::CoSyncMenuInject_InstallHooks", "Just calls the three individual menu-hook installers. Meant to be called once, as early as possible during plugin load.");
    InstallInitMainListHook();
    InstallMainMenuUpdateHook();
    InstallPauseMenuHook();
}

void CoSyncMenuInject_Init()
{
    LOG_REQUIREMENT("(global)::CoSyncMenuInject_Init", "Needs the game's UI system to be up. Meant to be called once, after game data has finished loading.");
    if (!g_ui_fixed || !*g_ui_fixed)
    {
        LOG_ERROR("[CoSyncMenuInject] g_ui_fixed not ready, cannot register MainMenu sink");
        return;
    }

    // No MenuOpenCloseEvent sink: UI_RegisterMenuOpenCloseSink's RVA
    // (0x00666210) is an empty `ret` on 1.11.240 (IDA 2026-09-26) and the
    // dispatcher offset was wrong too, so it never registered anything. The
    // MULTIPLAYER row is injected by the MainMenu::InitMainList hook instead.

    // MainMenu's title-screen open event fires before this Init() runs
    // (GameLoaded), so do an immediate catch-up check/injection here.
    TryInjectIntoMainMenu();
}

void CoSyncMenuInject_OnScaleformView(GFxMovieView* view)
{
    LOG_REQUIREMENT("(global)::CoSyncMenuInject_OnScaleformView", "Gets called for every UI movie the game loads, not just the main menu -- safely does nothing for the ones we don't care about.");
    if (!view || !view->movieRoot)
        return;

    GFxMovieRoot* root = view->movieRoot;

    // root.Menu_mc.MainPanel_mc.List_mc only exists on the real MainMenu --
    // safe, cheap way to identify it without dumping the whole display list.
    GFxValue listMc;
    if (!root->GetVariable(&listMc, "root.Menu_mc.MainPanel_mc.List_mc") || !listMc.IsObject())
        return;

    // Purely synchronous, read-only probe. CRASH #2/#3/#4 all shared the same
    // RIP and were caused by addEventListener("enterFrame", CreateFunction<...>)
    // at registration time -- not by VisitMembers, not by the handler body, not
    // by the target/arg-count. So: no CreateFunction, no addEventListener, no
    // new GFxFunctionHandler types here. OnScaleformRegister fires ~3x per
    // session for Menu_mc-containing movies; log entryList.size on every call
    // (don't gate to first-time-only) to see whether vanilla populates it
    // lazily across re-registrations.
    GFxValue entryList;
    if (!root->GetVariable(&entryList, "root.Menu_mc.MainPanel_mc.List_mc.entryList") || !entryList.IsArray())
    {
        LOG_INFO("[CoSyncMenuInject] MainMenu detected, entryList not array yet");
        return;
    }

    UInt32 count = entryList.GetArraySize();
    LOG_INFO("[CoSyncMenuInject] MainMenu detected, entryList size=%u", count);

    if (count == 0)
    {
        // Vanilla hasn't populated entryList yet on this registration -- CRASH #1
        // happened because we pushed {text,index} into a still-empty array and
        // vanilla's renderer choked on it ~7s later once it tried to populate
        // around our entry. PDF reference confirms the entry shape
        // {text, index, disabled?} is correct and index=15 is unused by vanilla
        // (CONTINUE..INSTALLED_CONTENT = 0-14) -- the shape was never the problem,
        // the timing was. OnScaleformRegister fires ~3x per session for
        // Menu_mc-containing movies (per 4th-test display-list dump); a later
        // registration should see a populated entryList.
        return;
    }

    InjectMultiplayerEntry(view);
}
