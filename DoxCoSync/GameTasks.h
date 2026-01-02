#pragma once

#include <cstdint>

// ============================================================
// Bethesda Task System (Fallout 4)
// ============================================================

// Forward declaration
class ITaskDelegate;

// ------------------------------------------------------------
// Task Interface (engine-owned)
// ------------------------------------------------------------
class ITaskInterface
{
public:
    virtual void AddTask(ITaskDelegate* task) = 0;
};

// ------------------------------------------------------------
// Task Delegate Base
// ------------------------------------------------------------
class ITaskDelegate
{
public:
    virtual ~ITaskDelegate() = default;

    // Called on the MAIN GAME THREAD
    virtual void Run() = 0;

    // Engine calls this when finished
    // You MUST delete yourself here
    virtual void Dispose() = 0;
};

// ------------------------------------------------------------
// Global task interface pointer
// ------------------------------------------------------------
// This is resolved by F4SE at runtime.
// DO NOT create your own instance.
extern ITaskInterface* g_taskInterface;
