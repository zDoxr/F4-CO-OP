// Offline tests for Networking/CoSyncContentManifest.cpp (no game needed).
// Build + run: Tools\ContentSyncTests\run_tests.cmd
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "CoSyncContentManifest.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace CoSyncContent;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::wstring TempDir()
{
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    std::wstring d = std::wstring(buf) + L"cosync_content_tests";
    CreateDirectories(d);
    return d;
}

static void TestPaths()
{
    std::string why;
    // Syntax.
    CHECK(IsSafeRelativePath("HOWA.esp", why));
    CHECK(IsSafeRelativePath("Textures\\SetDressing\\a_d.dds", why));
    CHECK(!IsSafeRelativePath("..\\Fallout4.exe", why));
    CHECK(!IsSafeRelativePath("Textures\\..\\..\\x.dds", why));
    CHECK(!IsSafeRelativePath("C:\\Windows\\x.dds", why));
    CHECK(!IsSafeRelativePath("\\\\server\\share\\x.esp", why));
    CHECK(!IsSafeRelativePath("\\x.esp", why));
    CHECK(!IsSafeRelativePath("Textures/x.dds", why));
    CHECK(!IsSafeRelativePath("Textures\\\\x.dds", why));
    CHECK(!IsSafeRelativePath("x.esp:stream", why));
    CHECK(!IsSafeRelativePath("CON.esp", why));
    CHECK(!IsSafeRelativePath("Textures\\nul.dds", why));
    CHECK(!IsSafeRelativePath("Textures\\a.dds.", why));
    CHECK(!IsSafeRelativePath("Textures \\a.dds", why));
    CHECK(!IsSafeRelativePath("PROGRA~1\\a.dds", why));
    CHECK(!IsSafeRelativePath(std::string("a\x01.esp"), why));
    CHECK(!IsSafeRelativePath(std::string("\xC3\xA9.esp"), why));
    CHECK(!IsSafeRelativePath(std::string(250, 'a') + ".esp", why));
    CHECK(!IsSafeRelativePath("", why));

    // Loose policy.
    CHECK(IsAllowedLoosePath("Textures\\a\\b.dds", why));
    CHECK(IsAllowedLoosePath("Scripts\\MyQuest.pex", why));
    CHECK(IsAllowedLoosePath("meshes\\x.nif", why));
    CHECK(!IsAllowedLoosePath("F4SE\\Plugins\\evil.dll", why));
    CHECK(!IsAllowedLoosePath("F4SE\\Plugins\\x.pex", why));
    CHECK(!IsAllowedLoosePath("PrismaUI_F4\\views\\x.js", why));
    CHECK(!IsAllowedLoosePath("Textures\\run.exe", why));
    CHECK(!IsAllowedLoosePath("Textures\\x.dll", why));
    CHECK(!IsAllowedLoosePath("Textures\\x.bat", why));
    CHECK(!IsAllowedLoosePath("x.dds", why));
    CHECK(!IsAllowedLoosePath("Video\\Intro.bk2", why));
    CHECK(!IsAllowedLoosePath("CoSyncContent\\staging\\x.dds", why));
    CHECK(!IsAllowedLoosePath("Textures\\x.ini", why));

    // Kinds.
    Entry p; p.kind = Kind::Plugin; p.relPath = "HOWA.esp";
    CHECK(ValidateEntryPath(p, why));
    p.relPath = "Sub\\HOWA.esp";
    CHECK(!ValidateEntryPath(p, why));
    p.relPath = "HOWA.exe";
    CHECK(!ValidateEntryPath(p, why));

    Entry c; c.kind = Kind::Companion; c.owner = "HOWA.esp";
    c.relPath = "HOWA - Main.ba2";               CHECK(ValidateEntryPath(c, why));
    c.relPath = "howa - textures.BA2";           CHECK(ValidateEntryPath(c, why));
    c.relPath = "HOWA - Voices_en.ba2";          CHECK(ValidateEntryPath(c, why));
    c.relPath = "HOWA - Geometry.csg";           CHECK(ValidateEntryPath(c, why));
    c.relPath = "HOWA.cdx";                      CHECK(ValidateEntryPath(c, why));
    c.relPath = "HOWA.ini";                      CHECK(!ValidateEntryPath(c, why));
    c.relPath = "Fallout4 - Textures1.ba2";      CHECK(!ValidateEntryPath(c, why));
    c.relPath = "HOWA - Voices_../x.ba2";        CHECK(!ValidateEntryPath(c, why));
    c.relPath = "HOWA - Other.ba2";              CHECK(!ValidateEntryPath(c, why));
    c.relPath = "HOWA - Main.ba2"; c.owner = "";  CHECK(!ValidateEntryPath(c, why));

    // Classification.
    std::vector<std::string> ccc = { "ccBGSFO4044-HellfirePowerArmor.esl" };
    CHECK(ClassifyPlugin("Fallout4.esm", ccc, "") == Origin::BaseGame);
    CHECK(ClassifyPlugin("DLCNukaWorld.esm", ccc, "") == Origin::BaseGame);
    CHECK(ClassifyPlugin("ccbgsfo4044-hellfirepowerarmor.esl", ccc, "") == Origin::CreationClub);
    CHECK(ClassifyPlugin("ccFRSFO4002-AntimaterielRifle.esl", ccc, "") == Origin::CreationClub);   // by name pattern
    CHECK(ClassifyPlugin("HOWA.esp", ccc, "") == Origin::Mod);
    CHECK(ClassifyPlugin("SomeCreation.esp", ccc, "{\"x\":{\"Files\":[\"SomeCreation.esp\"]}}") == Origin::Creations);
    CHECK(ClassifyPlugin("cc.esp", ccc, "") == Origin::Mod);
}

