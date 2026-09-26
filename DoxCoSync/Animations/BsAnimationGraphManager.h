#pragma once

#include <cstdint>
#include <Windows.h>
#include "CoSyncRequirementsLog.h"

struct BShkbAnimationGraph;

#ifndef COSYNC_BSTSMALLARRAY_DEFINED
#define COSYNC_BSTSMALLARRAY_DEFINED
template <class T>
struct BSTSmallArray
{
    int32_t capacity;
    T** data;
    uint32_t size;

    T* Get(uint32_t aIndex)
    {
        LOG_REQUIREMENT("BSTSmallArray::Get", "The index must be within the array's real element count. This struct's own layout has to match the game engine's real array layout for this game version.");

        // The engine's graph array is transiently empty during a graph
        // rebuild (e.g. equip/unequip) -- every caller already null-checks
        // the return, so bounds-checking here (rather than trusting every
        // call site to check `size` first) is what actually enforces the
        // precondition above instead of just documenting it. Without this,
        // Get(0) on an empty array dereferenced a null/dangling `data`,
        // which was the real cause of the recurring GetVariableSetDirect
        // crash-loop (thousands of AVs/session, every caller hit it).
        if (aIndex >= size || !data)
            return nullptr;

        auto pData = reinterpret_cast<T**>(&data);
        if (capacity >= 0)
            pData = data;
        return pData[aIndex];
    }
};
#endif

#ifndef COSYNC_BSRECURSIVELOCK_DEFINED
#define COSYNC_BSRECURSIVELOCK_DEFINED
struct BSRecursiveLock
{
    volatile DWORD m_tid;
    volatile LONG m_counter;

    void Lock();
    void Unlock();
};

template <class T>
struct BSScopedLock
{
    BSScopedLock(T& aLock) : m_lock(aLock) { LOG_REQUIREMENT("BSScopedLock::BSScopedLock", "Needs a valid, already-built lock object with a callable Lock() method."); m_lock.Lock(); }
    ~BSScopedLock() { LOG_REQUIREMENT("BSScopedLock::~BSScopedLock", "The stored lock must still be a valid reference to the same object passed to the constructor, with a callable Unlock() method."); m_lock.Unlock(); }
private:
    T& m_lock;
};
#endif

struct BSAnimationGraphManager
{
    virtual ~BSAnimationGraphManager();

    void Release()
    {
        LOG_REQUIREMENT("BSAnimationGraphManager::Release", "The reference count must still be valid for this object (it can't already be destroyed). Dropping the count to 0 destroys this object.");
        if (InterlockedDecrement(&refCount) == 0)
            this->~BSAnimationGraphManager();
    }

    volatile LONG refCount;
    void* pad_ptrs[6];
    BSTSmallArray<BShkbAnimationGraph> animationGraphs;
    uint8_t pad58[0xC8 - 0x58];
    BSRecursiveLock lock;
    void* unkD0;
    uint32_t animationGraphIndex;
};

static_assert(offsetof(BSAnimationGraphManager, animationGraphs) == 0x40,
    "BSAnimationGraphManager::animationGraphs offset mismatch - game version may differ");
static_assert(offsetof(BSAnimationGraphManager, animationGraphIndex) == 0xD8,
    "BSAnimationGraphManager::animationGraphIndex offset mismatch - game version may differ");
