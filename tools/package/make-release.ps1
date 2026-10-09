# Builds the release zip: extracted into the game folder, it's the mod and everything it needs.
#
#   xmake f -m release --vs_sdkver=10.0.22621.0 -y; xmake build Client
#   powershell -ExecutionPolicy Bypass -File tools\package\make-release.ps1 -Version 0.2.0
#
# The requirements (requirements.json) are downloaded from their GitHub releases and checked against their sha256;
# a changed file stops the build. After changing a version there, run with -UpdateHashes and commit the hashes it
# writes. Downloads are kept in build\package\downloads.
#
# The zip, for the game folder:
#   bin/, engine/, r6/, red4ext/    the requirements, as their own zips lay them out
#   red4ext/plugins/zzzCyberpunkCoop/
#     CyberpunkCoop.dll, coop.ini, README.txt, LICENSE.md, THIRD_PARTY.txt, licenses/
#     assets/redscript, assets/Tweaks, assets/Inputs, assets/Archives   (where a release build looks for them)
param(
    [string]$Version = "dev",
    [string]$Dll = "",
    [string]$OutDir = "",
    [switch]$UpdateHashes
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is many times slower with the progress bar
Add-Type -AssemblyName System.IO.Compression.FileSystem

$Root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if (-not $Dll) { $Dll = Join-Path $Root "build/windows/x64/release/CyberpunkCoop.dll" }
if (-not $OutDir) { $OutDir = Join-Path $Root "build/package" }

# The folder name starts with "zzz": RED4ext loads plugins alphabetically, and Input Loader has to be loaded first.
$PluginPath = "red4ext/plugins/zzzCyberpunkCoop"
# Where the requirements' zips may put files. Anything else is a surprise worth stopping for.
$AllowedRoots = @("bin", "engine", "r6", "red4ext", "archive")

function Fail([string]$Message) {
    Write-Host "make-release: $Message" -ForegroundColor Red
    exit 1
}

function Get-Sha256([string]$Path) {
    (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

if (!(Test-Path $Dll)) {
    Fail "$Dll not found. Build it first: xmake f -m release ... -y; xmake build Client"
}

$requirementsFile = Join-Path $PSScriptRoot "requirements.json"
$manifest = Get-Content $requirementsFile -Raw | ConvertFrom-Json

$stage = Join-Path $OutDir "stage"
$downloads = Join-Path $OutDir "downloads"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, $downloads | Out-Null

$plugin = Join-Path $stage $PluginPath
$licenses = Join-Path $plugin "licenses"
New-Item -ItemType Directory -Force -Path $licenses | Out-Null

# The requirements
$hashesChanged = $false
$thirdParty = @()
foreach ($requirement in $manifest.requirements) {
    $name = $requirement.name
    $url = "https://github.com/$($requirement.repo)/releases/download/$($requirement.tag)/$($requirement.asset)"
    $file = Join-Path $downloads $requirement.asset

    if (!(Test-Path $file) -or (Get-Sha256 $file) -ne $requirement.sha256) {
        Write-Host "Downloading $name $($requirement.tag): $url"
        Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
    }
    $hash = Get-Sha256 $file
    if ($hash -ne $requirement.sha256) {
        if ($UpdateHashes) {
            Write-Host "  $name sha256: $($requirement.sha256) -> $hash" -ForegroundColor Yellow
            $requirement.sha256 = $hash
            $hashesChanged = $true
        } else {
            Fail "$name ($($requirement.asset)) has sha256 $hash, requirements.json says $($requirement.sha256). If the version was changed on purpose, run with -UpdateHashes."
        }
    }

    $zip = [System.IO.Compression.ZipFile]::OpenRead($file)
    try {
        $count = 0
        foreach ($entry in $zip.Entries) {
            $relative = $entry.FullName -replace '\\', '/'
            if ($relative.EndsWith("/")) { continue }
            $parts = $relative.Split('/')
            if ([System.IO.Path]::IsPathRooted($relative) -or $parts -contains ".." -or $AllowedRoots -notcontains $parts[0]) {
                Fail "$name's zip has '$($entry.FullName)', outside of $($AllowedRoots -join ', '). Check the release before bundling it."
            }
            $target = Join-Path $stage $relative
            if (Test-Path $target) {
                Fail "$name's zip has '$relative', which another requirement already put there."
            }
            New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target)
            $count++
        }
    } finally {
        $zip.Dispose()
    }

    # The license: in the requirement's zip, or (not in it) from tools\package\licenses into ours.
    if ($requirement.license.StartsWith("licenses/")) {
        $source = Join-Path $PSScriptRoot $requirement.license
        if (!(Test-Path $source)) { Fail "$name's license $source is missing." }
        Copy-Item $source (Join-Path $licenses (Split-Path $source -Leaf))
        $licenseInZip = "$PluginPath/licenses/$(Split-Path $source -Leaf)"
    } else {
        $licenseInZip = $requirement.license
        if (!(Test-Path (Join-Path $stage $licenseInZip))) { Fail "$name's zip has no $licenseInZip any more." }
    }

    Write-Host "  $name $($requirement.tag): $count files"
    $thirdParty += "$name $($requirement.tag)`r`n  https://github.com/$($requirement.repo)`r`n  License: $($licenseInZip.Replace('/', '\'))`r`n"
}

if ($hashesChanged) {
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -Path $requirementsFile -Encoding utf8
    Write-Host "requirements.json updated with the new hashes: check them, then commit." -ForegroundColor Yellow
}

# The mod
Copy-Item $Dll $plugin
Copy-Item (Join-Path $Root "code/assets/coop.ini") $plugin
Copy-Item (Join-Path $Root "LICENSE.md") $plugin
(Get-Content (Join-Path $PSScriptRoot "README.txt") -Raw).Replace("{VERSION}", $Version) |
    Set-Content -Path (Join-Path $plugin "README.txt") -Encoding utf8 -NoNewline
$thirdPartyText = "Cyberpunk Coop bundles these, unchanged, as their authors release them:`r`n`r`n" + ($thirdParty -join "`r`n")
Set-Content -Path (Join-Path $plugin "THIRD_PARTY.txt") -Value $thirdPartyText -Encoding utf8 -NoNewline

$assets = Join-Path $plugin "assets"
$sources = Join-Path $Root "code/assets"
foreach ($pattern in @(
        @{ Folder = "redscript"; Filter = "*.reds" },
        @{ Folder = "Tweaks"; Filter = "*.tweak" })) {
    $from = Join-Path $sources $pattern.Folder
    Get-ChildItem $from -Recurse -File -Filter $pattern.Filter | ForEach-Object {
        $relative = $_.FullName.Substring($from.Length).TrimStart('\', '/')
        $target = Join-Path (Join-Path $assets $pattern.Folder) $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item $_.FullName $target
    }
}
New-Item -ItemType Directory -Force -Path (Join-Path $assets "Inputs"), (Join-Path $assets "Archives") | Out-Null
Copy-Item (Join-Path $sources "Inputs/CyberpunkMP.xml") (Join-Path $assets "Inputs")
Copy-Item (Join-Path $sources "Archives/packed/archive/pc/mod/CyberpunkMP.archive") (Join-Path $assets "Archives")

# What the DLL loads at start (Main.cpp): without these the game stops with an error.
foreach ($required in @("$PluginPath/CyberpunkCoop.dll", "$PluginPath/assets/redscript", "$PluginPath/assets/Tweaks",
                        "$PluginPath/assets/Inputs/CyberpunkMP.xml", "$PluginPath/assets/Archives/CyberpunkMP.archive",
                        "red4ext/plugins/input_loader/input_loader.dll", "red4ext/plugins/ArchiveXL/ArchiveXL.dll",
                        "red4ext/plugins/TweakXL/TweakXL.dll", "red4ext/plugins/Codeware/Codeware.dll",
                        "red4ext/RED4ext.dll", "bin/x64/winmm.dll", "engine/tools/scc.exe")) {
    if (!(Test-Path (Join-Path $stage $required))) { Fail "The package has no $required." }
}

# The zip
$zipPath = Join-Path $OutDir "CyberpunkCoop-$Version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
$stageFull = (Resolve-Path $stage).Path
$files = Get-ChildItem $stageFull -Recurse -File | Sort-Object FullName
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in $files) {
        # Zip entries use "/" whatever the system.
        $entryName = $file.FullName.Substring($stageFull.Length).TrimStart('\', '/') -replace '\\', '/'
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $zip.Dispose()
}

$files = $files.Count
$size = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host ""
Write-Host "$zipPath ($files files, $size MB)" -ForegroundColor Green
Write-Host "sha256 $(Get-Sha256 $zipPath)"
