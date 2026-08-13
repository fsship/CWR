[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = "Stop"

$modRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $modRoot "..\..")).Path
$tool = Join-Path $repoRoot "dist\x64-win-rwdi\PoseidonTools.exe"
$source = Join-Path $modRoot "editable-config.cpp"
$backup = Join-Path $repoRoot "games-res\BIN\CONFIG.BIN.before-cwr-radar-hmmwv"
$destination = Join-Path $repoRoot "games-res\BIN\CONFIG.BIN"
$candidate = Join-Path $repoRoot "build\cwr-radar-hmmwv-CONFIG.BIN"

if (-not (Test-Path -LiteralPath $tool)) {
    throw "PoseidonTools not found: $tool"
}
if (-not (Test-Path -LiteralPath $source)) {
    throw "Editable config not found: $source"
}
if (-not (Test-Path -LiteralPath $backup)) {
    throw "Original CONFIG.BIN backup not found: $backup"
}

# Merge onto the untouched binary backup instead of directly binarising the
# text. That preserves the binary config's enum/variable table while applying
# every value and class from the editable source.
& $tool config merge $backup $source -o $candidate --force-overwrite
if ($LASTEXITCODE -ne 0) {
    throw "CONFIG.BIN merge failed; the live game configuration was not changed."
}

if ($PSCmdlet.ShouldProcess($destination, "Replace with compiled CONFIG.BIN")) {
    Copy-Item -LiteralPath $candidate -Destination $destination -Force
    Write-Host "Installed $destination"
}
