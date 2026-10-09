# Parses every PowerShell script in tools/ without running it: a syntax error otherwise only shows when the script
# runs (make-release.ps1 at the end of a Windows build, check-env.ps1 on someone's PC). Runs in the Linux build.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "../..")
$failed = 0
foreach ($script in Get-ChildItem (Join-Path $root "tools") -Recurse -Filter *.ps1) {
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$errors) | Out-Null
    foreach ($e in $errors) {
        Write-Host "$($script.FullName):$($e.Extent.StartLineNumber): $($e.Message)" -ForegroundColor Red
        $failed++
    }
}
if ($failed -gt 0) {
    exit 1
}
Write-Host "PowerShell scripts parse."
