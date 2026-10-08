# Starts a local server and two games that connect to it, to test co-op on one PC.
#
#   powershell -ExecutionPolicy Bypass -File tools\coop\start-local.ps1 -Game "D:\Games\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"
#
# Build first (see README.md): xmake build Server.Loader, and xmake build Cyberpunk2077 (which links the mod into
# the game). Each game starts with --online and its own --name; load a save in each, then hold "/" to connect.
param(
    [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe",
    [ValidateSet("debug", "releasedbg", "release")]
    [string]$Mode = "debug",
    [int]$Port = 11778,
    [string[]]$Names = @("Host", "Guest"),
    [int]$DelayBetweenGames = 25,
    [switch]$NoServer
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$build = Join-Path $root "build\windows\x64\$Mode"

if (!(Test-Path $Game)) {
    throw "Cyberpunk2077.exe not found at '$Game'. Pass -Game with its full path."
}

if (-not $NoServer) {
    $server = Join-Path $build "Server.Loader.exe"
    if (!(Test-Path $server)) {
        throw "No server at '$server'. Build it first: xmake f -m $Mode; xmake build Server.Loader"
    }
    # Release servers refuse to start without admin credentials for their web API; local test values.
    $env:CYBERPUNKMP_ADMIN_USERNAME = "admin"
    $env:CYBERPUNKMP_ADMIN_PASSWORD = [guid]::NewGuid().ToString()
    Write-Host "Starting the server ($server)"
    Start-Process -FilePath $server -WorkingDirectory $build
    Start-Sleep -Seconds 3
}

$first = $true
foreach ($name in $Names) {
    if (-not $first) {
        # Let the first game get through its startup before the second one competes for the disk.
        Write-Host "Waiting $DelayBetweenGames s before the next game"
        Start-Sleep -Seconds $DelayBetweenGames
    }
    $first = $false
    Write-Host "Starting the game as '$name'"
    Start-Process -FilePath $Game -WorkingDirectory (Split-Path $Game) `
        -ArgumentList "--online", "--ip=127.0.0.1", "--port=$Port", "--name=$name", "--skipStartMenu"
}

Write-Host ""
Write-Host "In each game: load a save, then hold '/' to connect. The first to connect is the story host."
Write-Host "Chat: ';' to type, '/help' for the co-op commands. Logs: <game>\red4ext\logs and the server window."
Write-Host "Don't save in both games at once: they share the same save folder."
