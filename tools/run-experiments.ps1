# Experiment runner.
#
# measure.ps1 executes ONE run. This drives a whole experiment: a sweep over a
# parameter, N repeats per point, one directory per repeat, and a manifest per
# experiment recording exactly what was swept.
#
# Repeats are not optional. Section 17 of the interactions design records that
# per-server object counts and handoff event counts vary by +/-1 between runs at the
# same seed, and that closing that gap needs a global tick barrier. Performance
# numbers must therefore come from repeated runs with a spread, not from single runs.
#
# Examples:
#   # Scaling: 1,2,4 servers at 400 objects, 5 repeats each
#   .\tools\run-experiments.ps1 -Name scaling -Sweep servers -Values 1,2,4 -Repeats 5
#
#   # Load: object-count sweep on 2 servers
#   .\tools\run-experiments.ps1 -Name load -Sweep objects -Values 100,400,1600 -Repeats 5
param(
    [Parameter(Mandatory = $true)][string]$Name,
    # Which parameter to sweep.
    [Parameter(Mandatory = $true)][ValidateSet("servers", "objects", "ticks")][string]$Sweep,
    # Comma-separated, e.g. -Values 1,2,4. A string rather than int[] because
    # `powershell -File` does not parse array arguments: -Values 1,2 arrives as the
    # single value 12, which silently runs a 12-server experiment instead of two.
    [Parameter(Mandatory = $true)][string]$Values,
    [int]$Repeats = 5,

    # Held fixed unless swept.
    [int]$Servers = 2,
    [int]$Objects = 400,
    [int]$Ticks = 7200,
    [int]$Seed = 42,
    [string]$Workload = "shuttle",

    # Interaction drivers. All default to off so a baseline measures the physics and
    # handoff path alone.
    [int]$ImpulseTest = 0,
    [int]$MisrouteEvery = 0,
    [int]$BlastEvery = 0,
    [int]$SpawnEvery = 0,
    [int]$DestroyEvery = 0,
    [int]$DriveEvery = 0,

    [int]$HandoffLookahead = 0,
    [string]$OutDir = ""
)
$ErrorActionPreference = "Continue"

$valueList = @($Values -split '[,;]' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object { [int]$_ })
if ($valueList.Count -eq 0) {
    Write-Host "No sweep values parsed from '$Values'" -ForegroundColor Red
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "runs"
}
$experimentDir = Join-Path $OutDir "exp-$Name"

Remove-Item $experimentDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $experimentDir | Out-Null

# One manifest per experiment. Determinism is per-configuration, so a dataset
# without its build metadata cannot be reproduced or even interpreted.
$manifest = [ordered]@{
    name       = $Name
    sweep      = $Sweep
    values     = $valueList
    repeats    = $Repeats
    fixed      = [ordered]@{
        servers = $Servers; objects = $Objects; ticks = $Ticks
        seed = $Seed; workload = $Workload
        impulseTest = $ImpulseTest; misrouteEvery = $MisrouteEvery
        blastEvery = $BlastEvery; spawnEvery = $SpawnEvery
        destroyEvery = $DestroyEvery; driveEvery = $DriveEvery
        handoffLookahead = $HandoffLookahead
    }
    gitCommit  = (& git -C $repoRoot rev-parse HEAD 2>$null)
    gitDirty   = [bool](& git -C $repoRoot status --porcelain 2>$null)
    machine    = $env:COMPUTERNAME
    os         = (Get-CimInstance Win32_OperatingSystem).Caption
    cpu        = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    startedUtc = (Get-Date).ToUniversalTime().ToString("o")
}
$manifest | ConvertTo-Json -Depth 5 | Out-File -FilePath (Join-Path $experimentDir "experiment.json") -Encoding utf8

Write-Host "=== experiment '$Name': sweeping $Sweep over [$($valueList -join ', ')], $Repeats repeat(s) each ==="
if ($manifest.gitDirty) {
    Write-Host "WARNING: working tree is dirty - this dataset is not reproducible from the recorded commit." -ForegroundColor Yellow
}

$total = $valueList.Count * $Repeats
$done = 0

foreach ($value in $valueList) {
    $runServers = $Servers; $runObjects = $Objects; $runTicks = $Ticks
    switch ($Sweep) {
        "servers" { $runServers = $value }
        "objects" { $runObjects = $value }
        "ticks"   { $runTicks = $value }
    }

    for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
        $tag = "$Sweep$value-r$repeat"
        $done++
        Write-Host ""
        Write-Host "--- [$done/$total] $tag (servers=$runServers objects=$runObjects ticks=$runTicks) ---"

        & (Join-Path $PSScriptRoot "measure.ps1") `
            -Servers $runServers -Objects $runObjects -Ticks $runTicks `
            -Seed $Seed -Workload $Workload `
            -ImpulseTest $ImpulseTest -MisrouteEvery $MisrouteEvery -BlastEvery $BlastEvery `
            -SpawnEvery $SpawnEvery -DestroyEvery $DestroyEvery -DriveEvery $DriveEvery `
            -HandoffLookahead $HandoffLookahead `
            -Tag $tag -OutDir $experimentDir | Out-Null

        # A run that produced no CSV is a failed run, not a slow one. Say so loudly:
        # silently averaging over missing points is how a scaling curve ends up
        # describing fewer servers than it claims.
        $csvs = @(Get-ChildItem (Join-Path $experimentDir "$tag\ticks-server*.csv") -ErrorAction SilentlyContinue)
        if ($csvs.Count -lt $runServers) {
            Write-Host "  FAILED: $($csvs.Count)/$runServers servers produced metrics" -ForegroundColor Red
        }
        else {
            Write-Host "  ok: $($csvs.Count)/$runServers servers"
        }
    }
}

Write-Host ""
Write-Host "=== experiment complete -> $experimentDir ==="
Write-Host "Analyse with: python tools\analyse.py $experimentDir"
