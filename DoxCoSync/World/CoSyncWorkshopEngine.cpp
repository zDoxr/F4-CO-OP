#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncWorkshopEngine.h"

#include "ConsoleLogger.h"
#include "CoSyncRequirementsLog.h"

#include "GameReferences.h"

#include <cstdint>

// Every RVA and offset here is 1.11.240 and is cross-checked in
// docs/WORKSHOP_FACTIONS_RE.md (IDA + CommonLibF4/Address Library + F4SE).
namespace
{
    // Globals
    constexpr uintptr_t kRVA_HandleTable        = 0x30E5490;  // qword: BSPointerHandle entry array
    constexpr uintptr_t kRVA_CurrentWorkshop    = 0x30F7698;  // u32 handle
    constexpr uintptr_t kRVA_PlacementItem      = 0x30F7680;  // u32 handle
    constexpr uintptr_t kRVA_WorkshopItemKeyword= 0x30F7818;  // BGSKeyword*
    constexpr uintptr_t kRVA_WorkshopKeyword    = 0x30F77E0;  // BGSKeyword*

    // Functions
    constexpr uintptr_t kRVA_FindNearestValidWorkshop = 0x385720;
    constexpr uintptr_t kRVA_SetLinkedRef       = 0x5648D0;   // (ref, target, kw)
    constexpr uintptr_t kRVA_GetLinkedRef       = 0x282F40;   // (extraList, kw, bool)
    constexpr uintptr_t kRVA_PlacedSource       = 0x3A9740;
    constexpr uintptr_t kRVA_PlacedNotify       = 0x3C0540;
    constexpr uintptr_t kRVA_DestroyedSource    = 0x3A94D0;
    constexpr uintptr_t kRVA_DestroyedNotify    = 0x3BFD90;
    constexpr uintptr_t kRVA_MovedSource        = 0x3A9670;
    constexpr uintptr_t kRVA_MovedNotify        = 0x3C02B0;

    // TESObjectREFR offsets (F4SE 0.7.9 GameReferences.h, and the engine's
    // own reads in the functions above).
    constexpr size_t kRefKeywordForm = 0x50;   // IKeywordFormBase
    constexpr size_t kRefExtraList   = 0x100;
    constexpr size_t kRefHandleBits  = 0x28;   // refcount (low 10) | handle index << 11

    // Handle bits (identical in every decompiled resolve).
    constexpr uint32_t kHandleIndexMask = 0x1FFFFF;
    constexpr uint32_t kHandleGenMask   = 0x3E00000;
    constexpr uint32_t kHandleValidBit  = 0x4000000;

    // Event payload shared by ItemPlaced/Moved/Destroyed: {workshop, item}.
    struct WorkshopItemEvent
    {
        TESObjectREFR* workshop;
        TESObjectREFR* item;
    };

    uintptr_t Base()
    {
        static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return b;
    }

    template <class T> T At(uintptr_t rva) { return reinterpret_cast<T>(Base() + rva); }

    using FindNearest_t   = TESObjectREFR* (*)(TESObjectREFR*);
    using SetLinkedRef_t  = void (*)(TESObjectREFR*, TESObjectREFR*, void*);
    using GetLinkedRef_t  = TESObjectREFR* (*)(void*, void*, bool);
    using EventSource_t   = void* (*)();
    using EventNotify_t   = void (*)(void*, const WorkshopItemEvent*);
    using HasKeyword_t    = bool (*)(void*, void*, uint32_t);

