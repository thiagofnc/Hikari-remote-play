<#
.SYNOPSIS
    One-command local build of Hikari (Hikari.exe, tools, tests).

.DESCRIPTION
    Toolchain selection, in order:
      1. -LlvmMingw <path>            (explicit llvm-mingw; skips Visual Studio)
      2. Visual Studio with the C++ x64 tools, found via vswhere; the script
         enters its x64 developer environment itself.
      3. llvm-mingw in $env:USERPROFILE\hk-tools\llvm
    CMake and Ninja come from Visual Studio, PATH, or the Python user scripts
    directory, and are pip-installed (user scope) if missing.

    The build directory defaults to a short path outside the source tree
    ($env:LOCALAPPDATA\Hikari-build\<toolchain>-<config>) to stay clear of
    MAX_PATH problems in deep / OneDrive checkouts.

.EXAMPLE
    .\scripts\build.ps1                       # Release build with whatever toolchain is found
.EXAMPLE
    .\scripts\build.ps1 -Config Debug -Tests  # Debug build + run unit tests
.EXAMPLE
    .\scripts\build.ps1 -LlvmMingw C:\llvm-mingw -Opus -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',

    # Build with Opus audio (downloads libopus on first configure).
    [switch]$Opus,

    # Run the unit tests (ctest) after building.
    [switch]$Tests,

    # Delete the build directory first.
    [switch]$Clean,

    # Build directory (default: $env:LOCALAPPDATA\Hikari-build\<toolchain>-<config>).
    [string]$BuildDir,

    # Path to an llvm-mingw install (the folder containing bin\clang.exe).
    [string]$LlvmMingw,

    # Build only these targets (default: all).
    [string[]]$Target,

    # Extra arguments passed to the CMake configure step,
    # e.g. -CMakeArgs '-DHIKARI_BUILD_TOOLS=OFF'.
    [string[]]$CMakeArgs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Run a native command without PowerShell 5.1 turning its stderr output into
# terminating errors (which happens under ErrorActionPreference=Stop when
# output is redirected). Returns the exit code.
function Invoke-Native([string]$exe, [string[]]$arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @arguments 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_.Exception.Message } else { Write-Host $_ }
        }
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
}

function Write-Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

