#include "CoSyncBehaviorGraph.h"
#include "ConsoleLogger.h"
#include "GameReferences.h"
#include "GameTypes.h"
#include "CoSyncRequirementsLog.h"

namespace CoSync
{

    static bool IsValidBehaviorGraph(void* ptr)
    {
        LOG_REQUIREMENT("CoSync::IsValidBehaviorGraph", "Either the pointer is null, or it needs to look like a real, readable graph object. Guarded so a bad pointer cannot crash the game.");
        if (!ptr)
            return false;

        __try
        {

            void** vtable = *(void***)ptr;
            if (!vtable)
                return false;


            uintptr_t vtableAddr = (uintptr_t)vtable;


            if (vtableAddr < 0x10000 || vtableAddr > 0x7FFFFFFFFFFF)
                return false;


            void* firstFunc = vtable[0];
            void* secondFunc = vtable[1];

            if (!firstFunc || !secondFunc)
                return false;


            uintptr_t firstAddr = (uintptr_t)firstFunc;
            uintptr_t secondAddr = (uintptr_t)secondFunc;

            if (firstAddr < 0x10000 || firstAddr > 0x7FFFFFFFFFFF)
                return false;
            if (secondAddr < 0x10000 || secondAddr > 0x7FFFFFFFFFFF)
                return false;

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* BehaviorGraphHelper::GetBehaviorGraphFromActor(Actor* actor)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::GetBehaviorGraphFromActor", "Needs a valid actor with its internal process data set up. Looks for the animation graph in its usual spot, or scans nearby slots if it is not there.");
        if (!actor)
            return nullptr;

        auto middleProcess = actor->middleProcess;
        if (!middleProcess)
            return nullptr;

        auto data08 = middleProcess->unk08;
        if (!data08)
            return nullptr;

        void** data08Ptrs = (void**)data08;


        void* possibleGraph = data08Ptrs[11];

        if (possibleGraph && IsValidBehaviorGraph(possibleGraph))
        {
            return possibleGraph;
        }


        for (int i = 0; i < 64; i++)
        {
            if (i == 11) continue;

            possibleGraph = data08Ptrs[i];
            if (possibleGraph && IsValidBehaviorGraph(possibleGraph))
            {
                LOG_WARN("[BehaviorGraph] Found at unexpected offset 0x%X (index %d)", i * 8, i);
                return possibleGraph;
            }
        }

        return nullptr;
    }

    bool BehaviorGraphHelper::GetVariableValueInternal(void* graph, const char* varName, float& outValue)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::GetVariableValueInternal", "Needs a valid graph and variable name. Guarded so a bad function pointer on the graph cannot crash the game.");
        if (!graph || !varName)
            return false;

        uintptr_t** vtable = (uintptr_t**)graph;
        if (!vtable || !*vtable)
            return false;


        typedef bool (*GetVarFunc)(void*, BSFixedString*, float*);

        const int vtableIndex = 42;
        if (!(*vtable)[vtableIndex])
            return false;

        GetVarFunc getVar = (GetVarFunc)((*vtable)[vtableIndex]);
        BSFixedString bsVarName(varName);

        __try
        {
            if (getVar(graph, &bsVarName, &outValue))
                return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        return false;
    }

    bool BehaviorGraphHelper::SetVariableValueInternal(void* graph, const char* varName, float value)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::SetVariableValueInternal", "Needs a valid graph and variable name. Guarded so a bad function pointer on the graph cannot crash the game.");
        if (!graph || !varName)
            return false;

        uintptr_t** vtable = (uintptr_t**)graph;
        if (!vtable || !*vtable)
            return false;


        typedef bool (*SetVarFunc)(void*, BSFixedString*, float);

        const int possibleIndices[] = { 43, 42, 44, 41 };
        BSFixedString bsVarName(varName);

        for (int idx : possibleIndices)
        {
            if (!(*vtable)[idx])
                continue;

            SetVarFunc setVar = (SetVarFunc)((*vtable)[idx]);

            __try
            {
                if (setVar(graph, &bsVarName, value))
                    return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
        }

        return false;
    }

    bool BehaviorGraphHelper::GetIntVariable(Actor* actor, const char* varName, int& outValue)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::GetIntVariable", "Needs the actor to have a valid animation graph for the read to succeed.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
            return false;

        float floatValue = 0.0f;
        if (!GetVariableValueInternal(graph, varName, floatValue))
            return false;

        outValue = static_cast<int>(floatValue);
        return true;
    }

