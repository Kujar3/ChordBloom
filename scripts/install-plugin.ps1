# Installs the built Chordbloom VST3 (and CLAP, if built) into the standard system plugin folders so FL Studio finds them.
# Usage (from the repo root, in PowerShell):   powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1
# It asks for administrator rights once (Program Files is protected).

param([string]$BuildDir = "build")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$artefacts = Join-Path $root "$BuildDir\Chordbloom_artefacts\Release"
$vst3 = Join-Path $artefacts "VST3\Chordbloom.vst3"
$clap = Join-Path $artefacts "CLAP\Chordbloom.clap"

if (-not (Test-Path $vst3)) { throw "Build not found: $vst3 (run scripts\build.bat first)" }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Start-Process powershell -Verb RunAs -Wait -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-BuildDir", "`"$BuildDir`"")
    exit $LASTEXITCODE
}

$vst3Dest = "C:\Program Files\Common Files\VST3"
$clapDest = "C:\Program Files\Common Files\CLAP"
New-Item -ItemType Directory -Force $vst3Dest, $clapDest | Out-Null

$target = Join-Path $vst3Dest "Chordbloom.vst3"
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
Copy-Item -Recurse $vst3 $vst3Dest
Write-Host "Installed $target"

if (Test-Path $clap) {
    Copy-Item -Force $clap $clapDest
    Write-Host "Installed $(Join-Path $clapDest 'Chordbloom.clap')"
}

Write-Host ""
Write-Host "Done. In FL Studio: Options > Manage plugins > Find more plugins (or Find installed plugins), then add"
Write-Host "Chordbloom from the Channel Rack (+) menu. Close any FL project using an older Chordbloom before re-installing."
Start-Sleep -Seconds 4
