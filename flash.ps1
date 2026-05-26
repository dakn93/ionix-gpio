# Build and upload firmware to the ESP32.
# Usage:
#   .\flash.ps1              # list ports, then prompt for COM
#   .\flash.ps1 COM7         # use this port directly
#
# Requires: Python 3.12 + PlatformIO (pip install platformio)
# or add ...\Python312\Scripts to PATH (see $Pio below).

param(
  [Parameter(Mandatory = $false)]
  [string] $Port
)

$ErrorActionPreference = "Stop"

$Pio = Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\Scripts\pio.exe"
if (-not (Test-Path $Pio)) {
  $Pio = "pio.exe"
}

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectRoot

Write-Host "Serial ports:" -ForegroundColor Cyan
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m serial.tools.list_ports -v 2>$null
if (-not $Port) {
  $Port = Read-Host "Enter ESP32 COM port (e.g. COM7). Empty = cancel"
}
if (-not $Port) {
  Write-Host "Cancelled." -ForegroundColor Yellow
  exit 1
}

Write-Host "Uploading to $Port ..." -ForegroundColor Cyan
& $Pio run -t upload --upload-port $Port
if ($LASTEXITCODE -ne 0) {
  Write-Host "`nTip: pick the USB serial COM (CP210x/CH340), not Bluetooth. Close Serial Monitor if the port is busy." -ForegroundColor Yellow
  exit $LASTEXITCODE
}
Write-Host "Done." -ForegroundColor Green
