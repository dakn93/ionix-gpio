# List COM ports; highlights typical ESP32 USB bridges (CH340/CP210x/FTDI).
$py = Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\python.exe"
if (-not (Test-Path $py)) {
  Write-Host "Python not found at $py" -ForegroundColor Red
  exit 1
}

Write-Host "`n=== serial.tools.list_ports ===" -ForegroundColor Cyan
& $py -m serial.tools.list_ports -v

Write-Host "`n=== PnP Ports class (USB / CH340 / CP210 / FTDI keywords) ===" -ForegroundColor Cyan
Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
  Where-Object {
    $n = $_.FriendlyName
    $n -match 'CH340|CH341|CP210|Silicon|FTDI|USB.*Serial|Serielles USB' -or
    ($n -match 'COM\d+' -and $n -notmatch 'Bluetooth')
  } |
  Select-Object FriendlyName, Status |
  Format-Table -AutoSize -Wrap

Write-Host "Note: Bluetooth COM ports are not the ESP32. Check Device Manager > Ports (COM & LPT) for USB serial." -ForegroundColor Yellow
