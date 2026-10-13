# Run the Mosaic test suite built by scripts/build.ps1.
#   scripts/run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]
# Catch2 runs in random order with the given seed (default 1). Writes
# test-results/junit-<Config>-seed<N>.xml.
$ErrorActionPreference = 'Stop'

$Usage = "usage: scripts/run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]"
$Config = $args[0]
if ($Config -notin @('Debug', 'Release', 'Dist')) { Write-Error $Usage; exit 2 }
$Seed = '1'
for ($i = 1; $i -lt $args.Count; $i++) {
    if ($args[$i] -eq '--rng-seed' -and ($i + 1) -lt $args.Count) { $Seed = "$($args[$i + 1])"; $i++ }
    else { Write-Error "run-tests.ps1: unknown argument $($args[$i]). $Usage"; exit 2 }
}
if ($Seed -notmatch '^[0-9]+$') { Write-Error "run-tests.ps1: seed must be a non-negative integer"; exit 2 }

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$Exes = @(Get-ChildItem -Path (Join-Path $Root 'bin') -Directory -Filter "$Config-*" |
    ForEach-Object { Join-Path $_.FullName 'MosaicTests\MosaicTests.exe' } |
    Where-Object { Test-Path $_ })
if ($Exes.Count -ne 1) {
    Write-Error "run-tests.ps1: expected exactly one MosaicTests.exe under bin\$Config-*, found $($Exes.Count) (run scripts/build.ps1 $Config)"
    exit 1
}

New-Item -ItemType Directory -Force -Path (Join-Path $Root 'test-results') | Out-Null
$Junit = "test-results/junit-$Config-seed$Seed.xml"
Write-Host "== $($Exes[0]) --order rand --rng-seed $Seed"
& $Exes[0] --order rand --rng-seed $Seed --reporter console --reporter "JUnit::out=$Junit"
exit $LASTEXITCODE
