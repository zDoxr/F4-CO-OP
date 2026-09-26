#pragma once

// f4se's BSTEventDispatcher<T>::AddEventSink (GameEvents.h) reimplements
// tArray::Insert, which on array growth allocates through f4se's g_mainHeap
// (GameAPI.cpp, RVA 0x03E5E170). That RVA is stale for FO4 NG 1.11.221 --
// confirmed via IDA: zero xrefs, raw 0xFF bytes sitting in .data, not a real
// global. Any AddEventSink call that needs to grow the array (always true on
// the FIRST registration, when capacity is 0) silently corrupts the engine's
// real heap instead of crashing immediately. The corruption doesn't surface
// where it happens -- it sits in wrecked allocator bookkeeping until the next
// unrelated allocation/free disturbs that same region.
//
// This was already found and fixed once for MenuOpenCloseEvent
// (CoSyncMenuInject.cpp), by calling the engine's own native registration
// function directly instead of f4se's broken wrapper. This header generalizes
// that fix: confirmed via IDA decompile + disassembly of
// UI_RegisterMenuOpenCloseSink (RVA 0x665EF0) that it grows the array through
// sub_141659520 (RVA 0x1659520), the engine's own generic tArray-growth
// primitive -- 8827 xrefs across 5760 distinct callers, not specific to any
// one event type -- using a BSTArrayAllocatorFunctor<BSTArrayHeapAllocator>
// vtable call (RVA 0x245FFB0) instead of any hardcoded heap RVA. That vtable
// call resolves the engine's real allocator correctly regardless of build,
// because it goes through the same compiled vtable the engine itself uses --
// there's no separate stale constant to go wrong.
//
// tArray<T>'s field layout (GameTypes.h: entries@0x00, capacity@0x08,
// count@0x10) is binary-compatible with the engine's real tArray, confirmed
// by cross-referencing the disassembly's offsets (r14+0x00=entries,
// r14+0x08=capacity, r14+0x10=count) -- so this can operate directly on our
// existing BSTEventDispatcher<T>::eventSinks field with no translation.
//
// Use SafeAddEventSink instead of BSTEventDispatcher<T>::AddEventSink for
// every new sink registration in CoSync. RemoveEventSink is NOT affected by
// this bug -- tArray::Remove (GameTypes.h) only frees/reallocates when
// capacity exceeds count by more than nShrink (default 10), which never
// happens for the small, single-digit sink counts CoSync registers.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "GameEvents.h"

#include <cstdint>

namespace CoSyncEventSinkSafety
{
    struct ArrayAllocatorFunctor
    {
        void* vtable;
        void* arrayBase;  // &tArray<T> itself (== &entries, offset 0)
    };

    constexpr uintptr_t kArrayAllocatorFunctorVtbl_RVA = 0x0245FFB0;
    constexpr uintptr_t kArrayGrow_RVA                 = 0x01659840;

    // UInt32 (GameTypes.h's ITypes.h) is `unsigned long`, not `unsigned int` --
    // both are 4 bytes on Win64/LLP64, but C++ treats the pointer types as
    // distinct, so this must match tArray<T>::count/capacity's real type
    // exactly or the call below won't compile against &arr.count/arr.capacity.
    using ArrayGrowFn = unsigned int(__fastcall*)(UInt32* countPtr,
                                                   ArrayAllocatorFunctor* functor,
                                                   UInt32 capacity,
                                                   UInt32 elemSize);

    template <typename EventT>
    bool SafeAddEventSink(BSTEventDispatcher<EventT>* dispatcher, BSTEventSink<EventT>* sink)
    {
        if (!dispatcher || !sink)
            return false;

        SimpleLocker locker(&dispatcher->lock);

        auto& arr = dispatcher->eventSinks;

        for (UInt32 i = 0; i < arr.count; i++)
        {
            if (arr.entries[i] == sink)
                return false;  // already registered
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));

        ArrayAllocatorFunctor functor;
        functor.vtable    = reinterpret_cast<void*>(base + kArrayAllocatorFunctorVtbl_RVA);
        functor.arrayBase = &arr;

        auto grow = reinterpret_cast<ArrayGrowFn>(base + kArrayGrow_RVA);
        unsigned int idx = grow(&arr.count, &functor, arr.capacity, static_cast<UInt32>(sizeof(void*)));

        if (idx == 0xFFFFFFFFu)
            return false;  // growth failed (out of memory)

        // Re-read entries AFTER the grow call -- the buffer may have moved.
        arr.entries[idx] = sink;
        return true;
    }
}
