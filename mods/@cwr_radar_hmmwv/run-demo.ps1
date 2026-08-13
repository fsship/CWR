[CmdletBinding()]
param(
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
$modRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $modRoot "..\..")).Path
$game = Join-Path $repoRoot "dist\x64-win-rwdi\PoseidonGame.exe"
$data = Join-Path $repoRoot "games-res"
$mission = Join-Path $modRoot "missions\CWRRadarHMMWVDemo.Eden"
$arguments = @(
    "--work-dir", $data,
    "--mods-dir", (Split-Path $modRoot -Parent),
    "--mod", (Split-Path $modRoot -Leaf),
    "--test-mission", $mission,
    "--window",
    "--no-splash"
)
if ($NoSound) {
    $arguments += "--nosound"
}

& $game @arguments