function Add-ToPath([string]$dir) {
    if ($dir -and (Test-Path $dir) -and (($env:PATH -split ';') -notcontains $dir)) {
        $env:PATH = "$dir;$env:PATH"
    }
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -prerelease `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $path) { return $null }
    return ($path | Select-Object -First 1).Trim()
}

function Enter-VsDevEnvironment([string]$vsPath) {
    $dll = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (Test-Path $dll) {
        Import-Module $dll
        Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
            -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    } else {
        # Older VS: import the environment from vcvars64.bat.
        $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
        if (-not (Test-Path $vcvars)) { throw "Neither DevShell nor vcvars64.bat found in $vsPath" }
        cmd /s /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "Entered the Visual Studio environment but cl.exe is not on PATH."
    }
}

function Get-PythonExe {
    foreach ($c in 'python', 'py') {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source -notmatch 'WindowsApps') { return $cmd.Source }
    }
    return $null
}

function Get-PythonUserScripts([string]$py) {
    if (-not $py) { return $null }
    $dir = & $py -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))" 2>$null
    if ($LASTEXITCODE -eq 0 -and $dir) { return $dir.Trim() }
    return $null
}

function Initialize-BuildTools {
    $py = Get-PythonExe
    Add-ToPath (Get-PythonUserScripts $py)

    $missing = @('cmake', 'ninja') | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) }
    if ($missing) {
        if (-not $py) {
            throw "Missing $($missing -join ' and '). Install Visual Studio 2022 (Desktop C++ workload), or Python 3 so this script can 'pip install cmake ninja'."
        }
        Write-Step "Installing $($missing -join ', ') with pip (user scope)"
        $rc = Invoke-Native $py (@('-m', 'pip', 'install', '--user', '--upgrade', '--disable-pip-version-check') + $missing)
        if ($rc -ne 0) { throw "pip install $($missing -join ' ') failed" }
        Add-ToPath (Get-PythonUserScripts $py)
        foreach ($m in $missing) {
            if (-not (Get-Command $m -ErrorAction SilentlyContinue)) { throw "$m still not found after pip install" }
        }
    }
}

# ---------------------------------------------------------------- toolchain
$toolchain = $null
$cfgArgs = @()

if (-not $LlvmMingw) {
    $vs = Find-VisualStudio
    if ($vs) {
        Write-Step "Using Visual Studio at $vs (x64)"
        Enter-VsDevEnvironment $vs
        # VS ships its own CMake + Ninja; DevShell normally puts them on PATH.
        Add-ToPath (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
        Add-ToPath (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin')
        $toolchain = 'msvc'
        $cfgArgs += '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl'
    } else {
        $default = Join-Path $env:USERPROFILE 'hk-tools\llvm'
        if (Test-Path (Join-Path $default 'bin\clang.exe')) { $LlvmMingw = $default }
    }
}

if (-not $toolchain) {
    if (-not $LlvmMingw) {
        throw @"
No C++ toolchain found. Either:
  * install Visual Studio 2022 (or Build Tools) with the 'Desktop development with C++' workload, or
  * download llvm-mingw (ucrt-x86_64) from https://github.com/mstorsjo/llvm-mingw/releases,
    extract it to $env:USERPROFILE\hk-tools\llvm, or pass -LlvmMingw <path>.
"@
    }
    $LlvmMingw = (Resolve-Path $LlvmMingw).Path
    $clang = Join-Path $LlvmMingw 'bin\clang.exe'
    if (-not (Test-Path $clang)) { throw "No bin\clang.exe under -LlvmMingw '$LlvmMingw'" }
    Write-Step "Using llvm-mingw at $LlvmMingw"
    Add-ToPath (Join-Path $LlvmMingw 'bin')
    $toolchain = 'clang'
    $cfgArgs += "-DCMAKE_C_COMPILER=$($clang -replace '\\', '/')",
                  "-DCMAKE_CXX_COMPILER=$((Join-Path $LlvmMingw 'bin\clang++.exe') -replace '\\', '/')"
}

Initialize-BuildTools
Write-Host ("    cmake: " + (Get-Command cmake).Source)
Write-Host ("    ninja: " + (Get-Command ninja).Source)

# ---------------------------------------------------------------- build dir
if (-not $BuildDir) {
    $base = if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { $env:TEMP }
    $BuildDir = Join-Path $base "Hikari-build\$toolchain-$Config"
}
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if ($BuildDir.Length -gt 80) {
    Write-Warning "Build dir path is $($BuildDir.Length) chars; deep paths can hit MAX_PATH. Consider -BuildDir C:\hk-build."
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Cleaning $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# A cache from a different compiler can't be reused.
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path $cache) {
    $cachedCxx = (Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:\w+=(.*)$' | Select-Object -First 1)
    if ($cachedCxx) {
        $isMsvc = $cachedCxx.Matches[0].Groups[1].Value -match '(^|[\\/])cl(\.exe)?$'
        if ($isMsvc -ne ($toolchain -eq 'msvc')) {
            Write-Step "Toolchain changed; wiping $BuildDir"
            Remove-Item -Recurse -Force $BuildDir
        }
    }
}

# ---------------------------------------------------------------- configure
$onOff = { param($b) if ($b) { 'ON' } else { 'OFF' } }
$cfgArgs += @(
    '-S', $RepoRoot,
    '-B', $BuildDir,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DHIKARI_WITH_OPUS=$(& $onOff $Opus.IsPresent)",
    '-DHIKARI_BUILD_TESTS=ON',
    '-DHIKARI_BUILD_TOOLS=ON'
)
if ($CMakeArgs) { $cfgArgs += $CMakeArgs }

Write-Step "Configuring ($toolchain, $Config, opus=$($Opus.IsPresent)) -> $BuildDir"
$rc = Invoke-Native cmake $cfgArgs
if ($rc -ne 0) {
    Write-Host "CMake configure failed (exit $rc); see errors above." -ForegroundColor Red
    exit $rc
}

# ---------------------------------------------------------------- build
$buildArgs = @('--build', $BuildDir, '--config', $Config)
if ($Target) { $buildArgs += '--target'; $buildArgs += $Target }
# -k 0: keep going so independent targets (e.g. tests) still get built when
# one component fails.
$buildArgs += '--', '-k', '0'

Write-Step "Building"
$buildExit = Invoke-Native cmake $buildArgs
if ($buildExit -ne 0) { Write-Host "Build FAILED (exit $buildExit); see errors above." -ForegroundColor Red }

# ---------------------------------------------------------------- tests
$testExit = 0
if ($Tests) {
    $testExe = Join-Path $BuildDir 'bin\hikari_tests.exe'
    if (Test-Path $testExe) {
        Write-Step "Running tests"
        $testExit = Invoke-Native ctest @('--test-dir', $BuildDir, '--output-on-failure', '-C', $Config)
        if ($testExit -ne 0) { Write-Host "Tests FAILED (exit $testExit)" -ForegroundColor Red }
    } else {
        Write-Host "Tests not run: hikari_tests.exe was not built." -ForegroundColor Red
        $testExit = 1
    }
}

# ---------------------------------------------------------------- summary
$binDir = Join-Path $BuildDir 'bin'
Write-Host ""
if (Test-Path $binDir) {
    $exes = @(Get-ChildItem $binDir -Filter *.exe -File | Sort-Object Name)
    $app = Join-Path $binDir 'Hikari.exe'
    if (Test-Path $app) {
        Write-Host "Hikari.exe: $app" -ForegroundColor Green
    } else {
        Write-Host "Hikari.exe was not produced." -ForegroundColor Yellow
    }
    if ($exes.Count -gt 0) {
        Write-Host "Executables in ${binDir}:"
        $exes | ForEach-Object { '  {0,-28} {1,10:N0} bytes' -f $_.Name, $_.Length | Write-Host }
    }
}

if ($buildExit -ne 0) { exit $buildExit }
if ($testExit -ne 0) { exit $testExit }
exit 0
