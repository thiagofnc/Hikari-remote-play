<#
.SYNOPSIS
    Flashes the Hikari controller-bridge firmware onto an ESP32-S3 board.

.DESCRIPTION
    1. Finds the board's serial port (Espressif native USB, Silicon Labs CP210x
       or WCH CH34x/CH343 USB-UART bridges).
    2. Installs esptool with pip (user scope) if it is missing.
    3. Downloads hikari-bridge-esp32s3.bin from the latest GitHub release
       (and checks it against SHA256SUMS.txt) unless -Bin is given.
    4. Writes the merged image at address 0x0.

    Use the board's UART port (on an ESP32-S3-DevKitC-1 the one labelled
    "UART" / "COM"), not the "USB" port that goes to the Switch dock.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\flash-firmware.ps1
.EXAMPLE
    .\scripts\flash-firmware.ps1 -Bin .\hikari-bridge-esp32s3.bin -Port COM7
.EXAMPLE
    .\scripts\flash-firmware.ps1 -Tag v0.2.0 -Erase
#>
[CmdletBinding()]
param(
    # Local firmware image. If omitted, it is downloaded from GitHub.
    [string]$Bin,

    # Serial port, e.g. COM7. Auto-detected if omitted.
    [string]$Port,

    # Release tag to download (default: latest release).
    [string]$Tag,

    [string]$Repo = 'thiagofnc/Hikari-remote-play',

    [int]$Baud = 921600,

    # Erase the whole flash first (clears saved settings).
    [switch]$Erase,

    # Do everything except actually writing to the board.
    [switch]$DryRun,

    # Seconds to wait for a board to be plugged in.
    [int]$WaitSeconds = 60
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$ImageName = 'hikari-bridge-esp32s3.bin'

function Write-Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Hint([string]$msg) { Write-Host "    $msg" -ForegroundColor Yellow }
function Fail([string]$msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# Runs a native command, echoing its output, without PowerShell 5.1 turning
# stderr lines into terminating errors. Returns the exit code.
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

# ------------------------------------------------------------ serial ports
# USB VID/PIDs of the boards we support.
$KnownBridges = @(
    @{ Pattern = 'VID_303A';            Kind = 'Espressif native USB (USB-Serial/JTAG)' },
    @{ Pattern = 'VID_10C4&PID_EA60';   Kind = 'Silicon Labs CP210x UART bridge' },
    @{ Pattern = 'VID_1A86&PID_55D3';   Kind = 'WCH CH343 UART bridge' },
    @{ Pattern = 'VID_1A86&PID_55D4';   Kind = 'WCH CH9102 UART bridge' },
    @{ Pattern = 'VID_1A86&PID_7523';   Kind = 'WCH CH340 UART bridge' }
)

function Get-EspPorts {
    $devices = @(Get-CimInstance -ClassName Win32_PnPEntity -Filter "Name LIKE '%(COM%'" -ErrorAction SilentlyContinue)
    foreach ($d in $devices) {
        if (-not $d.PNPDeviceID -or $d.Name -notmatch '\((COM\d+)\)') { continue }
        $com = $Matches[1]
        foreach ($k in $KnownBridges) {
            if ($d.PNPDeviceID.ToUpperInvariant().Contains($k.Pattern)) {
                [pscustomobject]@{ Port = $com; Kind = $k.Kind; Name = $d.Name }
                break
            }
        }
    }
}

function Find-Port {
    $deadline = (Get-Date).AddSeconds([Math]::Max(0, $WaitSeconds))
    $told = $false
    while ($true) {
        $ports = @(Get-EspPorts | Sort-Object Port -Unique)
        if ($ports.Count -eq 1) { return $ports[0] }
        if ($ports.Count -gt 1) {
            Write-Host "Several ESP32 boards found:"
            for ($i = 0; $i -lt $ports.Count; $i++) {
                Write-Host ("  [{0}] {1}  {2}" -f ($i + 1), $ports[$i].Port, $ports[$i].Kind)
            }
            if (-not [Environment]::UserInteractive) { Fail "Pass -Port COMx to choose one." }
            $choice = Read-Host "Which one? (1-$($ports.Count))"
            $n = 0
            if ([int]::TryParse($choice, [ref]$n) -and $n -ge 1 -and $n -le $ports.Count) { return $ports[$n - 1] }
            Fail "Invalid choice. Pass -Port COMx to choose one."
        }
        if (-not $told) {
            Write-Host ""
            Write-Host "No ESP32 board found." -ForegroundColor Yellow
            Write-Hint "Plug the board's UART port (labelled UART/COM on a DevKitC-1) into this PC with a data USB cable."
            Write-Hint "Waiting up to $WaitSeconds s... (Ctrl+C to cancel)"
            $told = $true
        }
        if ((Get-Date) -ge $deadline) {
            Write-Hint "Still nothing. Check that:"
            Write-Hint " - the cable carries data (many charge-only cables don't);"
            Write-Hint " - the USB-UART driver is installed (CP210x: silabs.com, CH343/CH340: wch-ic.com);"
            Write-Hint " - the board shows up under 'Ports (COM & LPT)' in Device Manager."
            Write-Hint "Then rerun this script, or pass -Port COMx explicitly."
            Fail "No ESP32 serial port found."
        }
        Start-Sleep -Seconds 1
    }
}

# ------------------------------------------------------------ esptool
function Get-Python {
    foreach ($c in 'python', 'py') {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source -notmatch 'WindowsApps') { return $cmd.Source }
    }
    return $null
}

function Test-Esptool([string]$py) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $null = & $py -m esptool version 2>&1
        return ($LASTEXITCODE -eq 0)
    } finally { $ErrorActionPreference = $saved }
}

