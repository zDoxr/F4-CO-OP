# Builds the CoSync release for Vortex / Mod Organizer 2.
#
#   powershell -ExecutionPolicy Bypass -File Tools\make_release.ps1
#
# Output (repo root):
#   Release\CoSync\              the files, laid out as the game's Data folder
#   Release\CoSync-<ver>.zip     the same, zipped -- this is what you upload
#
# Everything installs into Data (no files next to Fallout4.exe), so both mod
# managers take the archive as-is: esp, Scripts, F4SE and PrismaUI_F4 sit at
# the root of the zip, which is what they look for.
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

$out = Join-Path $root 'Release\CoSync'
$zip = Join-Path $root "Release\CoSync-$ver.zip"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
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

# Name / version / author for the mod managers (metadata only, no installer).
New-Item -ItemType Directory -Force (Join-Path $out 'fomod') | Out-Null
@"
<?xml version="1.0" encoding="UTF-8"?>
<fomod>
  <Name>CoSync</Name>
  <Author>Doxr</Author>
  <Version>$ver</Version>
  <Description>Co-op multiplayer for Fallout 4. Needs F4SE (1.11.240) and PrismaUI.</Description>
</fomod>
"@ | Set-Content -Encoding UTF8 (Join-Path $out 'fomod\info.xml')

# Zip with '/' separators (Windows PowerShell's Compress-Archive writes '\',
# which some extractors take as part of the file name).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$fs = [System.IO.File]::Open($zip, 'CreateNew')
$za = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem $out -Recurse -File | ForEach-Object {
        $name = $_.FullName.Substring($out.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, $name, 'Optimal') | Out-Null
    }
} finally { $za.Dispose(); $fs.Dispose() }
Write-Host "CoSync $ver"
Get-ChildItem $out -Recurse -File | ForEach-Object { '  ' + $_.FullName.Substring($out.Length + 1) }
Write-Host "-> $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
