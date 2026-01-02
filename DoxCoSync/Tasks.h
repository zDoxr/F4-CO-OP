#pragma once

// Single authoritative definition for your project.
// Matches how Hooks_Threads.cpp uses it (Run + delete).
class ITaskDelegate
{
public:
    virtual void Run() = 0;
    virtual ~ITaskDelegate() = default;
};