    __declspec(noinline) TESObjectREFR* ResolveHandleRaw(uint32_t handle)
    {
        __try
        {
            if (handle == 0)
                return nullptr;
            const uintptr_t table = *At<uintptr_t*>(kRVA_HandleTable);
            if (!table)
                return nullptr;
            const uintptr_t entry = table + 16ull * (handle & kHandleIndexMask);
            const uint32_t  flags = *reinterpret_cast<const uint32_t*>(entry);
            const uintptr_t obj   = *reinterpret_cast<const uintptr_t*>(entry + 8);
            if (!obj)
                return nullptr;
            const uintptr_t ref = obj - 32;
            if ((flags & kHandleValidBit) == 0 ||
                (flags & kHandleGenMask) != (handle & kHandleGenMask) ||
                (*reinterpret_cast<const uint32_t*>(ref + kRefHandleBits) >> 11) != (handle & kHandleIndexMask))
                return nullptr;
            return reinterpret_cast<TESObjectREFR*>(ref);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    __declspec(noinline) bool HasKeywordRaw(TESObjectREFR* ref, void* kw)
    {
        __try
        {
            if (!ref || !kw)
                return false;
            void* ifc = reinterpret_cast<uint8_t*>(ref) + kRefKeywordForm;
            void** vtbl = *reinterpret_cast<void***>(ifc);
            return reinterpret_cast<HasKeyword_t>(vtbl[1])(ifc, kw, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) void DispatchRaw(uintptr_t sourceRva, uintptr_t notifyRva, TESObjectREFR* ws, TESObjectREFR* item, const char* what)
    {
        __try
        {
            void* src = At<EventSource_t>(sourceRva)();
            if (!src)
                return;
            WorkshopItemEvent ev{ ws, item };
            At<EventNotify_t>(notifyRva)(src, &ev);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[WorkshopEngine] %s dispatch faulted -- caught", what);
        }
    }
}

namespace CoSyncWorkshopEngine
{
    TESObjectREFR* ResolveHandle(uint32_t handle)
    {
        return ResolveHandleRaw(handle);
    }

    TESObjectREFR* GetCurrentWorkshop()
    {
        return ResolveHandleRaw(*At<volatile uint32_t*>(kRVA_CurrentWorkshop));
    }

    TESObjectREFR* GetPlacementItem()
    {
        return ResolveHandleRaw(*At<volatile uint32_t*>(kRVA_PlacementItem));
    }

    bool IsWorkshopRef(TESObjectREFR* ref)
    {
        return HasKeywordRaw(ref, *At<void**>(kRVA_WorkshopKeyword));
    }

    __declspec(noinline) TESObjectREFR* GetLinkedWorkshop(TESObjectREFR* item)
    {
        __try
        {
            if (!item)
                return nullptr;
            void* extra = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(item) + kRefExtraList);
            void* kw = *At<void**>(kRVA_WorkshopItemKeyword);
            if (!extra || !kw)
                return nullptr;
            return At<GetLinkedRef_t>(kRVA_GetLinkedRef)(extra, kw, true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    __declspec(noinline) bool SetLinkedWorkshop(TESObjectREFR* item, TESObjectREFR* workshop)
    {
        __try
        {
            void* kw = *At<void**>(kRVA_WorkshopItemKeyword);
            if (!item || !kw)
                return false;
            At<SetLinkedRef_t>(kRVA_SetLinkedRef)(item, workshop, kw);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_WARN("[WorkshopEngine] SetLinkedRef faulted -- caught");
            return false;
        }
    }

    __declspec(noinline) TESObjectREFR* FindNearestValidWorkshop(TESObjectREFR* from)
    {
        __try
        {
            return from ? At<FindNearest_t>(kRVA_FindNearestValidWorkshop)(from) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void DispatchItemPlaced(TESObjectREFR* ws, TESObjectREFR* item)
    {
        if (ws && item) DispatchRaw(kRVA_PlacedSource, kRVA_PlacedNotify, ws, item, "ItemPlaced");
    }

    void DispatchItemDestroyed(TESObjectREFR* ws, TESObjectREFR* item)
    {
        if (ws && item) DispatchRaw(kRVA_DestroyedSource, kRVA_DestroyedNotify, ws, item, "ItemDestroyed");
    }

    void DispatchItemMoved(TESObjectREFR* ws, TESObjectREFR* item)
    {
        if (ws && item) DispatchRaw(kRVA_MovedSource, kRVA_MovedNotify, ws, item, "ItemMoved");
    }
}
