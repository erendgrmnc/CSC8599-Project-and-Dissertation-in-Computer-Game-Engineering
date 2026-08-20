# Experiment runner.
#
# measure.ps1 executes ONE run. This drives a whole experiment: a sweep over a
# parameter, N repeats per point, one directory per repeat, and a manifest per
# experiment recording exactly what was swept.
#
# Repeats are not optional. Per-server object counts and handoff event counts vary
# between runs at the same seed, and several things measured later - halo lateness,
# snapshot volume, wall clock - vary a great deal more. Performance numbers must come
# from repeated runs with a spread, not from single runs.
#
# Examples:
#   # Locality (I6): per-server state as the world grows. -World must grow with it,
#   # or adding servers only subdivides a fixed world, which measures something else.
#   .\tools\run-experiments.ps1 -Name locality -Sweep servers -Values 1,2,4 -Repeats 3 `
#       -Workload uniform -Objects 400
#
#   # Interest management: snapshot volume against the radius a client asks for.
#   .\tools\run-experiments.ps1 -Name interest -Sweep interestRadius -Values 0,25,50,100 `
#       -Repeats 3 -Objects 4000 -Workload uniform -Seconds 30
#
#   # Load balancing: static (0) against dynamic (a reporting interval).
#   .\tools\run-experiments.ps1 -Name balance -Sweep rebalanceInterval -Values 0,400 `
#       -Repeats 3 -Workload cluster -Objects 4000 -Ticks 7200
#
#   # Parallel physics.
#   .\tools\run-experiments.ps1 -Name threads -Sweep physicsThreads -Values 0,2,4 `
#       -Repeats 3 -Objects 8000 -Servers 1 -Workload uniform
param(
    [Parameter(Mandatory = $true)][string]$Name,
    # Which parameter to sweep.
    [Parameter(Mandatory = $true)]
    [ValidateSet("servers", "objects", "ticks", "seconds", "haloWidth", "interestRadius",
                 "physicsThreads", "rebalanceInterval")]
    [string]$Sweep,
    # Comma-separated, e.g. -Values 1,2,4. A string rather than an array because
    # `powershell -File` does not parse array arguments: -Values 1,2 arrives as the
    # single value 12, which silently runs a 12-server experiment instead of two.
    #
    # Parsed as DOUBLE, since halo width and interest radius are distances, then cast
    # back where the underlying flag is an integer.
    [Parameter(Mandatory = $true)][string]$Values,
    [int]$Repeats = 5,

    # Held fixed unless swept.
    [int]$Servers = 2,
    [int]$Objects = 400,
    [int]$Ticks = 7200,
    # Only used when -Ticks is 0. Paced tick runs are for correctness and
    # reproducibility; wall-clock runs are for performance claims that need the loop
    # to be fed real deltas.
    [int]$Seconds = 30,
    [int]$Seed = 42,
    [string]$Workload = "shuttle",
    [string]$World = "-150,150,-150,150",

    # Interaction drivers. All default to off so a baseline measures the physics and
    # handoff path alone.
    [int]$ImpulseTest = 0,
    [int]$MisrouteEvery = 0,
    [int]$BlastEvery = 0,
    [int]$SpawnEvery = 0,
    [int]$DestroyEvery = 0,
    [int]$DriveEvery = 0,

    [int]$HandoffLookahead = 0,
    [int]$EpochAlignUs = 0,
    [int]$DrainSeconds = -1,
    [int]$HandoffRetryTicks = -1,
    [int]$HandoffMaxAttempts = -1,

    # Cross-border collision. 0 disables the halo, which is how everything before that
    # increment behaved.
    [double]$HaloWidth = 0,
    [int]$HaloLookahead = 4,
    # Reliable halo updates. Needed for a bit-reproducible run, because which
    # unreliable updates drop is not the same from run to run.
    [switch]$HaloReliable,

    # Client area of interest. 0 asks for every object.
    [double]$InterestRadius = 0,

    # Parallel physics workers per server. 0 keeps everything on the server's thread.
    [int]$PhysicsThreads = 0,

    # Dynamic rebalancing. 0 is a static partition.
    [int]$RebalanceInterval = 0,
    [double]$RebalanceAlpha = 0.5,
    [double]$RebalanceThreshold = 0.1,

    # Locality (I6). When sweeping servers, grow the WORLD and the object count with
    # the server count so objects-per-region stays fixed.
    #
    # Without this, adding servers only subdivides a fixed world, and per-server state
    # falls simply because each region got smaller - which is arithmetic, not a
    # property of the design. The claim worth testing is that per-server state stays
    # FLAT as the world grows, and that needs the world to actually grow.
    #
    # The world is square, so its side scales with sqrt(servers) to keep area
    # proportional; objects scale linearly.
    [switch]$ScaleWorldWithServers,

    [string]$OutDir = ""
)
$ErrorActionPreference = "Continue"

$valueList = @($Values -split '[,;]' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object { [double]$_ })
if ($valueList.Count -eq 0) {
    Write-Host "No sweep values parsed from '$Values'" -ForegroundColor Red
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot "RunPaths.ps1")

$OutDir = Resolve-RunOutDir -OutDir $OutDir -RepoRoot $repoRoot
$experimentDir = Join-Path $OutDir "exp-$Name"

Remove-Item $experimentDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $experimentDir | Out-Null

