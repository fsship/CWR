[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidatePattern("^[A-Za-z0-9_-]+$")]
    [string]$Preset = "win-x64-clang-rwdi",

    [ValidateRange(1, 64)]
    [int]$Jobs = 8,

    # Re-run CMake even when the selected build directory is already configured.
    [switch]$Configure,

    # Remove the selected target's generated objects before building it.
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Find-VisualStudioInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installPath = (& $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath).Trim()
        if ($installPath) {
            return $installPath
        }
    }

    if ($env:VSINSTALLDIR -and (Test-Path -LiteralPath $env:VSINSTALLDIR)) {
        return $env:VSINSTALLDIR.TrimEnd("\\")
    }

    throw "Visual Studio C++ tools were not found. Install the Desktop development with C++ workload."
}

function Find-CMake([string]$VisualStudioInstall) {
    $bundledCMake = Join-Path $VisualStudioInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path -LiteralPath $bundledCMake) {
        return $bundledCMake
    }

    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeCommand) {
        return $cmakeCommand.Source
    }

    throw "CMake was not found in Visual Studio or on PATH."
}

$repoRoot = $PSScriptRoot
$buildDirectory = Join-Path $repoRoot (Join-Path "build" $Preset)
$cacheFile = Join-Path $buildDirectory "CMakeCache.txt"
$llvmRoot = Join-Path $repoRoot "build\tools\LLVM"
$llvmMt = Join-Path $llvmRoot "bin\llvm-mt.exe"
$llvmRc = Join-Path $llvmRoot "bin\llvm-rc.exe"
$visualStudioInstall = Find-VisualStudioInstall
$vcvars64 = Join-Path $visualStudioInstall "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Find-CMake $visualStudioInstall

if (-not (Test-Path -LiteralPath $vcvars64)) {
    throw "Visual Studio x64 environment setup script was not found: $vcvars64"
}
if (-not (Test-Path -LiteralPath $llvmMt) -or -not (Test-Path -LiteralPath $llvmRc)) {
    throw "The portable LLVM resource tools were not found under: $llvmRoot"
}

if ($env:VCPKG_ROOT -and (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"))) {
    $vcpkgRoot = $env:VCPKG_ROOT
}
else {
    $vcpkgRoot = Join-Path $visualStudioInstall "VC\vcpkg"
}

if (-not (Test-Path -LiteralPath (Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"))) {
    throw "vcpkg was not found. Set VCPKG_ROOT to a vcpkg installation, or install Visual Studio's bundled vcpkg."
}

$vcpkgBinaryCache = Join-Path $repoRoot "build\vcpkg-binary-cache"
$needsConfigure = $Configure -or -not (Test-Path -LiteralPath $cacheFile)

if ($needsConfigure -and -not (Test-Path -LiteralPath $vcpkgBinaryCache)) {
    New-Item -ItemType Directory -Path $vcpkgBinaryCache | Out-Null
}

$steps = @(
    'cd /d "{0}"' -f $repoRoot,
    'set "VCPKG_ROOT={0}"' -f $vcpkgRoot,
    'set "VCPKG_DEFAULT_BINARY_CACHE={0}"' -f $vcpkgBinaryCache,
    'set "POSEIDON_LLVM_ROOT={0}"' -f $llvmRoot,
    'call "{0}" >nul' -f $vcvars64,
    # vcpkg runs nested CMake configurations. Force the portable resource
    # tools into those child configurations, where automatic discovery can
    # otherwise leave CMAKE_MT unset.
    'set "MT={0}"' -f $llvmMt,
    'set "RC={0}"' -f $llvmRc,
    # vcpkg normally starts port builds with a clean environment. Keep the
    # VS-initialized PATH so its Windows SDK mt.exe remains discoverable.
    'set "VCPKG_KEEP_ENV_VARS=PATH;MT;RC"'
)

if ($needsConfigure) {
    Write-Host "Configuring preset '$Preset'..."
    # The shared preset defaults to ccache. Clear the launchers because this
    # Windows checkout uses the portable LLVM bundle and does not require it.
    $steps += '"{0}" --preset "{1}" -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=' -f $cmake, $Preset
}

if ($Clean) {
    $steps += '"{0}" --build "{1}" --target clean' -f $cmake, $buildDirectory
}

$steps += '"{0}" --build "{1}" --target PoseidonGame --parallel {2}' -f $cmake, $buildDirectory, $Jobs
$command = $steps -join " && "

Write-Host "Building PoseidonGame with preset '$Preset'..."
if (-not $PSCmdlet.ShouldProcess($buildDirectory, "Build PoseidonGame")) {
    return
}

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "PoseidonGame build failed with exit code $LASTEXITCODE."
}

$game = Join-Path $repoRoot "dist\x64-win-rwdi\PoseidonGame.exe"
if (Test-Path -LiteralPath $game) {
    Write-Host "Build succeeded: $game"
}
else {
    Write-Host "Build succeeded. Check the selected preset's output directory for PoseidonGame.exe."
}