static void TestManifest()
{
    Manifest m;
    m.build = 400;
    Entry p; p.kind = Kind::Plugin; p.origin = Origin::Mod; p.relPath = "HOWA.esp"; p.size = 1234;
    p.sha256 = std::string(64, 'a'); p.slot = 5; p.light = false; p.masters = { "Fallout4.esm" };
    Entry b; b.kind = Kind::Plugin; b.origin = Origin::BaseGame; b.relPath = "Fallout4.esm"; b.size = 99;
    b.sha256 = std::string(64, 'b'); b.slot = 0;
    Entry c; c.kind = Kind::Companion; c.origin = Origin::Mod; c.relPath = "HOWA - Main.ba2"; c.owner = "HOWA.esp";
    c.size = 5ull << 30; c.sha256 = std::string(64, 'c');
    Entry l; l.kind = Kind::Loose; l.origin = Origin::Mod; l.relPath = "Textures\\x\\y.dds"; l.owner = "HOWA.esp";
    l.size = 10; l.sha256 = std::string(64, 'd');
    m.entries = { b, p, c, l };
    m.nativePlugins = { "mcm.dll" };

    const std::string text = SerializeManifest(m);
    Manifest back; std::string why;
    CHECK(ParseManifest(text, back, why));
    CHECK(back.build == 400);
    CHECK(back.entries.size() == 4);
    CHECK(back.entries[1].relPath == "HOWA.esp" && back.entries[1].slot == 5 && back.entries[1].masters.size() == 1);
    CHECK(back.entries[2].size == (5ull << 30) && back.entries[2].owner == "HOWA.esp");
    CHECK(back.entries[0].origin == Origin::BaseGame && !back.entries[0].Transferable());
    CHECK(back.nativePlugins.size() == 1);

    // Tampering / malformed input is refused, never partially accepted.
    auto bad = [&](std::string t) { Manifest x; std::string w; return !ParseManifest(t, x, w); };
    std::string t;
    CHECK(bad(""));
    CHECK(bad(text.substr(0, text.size() - 2)));                                     // no end marker
    t = text; t.replace(t.find("HOWA - Main.ba2"), 15, "..\\..\\evil.dll"); CHECK(bad(t));
    t = text; t.replace(t.find("Textures\\x\\y.dds"), 16, "F4SE\\Plugins\\a.dll"); CHECK(bad(t));
    t = text; t.replace(t.find("|5368709120|"), 12, "|9999999999999|"); CHECK(bad(t));  // > 8 GB
    t = text; t.replace(t.find(std::string(64, 'c')), 64, std::string(64, 'Z')); CHECK(bad(t));
    t = text; t.replace(0, 3, "V|9"); CHECK(bad(t));                                  // protocol
    t = text + "P|M|1|" + std::string(64, 'a') + "|x.esp|1|0|\n"; CHECK(bad(t));        // data after E
    t = text; t.insert(t.find("E\n"), "L|M|1|" + std::string(64, 'e') + "|Textures\\x\\y.dds|HOWA.esp\n"); CHECK(bad(t)); // duplicate
    t = text; t.insert(t.find("E\n"), "L|M|1|" + std::string(64, 'e') + "|Textures\\z.dds|Other.esp\n"); CHECK(bad(t)); // unknown owner
    t = text; t.insert(t.find("E\n"), "N|..\\x.dll\n"); CHECK(bad(t));
    t = text; t.insert(t.find("E\n"), "Q|junk\n"); CHECK(bad(t));
    t = "V|1|400\n" + std::string(20000, 'x') + "\nE\n"; CHECK(bad(t));
}

