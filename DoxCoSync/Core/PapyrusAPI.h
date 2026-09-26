#pragma once
#include "f4se/PluginAPI.h"
#include "f4se/PapyrusInterfaces.h"
#include "CoSync_Main.h"

namespace cosync_papyrus
{
    bool Tick(StaticFunctionTag*)
    {
        cosync::CoSync_Main::Get().Tick(0.0);
        return true;
    }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* vm)
    {
        vm->RegisterFunction(
            new RE::BSScript::NativeFunction<false, bool, StaticFunctionTag*>(
                "Tick",
                "DoxCoSync",
                Tick
            )
        );
        return true;
    }
}
