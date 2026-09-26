#pragma once

#include <cstdint>
#include <string>

namespace CoSyncWorkshopSync
{
    void Init();
    void Shutdown();

    void Tick(double now);


    void OnReceiveWorkshopMode(const std::string& msg);
    void OnReceiveWorkshopDestroy(const std::string& msg);

    // NOTE: build (WB|) was a dead stub -- RX side only logged, never applied
    // anything -- and has been replaced entirely by
    // CoSyncWorldSync::NotifyWorkshopBuildImmediate, which sends the real
    // "WBF|" packet the poll-based apply pipeline already knows how to spawn
    // from. Destroy (WD|) is left as-is: CoSyncWorkshopScrapHook's "WDS|"
    // (hooked off the engine's real ScrapReference) already applies removal
    // end-to-end, and also wiring up WD| to apply would risk a double-removal
    // picking the wrong nearby duplicate object in a dense build.

    void NotifyWorkshopDestroy(uint32_t workshopFormID, uint32_t scrappedFormID, uint32_t entityID);
    void NotifyWorkshopMode(uint32_t workshopFormID, uint32_t entityID, bool isEntering);
}
