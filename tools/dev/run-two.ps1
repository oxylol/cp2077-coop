# Starts two game instances side by side for co-op testing on one PC (docs/05-local-testing.md §2).
#
# This only works if spike S13 shows that your store lets a second instance start; run that check first
# (start the game normally, then run Cyberpunk2077.exe from bin\x64 a second time and note what happens).
# With 16 GB RAM expect heavy paging; set the lowest preset and 1280x720 windowed in the game settings first.
#
#   powershell -ExecutionPolicy Bypass -File tools\dev\run-two.ps1 -GameDir "C:\Games\Cyberpunk 2077"
#
# Each instance gets -coopInstance=N, which the plugin uses for its own player name and client id.
# Separate saves/settings per instance come with dev instance mode in M0b, once S13 shows what is needed;
# until then don't save in the second instance. Host and join from the CET dev panel ("Co-op (dev)").

param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [int]$DelaySeconds = 60
)

$exe = Join-Path $GameDir "bin\x64\Cyberpunk2077.exe"
if (-not (Test-Path $exe)) {
    throw "Cyberpunk2077.exe not found in $GameDir\bin\x64"
}

Write-Host "Starting instance 1 (host)..."
$first = Start-Process -FilePath $exe -ArgumentList "-coopInstance=1" -WorkingDirectory (Split-Path $exe) -PassThru

Write-Host "Waiting $DelaySeconds s before starting instance 2..."
Start-Sleep -Seconds $DelaySeconds

if ($first.HasExited) {
    throw "Instance 1 exited early (exit code $($first.ExitCode))."
}

Write-Host "Starting instance 2 (client)..."
$second = Start-Process -FilePath $exe -ArgumentList "-coopInstance=2" -WorkingDirectory (Split-Path $exe) -PassThru
Start-Sleep -Seconds 20

if ($second.HasExited) {
    Write-Warning "Instance 2 exited (exit code $($second.ExitCode)). The game or store probably blocks a second instance (spike S13)."
    Write-Warning "Use the headless tools instead: build\windows\x64\releasedbg\coop-sim.exe client --connect 127.0.0.1:27077"
} else {
    Write-Host "Both instances are running. Host in window 1, join 127.0.0.1:27077 in window 2."
}
