# Pre-join content validation and transfer (0.4.0)

Status: **built, offline-tested, main-menu load-tested. Not tested in a real two-machine multiplayer session.**

The open items from the previous sessions (the hang's unidentified object, NPC idles the game picks itself, animated PA entry needing the real frame nearby, remote wire removal, a client's own PlaceAtMe, PA ownership persistence) were **not touched** and remain open. Nothing in this work depended on them.

---

## 1. What was there before

| Piece | Where | What it did |
|---|---|---|
| Join handshake | `CoSyncNet::OnGNSConnected` → `CoSyncWorldTransfer::ClientBegin` | On connect the client sent `WORLDREQ|build|key|name`; the host saved its world and streamed it (`WORLDBEGIN/CHUNK/END`, CRC32). After the load, `TrySendHello` sent `HELLO`, which makes the host spawn the player. |
| Version check | `WORLDREQ` build field, later `VER|` | Mismatched builds refused. |
| Mod "sync" | `CoSyncModSync` `MODLIST` / `MODFILE` | Sent **after HELLO** (after the client had already loaded the host's world). The host pushed any plugin the client lacked as **one hex-encoded message**, capped at ~256 KB, **no hash**, no archives, written straight into `Data\`. Larger plugins were only reported. |
| Load order | `LOADORDER*` + `CoSyncFormIdMap` | Compared and translated FormIDs between load orders. Kept unchanged. |
| Files | `CoSyncWorldTransfer` chunking, CRT file I/O | Reused as a pattern (chunk size under GNS's 512 KB message cap). |

The world is the host's save. A save loaded without one of its plugins silently drops everything that plugin added. So the content has to match **before** `WORLDREQ`, and the old post-HELLO push could never have fixed that.

## 2. Engine facts (verified, not assumed)

All from Fallout4.exe 1.11.240 in IDA plus the F4SE 0.7.9 source in the repo:

- **Plugin companion files.** `sub_1402EEFF0` runs for every loaded `.ESP/.ESM/.ESL`. It is called from the data loader `sub_1402E8020`. It registers `<base> - Main.ba2` and `<base> - Textures.ba2` through `sub_1402EED40` (`TESDataHandler::ArchiveRegistrationListener`), `<base> - Geometry.psg`/`.csg` (`PSGRegistrationListener`), `<base>.cdx` and `<base> - Voices_<language>.ba2`. The suffix table is at `0x2EDB248`. No other archive is tied to a plugin.
- **Plugin INI.** `sub_1402EC650` also counts `<base>.ini` in a plugin's size. Nothing found proves the engine loads it, and an INI can carry `sStartingConsoleCommand`, so it is **deliberately never transferred**.
- **Plugins.txt.** The reader is `sub_14024D760` (`"%sPlugins.txt"`). It skips `#` lines, and when reading the active list it keeps only `*`-prefixed lines, with the `*` removed. The writer is `sub_14024D1B0` (`"%s%s\n"` with the `*` prefix). The file on this machine is `%LOCALAPPDATA%\Fallout4\Plugins.txt` and matches that format. `UserDownloadedContent.txt` is only written by `sub_14024D490` from the loaded list, never read for loading.
- **When data loads.** `Plugins.txt` reading (`sub_1402E9020`) and plugin/archive loading (`sub_1402E8020`) are both reached only from `InitGameDataThread::Run` (`0x140C41840 → sub_140C3EC50 → sub_140C323F0`). F4SE (`f4se.cpp`) runs every plugin's `F4SEPlugin_Load` **before** it hooks that thread (`Hooks_GameData_Commit`), and `kMessage_GameLoaded` fires after that thread finishes. So:
  - new plugins/archives only load after a **restart**;
  - a file the running game already has open (a loaded plugin or BA2 of a different version) cannot be replaced while it runs;
  - `F4SEPlugin_Load` is a point where **nothing is open yet**. The install runs there.
- **Official content.** The exe names `Fallout4.esm`, `DLCRobot`, `DLCworkshop01`, `DLCCoast`, `DLCworkshop02`, `DLCworkshop03`, `DLCNukaWorld`, `DLCUltraHighResolution`, `DLCDummy10thAnniversary` (`.esm`). Creation Club files are listed in `<game>\Fallout4.ccc`. Bethesda.net Creations are listed in `%LOCALAPPDATA%\Fallout4\ContentCatalog.txt` (read by `sub_1411B7330`).
- **Plugin header.** TES4 record flags `0x1` = master and `0x200` = light. `MAST` subrecords hold the masters. This was checked by parsing the real `HOWA.esp`, `CoSync.esp` (flags 0) and `FO4CreatureLoadingScreenRemoval.esl` (flags 0x201).
- **Quit.** The Papyrus `Debug.QuitGame` native is `sub_14111C8C0`, registered at `0x14111E1EE`. It sets `Main+0x24` (the quit flag). It is called with a prologue check.

## 3. What counts as required content

| Kind | Rule | Transferred? |
|---|---|---|
| Plugins | Every plugin the **host's engine loaded** (`DataHandler` full + light lists), in load order | User mods: yes. Base game / DLC / Creation Club / Creations: **never**. They are hash-checked, and a mismatch blocks the join with a clear reason. |
| Companions | The engine's own per-plugin files listed above, when present | Yes, for user-mod plugins |
| Loose files | Only files a mod manager deployed **together with** a session plugin (`Data\vortex.deployment.json`: same `source` mod), plus anything the host lists in `Data\CoSyncContent\include.txt` | Only allow-listed folders/extensions (below) |
| F4SE plugins (`.dll`) | Listed to the client as "the host also runs these" | **Never** (native code) |

A texture replacer with no plugin (like "Milk Vending Machine" here) is not session content. Players keep their own.

Each entry has: kind, origin, relative path, size, SHA-256, owner plugin, load slot, light flag, and masters (dependencies).

## 4. Handshake and join gate

```
client                                   host
CSREQ|build|proto        ─────────────►  build/protocol check, world ready?
                         ◄─────────────  CSWAIT (while hashing, first time only)
                         ◄─────────────  CSBIG MAN: nonce + manifest
check own files (worker: SHA-256 w/ cache,
possession proof per matching file)
CSBIG REP: loaded order + M/X/D + proofs ─►  recompute every proof on its own files
                         ◄─────────────  CSBIG VER: OK | NEED(T idx) | ACTIVATE(A name) | BLOCK(B code name)
 OK      → CoSyncWorldTransfer::ClientBegin (WORLDREQ) → load → HELLO   (unchanged)
 NEED    → consent → CSGET/CSDATA download → verify → queue → restart
 ACTIVATE→ queue Plugins.txt change → restart
 BLOCK   → reason shown, client leaves (host kicks after 30 s if not)
```

**Host authority.** The host builds the manifest from what its own engine loaded, and the host decides the verdict. The client cannot claim a file by echoing the manifest hash. For each file it says it has, it must return `SHA-256(nonce ‖ size ‖ 8 blocks at nonce-derived offsets)`, and the host recomputes that over its own copy. A wrong proof is treated as "different".

**Join gate (host).** `CoSyncNet::OnReceive` routes `CS*` messages first, before the FormID translation, which would corrupt binary chunks. It then drops every message from a connection whose content is not verified (`HostAllowsMessage`). `DISCONNECT|` and `WORLDREQ|` pass that filter, and `CoSyncWorldTransfer` refuses `WORLDREQ` with `WORLDERR|content_not_verified` until the connection is verified. So the host neither saves/sends its world nor spawns anything for an unverified client. That covers HELLO, which is what spawns the player.

**Client states** (`BuildUiJson` → menu): `requesting → preparing (host hashing) → checking → waiting → confirm → downloading → installing → restart`, or `ready` (joins), or `blocked` / `failed` / `cancelled`.

## 5. Transfer

- **Pull-based.** The client requests 256 KB chunks with a window of 12 (3 MB in flight). The host serves only indices granted in the verdict, only inside the file, at most 256 KB per request, 32 queued, and a byte budget of 3× the granted size + 64 MB. A violation gets `CSFAIL` and a kick. This limits flooding and gives natural flow control over GNS.
- **Host.** Chunks are read on the worker thread; the file's size and mtime are rechecked on every read (`CSERR|changed`). Packets are queued and sent by the main-thread tick within a 16 MB per-frame budget.
- **Client.** Each chunk is CRC32-checked. A damaged chunk is requested again (5 tries). Chunks are written at their offset into `Data\CoSyncContent\staging\<sha>_<pathhash>.part`, which is pre-sized so a full disk fails at the start. Every 16 MB the data is flushed and a resume point (`.meta`) is written. When the file is complete it is **hashed in full (SHA-256)**. On a mismatch the part is deleted and downloaded once more; a second mismatch fails the join.
- **Resume.** After a disconnect, cancel or crash, the next join resumes each file from its last saved point. Completed, verified parts are not downloaded again. The test showed the second session requesting 33.9 MB of 54.3 MB.
- **Limits.** 8 GB per file, 64 GB per join, 4096 files per manifest. Free disk space must be at least the download plus 512 MB. No compression is used, so there are no decompression bombs.
- **Transport.** It is the existing GNS connection (LAN and Steam P2P/relay), with no LAN-only assumptions. Content messages bypass `CoSyncTransport`'s FormID translation. `CSDATA`/`CSGET` are counted, not logged, per message.

## 6. Verification and install

1. Every file is **SHA-256 verified** against the manifest after download. The file's size and mtime at that moment are recorded.
2. The verified files and the plugins to enable are written to `Data\CoSyncContent\pending.txt`, along with a rejoin note (`rejoin.txt`: target and time). The client leaves and the menu shows **Restart required** with **Quit Game**.
3. **Next start, in `F4SEPlugin_Load`**, before the engine reads `Plugins.txt` or opens any plugin or archive:
   - An install journal left without `COMMIT` is rolled back first.
   - Every pending entry is re-validated: the staged path must be inside staging, the destination inside `Data\` and allow-listed, and the size and mtime must be unchanged since verification. If any entry fails, **nothing** is installed.
   - For each file: an existing file is **moved to** `Data\CoSyncContent\backup\<time>\` (never deleted), then the staged file is renamed into place (same volume, atomic). Every step is journaled first.
   - `Plugins.txt` is backed up, then rewritten (temp file + rename) with the new plugins enabled, inserted after the nearest earlier plugin of the host's order.
   - Any failure rolls back every step taken. It is all or nothing.
4. At `kMessage_GameLoaded` the rejoin note (if under 30 minutes old) arms an automatic rejoin 5 s later. The content check runs again and now passes, then the world transfer and HELLO follow.

## 7. Security

- **Paths.** Only printable ASCII is allowed. Rejected: `< > : " / | ? *` and control characters; `.`/`..` components; leading or trailing separators, spaces or dots; drive letters, UNC and ADS (`:`); device names (`CON`, `NUL`, `COM1`…); 8.3 aliases (`PROGRA~1`); more than 200 characters or 10 levels.
- **Confinement.** After normalizing (`GetFullPathNameW`), the destination must start with `Data\`. Every existing component below `Data` must not be a junction or symlink, and this is re-checked after creating folders.
- **Allow-list.** Plugins and companions go in the `Data` root only, and companion names must derive from their owner plugin. Loose files are allowed only in `meshes, textures, materials, sound, music, scripts, interface, strings, lodsettings, vis, seq`, with data extensions only: `.nif .dds .bgsm .bgem .hkx .tri .btr .bto .btt .wav .xwm .fuz .lip .pex .psc .swf .strings .dlstrings .ilstrings .txt .lod .seq .uvd`. Never allowed: `F4SE\`, `PrismaUI_F4\` (HTML/JS), `Video\`, CoSync's own folder, `.dll/.exe/.ini` or anything else.
- **Official content.** The client classifies content itself (exe list, `Fallout4.ccc`, `ContentCatalog.txt`, and the Creation Club file-name pattern). A host that labels `Fallout4.esm` as a "mod", or asks to send any official file, is refused with `refused_official`.
- **The host is not trusted.** The client strictly parses the whole manifest: any bad field, unsafe path, duplicate, unknown owner or truncation rejects **all** of it. The verdict is checked against the client's own state. File bytes must match the SHA-256 the manifest declared.
- **The client is not trusted.** Possession proofs, verdict-granted indices only, request bounds, a queue cap, a byte budget, 3 content checks per connection, and malformed input answered with `CSFAIL|protocol`.
- **Malformed packets** are parsed with bounds-checked field splitting, overflow-checked integers and capped reassembly (64 parts × 256 KB). The offline test feeds garbage to both sides without a crash.
- **Consent.** Nothing downloads without **Download & Install**. `[Content] bAutoAcceptDownloads=1` in `CoSync.ini` skips the prompt; it is off by default. The screen says when game scripts (`.pex`/`.swf`) are included. Those run inside the game's script/UI sandboxes, which is inherent to sharing mods.

## 8. What was verified

- **Build:** Release x64 (v145), 0 warnings and 0 errors. It was deployed to the game folder.
- **Engine facts:** IDA plus the F4SE source (section 2).
- **Offline unit tests** (`Tools\ContentSyncTests\run_tests.cmd`), `ManifestTests` **112/112**: path attacks (traversal, absolute, UNC, ADS, devices, 8.3, non-ASCII, excluded folders and extensions); manifest round trip and tampering (every refusal); SHA-256/CRC32 known vectors; hash cache; proof determinism and nonce dependence; TES4 parsing (synthetic with XXXX, truncated, and the three real plugins); junction refusal (a real `mklink /J`); `Plugins.txt` activation and order; the Vortex parser (synthetic plus this machine's real 1536-file deployment).
- **Offline end-to-end** (the real host and client state machines, two game folders, in-memory wire), `SyncTests` **102/102**, covering these scenarios:
  1. **Identical mods** → ready, nothing written.
  2. and 3. **Missing one or several mods** (plugin, both BA2s, Vortex-attributed loose texture) → confirm → download → restart → install at start → enabled in Plugins.txt → auto-rejoin → verified → world requested.
  4. **Different version** (Mod2.esp) → replaced, old copy in backup.
  5. **Already matching** (Fallout4.esm) → not listed, not sent.
  6. **Interrupted transfer** → `connection_lost`, resume point at 16 MB, second session resumes (33.9 of 54.3 MB requested).
  7. **Corrupted chunk** → CRC catches it, re-requested, completes. **Wrong bytes with a valid CRC** → SHA-256 mismatch twice → `hash_mismatch`, nothing queued.
  8. **Insufficient disk space** → refused before a byte is written.
  9. **Invalid paths** → five hostile manifests refused as `bad_manifest` with nothing written; a tampered `pending.txt` (traversal, foreign source, changed staged file) installs nothing.
  10. **Large file** (48 MB archive over 192 chunks, about 24 MB in 0.7 s locally).
  11. **Multiple mods** at once.
  12. **Sync then join**: verified connection passes the gate, unverified is dropped.
  - Also: official DLC missing → blocked, never sent; host pushing official content → refused; client lying about a file → host still requires it; granted-only chunk serving and flood kick; malformed messages on both sides; wrong build; activation-only; "enabled but not loaded" → mod manager message; cancel; an interrupted install journal rolled back at startup.
- **Menu:** the JS parses (esprima). Headless renders of the confirm, downloading, restart, blocked and failed screens look right.
- **Real game, main menu only:** 0.4.0 loads. A queued install (a probe text file) was applied in `F4SEPlugin_Load` at 18:09:12.42, **5 s before** `GameLoaded` (18:09:17.44), with no errors, and the game exited normally. The probe was removed afterwards.

## 9. What was NOT verified (needs a real two-machine session)

- The whole join over GNS between two PCs (LAN and Steam), including the manifest/report/verdict over the real transport and real throughput.
- Two instances on one PC **cannot exercise downloads**: they share one `Data` folder and one `Plugins.txt`, so the content always matches. The test plan needs two machines, or a second FO4 install as the client.
- That a freshly installed plugin + BA2 actually loads after the restart, and that the host's save then loads on the client with it.
- The automatic rejoin at the main menu (timing against the menu), for both IP and Steam lobby targets.
- **Quit Game** (`Debug.QuitGame`) from the menu.
- Behaviour under Mod Organizer 2 (virtual filesystem, its own `Plugins.txt`) and after Vortex re-deploys over replaced files.

## 10. Limitations and next steps

- A **restart is always required** when anything is installed. The engine loads data once per process (section 2), and files already loaded are locked. The IDA work also shows an in-process reload path (the Creations menu reaches the same loaders), but using it would invalidate every other F4SE plugin's cached forms, so it is not used.
- **MO2:** writes into `Data` and `Plugins.txt` go through its virtual filesystem. When the files arrive but the plugin still isn't loaded, the result is the "not loaded, enable it in your mod manager" failure. That message is from a real code path, but it has not been tested under MO2.
- **Loose-file attribution** exists only for Vortex hardlink deployments or the host's `include.txt`. Loose files from manual/MO2 installs are not detected.
- **Non-ASCII file names** are refused (strict path rule).
- **Official content** differences (base/DLC/CC version) block the join. That includes `DLCUltraHighResolution.esm`, a texture-pack master, which may be stricter than necessary.
- `Voices_<lang>` archives are sent for every language the host has.
- Client extra plugins are reported, not disabled. FormIDs of shared plugins are still translated by `CoSyncFormIdMap`.
- F4SE plugins are only listed, never installed. When a mod needs one, the player must install it themselves.
- Host UI: a syncing client shows as "Joining…" in the roster. There is no per-client transfer progress on the host.
- An old (0.3.x) host ignores `CSREQ`; a 0.4.0 client then waits 60 s and reports "the host did not answer". LAN discovery already marks different builds as incompatible.
