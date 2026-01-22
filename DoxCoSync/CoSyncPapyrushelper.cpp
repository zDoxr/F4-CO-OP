// CoSyncPapyrusHelper.cpp
#include "CoSyncPapyrusHelper.h"

#include "ConsoleLogger.h"
#include "GameForms.h"
#include "GameData.h"
#include "GameRTTI.h"
#include "PapyrusVM.h"
#include "PapyrusEvents.h"

extern RelocPtr<DataHandler*> g_dataHandler;
extern RelocPtr<GameVM*> g_gameVM;
extern RelocAddr<_LookupFormByID> LookupFormByID;

namespace
{
    TESQuest* s_coSyncQuest = nullptr;
    bool s_initialized = false;

    // FormID of CoSyncQuest from Creation Kit
    // Your quest FormID: 0100359A
    constexpr UInt32 kCoSyncQuestFormID = 0x0100359A;
}

void CoSyncPapyrusHelper::Init()
{
    if (s_initialized)
        return;

    s_initialized = true;

    // Look up the quest by formID
    TESForm* form = LookupFormByID(kCoSyncQuestFormID);
    if (!form)
    {
        LOG_ERROR("[PapyrusHelper] Quest form not found: 0x%08X", kCoSyncQuestFormID);
        LOG_ERROR("[PapyrusHelper] Make sure CoSync.esp is loaded!");
        return;
    }

    // Cast to TESQuest
    s_coSyncQuest = DYNAMIC_CAST(form, TESForm, TESQuest);
    if (!s_coSyncQuest)
    {
        LOG_ERROR("[PapyrusHelper] Form 0x%08X is not a quest (type=%d)",
            kCoSyncQuestFormID, form->formType);
        return;
    }

    LOG_INFO("[PapyrusHelper] Found CoSyncQuest: 0x%08X name='%s'",
        kCoSyncQuestFormID,
        s_coSyncQuest->fullName.name.c_str());
}

TESQuest* CoSyncPapyrusHelper::GetCoSyncQuest()
{
    if (!s_initialized)
        Init();

    return s_coSyncQuest;
}

void CoSyncPapyrusHelper::SpawnRemotePlayer(uint32_t entityID)
{
    if (!s_coSyncQuest)
    {
        LOG_WARN("[PapyrusHelper] Cannot spawn - quest not found");
        return;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM || !gameVM->m_virtualMachine)
    {
        LOG_ERROR("[PapyrusHelper] VM not available");
        return;
    }

    // Build args array: just the entityID
    VMArray<VMVariable> args;
    VMVariable entityIDVar;
    SInt32 entityIDValue = static_cast<SInt32>(entityID);
    entityIDVar.Set(&entityIDValue);
    args.Push(&entityIDVar);

    // Call: CoSyncQuest.SpawnRemotePlayer(entityID)
    CallFunctionNoWait(
        s_coSyncQuest,
        BSFixedString("SpawnRemotePlayer"),
        args
    );

    LOG_INFO("[PapyrusHelper] Called SpawnRemotePlayer(%u)", entityID);
}

void CoSyncPapyrusHelper::DespawnRemotePlayer(uint32_t entityID)
{
    if (!s_coSyncQuest)
    {
        LOG_WARN("[PapyrusHelper] Cannot despawn - quest not found");
        return;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM || !gameVM->m_virtualMachine)
    {
        LOG_ERROR("[PapyrusHelper] VM not available");
        return;
    }

    VMArray<VMVariable> args;
    VMVariable entityIDVar;
    SInt32 entityIDValue = static_cast<SInt32>(entityID);
    entityIDVar.Set(&entityIDValue);
    args.Push(&entityIDVar);

    CallFunctionNoWait(
        s_coSyncQuest,
        BSFixedString("DespawnRemotePlayer"),
        args
    );

    LOG_INFO("[PapyrusHelper] Called DespawnRemotePlayer(%u)", entityID);
}

Actor* CoSyncPapyrusHelper::GetRemotePlayerActor(uint32_t entityID)
{
    if (!s_coSyncQuest)
    {
        LOG_WARN("[PapyrusHelper] Cannot get actor - quest not found");
        return nullptr;
    }

    GameVM* gameVM = *g_gameVM;
    if (!gameVM || !gameVM->m_virtualMachine)
    {
        LOG_ERROR("[PapyrusHelper] VM not available");
        return nullptr;
    }

    // For now, we'll need to handle this differently
    // Getting return values from Papyrus requires a callback approach
    // which is more complex. For now, we rely on CoSyncPlayer tracking

    LOG_WARN("[PapyrusHelper] GetRemotePlayerActor not fully implemented yet");
    return nullptr;
}