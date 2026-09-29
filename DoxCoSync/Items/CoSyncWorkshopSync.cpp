#include "CoSyncWorkshopSync.h"

#include "ConsoleLogger.h"
#include "CoSyncNet.h"
#include "CoSyncTransport.h"
#include "CoSyncRequirementsLog.h"

#include "GameWorkshop.h"
#include "GameReferences.h"

#include <sstream>
#include <cstdlib>
#include <string>

// Was extern RelocPtr<PlayerCharacter*> g_player -- switched to CoSyncGetPlayer()
// (see GameGlobals.cpp), the established, confirmed-working replacement for
// raw g_player dereferences used throughout the rest of the codebase.
extern PlayerCharacter* CoSyncGetPlayer();

static std::string SerializeWorkshopMode(uint32_t entityID, uint32_t workshopFormID, bool isEntering)
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::SerializeWorkshopMode", "Doesn't touch the game. Just packs the entity, workshop, and enter/exit flag into a message.");
    std::ostringstream ss;
    ss << "WM|" << entityID << "|" << workshopFormID << "|" << (isEntering ? 1 : 0);
    return ss.str();
}

static bool DeserializeWorkshopMode(const std::string& msg,
                                     uint32_t& outEntityID,
                                     uint32_t& outWorkshopFormID,
                                     bool&     outIsEntering)
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::DeserializeWorkshopMode", "Doesn't touch the game. Just needs a valid message to unpack.");
    if (msg.rfind("WM|", 0) != 0)
        return false;

    std::stringstream ss(msg.substr(3));
    std::string tok;

    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outEntityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    if (!std::getline(ss, tok, '|')) return false;
    outWorkshopFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));

    if (!std::getline(ss, tok, '|')) return false;
    outIsEntering = (tok == "1");

    return true;
}


static std::string SerializeWorkshopDestroy(uint32_t entityID, uint32_t workshopFormID, uint32_t scrappedFormID)
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::SerializeWorkshopDestroy", "Doesn't touch the game. Just packs the entity, workshop, and scrapped item into a message.");
    std::ostringstream ss;
    ss << "WD|" << entityID << "|" << workshopFormID << "|" << scrappedFormID;
    return ss.str();
}

static bool DeserializeWorkshopDestroy(const std::string& msg,
                                        uint32_t& outEntityID,
                                        uint32_t& outWorkshopFormID,
                                        uint32_t& outScrappedFormID)
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::DeserializeWorkshopDestroy", "Doesn't touch the game. Just needs a valid message to unpack.");
    if (msg.rfind("WD|", 0) != 0) return false;
    std::stringstream ss(msg.substr(3));
    std::string tok;
    if (!std::getline(ss, tok, '|') || tok.empty()) return false;
    outEntityID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (!std::getline(ss, tok, '|')) return false;
    outWorkshopFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    if (!std::getline(ss, tok, '|')) return false;
    outScrappedFormID = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
    return true;
}

namespace
{
    bool     s_initialized          = false;
    bool     s_wasInWorkshop        = false;
    uint32_t s_lastWorkshopFormID   = 0;
}

void CoSyncWorkshopSync::Init()
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::Init", "Safe to call any time during startup -- just resets this module's own state.");
    s_initialized        = true;
    s_wasInWorkshop      = false;
    s_lastWorkshopFormID = 0;
    LOG_INFO("[WorkshopSync] Init OK");
}

void CoSyncWorkshopSync::Shutdown()
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::Shutdown", "Safe to call even if Init() was never called.");
    s_initialized = false;
}