# One manifest per experiment. Determinism is per-configuration, so a dataset without
# its build metadata cannot be reproduced or even interpreted - and since several of
# these knobs change whether a run is reproducible at all, the manifest has to record
# every one of them, not just the swept parameter.
$manifest = [ordered]@{
    name       = $Name
    sweep      = $Sweep
    values     = $valueList
    repeats    = $Repeats
    fixed      = [ordered]@{
        servers = $Servers; objects = $Objects; ticks = $Ticks; seconds = $Seconds
        seed = $Seed; workload = $Workload; world = $World
        impulseTest = $ImpulseTest; misrouteEvery = $MisrouteEvery
        blastEvery = $BlastEvery; spawnEvery = $SpawnEvery
        destroyEvery = $DestroyEvery; driveEvery = $DriveEvery
        handoffLookahead = $HandoffLookahead; epochAlignUs = $EpochAlignUs
        drainSeconds = $DrainSeconds
        handoffRetryTicks = $HandoffRetryTicks; handoffMaxAttempts = $HandoffMaxAttempts
        haloWidth = $HaloWidth; haloLookahead = $HaloLookahead
        haloReliable = [bool]$HaloReliable
        interestRadius = $InterestRadius
        physicsThreads = $PhysicsThreads
        rebalanceInterval = $RebalanceInterval
        rebalanceAlpha = $RebalanceAlpha; rebalanceThreshold = $RebalanceThreshold
        scaleWorldWithServers = [bool]$ScaleWorldWithServers
    }
    gitCommit  = (& git -C $repoRoot rev-parse HEAD 2>$null)
    gitDirty   = [bool](& git -C $repoRoot status --porcelain 2>$null)
    machine    = $env:COMPUTERNAME
    os         = (Get-CimInstance Win32_OperatingSystem).Caption
    cpu        = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
    cores      = (Get-CimInstance Win32_Processor | Select-Object -First 1).NumberOfCores
    # Every server, the manager, the midware and the client run on THIS machine, so a
    # multi-server run shares these cores. Recorded because it is the reason a
    # wall-clock comparison across server counts measures contention, not distribution.
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
    $runServers = $Servers; $runObjects = $Objects; $runTicks = $Ticks; $runSeconds = $Seconds
    $runHaloWidth = $HaloWidth; $runInterest = $InterestRadius
    $runThreads = $PhysicsThreads; $runRebalance = $RebalanceInterval

    $runWorld = $World
    switch ($Sweep) {
        "servers"           { $runServers = [int]$value }
        "objects"           { $runObjects = [int]$value }
        "ticks"             { $runTicks = [int]$value }
        "seconds"           { $runSeconds = [int]$value; $runTicks = 0 }
        "haloWidth"         { $runHaloWidth = $value }
        "interestRadius"    { $runInterest = $value }
        "physicsThreads"    { $runThreads = [int]$value }
        "rebalanceInterval" { $runRebalance = [int]$value }
    }

    if ($ScaleWorldWithServers -and $Sweep -eq "servers") {
        # Baseline extent taken from -World, scaled so area grows with the server
        # count. Objects grow linearly, so objects per unit area - and therefore per
        # region - is held constant.
        $bounds = @($World -split ',' | ForEach-Object { [double]$_.Trim() })
        if ($bounds.Count -eq 4) {
            $scale = [Math]::Sqrt($runServers / [double]$Servers)
            $minX = $bounds[0] * $scale; $maxX = $bounds[1] * $scale
            $minZ = $bounds[2] * $scale; $maxZ = $bounds[3] * $scale
            $runWorld = "{0},{1},{2},{3}" -f $minX, $maxX, $minZ, $maxZ
            $runObjects = [int]([Math]::Round($Objects * ($runServers / [double]$Servers)))
        }
    }

    for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
        # The value can be fractional, and a '.' in a directory name is legal but
        # awkward to match with a glob, so it is replaced.
        $valueTag = ([string]$value) -replace '\.', 'p' -replace '-', 'm'
        $tag = "$Sweep$valueTag-r$repeat"
        $done++
        Write-Host ""
        Write-Host "--- [$done/$total] $tag (servers=$runServers objects=$runObjects world=$runWorld ticks=$runTicks halo=$runHaloWidth interest=$runInterest threads=$runThreads rebalance=$runRebalance) ---"

        & (Join-Path $PSScriptRoot "measure.ps1") `
            -Servers $runServers -Objects $runObjects -Ticks $runTicks -Seconds $runSeconds `
            -Seed $Seed -Workload $Workload -World $runWorld `
            -ImpulseTest $ImpulseTest -MisrouteEvery $MisrouteEvery -BlastEvery $BlastEvery `
            -SpawnEvery $SpawnEvery -DestroyEvery $DestroyEvery -DriveEvery $DriveEvery `
            -HandoffLookahead $HandoffLookahead -EpochAlignUs $EpochAlignUs -DrainSeconds $DrainSeconds `
            -HandoffRetryTicks $HandoffRetryTicks -HandoffMaxAttempts $HandoffMaxAttempts `
            -HaloWidth $runHaloWidth -HaloLookahead $HaloLookahead -HaloReliable:$HaloReliable `
            -InterestRadius $runInterest -PhysicsThreads $runThreads `
            -RebalanceInterval $runRebalance -RebalanceAlpha $RebalanceAlpha `
            -RebalanceThreshold $RebalanceThreshold `
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
