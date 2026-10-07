<#
.SYNOPSIS
    Copy the freshly built MotionCraft plugin into a local OBS Studio install.

.DESCRIPTION
    Run this after rebuilding (cmake --build build_x64 --config RelWithDebInfo).
    It copies motioncraft.dll into obs-plugins\64bit and the data folder
    (locale\) into data\obs-plugins\motioncraft. Writing under Program Files
    needs admin, so the script self-elevates; it refuses to run while OBS is
    open, because the DLL is locked and the copy would silently fail.

.PARAMETER ObsDir
    OBS Studio install root. Defaults to C:\Program Files\obs-studio.

.PARAMETER Config
    Build config to deploy from. Defaults to RelWithDebInfo.

.EXAMPLE
    .\deploy-to-obs.ps1
    .\deploy-to-obs.ps1 -ObsDir "D:\OBS" -Config Release
#>
[CmdletBinding()]
param(
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [ValidateSet("RelWithDebInfo", "Release", "Debug", "MinSizeRel")]
    [string]$Config = "RelWithDebInfo",
    # Set by the elevated relaunch so the new window waits before closing.
    [switch]$Pause
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir = Join-Path $repoRoot "build_x64\rundir\$Config"
$srcDll = Join-Path $srcDir "motioncraft.dll"
$srcData = Join-Path $srcDir "motioncraft"   # holds locale\

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; if ($Pause) { Read-Host "Press Enter to close" }; exit 1 }

# 1. The build must exist.
if (-not (Test-Path $srcDll)) {
    Fail "Not built: $srcDll`n       Run:  cmake --build build_x64 --config $Config"
}

# 2. OBS must be closed, or the DLL is locked and the copy no-ops.
if (Get-Process -Name obs64, obs32, obs -ErrorAction SilentlyContinue) {
    Fail "OBS is running. Close it, then re-run this script."
}

# 3. The target must look like an OBS install.
$pluginDest = Join-Path $ObsDir "obs-plugins\64bit"
$dataDest = Join-Path $ObsDir "data\obs-plugins\motioncraft"
if (-not (Test-Path $pluginDest)) {
    Fail "No OBS plugin folder at: $pluginDest`n       Pass the right root with -ObsDir."
}

# 4. Writing under Program Files needs admin: relaunch elevated once.
$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Elevation needed to write to $ObsDir - requesting admin..." -ForegroundColor Yellow
    $argList = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", "`"$($MyInvocation.MyCommand.Path)`"",
        "-ObsDir", "`"$ObsDir`"", "-Config", $Config, "-Pause"
    )
    Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs -ArgumentList $argList
    exit
}

# 5. Copy.
Write-Host "Deploying $Config build to $ObsDir ..." -ForegroundColor Cyan

Copy-Item -Path $srcDll -Destination $pluginDest -Force
Write-Host "  + motioncraft.dll" -ForegroundColor Green

$srcPdb = Join-Path $srcDir "motioncraft.pdb"
if (Test-Path $srcPdb) {
    Copy-Item -Path $srcPdb -Destination $pluginDest -Force
    Write-Host "  + motioncraft.pdb (debug symbols)" -ForegroundColor Green
}

New-Item -ItemType Directory -Force -Path $dataDest | Out-Null
Copy-Item -Path (Join-Path $srcData "*") -Destination $dataDest -Recurse -Force
Write-Host "  + data\obs-plugins\motioncraft\ (locale)" -ForegroundColor Green

Write-Host "Done. Restart OBS to load the new build." -ForegroundColor Cyan
if ($Pause) { Read-Host "Press Enter to close" }