void CoSyncWorkshopSync::Tick(double )
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::Tick", "Needs Init() called first, a live connection, and the player to actually be in workshop mode.");
    if (!s_initialized)
        return;

    if (!CoSyncNet::IsInitialized() || !CoSyncNet::IsConnected())
        return;

    BSPointerHandle<TESObjectREFR>& workshopHandle = *Workshop::hCurrentWorkshop;
    const bool inWorkshop = (workshopHandle != BSPointerHandle<TESObjectREFR>());


    if (inWorkshop == s_wasInWorkshop)
        return;

    s_wasInWorkshop = inWorkshop;

    uint32_t workshopFormID = 0;
    if (inWorkshop)
    {

        PlayerCharacter* pc = CoSyncGetPlayer();
        if (pc)
        {
            TESObjectREFR* workshopRefr =
                Workshop::FindNearestValidWorkshop(reinterpret_cast<TESObjectREFR*>(pc));
            if (workshopRefr)
                workshopFormID = workshopRefr->formID;
        }


        if (workshopFormID == 0)
        {
            NiPointer<TESObjectREFR> workshopRef = workshopHandle.get();
            if (workshopRef.m_pObject)
                workshopFormID = workshopRef->formID;
        }

        s_lastWorkshopFormID = workshopFormID;

        if (workshopFormID == 0)
            LOG_WARN("[WorkshopSync] Could not resolve workshop formID (both paths returned 0)");
    }
    else
    {
        workshopFormID       = s_lastWorkshopFormID;
        s_lastWorkshopFormID = 0;
    }

    const uint32_t myEntityID = CoSyncNet::GetMyEntityID();
    const std::string msg     = SerializeWorkshopMode(myEntityID, workshopFormID, inWorkshop);

    LOG_INFO("[WorkshopSync] TX WM: entity=%u workshopForm=0x%08X entering=%d",
        myEntityID, workshopFormID, inWorkshop ? 1 : 0);

    CoSyncTransport::Send(msg);
}

void CoSyncWorkshopSync::OnReceiveWorkshopMode(const std::string& msg)
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::OnReceiveWorkshopMode", "Needs a live connection so we know our own role and ID when the message arrives.");
    uint32_t entityID        = 0;
    uint32_t workshopFormID  = 0;
    bool     isEntering      = false;

    if (!DeserializeWorkshopMode(msg, entityID, workshopFormID, isEntering))
    {
        LOG_WARN("[WorkshopSync] Malformed WM packet");
        return;
    }


    // The host's relay (to everyone but the sender) is in CoSyncNet's dispatch.
    if (entityID == CoSyncNet::GetMyEntityID())
        return;

    LOG_INFO("[WorkshopSync] RX WM: entity=%u workshopForm=0x%08X entering=%d",
        entityID, workshopFormID, isEntering ? 1 : 0);
}

void CoSyncWorkshopSync::OnReceiveWorkshopDestroy(const std::string& msg)
{
    uint32_t entityID        = 0;
    uint32_t workshopFormID  = 0;
    uint32_t scrappedFormID  = 0;

    LOG_REQUIREMENT("CoSyncWorkshopSync::OnReceiveWorkshopDestroy", "Needs a live connection so we know our own role and ID when the message arrives.");
    if (!DeserializeWorkshopDestroy(msg, entityID, workshopFormID, scrappedFormID))
    {
        LOG_WARN("[WorkshopSync] Malformed WD packet");
        return;
    }

    // The host's relay (to everyone but the sender) is in CoSyncNet's dispatch.
    if (entityID == CoSyncNet::GetMyEntityID())
        return;

    LOG_INFO("[WorkshopSync] RX WD: entity=%u workshop=0x%08X scrapped=0x%08X",
        entityID, workshopFormID, scrappedFormID);

}

void CoSyncWorkshopSync::NotifyWorkshopDestroy(uint32_t workshopFormID, uint32_t scrappedFormID, uint32_t entityID)
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::NotifyWorkshopDestroy", "Needs Init() called first and a live connection before sending.");
    if (!s_initialized || !CoSyncNet::IsInitialized() || !CoSyncNet::IsConnected())
        return;

    const std::string msg = SerializeWorkshopDestroy(entityID, workshopFormID, scrappedFormID);
    CoSyncTransport::Send(msg);
    LOG_INFO("[WorkshopSync] TX WD: entity=%u workshop=0x%08X scrapped=0x%08X",
        entityID, workshopFormID, scrappedFormID);
}

void CoSyncWorkshopSync::NotifyWorkshopMode(uint32_t workshopFormID, uint32_t entityID, bool isEntering)
{
    LOG_REQUIREMENT("CoSyncWorkshopSync::NotifyWorkshopMode", "Needs Init() called first and a live connection before sending.");
    if (!s_initialized || !CoSyncNet::IsInitialized() || !CoSyncNet::IsConnected())
        return;


    s_wasInWorkshop      = isEntering;
    s_lastWorkshopFormID = isEntering ? workshopFormID : 0;

    const std::string msg = SerializeWorkshopMode(entityID, workshopFormID, isEntering);
    CoSyncTransport::Send(msg);
    LOG_INFO("[WorkshopSync] TX WM (Papyrus): entity=%u workshopForm=0x%08X entering=%d",
        entityID, workshopFormID, isEntering ? 1 : 0);
}
