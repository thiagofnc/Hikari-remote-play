<#
.SYNOPSIS
    Packages a Hikari build into release artifacts.

.DESCRIPTION
    Produces, in -OutDir (default: <repo>\dist):

      Hikari.exe                     standalone app (what most users download)
      Hikari-<version>-win64.zip     Hikari-<version>\
                                       Hikari.exe
                                       tools\hikari-*.exe
                                       README.md, LICENSE, docs\
      hikari-bridge-esp32s3.bin      only if -FirmwareBin is given
      SHA256SUMS.txt                 sha256sum-compatible checksums of the above

    The same layout is produced by .github/workflows/release.yml.

.EXAMPLE
    .\scripts\package.ps1 -BuildDir $env:LOCALAPPDATA\Hikari-build\msvc-Release -Version 0.2.0

.EXAMPLE
    .\scripts\package.ps1 -BuildDir build -Version 0.2.0 -FirmwareBin firmware\esp32-s3\hikari-bridge-esp32s3.bin
#>
[CmdletBinding()]
param(
    # CMake build directory (the one containing bin\).
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    # Version string, without a leading "v" (a leading "v" is stripped).
    # Defaults to `git describe --tags`, then the version in CMakeLists.txt.
    [string]$Version,

    # Output directory. Created if needed; existing artifacts are replaced.
    [string]$OutDir,

    # Optional merged firmware image to publish next to the app.
    [string]$FirmwareBin,

    # Package even if Hikari.exe has not been built (development only).
    [switch]$AllowMissingApp
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }

function Get-DefaultVersion {
    try {
        $d = (& git -C $RepoRoot describe --tags --match 'v[0-9]*' 2>$null)
        if ($LASTEXITCODE -eq 0 -and $d) { return $d.Trim() }
    } catch { }
    $cm = Get-Content -Raw (Join-Path $RepoRoot 'CMakeLists.txt')
    if ($cm -match 'project\s*\(\s*Hikari[^)]*VERSION\s+([0-9][0-9.]*)') { return $Matches[1] }
    return '0.0.0'
}

if (-not $Version) { $Version = Get-DefaultVersion }
$Version = $Version.Trim() -replace '^[vV]', ''
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z.+_-]*$') {
    throw "Invalid version string '$Version'"
}

$BinDir = Join-Path (Resolve-Path $BuildDir).Path 'bin'
if (-not (Test-Path $BinDir)) { throw "No bin directory in build dir: $BinDir" }

$app = Join-Path $BinDir 'Hikari.exe'
$haveApp = Test-Path $app
if (-not $haveApp) {
    if ($AllowMissingApp) {
        Write-Warning "Hikari.exe not found in $BinDir; packaging without it (-AllowMissingApp)."
    } else {
        throw "Hikari.exe not found in $BinDir. Build the 'Hikari' target first (or pass -AllowMissingApp)."
    }
}
$tools = @(Get-ChildItem -Path $BinDir -Filter 'hikari-*.exe' -File | Sort-Object Name)

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$stem    = "Hikari-$Version-win64"
$zipPath = Join-Path $OutDir "$stem.zip"
$staging = Join-Path ([IO.Path]::GetTempPath()) ("hikari-pkg-" + [Guid]::NewGuid().ToString('N'))
$root    = Join-Path $staging "Hikari-$Version"

try {
    New-Item -ItemType Directory -Force $root | Out-Null

    if ($haveApp) { Copy-Item $app $root }
    if ($tools.Count -gt 0) {
        $toolsDir = Join-Path $root 'tools'
        New-Item -ItemType Directory -Force $toolsDir | Out-Null
        $tools | ForEach-Object { Copy-Item $_.FullName $toolsDir }
    }
    foreach ($f in 'README.md', 'LICENSE') {
        $p = Join-Path $RepoRoot $f
        if (Test-Path $p) { Copy-Item $p $root } else { Write-Warning "$f not found; skipped." }
    }
    $docs = Join-Path $RepoRoot 'docs'
    if (Test-Path $docs) { Copy-Item $docs (Join-Path $root 'docs') -Recurse }

    # Build the zip entry by entry with forward-slash names: on Windows
    # PowerShell 5.1 both Compress-Archive and ZipFile.CreateFromDirectory
    # write backslash entry names, which break non-Windows unzip tools.
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $prefix = $staging.TrimEnd('\') + '\'
        Get-ChildItem -Path $staging -Recurse -File | Sort-Object FullName | ForEach-Object {
            $entry = $_.FullName.Substring($prefix.Length) -replace '\\', '/'
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $_.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $zip.Dispose()
    }
} finally {
    Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
}

$artifacts = New-Object System.Collections.Generic.List[string]
if ($haveApp) {
    Copy-Item $app $OutDir -Force
    $artifacts.Add((Join-Path $OutDir 'Hikari.exe'))
}
$artifacts.Add($zipPath)

if ($FirmwareBin) {
    if (-not (Test-Path $FirmwareBin)) { throw "Firmware image not found: $FirmwareBin" }
    $fwOut = Join-Path $OutDir 'hikari-bridge-esp32s3.bin'
    Copy-Item $FirmwareBin $fwOut -Force
    $artifacts.Add($fwOut)
}

# sha256sum format: "<hash>  <name>", LF line endings, no BOM.
$lines = foreach ($a in $artifacts) {
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 $a).Hash.ToLowerInvariant(), (Split-Path $a -Leaf)
}
$sums = Join-Path $OutDir 'SHA256SUMS.txt'
[IO.File]::WriteAllText($sums, (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))

Write-Host ""
Write-Host "Packaged Hikari $Version -> $OutDir" -ForegroundColor Green
foreach ($a in @($artifacts) + @($sums)) {
    '  {0,-36} {1,10:N0} bytes' -f (Split-Path $a -Leaf), (Get-Item $a).Length | Write-Host
}
if ($tools.Count -gt 0) {
    Write-Host ("  tools in zip: " + (($tools | ForEach-Object Name) -join ', '))
} else {
    Write-Host "  (no hikari-*.exe tools found)"
}