function Initialize-Esptool {
    $py = Get-Python
    if (-not $py) {
        Write-Hint "Python 3 is needed to run esptool. Install it from https://www.python.org/downloads/"
        Write-Hint "(tick 'Add python.exe to PATH'), or use the browser flasher: https://espressif.github.io/esptool-js/"
        Fail "Python not found."
    }
    if (-not (Test-Esptool $py)) {
        Write-Step "Installing esptool (pip, user scope)"
        $rc = Invoke-Native $py @('-m', 'pip', 'install', '--user', '--upgrade', '--disable-pip-version-check', 'esptool')
        if ($rc -ne 0 -or -not (Test-Esptool $py)) { Fail "Could not install esptool. Try: $py -m pip install --user esptool" }
    }
    return $py
}

# ------------------------------------------------------------ firmware image
function Get-FirmwareImage {
    $base = if ($Tag) { "https://github.com/$Repo/releases/download/$Tag" }
            else      { "https://github.com/$Repo/releases/latest/download" }
    $dir = Join-Path $env:TEMP 'hikari-flash'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $out = Join-Path $dir $ImageName
    $label = if ($Tag) { $Tag } else { 'latest release' }

    Write-Step "Downloading $ImageName ($label)"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$base/$ImageName" -OutFile $out
    } catch {
        Fail "Download failed: $($_.Exception.Message)`n       URL: $base/$ImageName`n       Download it manually from https://github.com/$Repo/releases and pass -Bin <file>."
    }

    # Verify against the release's SHA256SUMS.txt when it is available.
    try {
        $sums = (Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS.txt").Content
        if ($sums -is [byte[]]) { $sums = [Text.Encoding]::UTF8.GetString($sums) }
        $line = ($sums -split "`n") | Where-Object { $_ -match "^\s*([0-9a-fA-F]{64})\s+\*?$([regex]::Escape($ImageName))\s*$" } | Select-Object -First 1
        if ($line) {
            $expected = ([regex]::Match($line, '[0-9a-fA-F]{64}')).Value.ToLowerInvariant()
            $actual = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLowerInvariant()
            if ($expected -ne $actual) { Fail "Checksum mismatch for $ImageName (expected $expected, got $actual). Try again." }
            Write-Host "    SHA-256 verified."
        }
    } catch {
        Write-Hint "Could not verify checksum ($($_.Exception.Message)); continuing."
    }
    return $out
}

# ------------------------------------------------------------ main
Write-Host "Hikari bridge firmware flasher" -ForegroundColor Green
Write-Host ""

if ($Bin) {
    if (-not (Test-Path $Bin)) { Fail "Firmware file not found: $Bin" }
    $Bin = (Resolve-Path $Bin).Path
} else {
    $Bin = Get-FirmwareImage
}
$size = (Get-Item $Bin).Length
if ($size -lt 4096) { Fail "$Bin is only $size bytes; that is not a merged firmware image." }
Write-Host "    Image: $Bin ($('{0:N0}' -f $size) bytes)"

$py = Initialize-Esptool

if ($Port) {
    $Port = $Port.ToUpperInvariant()
    Write-Step "Using $Port"
} else {
    Write-Step "Looking for the ESP32-S3 board"
    $found = Find-Port
    $Port = $found.Port
    Write-Host "    Found $Port ($($found.Kind))"
    if ($found.Kind -like 'Espressif native*') {
        Write-Hint "This is the board's native USB port. Flashing works, but the bridge firmware"
        Write-Hint "uses that port for the Switch; for day-to-day use plug the UART port into the PC."
    }
}

$common = @('-m', 'esptool', '--chip', 'esp32s3', '--port', $Port)

if ($DryRun) {
    Write-Step "Dry run: would run"
    Write-Host "    $py $($common -join ' ') --baud $Baud write_flash 0x0 `"$Bin`""
    exit 0
}

if ($Erase) {
    Write-Step "Erasing flash"
    $rc = Invoke-Native $py ($common + @('--baud', "$Baud", 'erase_flash'))
    if ($rc -ne 0) { Fail "Erase failed (see above)." }
}

Write-Step "Flashing $ImageName to $Port at 0x0"
$rc = Invoke-Native $py ($common + @('--baud', "$Baud", 'write_flash', '0x0', $Bin))
if ($rc -ne 0 -and $Baud -gt 115200) {
    Write-Hint "Flashing at $Baud baud failed; retrying at 115200 (some cables/adapters can't go faster)."
    $rc = Invoke-Native $py ($common + @('--baud', '115200', 'write_flash', '0x0', $Bin))
}
if ($rc -ne 0) {
    Write-Hint "If the board won't connect: hold the BOOT button, tap RESET (EN), release BOOT, then rerun."
    Write-Hint "Close any serial monitor or app that might have $Port open."
    Fail "Flashing failed (see above)."
}

Write-Host ""
Write-Host "Done! The bridge firmware is installed." -ForegroundColor Green
Write-Host "Next:"
Write-Host "  1. Keep the board's UART port plugged into this PC (the Hikari Host finds it automatically)."
Write-Host "  2. Plug the board's USB port into the Switch 2 dock."
exit 0
