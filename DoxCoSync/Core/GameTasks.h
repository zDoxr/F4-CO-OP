#pragma once

#include <cstdint>

class ITaskDelegate;

class ITaskInterface
{
public:
    virtual void AddTask(ITaskDelegate* task) = 0;
};

class ITaskDelegate
{
public:
    virtual ~ITaskDelegate() = default;


    virtual void Run() = 0;

    virtual void Dispose() = 0;
};

extern ITaskInterface* g_taskInterface;
