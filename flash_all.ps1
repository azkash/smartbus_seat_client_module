# ─────────────────────────────────────────────────────────────────────
#  flash_all.ps1 — parallel COM-port flasher for the seat nodes
# ─────────────────────────────────────────────────────────────────────
#  Builds the firmware once, then flashes every connected device at the
#  same time. Designed for batch rollout: plug in a batch (e.g. 8 devices
#  on a powered USB hub), run this, swap the batch, run again.
#
#  Usage (from the PlatformIO PowerShell terminal, in this project folder):
#     ./flash_all.ps1                       # auto-detect all COM ports
#     ./flash_all.ps1 -Ports COM3,COM5,COM7 # only these ports
#     ./flash_all.ps1 -SkipBuild            # reuse existing build (faster re-runs)
#
#  After flashing, an empty-NVS device starts as RandomID. Press its call
#  button and assign the friendly ID from the head-unit popup. Serial remains
#  available as a fallback: setid=1A (query current id with: id? ).
# ─────────────────────────────────────────────────────────────────────

param(
  [string[]]$Ports,                        # explicit port list; omit to auto-detect
  [string]  $Env      = "esp32-wroom-32u",
  [switch]  $SkipBuild                     # skip the one-time compile
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# ── Resolve the pio executable (PATH, else the default penv location) ──
$pio = "pio"
if (-not (Get-Command $pio -ErrorAction SilentlyContinue)) {
  $pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
  if (-not (Test-Path $pio)) { throw "pio not found on PATH or at $pio" }
}

# ── 1. Build once so the parallel uploads never recompile or clash ────
if (-not $SkipBuild) {
  Write-Host "[build] compiling $Env ..." -ForegroundColor Cyan
  & $pio run -e $Env
  if ($LASTEXITCODE -ne 0) { throw "build failed — fix errors before flashing" }
}

# ── 2. Decide which COM ports to target ───────────────────────────────
if (-not $Ports -or $Ports.Count -eq 0) {
  # COM1 is almost always a non-USB/legacy port — skip it by default.
  $Ports = [System.IO.Ports.SerialPort]::GetPortNames() |
             Sort-Object -Unique | Where-Object { $_ -ne "COM1" }
}
if ($Ports.Count -eq 0) {
  throw "no COM ports detected — plug in devices, or pass -Ports COM3,COM4"
}
Write-Host "[flash] $($Ports.Count) device(s): $($Ports -join ', ')" -ForegroundColor Cyan

# ── 3. Flash every port in parallel (one background job per port) ─────
$jobs = foreach ($p in $Ports) {
  Start-Job -Name $p -ScriptBlock {
    param($pio, $dir, $env, $port)
    Set-Location $dir
    & $pio run -e $env -t upload --upload-port $port 2>&1
    if ($LASTEXITCODE -eq 0) { "___RESULT OK $port" } else { "___RESULT FAIL $port" }
  } -ArgumentList $pio, $PSScriptRoot, $Env, $p
}

Write-Host "[flash] uploading — this takes ~15-30s per device ..." -ForegroundColor Cyan
$jobs | Wait-Job | Out-Null

# ── 4. Per-port pass/fail summary ─────────────────────────────────────
$ok = @(); $bad = @()
foreach ($j in $jobs) {
  $lines  = Receive-Job $j
  $result = ($lines | Where-Object { $_ -like "___RESULT *" } | Select-Object -Last 1)
  if ($result -like "___RESULT OK *")   { $ok  += $j.Name }
  else {
    $bad += $j.Name
    Write-Host "`n----- $($j.Name) FAILED -----" -ForegroundColor Red
    $lines | Where-Object { $_ -notlike "___RESULT *" } | Select-Object -Last 15
  }
}
Remove-Job $jobs

Write-Host "`n═══════════════ SUMMARY ═══════════════" -ForegroundColor Yellow
Write-Host ("  OK   ({0}): {1}" -f $ok.Count,  ($ok  -join ', ')) -ForegroundColor Green
if ($bad.Count) {
  Write-Host ("  FAIL ({0}): {1}" -f $bad.Count, ($bad -join ', ')) -ForegroundColor Red
  Write-Host "  Re-run just the failures:  ./flash_all.ps1 -SkipBuild -Ports $($bad -join ',')"
  exit 1
}
Write-Host "  All devices flashed successfully." -ForegroundColor Green
