# Starts two copies of the game on this PC to try co-op: one as "Host", one as "Guest".
#
#   powershell -ExecutionPolicy Bypass -File tools\coop\start-local.ps1 -Game "D:\Games\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"
#
# Needs the mod in that game (the release zip, or `xmake build Cyberpunk2077` for a dev build). Both games read the
# same coop.ini (same password, join_address 127.0.0.1:11778 by default); --name tells them apart.
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
    $first = $false
    Write-Host "Starting the game as '$name'"
    Start-Process -FilePath $Game -WorkingDirectory (Split-Path $Game) -ArgumentList "--name=$name", "--skipStartMenu"
}

Write-Host ""
Write-Host "In '$($Names[0])': load a save, hold '/' to host. In '$($Names[1])': load a save, hold '.' to join."
Write-Host "Logs: <game>\red4ext\logs. Don't save in both games at once: they share the same save folder."
