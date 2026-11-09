# SPDX-License-Identifier: Apache-2.0
# The release's test tiers (docs/DEVELOPING.md, "Releasing", step 3), every test once: each tier a
# label set that shares no test with another, the app's tree only its own tests (it builds the
# core's too, which ran on Maya's). Logs in build/probe/rtest_<tier>.log; prints each tier's result
# and its failed tests. A timing test that fails under load is rerun alone (ctest -R), not waved through.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/release_tests.ps1
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
New-Item -ItemType Directory -Force build/probe | Out-Null
$runs = @(
  @('ninja-maya2022', 'quick',     '-j', '8', '-L', 'quick'),
  @('ninja-maya2022', 'maya',      '-j', '4', '-L', 'maya', '-LE', 'gui|arnold|vray|renderman'),
  @('ninja-maya2022', 'blender',   '-j', '4', '-L', 'blender', '-LE', 'vray'),
  @('ninja-maya2022', 'gpu',       '-j', '3', '-L', 'gpu', '-LE', 'quick|bench'),
  @('ninja-maya2022', 'arnold',    '-j', '4', '-L', 'arnold'),
  @('ninja-maya2022', 'vray_rman', '-j', '2', '-L', 'vray|renderman', '-LE', 'quick|app'),
  @('ninja-maya2022', 'budget',    '-j', '1', '-L', 'budget'),
  @('ninja-maya2022', 'gui',       '-j', '1', '-L', '^gui$'),
  @('ninja-app',      'app',       '-j', '6', '-L', 'app')
)
$started = Get-Date
foreach ($r in $runs) {
  $preset = $r[0]; $name = $r[1]; $rest = $r[2..($r.Length-1)]
  $log = "build/probe/rtest_$name.log"
  $t0 = Get-Date
  ctest --preset $preset --output-on-failure @rest 2>&1 | Out-File -Encoding utf8 $log
  $secs = [int]((Get-Date) - $t0).TotalSeconds
  $tail = (Get-Content $log | Select-String -Pattern 'tests passed|tests failed|No tests were found' | Select-Object -Last 1)
  Write-Output "=== $name ($secs s): $tail"
  Get-Content $log | Select-String -Pattern '^\s+\d+ - .*\((Failed|Timeout|SEGFAULT|Exception|Not Run)' | ForEach-Object { Write-Output "   $_" }
}
Write-Output ("TESTS DONE in {0} min" -f [int]((Get-Date) - $started).TotalMinutes)
