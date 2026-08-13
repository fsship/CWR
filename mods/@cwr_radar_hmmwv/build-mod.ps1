[CmdletBinding()]
param(
    [string]$Configuration = "x64-win-rwdi"
)

$ErrorActionPreference = "Stop"
$modRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $modRoot "..\..")).Path
$tool = Join-Path $repoRoot "dist\$Configuration\PoseidonTools.exe"
$source = Join-Path $modRoot "src\cwr_radar_hmmwv"
$output = Join-Path $modRoot "addons\cwr_radar_hmmwv.pbo"

if (-not (Test-Path -LiteralPath $tool)) {
    throw "PoseidonTools not found: $tool"
}

& $tool config tojson (Join-Path $source "config.cpp") | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "config.cpp validation failed"
}

& $tool pbo pack $source $output
if ($LASTEXITCODE -ne 0) {
    throw "PBO packing failed"
}

Write-Host "Built $output"
