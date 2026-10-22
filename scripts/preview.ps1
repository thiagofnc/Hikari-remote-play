<#
.SYNOPSIS
    Opens an offline UI preview without a second PC or console hardware.
#>
[CmdletBinding()]
param(
    [ValidateSet('setup', 'connection', 'host', 'controller', 'controller-compact', 'controller-switch2', 'controller-switch2-compact', 'ready', 'ready-compact', 'connection-needed', 'switch2', 'asleep', 'waking', 'failed', 'stream', 'stream-clean')]
    [string]$Screen = 'ready',
    [string]$BuildDir = "$env:LOCALAPPDATA\Hikari-build\clang-Release"
)
$ErrorActionPreference = 'Stop'
$app = Join-Path $BuildDir 'bin\Hikari.exe'
if (-not (Test-Path -LiteralPath $app)) {
    throw "Build Hikari first with scripts\build.ps1, or pass -BuildDir pointing to your build."
}
# This is an interactive preview explicitly requested by the caller.
Start-Process -FilePath $app -ArgumentList "--preview=$Screen" -WindowStyle Normal
