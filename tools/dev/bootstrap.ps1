# Sets up a fresh checkout on Windows: git repository, pinned SDK submodules, xmake configuration.
# Run from the repository root:  powershell -ExecutionPolicy Bypass -File tools\dev\bootstrap.ps1
#
# Requirements: git, xmake (https://xmake.io), Visual Studio 2022 with "Desktop development with C++".

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..\..")

# SDK versions this code was written and compile-checked against.
$submodules = @(
    @{ Path = "vendor/RED4ext.SDK"; Url = "https://github.com/WopsS/RED4ext.SDK.git";     Commit = "ad7277714ad30d6885d7050c5ba24fa0102f6920" },
    @{ Path = "vendor/RedLib";      Url = "https://github.com/psiberx/cp2077-red-lib.git"; Commit = "6822105d15d1b8bd1e6aae5e57827835e585cfe8" }
)

if (-not (Test-Path ".git")) {
    git init | Out-Null
    Write-Host "Initialized git repository"
}

foreach ($module in $submodules) {
    if (-not (Test-Path (Join-Path $module.Path ".git"))) {
        git submodule add --force $module.Url $module.Path
    }
    git -C $module.Path fetch --quiet origin
    git -C $module.Path checkout --quiet $module.Commit
    Write-Host ("{0} at {1}" -f $module.Path, $module.Commit.Substring(0, 10))
}

xmake f -p windows -a x64 -m releasedbg -y
Write-Host ""
Write-Host "Done. Build with:  xmake"
Write-Host "Copy into the game with:  xmake f --game_dir=""C:\Path\To\Cyberpunk 2077"" ; xmake"