    bool BehaviorGraphHelper::SetIntVariable(Actor* actor, const char* varName, int value)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::SetIntVariable", "Needs the actor to have a valid animation graph for the write to succeed.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
            return false;

        return SetVariableValueInternal(graph, varName, static_cast<float>(value));
    }

    bool BehaviorGraphHelper::GetFloatVariable(Actor* actor, const char* varName, float& outValue)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::GetFloatVariable", "Needs the actor to have a valid animation graph for the read to succeed.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
            return false;

        return GetVariableValueInternal(graph, varName, outValue);
    }

    bool BehaviorGraphHelper::SetFloatVariable(Actor* actor, const char* varName, float value)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::SetFloatVariable", "Needs the actor to have a valid animation graph for the write to succeed. Logs an error if not.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
        {
            LOG_ERROR("[BehaviorGraph] SetFloatVariable(%s): No behavior graph found for actor", varName);
            return false;
        }

        bool result = SetVariableValueInternal(graph, varName, value);
        if (!result)
        {
            LOG_WARN("[BehaviorGraph] SetFloatVariable(%s): Failed to set value %.2f", varName, value);
        }

        return result;
    }

    bool BehaviorGraphHelper::GetBoolVariable(Actor* actor, const char* varName, bool& outValue)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::GetBoolVariable", "Needs the actor to have a valid animation graph for the read to succeed.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
            return false;

        float floatValue = 0.0f;
        if (!GetVariableValueInternal(graph, varName, floatValue))
            return false;

        outValue = (floatValue != 0.0f);
        return true;
    }

    bool BehaviorGraphHelper::SetBoolVariable(Actor* actor, const char* varName, bool value)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::SetBoolVariable", "Needs the actor to have a valid animation graph for the write to succeed.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
            return false;

        return SetVariableValueInternal(graph, varName, value ? 1.0f : 0.0f);
    }

    bool BehaviorGraphHelper::GetAllAnimationVariables(
        Actor* actor,
        int& weaponDraw,
        int& sighted,
        int& sneak,
        int& sprint)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::GetAllAnimationVariables", "Needs the actor to have a valid animation graph, with all four expected variables present on it for a full success.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
            return false;

        float value = 0.0f;
        bool success = true;

        if (GetVariableValueInternal(graph, "iSyncWeaponDrawState", value))
            weaponDraw = static_cast<int>(value);
        else
            success = false;

        if (GetVariableValueInternal(graph, "iSyncSightedState", value))
            sighted = static_cast<int>(value);
        else
            success = false;

        if (GetVariableValueInternal(graph, "iIsInSneak", value))
            sneak = static_cast<int>(value);
        else
            success = false;

        if (GetVariableValueInternal(graph, "iSyncSprintState", value))
            sprint = static_cast<int>(value);
        else
            success = false;

        return success;
    }

    bool BehaviorGraphHelper::SetAllAnimationVariables(
        Actor* actor,
        int weaponDraw,
        int sighted,
        int sneak,
        int sprint)
    {
        LOG_REQUIREMENT("CoSync::BehaviorGraphHelper::SetAllAnimationVariables", "Needs the actor to have a valid animation graph, with all four expected variables settable on it for a full success.");
        void* graph = GetBehaviorGraphFromActor(actor);
        if (!graph)
            return false;

        bool success = true;

        success &= SetVariableValueInternal(graph, "iSyncWeaponDrawState", static_cast<float>(weaponDraw));
        success &= SetVariableValueInternal(graph, "iSyncSightedState", static_cast<float>(sighted));
        success &= SetVariableValueInternal(graph, "iIsInSneak", static_cast<float>(sneak));
        success &= SetVariableValueInternal(graph, "iSyncSprintState", static_cast<float>(sprint));

        return success;
    }

}
