#pragma once

// Slot order is an ABI contract with f4se_1_11_221.dll's task pump
// (f4se+0x16130: call [vtbl+8] = Run, call [vtbl+0] = deleting dtor):
// the virtual destructor MUST be declared before Run.
class ITaskDelegate
{
public:
    virtual ~ITaskDelegate() = default;
    virtual void Run() = 0;
};
