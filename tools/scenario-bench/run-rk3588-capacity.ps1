# Batch runner for RK3588 capacity matrix (9 scenarios, sequential)
$ErrorActionPreference = "Continue"
$device = "http://192.168.112.199:18000"
$user = "admin"
$pass = "admin"
$base = "scenarios/rk3588-capacity"
$outBase = "results/rk3588-capacity"

$scenarios = @(
  "person-detector-24fps",
  "person-detector-10fps",
  "person-detector-7fps",
  "person-detector-5fps",
  "no-safety-helmet-24fps",
  "no-safety-helmet-10fps",
  "no-safety-helmet-7fps",
  "no-safety-helmet-5fps",
  "concurrent-mixed-5fps"
)

foreach ($s in $scenarios) {
  $out = Join-Path $outBase $s
  Write-Output "===== START $s $(Get-Date -Format 'HH:mm:ss') ====="
  node src/cli.js run --device $device --user $user --password $pass `
    --scenario (Join-Path $base $s) --output $out --cleanup 2>&1
  $code = $LASTEXITCODE
  Write-Output "===== END $s exit=$code $(Get-Date -Format 'HH:mm:ss') ====="
}
Write-Output "ALL DONE"