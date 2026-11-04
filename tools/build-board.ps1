# build-board.ps1 — canonical board build for this project (ESP-IDF v6.0.2).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-board.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-board.ps1 -Board bread-compact-wifi-lcd -Name bread-compact-wifi-lcd
#
param(
    [string]$Board = 'bread-compact-wifi-lcd',
    [string]$Name  = 'bread-compact-wifi-lcd'
)

$env:MSYSTEM = ''
$env:IDF_COMPONENT_CACHE_PATH = 'D:\PI\idf-cache'

& 'C:\esp-idf-6.0.2\export.ps1' | Out-Null

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

Write-Host "board = $Board   name = $Name"
python scripts/build.py $Board --name $Name
exit $LASTEXITCODE
