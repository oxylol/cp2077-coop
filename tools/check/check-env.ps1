# Checks this PC can build cp2077-coop, before a long build fails halfway through. Run it before building, and
# after `xmake f` run `xmake check-deps` (the packages it resolved). Playing needs none of this: see README.md.
#
#   powershell -ExecutionPolicy Bypass -File tools\check\check-env.ps1
#
# Each problem says how to fix it. Exit code 1 when something has to be fixed first.
param(
    [string]$Root = "",
    [string]$KitsInclude = "",
    [string]$VsWhere = ""
)

$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path }
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows
$programFilesX86 = ${env:ProgramFiles(x86)}
if (-not $KitsInclude -and $programFilesX86) { $KitsInclude = Join-Path $programFilesX86 "Windows Kits\10\Include" }
if (-not $VsWhere -and $programFilesX86) { $VsWhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe" }

$script:failures = 0
$script:warnings = 0
function Ok([string]$message) { Write-Host "  ok    $message" -ForegroundColor Green }
function Warn([string]$message) { $script:warnings++; Write-Host "  warn  $message" -ForegroundColor Yellow }
function Fail([string]$message) { $script:failures++; Write-Host "  FAIL  $message" -ForegroundColor Red }

# Output of a command, or $null when it's missing or fails.
function Run([string]$command, [string[]]$arguments) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) { return $null }
    try {
        $output = & $command @arguments 2>$null | Out-String
        if ($LASTEXITCODE -ne 0) { return $null }
        return $output
    } catch {
        return $null
    }
}

# The submodule paths a .gitmodules lists.
function SubmodulePaths([string]$dir) {
    $file = Join-Path $dir ".gitmodules"
    if (-not (Test-Path $file)) { return @() }
    return @(Get-Content $file | Where-Object { $_ -match '^\s*path\s*=\s*(.+?)\s*$' } | ForEach-Object { $Matches[1] })
}

Write-Host "Checking the build environment for $Root"

# A git clone with its submodules. GitHub's "Download ZIP" has neither: vendor\ stays empty, and the C# SDK
# generator looks for the repository.
$inside = Run "git" @("-C", $Root, "rev-parse", "--is-inside-work-tree")
if ($null -eq $inside) {
    if (Get-Command git -ErrorAction SilentlyContinue) {
        Fail "$Root isn't a git clone (a ZIP download?). Clone it: git clone --recursive https://github.com/oxylol/cp2077-coop.git"
    } else {
        Fail "git not found. Install Git for Windows (https://git-scm.com), then clone with: git clone --recursive https://github.com/oxylol/cp2077-coop.git"
    }
} else {
    Ok "git clone"
}

