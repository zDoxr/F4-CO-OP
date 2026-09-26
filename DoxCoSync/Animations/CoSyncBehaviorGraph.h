#pragma once

#include "GameReferences.h"
#include "GameForms.h"

namespace CoSync
{

    class BehaviorGraph;


    class BehaviorGraphHelper
    {
    public:

        static bool GetIntVariable(Actor* actor, const char* varName, int& outValue);


        static bool SetIntVariable(Actor* actor, const char* varName, int value);

        static bool GetFloatVariable(Actor* actor, const char* varName, float& outValue);


        static bool SetFloatVariable(Actor* actor, const char* varName, float value);

        static bool GetBoolVariable(Actor* actor, const char* varName, bool& outValue);


        static bool SetBoolVariable(Actor* actor, const char* varName, bool value);

        static bool GetAllAnimationVariables(
            Actor* actor,
            int& weaponDraw,
            int& sighted,
            int& sneak,
            int& sprint
        );


        static bool SetAllAnimationVariables(
            Actor* actor,
            int weaponDraw,
            int sighted,
            int sneak,
            int sprint
        );

    private:

        static void* GetBehaviorGraphFromActor(Actor* actor);

        static bool GetVariableValueInternal(void* graph, const char* varName, float& outValue);


        static bool SetVariableValueInternal(void* graph, const char* varName, float value);
    };

}