static void TestHashing()
{
    CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);

    const std::wstring dir = TempDir();
    const std::wstring f = dir + L"\\hash.bin";
    std::string data(3 * 1024 * 1024 + 17, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 31) ^ (i >> 7));
    CHECK(WriteWholeFileAtomic(f, data));
    std::string sha; uint64_t size = 0;
    CHECK(HashFile(f, sha, size));
    CHECK(size == data.size() && sha == Sha256Hex(data.data(), data.size()));

    // Cache: a hit returns the cached value without re-reading.
    HashCache cache; std::string s2; uint64_t z2 = 0;
    CHECK(HashFileCached(cache, f, "x\\hash.bin", s2, z2) && s2 == sha);
    const std::wstring cpath = dir + L"\\cache.txt";
    cache.Save(cpath);
    HashCache c2; c2.Load(cpath);
    uint64_t sz = 0, mt = 0; GetFileSizeTime(f, sz, mt);
    std::string s3; CHECK(c2.Lookup("X\\HASH.BIN", sz, mt, s3) && s3 == sha);
    CHECK(!c2.Lookup("x\\hash.bin", sz + 1, mt, s3));

    // Proof: deterministic, nonce-dependent, content-dependent.
    const std::string n1 = "00112233445566778899aabbccddeeff", n2 = "ffeeddccbbaa99887766554433221100";
    std::string p1, p1b, p2;
    CHECK(ComputeProof(f, data.size(), n1, p1));
    CHECK(ComputeProof(f, data.size(), n1, p1b) && p1 == p1b);
    CHECK(ComputeProof(f, data.size(), n2, p2) && p1 != p2);
    CHECK(!ComputeProof(f, data.size() + 1, n1, p2));    // wrong size
    CHECK(!ComputeProof(f, data.size(), "zz", p2));       // bad nonce
    // Small file: whole content is covered.
    const std::wstring small = dir + L"\\small.bin";
    WriteWholeFileAtomic(small, "hello");
    std::string ps1, ps2;
    CHECK(ComputeProof(small, 5, n1, ps1));
    WriteWholeFileAtomic(small, "hellp");
    CHECK(ComputeProof(small, 5, n1, ps2) && ps1 != ps2);
}

static void TestPluginHeader()
{
    // Synthetic: TES4 with HEDR, two MASTs and an XXXX-sized subrecord.
    std::string rec;
    auto sub = [&](const char* type, const std::string& d)
    {
        rec.append(type, 4);
        const uint16_t sz = static_cast<uint16_t>(d.size());
        rec.append(reinterpret_cast<const char*>(&sz), 2);
        rec += d;
    };
    sub("HEDR", std::string(12, '\0'));
    sub("MAST", std::string("Fallout4.esm\0", 13));
    sub("DATA", std::string(8, '\0'));
    sub("MAST", std::string("DLCCoast.esm\0", 13));
    sub("DATA", std::string(8, '\0'));
    std::string big(70000, 'q');
    uint32_t bigSize = static_cast<uint32_t>(big.size());
    rec += "XXXX"; uint16_t four = 4; rec.append(reinterpret_cast<const char*>(&four), 2);
    rec.append(reinterpret_cast<const char*>(&bigSize), 4);
    rec += "ONAM"; uint16_t zero = 0; rec.append(reinterpret_cast<const char*>(&zero), 2); rec += big;
    std::string file = "TES4";
    uint32_t dataSize = static_cast<uint32_t>(rec.size()), flags = 0x201;
    file.append(reinterpret_cast<const char*>(&dataSize), 4);
    file.append(reinterpret_cast<const char*>(&flags), 4);
    file += std::string(12, '\0');
    file += rec;
    PluginHeader h = ParsePluginHeader(reinterpret_cast<const uint8_t*>(file.data()), file.size());
    CHECK(h.ok && h.flags == 0x201 && h.masters.size() == 2 && h.masters[1] == "DLCCoast.esm");
    // Truncated input is not a crash and not "ok".
    PluginHeader t = ParsePluginHeader(reinterpret_cast<const uint8_t*>(file.data()), 60);
    CHECK(!t.ok);
    CHECK(!ParsePluginHeader(reinterpret_cast<const uint8_t*>("TES3xxxxxxxxxxxxxxxxxxxxxxxxxxx"), 30).ok);

    // Real plugins from this machine's Data folder, if present.
    const std::wstring data = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4\\Data\\";
    for (const wchar_t* name : { L"HOWA.esp", L"CoSync.esp", L"FO4CreatureLoadingScreenRemoval.esl" })
    {
        PluginHeader r = ReadPluginHeader(data + name);
        if (!r.ok) { std::printf("  (skip real header %ls: not readable)\n", name); continue; }
        std::printf("  real %ls: flags=0x%X masters=", name, r.flags);
        for (auto& m : r.masters) std::printf("%s ", m.c_str());
        std::printf("\n");
        CHECK(!r.masters.empty() && IEquals(r.masters[0], "Fallout4.esm"));
    }
}

