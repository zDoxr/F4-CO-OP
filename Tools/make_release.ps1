# Builds the CoSync release for Vortex / Mod Organizer 2.
#
#   powershell -ExecutionPolicy Bypass -File Tools\make_release.ps1
#
# Output (repo root):
#   Release\CoSync\              the package: fomod\ (installer) + CoSync\ (Data files)
#   Release\CoSync-<ver>.zip     the same, zipped -- this is what you upload
#
# Everything installs into Data (no files next to Fallout4.exe). The FOMOD
# installs the CoSync\ folder into Data. It shows one page only when PrismaUI
# or Address Library (1.11.240) is missing, and otherwise installs with no
# questions. Mod managers can't enable other mods; that page tells the player
# what to get. CoSync enables its own CoSync.esp at game start.
# Build Release|x64 first (the DLL is taken from x64\Release).

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root 'DoxCoSync'

# Version from Core\Version.h (the one source of truth).
$vh = Get-Content (Join-Path $proj 'Core\Version.h') -Raw
$maj = [regex]::Match($vh, '#define COSYNC_VERSION_MAJOR (\d+)').Groups[1].Value
$min = [regex]::Match($vh, '#define COSYNC_VERSION_MINOR (\d+)').Groups[1].Value
$pat = [regex]::Match($vh, '#define COSYNC_VERSION_PATCH (\d+)').Groups[1].Value
$ver = "$maj.$min.$pat"

$pkg = Join-Path $root 'Release\CoSync'
$out = Join-Path $pkg 'CoSync'          # what lands in Data
$zip = Join-Path $root "Release\CoSync-$ver.zip"
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }

function Put($src, $dstRel) {
    if (-not (Test-Path $src)) { throw "Missing: $src" }
    $dst = Join-Path $out $dstRel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item $src $dst -Force
}

$data = Join-Path $root 'F4se-Data\Data'
Put (Join-Path $root 'x64\Release\DoxCoSync.dll')                 'F4SE\Plugins\DoxCoSync.dll'
Put (Join-Path $data 'F4SE\Plugins\CoSync.ini')                   'F4SE\Plugins\CoSync.ini'
foreach ($d in 'GameNetworkingSockets.dll', 'abseil_dll.dll', 'libcrypto-3-x64.dll', 'libprotobuf.dll') {
    Put (Join-Path $proj "F4se-Data\GameRoot\$d")                 "F4SE\Plugins\CoSync\$d"
}
Put (Join-Path $data 'CoSync.esp')                                'CoSync.esp'
foreach ($s in 'CoSync.pex', 'CoSyncPlayer.pex', 'CoSyncQuest.pex') {
    Put (Join-Path $data "Scripts\$s")                            "Scripts\$s"
}
$views = Join-Path $proj 'F4se-Data\Data\PrismaUI_F4\views\DoxCoSync'
New-Item -ItemType Directory -Force (Join-Path $out 'PrismaUI_F4\views') | Out-Null
Copy-Item $views (Join-Path $out 'PrismaUI_F4\views\DoxCoSync') -Recurse -Force

# ── FOMOD ──────────────────────────────────────────────────────────────────
$fomod = Join-Path $pkg 'fomod'
New-Item -ItemType Directory -Force $fomod | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $fomod 'info.xml'), @"
<?xml version="1.0" encoding="UTF-8"?>
<fomod>
  <Name>CoSync</Name>
  <Author>Doxr</Author>
  <Version>$ver</Version>
  <Description>Co-op multiplayer for Fallout 4. Needs F4SE (1.11.240), Address Library and PrismaUI.</Description>
</fomod>
"@, $utf8)
[System.IO.File]::WriteAllText((Join-Path $fomod 'ModuleConfig.xml'), @'
<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
  <moduleName>CoSync</moduleName>
  <requiredInstallFiles>
    <folder source="CoSync" destination="" />
  </requiredInstallFiles>
  <installSteps order="Explicit">
    <installStep name="Missing requirements">
      <visible>
        <dependencies operator="Or">
          <fileDependency file="F4SE\Plugins\PrismaUI_F4.dll" state="Missing" />
          <fileDependency file="F4SE\Plugins\version-1-11-240-0.bin" state="Missing" />
        </dependencies>
      </visible>
      <optionalFileGroups order="Explicit">
        <group name="CoSync won't run without these" type="SelectAll">
          <plugins order="Explicit">
            <plugin name="Install CoSync anyway">
              <description>You're missing PrismaUI or Address Library (All in One). Get both from Nexus and enable them. You also need F4SE for Fallout 4 1.11.240.</description>
              <typeDescriptor><type name="Required" /></typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>
'@, $utf8)
[xml](Get-Content (Join-Path $fomod 'ModuleConfig.xml') -Raw) | Out-Null   # must parse

# Zip with '/' separators (Windows PowerShell's Compress-Archive writes '\',
# which some extractors take as part of the file name).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$fs = [System.IO.File]::Open($zip, 'CreateNew')
$za = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem $pkg -Recurse -File | ForEach-Object {
        $name = $_.FullName.Substring($pkg.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, $name, 'Optimal') | Out-Null
    }
} finally { $za.Dispose(); $fs.Dispose() }
Write-Host "CoSync $ver"
Get-ChildItem $pkg -Recurse -File | ForEach-Object { '  ' + $_.FullName.Substring($pkg.Length + 1) }
Write-Host "-> $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
