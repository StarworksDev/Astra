# S4 regression gate + storage acceptance check (spec 2026-10-01 s2 S4, s9.3).
#
# Gate mode:       -Dir <folder with base_<r>.json and branch_<r>.json>
#   Every Google Benchmark present in both is compared on the MEDIAN real_time across rounds.
#   delta = (branch median - base median) / base median %, spread = base round-to-round (max - min) / median %.
#   LOUD  : spread > LoudSpread (5 %)                       -> unjudgeable in this window; re-measure, never a PASS.
#   PASS  : delta <= Threshold (2 %), and not LOUD.
#   FAIL  : delta > Threshold and delta > 2 x spread         (a delta well above the base's own noise is signal).
#   NOISY : delta > Threshold and delta <= 2 x spread        -> re-measure with 9 repetitions; a NOISY row whose
#           9-rep delta is still > Threshold with a stable sign is reported, never silently passed.
#   (Controller ruling 2026-10-25, Task 42 fix round 2: a tightening of the original PASS/NOISY/FAIL rule.)
#   Exit 0 only when every row PASSes, no base benchmark is missing on the branch, and no branch
#   benchmark (other than the branch-only BM_Storage_* rows) lacks a base counterpart: a branch-only
#   row would otherwise escape the comparison silently.
#
# Storage mode:    -StorageJson <AstraBenchmark --benchmark_filter=BM_Storage_ --benchmark_repetitions=3 json>
#   Prints the BM_Storage_ median table and checks S4 (2) Universal identity churn < Table, and
#   (3) Sparse add/remove churn < Table at 1K/10K/100K. Exit 0 only when both hold.
#
# -Markdown prints tables in RESULTS.md form. -BaseLabel names the base column (default 975cdb7).
param(
    [string]$Dir,
    [double]$Threshold = 2.0,
    [double]$LoudSpread = 5.0,
    [switch]$Markdown,
    [string]$StorageJson,
    [string]$BaseLabel = '975cdb7'
)

function Get-Median([double[]]$xs)
{
    $s = @($xs | Sort-Object)
    return $s[[int][math]::Floor($s.Count / 2)]
}

if ($StorageJson)
{
    $json = Get-Content $StorageJson -Raw | ConvertFrom-Json
    $med = @{}
    '| benchmark | median real time | unit |'
    '|---|---|---|'
    foreach ($b in @($json.benchmarks | Where-Object { $_.aggregate_name -eq 'median' }))
    {
        $med[$b.run_name] = [double]$b.real_time
        '| {0} | {1:N1} | {2} |' -f $b.run_name, $b.real_time, $b.time_unit
    }
    $ok = $true
    ''
    foreach ($n in 10000, 100000)
    {
        $t = $med["BM_Storage_IdentityChurn_Table/$n"]; $u = $med["BM_Storage_IdentityChurn_Universal/$n"]
        $pass = ($t -and $u -and $u -lt $t)
        $ok = $ok -and $pass
        'S4(2) identity churn N={0}: universal {1:N0} vs table {2:N0} ({3:N1}% faster) -> {4}' -f $n, $u, $t, ((1 - $u / $t) * 100), $(if ($pass) { 'PASS' } else { 'FAIL' })
    }
    foreach ($n in 1000, 10000, 100000)
    {
        $t = $med["BM_Storage_AddRemoveChurn_Table/$n"]; $s = $med["BM_Storage_AddRemoveChurn_Sparse/$n"]
        $pass = ($t -and $s -and $s -lt $t)
        $ok = $ok -and $pass
        'S4(3) add/remove churn N={0}: sparse {1:N0} vs table {2:N0} ({3:N1}% faster) -> {4}' -f $n, $s, $t, ((1 - $s / $t) * 100), $(if ($pass) { 'PASS' } else { 'FAIL' })
    }
    if ($ok) { 'S4 STORAGE ACCEPTANCE: PASS'; exit 0 } else { 'S4 STORAGE ACCEPTANCE: FAIL'; exit 1 }
}

if (-not $Dir) { Write-Error 'pass -Dir (gate mode) or -StorageJson (storage mode)'; exit 2 }

