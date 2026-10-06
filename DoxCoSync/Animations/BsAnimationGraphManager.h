#pragma once

#include <cstdint>
#include <Windows.h>

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
    BSScopedLock(T& aLock) : m_lock(aLock) { m_lock.Lock(); }
    ~BSScopedLock() { m_lock.Unlock(); }
private:
    T& m_lock;
};
#endif

struct BSAnimationGraphManager
{
    virtual ~BSAnimationGraphManager();

    void Release()
    {
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
