# Starts two copies of the game on this PC to try co-op: one as "Host", one as "Guest".
#
#   powershell -ExecutionPolicy Bypass -File tools\coop\start-local.ps1 -Game "D:\Games\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"
#
# Needs the mod in that game (the release zip, or `xmake build Cyberpunk2077` for a dev build). Both games read the
# same coop.ini (so the same password); --name tells them apart. Both are one Steam user here, so they connect by
# address instead of through Steam: --steam=false, and the guest joins 127.0.0.1 (--join).
# In the first game load a save and hold "/" to host; in the second load a save and hold "." to join.
param(
    [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe",
    [string[]]$Names = @("Host", "Guest"),
    [int]$DelayBetweenGames = 25
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $Game)) {
    throw "Cyberpunk2077.exe not found at '$Game'. Pass -Game with its full path."
}

$first = $true
foreach ($name in $Names) {
    if (-not $first) {
        # Let the first game get through its startup before the second one competes for the disk.
        Write-Host "Waiting $DelayBetweenGames s before the next game"
        Start-Sleep -Seconds $DelayBetweenGames
    }
    Write-Host "Starting the game as '$name'"
    $arguments = @("--name=$name", "--steam=false", "--skipStartMenu")
    if (-not $first) {
        $arguments += "--join=127.0.0.1:11778"
    }
    $first = $false
    Start-Process -FilePath $Game -WorkingDirectory (Split-Path $Game) -ArgumentList $arguments
}

Write-Host ""
Write-Host "In '$($Names[0])': load a save, hold '/' to host. In '$($Names[1])': load a save, hold '.' to join."
Write-Host "Logs: <game>\red4ext\plugins\zzzCyberpunkCoop\CyberpunkCoop.log (first game) and CyberpunkCoop-2.log (second)."
Write-Host "Don't save in both games at once: they share the save folder."