static void TestResolveInside()
{
    const std::wstring root = TempDir() + L"\\Data";
    CreateDirectories(root + L"\\Textures");
    std::wstring full; std::string why;
    CHECK(ResolveInside(root, "Textures\\a.dds", full, why));
    CHECK(full == root + L"\\Textures\\a.dds");
    CHECK(ResolveInside(root, "New\\Folder\\b.dds", full, why));
    CHECK(!ResolveInside(root, "..\\x.dds", full, why));
    CHECK(!ResolveInside(root, "Textures", full, why));   // existing folder, not a file

    // Junction below root must be refused (it could point anywhere).
    const std::wstring outside = TempDir() + L"\\Outside";
    CreateDirectories(outside);
    const std::wstring link = root + L"\\Linked";
    RemoveDirectoryW(link.c_str());
    std::wstring cmd = L"cmd /c mklink /J \"" + link + L"\" \"" + outside + L"\" >nul 2>&1";
    _wsystem(cmd.c_str());
    if (GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
        CHECK(!ResolveInside(root, "Linked\\x.dds", full, why));
    else
        std::printf("  (skip junction test: mklink failed)\n");
    RemoveDirectoryW(link.c_str());
}

static void TestPluginsTxt()
{
    const std::string txt = "# This file is used by Fallout 4 to keep track of your downloaded content.\r\n# Please do not modify this file.\r\n*A.esl\r\nB.esp\r\n*CoSync.esp\r\n";
    bool changed = false;
    std::string out = ActivateInPluginsTxt(txt, { "B.esp" }, {}, changed);
    CHECK(changed && out.find("*B.esp\r\n") != std::string::npos && out.find("# Please") != std::string::npos);
    out = ActivateInPluginsTxt(txt, { "New.esp" }, { "Fallout4.esm", "A.esl", "New.esp", "CoSync.esp" }, changed);
    CHECK(changed && out.find("*A.esl\r\n*New.esp\r\nB.esp") != std::string::npos);
    out = ActivateInPluginsTxt(txt, { "a.ESL" }, {}, changed);
    CHECK(!changed && out == txt);
    out = ActivateInPluginsTxt("", { "X.esp" }, {}, changed);
    CHECK(changed && out == "*X.esp\n");
    const auto names = ParseNameList(txt);
    CHECK(names.size() == 3 && names[0] == "A.esl" && names[1] == "B.esp");
}

static void TestVortex()
{
    const std::string json = "{\"instance\":\"x\",\"gameId\":\"fallout4\",\"targetPath\":\"c:\\\\Games\\\\Fallout 4\\\\Data\\\\\","
        "\"files\":[{\"relPath\":\"HOWA.esp\",\"source\":\"HOWA 20\",\"target\":\"\",\"time\":1},"
        "{\"relPath\":\"Textures\\\\a\\u0041.dds\",\"source\":\"HOWA 20\"},{\"relPath\":5,\"source\":\"x\"}]}";
    std::vector<std::pair<std::string, std::string>> files; std::string why;
    CHECK(ParseVortexDeployment(json, "C:\\Games\\Fallout 4\\Data", files, why));
    CHECK(files.size() == 2 && files[1].first == "Textures\\aA.dds" && files[1].second == "HOWA 20");
    CHECK(!ParseVortexDeployment(json, "D:\\Other\\Data", files, why));
    CHECK(!ParseVortexDeployment("{\"gameId\":\"skyrimse\"}", "x", files, why));
    CHECK(!ParseVortexDeployment("{\"gameId\":\"fallout4\",", "x", files, why));
    std::string deep(200, '['); CHECK(!ParseVortexDeployment(deep, "x", files, why));

    // The real deployment file on this machine, if present.
    std::string real;
    if (ReadWholeFile(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4\\Data\\vortex.deployment.json", real, 64u << 20))
    {
        const bool ok = ParseVortexDeployment(real, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4\\Data", files, why);
        std::printf("  real vortex.deployment.json: ok=%d files=%zu %s\n", ok ? 1 : 0, files.size(), ok ? "" : why.c_str());
        CHECK(ok && files.size() > 0);
    }
}

int main()
{
    TestPaths();
    TestManifest();
    TestHashing();
    TestPluginHeader();
    TestResolveInside();
    TestPluginsTxt();
    TestVortex();
    std::printf("\nManifestTests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