$empty = @()
$pending = New-Object System.Collections.Queue
$pending.Enqueue($Root)
while ($pending.Count -gt 0) {
    $dir = $pending.Dequeue()
    foreach ($sub in SubmodulePaths $dir) {
        $subdir = Join-Path $dir $sub
        if (-not (Test-Path $subdir) -or -not (Get-ChildItem $subdir -Force | Select-Object -First 1)) {
            $empty += $subdir.Substring($Root.Length).TrimStart("\", "/")
        } else {
            $pending.Enqueue($subdir)
        }
    }
}
if ($empty.Count -gt 0) {
    Fail ("Submodules missing: " + ($empty -join ", ") + ". Run: git submodule update --init --recursive")
} else {
    Ok "submodules (vendor\)"
}

# xmake 3.1.1 or newer (set_xmakever in xmake.lua): the package recipes are written for the newest xmake, and
# 2.9.9 can no longer install some of them.
$xmakeVersion = Run "xmake" @("--version")
if ($null -eq $xmakeVersion) {
    Fail "xmake not found. Install it from https://xmake.io (3.1.1 or newer)."
} elseif ($xmakeVersion -match 'v(\d+)\.(\d+)\.(\d+)') {
    $version = [version]"$($Matches[1]).$($Matches[2]).$($Matches[3])"
    if ($version -lt [version]"3.1.1") {
        Fail "xmake $version is too old; 3.1.1 or newer is needed. Run: xmake update"
    } else {
        Ok "xmake $version"
    }
} else {
    Warn "Couldn't read xmake's version."
}

if ($onWindows) {
    # Visual Studio 2022 with the C++ tools. The libraries are built and tested with it; Visual Studio 2026 isn't.
    if (-not $VsWhere -or -not (Test-Path $VsWhere)) {
        Fail "Visual Studio not found. Install Visual Studio 2022 with the 'Desktop development with C++' workload."
    } else {
        $vs2022 = Run $VsWhere @("-products", "*", "-version", "[17.0,18.0)", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationVersion")
        if (-not $vs2022 -or -not $vs2022.Trim()) {
            $others = Run $VsWhere @("-products", "*", "-property", "displayName")
            $found = ""
            if ($others -and $others.Trim()) { $found = " (found: " + (($others.Trim() -split "\r?\n") -join ", ") + ")" }
            Fail "Visual Studio 2022 with the C++ tools not found$found. Install it with the 'Desktop development with C++' workload."
        } else {
            Ok ("Visual Studio 2022 " + (($vs2022.Trim() -split "\r?\n")[0]))
        }
    }

    # A Windows SDK below 10.0.26100: protobuf doesn't build with 10.0.26100.
    $sdks = @()
    if ($KitsInclude -and (Test-Path $KitsInclude)) {
        $sdks = @(Get-ChildItem $KitsInclude -Directory |
            Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName "um\Windows.h")) } |
            ForEach-Object { [version]$_.Name } | Sort-Object)
    }
    $usable = @($sdks | Where-Object { $_.Build -lt 26100 })
    if ($usable.Count -eq 0) {
        $installed = "none"
        if ($sdks.Count -gt 0) { $installed = $sdks -join ", " }
        Fail "No Windows SDK below 10.0.26100 (installed: $installed). Visual Studio Installer > Modify > Individual components > 'Windows 11 SDK (10.0.22621.0)'."
    } else {
        Ok ("Windows SDK " + ($usable -join ", "))
    }
}

# The saved xmake configuration, if there is one yet.
$conf = Get-ChildItem (Join-Path $Root ".xmake") -Recurse -Filter "xmake.conf" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($conf) {
    $values = @{}
    foreach ($line in Get-Content $conf.FullName) {
        if ($line -match '^\s*(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"') { $values[$Matches[1]] = $Matches[2] -replace '\\\\', '\' }
    }
    if ($onWindows) {
        if (-not $values["vs_sdkver"]) {
            Fail "xmake is configured without --vs_sdkver, so it uses the newest Windows SDK. Reconfigure (README.md): xmake f -c -m releasedbg --vs_sdkver=10.0.22621.0 --game=<Cyberpunk2077.exe> -y"
        } elseif (($values["vs_sdkver"] -notmatch '^10\.0\.(\d+)') -or ([int]$Matches[1] -ge 26100)) {
            Fail "xmake is configured with Windows SDK $($values['vs_sdkver']); it has to be below 10.0.26100. Reconfigure with --vs_sdkver=10.0.22621.0 (xmake f -c ...)."
        } else {
            Ok "xmake configured with Windows SDK $($values['vs_sdkver'])"
        }
    }
    $game = $values["game"]
    if ($game -and $game -ne "Cyberpunk2077.exe" -and -not (Test-Path $game)) {
        Warn "The configured game ($game) doesn't exist; xmake build Cyberpunk2077 links the mod into it. Reconfigure with --game=<path to Cyberpunk2077.exe>."
    }

    # An older CyberpunkMP in the game would load next to this mod.
    if ($game -and (Test-Path $game)) {
        $plugins = Join-Path (Split-Path (Split-Path (Split-Path $game))) "red4ext/plugins"
        foreach ($old in @("zzzCyberpunkMP", "CyberpunkMP")) {
            $oldPath = Join-Path $plugins $old
            if (Test-Path $oldPath) {
                Fail "An older CyberpunkMP is installed in the game ($oldPath): it would load next to this mod. Delete that folder."
            }
        }
    }
} else {
    Write-Host "  (not configured yet: run xmake f as in README.md, then this again, then xmake check-deps)"
}

Write-Host ""
if ($script:failures -gt 0) {
    Write-Host "$script:failures problem(s) to fix first, $script:warnings warning(s)." -ForegroundColor Red
    exit 1
}
Write-Host "Ready to build ($script:warnings warning(s)). After xmake f, run: xmake check-deps" -ForegroundColor Green
exit 0
