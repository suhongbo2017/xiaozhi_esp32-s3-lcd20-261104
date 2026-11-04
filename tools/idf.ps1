# idf.ps1 — run idf.py inside a correctly configured ESP-IDF v6.0.2 environment.
#
# Why this exists: git-bash sets MSYSTEM=MINGW64, which makes ESP-IDF's
# idf_tools.py abort with "MSys/Mingw is not supported". Clearing it from
# git-bash does not work; it must be cleared inside PowerShell.
#
# Usage (from anywhere, e.g. git-bash):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 -- build
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 -- -p COM15 flash monitor
#
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$env:MSYSTEM = ''
$env:IDF_COMPONENT_CACHE_PATH = 'D:\PI\idf-cache'

& 'C:\esp-idf-6.0.2\export.ps1' | Out-Null

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

# allow an optional leading "--" separator
if ($IdfArgs.Count -gt 0 -and $IdfArgs[0] -eq '--') {
    $IdfArgs = $IdfArgs[1..($IdfArgs.Count - 1)]
}

Write-Host "IDF_PATH=$env:IDF_PATH"
Write-Host "project =$ProjectRoot"
Write-Host "idf.py  $($IdfArgs -join ' ')"

& idf.py @IdfArgs
exit $LASTEXITCODE