function Read-Runs([string]$pattern)
{
    $map = @{}
    foreach ($file in @(Get-ChildItem -Path $Dir -Filter $pattern | Sort-Object Name))
    {
        $json = Get-Content $file.FullName -Raw | ConvertFrom-Json
        foreach ($b in $json.benchmarks)
        {
            if ($b.run_type -eq 'aggregate') { continue }   # keep per-repetition iteration rows only
            if (-not $map.ContainsKey($b.name)) { $map[$b.name] = New-Object System.Collections.Generic.List[double] }
            $map[$b.name].Add([double]$b.real_time)
        }
    }
    return $map
}

$base = Read-Runs 'base_*.json'
$branch = Read-Runs 'branch_*.json'
if ($base.Count -eq 0 -or $branch.Count -eq 0) { Write-Error "no base_*.json / branch_*.json in $Dir"; exit 2 }

$rows = @()
foreach ($name in @($base.Keys | Sort-Object))
{
    if (-not $branch.ContainsKey($name)) { continue }
    $b = [double[]]$base[$name].ToArray()
    $n = [double[]]$branch[$name].ToArray()
    $mb = Get-Median $b
    $mn = Get-Median $n
    $spread = ((($b | Measure-Object -Maximum).Maximum) - (($b | Measure-Object -Minimum).Minimum)) / $mb * 100.0
    $delta = ($mn - $mb) / $mb * 100.0
    $verdict = if ($spread -gt $LoudSpread) { 'LOUD' }
               elseif ($delta -le $Threshold) { 'PASS' }
               elseif ($delta -gt 2.0 * $spread) { 'FAIL' }
               else { 'NOISY' }
    $rows += [pscustomobject]@{
        Benchmark = $name; Base = $mb; Branch = $mn; DeltaPct = $delta; BaseSpreadPct = $spread
        Rounds = '{0}/{1}' -f $b.Count, $n.Count; Verdict = $verdict
    }
}
$missing = @($base.Keys | Where-Object { -not $branch.ContainsKey($_) })
# Branch-only rows are legitimate only for the new BM_Storage_ benchmarks (judged by storage mode).
$unbased = @($branch.Keys | Where-Object { -not $base.ContainsKey($_) -and $_ -notmatch '^BM_Storage_' } | Sort-Object)

if ($Markdown)
{
    '| benchmark | {0} median | branch median | delta % | base spread % | rounds | verdict |' -f $BaseLabel
    '|---|---|---|---|---|---|---|'
    foreach ($r in $rows)
    {
        '| {0} | {1:N1} | {2:N1} | {3:+0.00;-0.00} | {4:N2} | {5} | {6} |' -f $r.Benchmark, $r.Base, $r.Branch, $r.DeltaPct, $r.BaseSpreadPct, $r.Rounds, $r.Verdict
    }
}
else
{
    $rows | Format-Table -AutoSize | Out-String -Width 240
}

$pass = @($rows | Where-Object { $_.Verdict -eq 'PASS' }).Count
$noisy = @($rows | Where-Object { $_.Verdict -eq 'NOISY' })
$fail = @($rows | Where-Object { $_.Verdict -eq 'FAIL' })
$loud = @($rows | Where-Object { $_.Verdict -eq 'LOUD' })
'S4 GATE: {0} compared, {1} PASS, {2} NOISY, {3} LOUD, {4} FAIL, {5} missing on branch, {6} branch-only without base' -f $rows.Count, $pass, $noisy.Count, $loud.Count, $fail.Count, $missing.Count, $unbased.Count
foreach ($m in $missing) { "  missing on branch: $m" }
foreach ($u in $unbased) { "  branch-only row without a base counterpart (escapes the gate): $u" }
foreach ($r in $loud) { "  LOUD (base spread {0:N2}% > {1}%, unjudgeable; re-measure): $($r.Benchmark) delta {2:+0.00;-0.00}%" -f $r.BaseSpreadPct, $LoudSpread, $r.DeltaPct }
foreach ($r in $noisy) { "  NOISY (re-measure): $($r.Benchmark) delta {0:+0.00;-0.00}% spread {1:N2}%" -f $r.DeltaPct, $r.BaseSpreadPct }
foreach ($r in $fail) { "  FAIL: $($r.Benchmark) {0:+0.00}% (base spread {1:N2}%)" -f $r.DeltaPct, $r.BaseSpreadPct }
if ($noisy.Count -gt 0 -or $loud.Count -gt 0 -or $fail.Count -gt 0 -or $missing.Count -gt 0 -or $unbased.Count -gt 0) { exit 1 }
exit 0
