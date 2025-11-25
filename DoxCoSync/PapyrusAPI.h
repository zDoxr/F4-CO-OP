#pragma once
#include "f4se/PluginAPI.h"
#include "f4se/PapyrusInterfaces.h"
#include "F4MP_Main.h"

namespace f4mp_papyrus
{
    bool Tick(StaticFunctionTag*)
    {
        f4mp::F4MP_Main::Tick();
        return true;
    }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* vm)
    {
        vm->RegisterFunction(
            new RE::BSScript::NativeFunction<false, bool, StaticFunctionTag*>(
                "Tick",            // Papyrus function name
                "DoxCoSync",       // Script namespace
                Tick               // C++ function
            )
        );
        return true;
    }
}
