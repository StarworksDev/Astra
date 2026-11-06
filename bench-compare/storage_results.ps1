# Folds bench_storage round CSVs (lib,op,N,ns_per_op,items) into the RESULTS.md table:
# median [min, max] ns/op per (op, N) across rounds.
param([string]$Pattern = 'storage_round*.csv')

$rows = foreach ($file in Get-ChildItem -Path $PSScriptRoot -Filter $Pattern)
{
    Get-Content $file.FullName | Where-Object { $_ -match '^astra-storage,' } | ForEach-Object {
        $p = $_.Split(',')
        [pscustomobject]@{ Op = $p[1]; N = [int64]$p[2]; Ns = [double]$p[3] }
    }
}
if (-not $rows) { Write-Error "no rows matched $Pattern in $PSScriptRoot"; exit 1 }

'| op | N | median ns/op | min | max | rounds |'
'|---|---|---|---|---|---|'
$groups = $rows | Group-Object -Property Op, N | Sort-Object { $_.Group[0].Op }, { $_.Group[0].N }
foreach ($g in $groups)
{
    $v = @($g.Group | ForEach-Object { $_.Ns } | Sort-Object)
    $median = $v[[int][math]::Floor($v.Count / 2)]
    '| {0} | {1} | {2:N3} | {3:N3} | {4:N3} | {5} |' -f $g.Group[0].Op, $g.Group[0].N, $median, $v[0], $v[$v.Count - 1], $v.Count
}
