# Build Mosaic + MosaicTests on Windows with the newest Visual Studio on the
# machine (located via vswhere).
#   scripts/build.ps1 <Debug|Release|Dist>
# Generator: the vendored vendor/premake5/premake5.exe (premake 5.0.0-beta8,
# byte-identical to the upstream windows zip; SHA-256 verified below).
$ErrorActionPreference = 'Stop'

$Config = $args[0]
if ($args.Count -ne 1 -or $Config -notin @('Debug', 'Release', 'Dist')) {
    Write-Error "usage: scripts/build.ps1 <Debug|Release|Dist>"
    exit 2
}

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$PremakeSha256 = '2301e3e23ff3074cb83a5ea6103d68c7ea81dad56b786807c84b0643cddea31b'
$Premake = Join-Path $Root 'vendor\premake5\premake5.exe'
$Actual = (Get-FileHash -Algorithm SHA256 $Premake).Hash.ToLowerInvariant()
if ($Actual -ne $PremakeSha256) {
    Write-Error "vendored premake5.exe SHA-256 mismatch: expected $PremakeSha256, got $Actual"
    exit 1
}

$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$VsVersion = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -property installationVersion
$MsBuild = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $VsVersion -or -not $MsBuild) {
    Write-Error "no Visual Studio with MSBuild found"
    exit 1
}
$Major = [int]($VsVersion.Split('.')[0])
$Action = switch ($Major) {
    17 { 'vs2022' }
    18 { 'vs2026' }
    default { Write-Error "unsupported Visual Studio major version $Major ($VsVersion)"; exit 1 }
}

Write-Host "== Visual Studio $VsVersion -> premake5 $Action"
Write-Host "== $MsBuild"
& $Premake $Action
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Determinism guard: /fp:strict on the library + tests, /fp:fast nowhere.
$Projects = @(Get-ChildItem -Path $Root -Recurse -Filter '*.vcxproj' | Where-Object { $_.FullName -notmatch '\\bin(-int)?\\' })
foreach ($P in $Projects) {
    if (Select-String -Path $P.FullName -Pattern '<FloatingPointModel>Fast|/fp:fast' -Quiet) {
        Write-Error "build.ps1: /fp:fast in $($P.FullName) (banned, see docs/ci.md)"
        exit 1
    }
}
foreach ($Name in @('Mosaic.vcxproj', 'MosaicTests.vcxproj')) {
    $P = $Projects | Where-Object { $_.Name -eq $Name } | Select-Object -First 1
    if (-not $P -or -not (Select-String -Path $P.FullName -Pattern '<FloatingPointModel>Strict' -Quiet)) {
        Write-Error "build.ps1: $Name is missing <FloatingPointModel>Strict"
        exit 1
    }
}

$Solution = if ($Action -eq 'vs2026') { 'Mosaic.slnx' } else { 'Mosaic.sln' }
& $MsBuild (Join-Path $Root $Solution) "/p:Configuration=$Config" /m /nologo /v:minimal
exit $LASTEXITCODE
