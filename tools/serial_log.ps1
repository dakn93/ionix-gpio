# ESP32 USB serial log to file (Windows). Stop with Ctrl+C.
# Use Windows PowerShell 5.1 (SerialPort). PowerShell 7+ may lack System.IO.Ports.
#   & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ExecutionPolicy Bypass -File tools\serial_log.ps1
# Opening the COM port can toggle DTR/RTS and reset the ESP — close other serial monitors first.
param(
  [string]$Port = "COM7",
  [int]$Baud = 115200
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Ports
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$logDir = Join-Path $root "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$out = Join-Path $logDir ("esp_serial_{0:yyyyMMdd_HHmmss}.txt" -f (Get-Date))
Write-Host "Logging $Port @ $Baud -> $out (Ctrl+C to stop)"
$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, None, 8, one
$sp.NewLine = "`n"
$sp.ReadTimeout = 500
$sp.Open()
try {
  $sw = [System.IO.StreamWriter]::new($out, $false, [System.Text.UTF8Encoding]::new($false))
  $sw.WriteLine("# started {0:o} port={1} baud={2}" -f (Get-Date), $Port, $Baud)
  while ($true) {
    try {
      $line = $sp.ReadLine()
      $ts = (Get-Date).ToString("o")
      $sw.WriteLine(("{0} {1}" -f $ts, $line))
      $sw.Flush()
      Write-Host $line
    } catch [System.TimeoutException] {
      continue
    }
  }
} finally {
  if ($sw) { $sw.Close(); $sw.Dispose() }
  if ($sp -and $sp.IsOpen) { $sp.Close() }
  $sp.Dispose()
}
