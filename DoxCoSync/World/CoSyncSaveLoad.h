#pragma once

#include <cstdint>
#include <string>

class TESObjectREFR;

// Engine save / load by NAME (Fallout4.exe 1.11.240), used by the shared host
// save: the host saves "CoSync_World" to send it, a client loads the copy it
// received. Found from the console 'save' / 'load' commands and verified in
// IDA on 2026-09-25; every call first checks the target's prologue bytes and
// refuses (logged) on a mismatch instead of jumping into the wrong code.
namespace CoSyncSaveLoad
{
    // True when every address verified (checked once, cached).
    bool IsAvailable();

    // Name without extension, relative to the save folder (the engine adds
    // .fos). MAIN THREAD ONLY. Both are synchronous: Save writes the file
    // before returning; Load runs the whole load before returning.
    bool SaveNamed(const char* name);
    bool LoadNamed(const char* name);

    // The engine's own "can save right now?" gate (sub_140BFB3A0), which
    // SaveGame checks first. 1 = yes, 0 = the engine refuses saving at this
    // moment, -1 = unavailable (wrong exe / no manager). MAIN THREAD ONLY.
    int EngineCanSave();

    // Documents\My Games\Fallout4\<SLocalSavePath>, with a trailing slash.
    std::string GetSaveFolder();

    // Keep CoSync proxy actors OUT of every save. The engine asks each changed
    // reference "should you be saved?" (TESObjectREFR vtable +0x80, one shared
    // function for REFR/Actor/PlayerCharacter); answering no makes the engine
    // drop that reference's changes itself. Loading a save that contained a
    // proxy crashed (2026-08-08). predicate runs on the main thread during
    // saves. Install once at plugin load.
    using ProxyPredicate = bool (*)(TESObjectREFR* ref);
    bool InstallSaveFilter(ProxyPredicate isCoSyncProxy);

    // Client saves are blocked: a client plays in a COPY of the host's world,
    // and the host's save is the only real one. Hooks both engine save entry
    // points -- BGSSaveLoadManager::SaveGame (0x140BF9330: console, pause
    // menu, quicksave, Papyrus) and the typed save (0x140BFD920: autosaves,
    // exit saves) -- BEFORE either creates the save file, and refuses while
    // shouldBlock() is true (logged, and the player sees a short message).
    // The host's own SaveNamed is unaffected (shouldBlock is false for it).
    using SaveBlocker = bool (*)();
    bool InstallSaveBlock(SaveBlocker shouldBlock);
}
