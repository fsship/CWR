[CmdletBinding()]
param(
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot
$game = Join-Path $repoRoot "dist\x64-win-rwdi\PoseidonGame.exe"
$data = Join-Path $repoRoot "games-res"
$arguments = @(
    "--work-dir", $data,
    "--no-splash"
)
if ($NoSound) {
    $arguments += "--nosound"
}

& $game @arguments
