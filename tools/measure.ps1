# Bounded measurement run.
#
# Servers exit cleanly after --run-seconds and flush their per-tick metrics to CSV.
# A force-killed server loses its whole buffer, which is why the run is bounded
# rather than terminated externally.
#
# Every measurement run must pass --fixed-step and --seed or the numbers are not
# comparable: without them servers under different load integrate with different dt
# and build different worlds.
param(
    [int]$Servers = 2,
    [int]$Objects = 400,
    [int]$Seconds = 60,
    # Reproducible mode. When > 0 the servers run a fixed number of ticks with a
    # pinned dt instead of a wall-clock window, which is what makes two runs of the
    # same binary produce identical results. Takes precedence over -Seconds.
    [int]$Ticks = 0,
    [int]$Seed = 42,
    [string]$Workload = "shuttle",
    [string]$Tag = "run",
    # Fires one interaction command every N client ticks. 0 disables. Exercises the
    # command channel so the I4 accounting invariant can be checked.
    [int]$ImpulseTest = 0,
    # Sends every Nth driven command to a server that does not own the object, so the
    # misroute/relay path is exercised. 0 disables.
    [int]$MisrouteEvery = 0,
    # Fires a radial impulse on a region seam every N client ticks, exercising the
    # cross-border fan-out. 0 disables.
    [int]$BlastEvery = 0,
    # Spawns an object every N client ticks, alternating sides of the seam. 0 disables.
    [int]$SpawnEvery = 0,
    [int]$BlastOffsetX = 0,
    # Destroys an object every N client ticks. 0 disables.
    [int]$DestroyEvery = 0,
    # Starts a SECOND client this many seconds after the first. Every peer normally
    # joins during bootstrap, before the world exists, so without this there is no
    # late joiner and the manifest path is never exercised.
    [int]$LateClientAfter = 0,
    # Drives one object along +X every N client ticks so it crosses a border while
    # under control. 0 disables.
    [int]$DriveEvery = 0,
    # Fault injection: delays each handoff packet by N ticks so races W3 and the
    # client resurrection guard are actually reached. Must be 0 for measurement.
    [int]$HandoffDelayTicks = 0,
    # Schedules incoming handoffs at senderTick + N instead of on arrival, making
    # handoff application deterministic. 0 keeps apply-on-arrival.
    [int]$HandoffLookahead = 0,
    # Aligns every server's tick 0 to a shared monotonic-clock boundary (us).
    [int]$EpochAlignUs = 0,
    # World bounds, minX,maxX,minZ,maxZ. Needed for the locality experiment (I6),
    # which holds objects-per-region fixed and grows the WORLD as servers are added
    # - with a fixed world, adding servers only subdivides it, which measures
    # something else entirely.
    # Halo band width in world units. 0 disables the halo entirely, which is how
    # every run before the cross-border collision increment behaved.
    [double]$HaloWidth = 0,
    # Ticks between a halo state being sampled and being applied on the neighbour.
    # Small on purpose - unlike the handoff lookahead, this is permanent lag on a
    # continuously tracked position, not a one-off gap.
    [int]$HaloLookahead = 4,
    # Send halo updates reliably. Costs bandwidth, but a dropped update leaves a
    # shadow extrapolating from an older sample and drops are not the same from
    # run to run - so reproducible runs need this.
    [switch]$HaloReliable,
    # Forces one partition change at an ABSOLUTE tick, for testing the repartition
    # mechanism before any policy decides when to use it. 0 disables.
    # Worker threads for the parallel physics phases, per server. 0 keeps
    # everything on the server's own thread; -1 derives a default from the
    # hardware. Contact resolution is never parallel - see PhysicsSystem.h.
    # Client area-of-interest radius, about the world origin. 0 asks for every
    # object, which is what every run before interest management did.
    [double]$InterestRadius = 0,
    [int]$PhysicsThreads = 0,
    [int]$RepartitionAt = 0,
    # Interior X boundaries of the new partition: N-1 values for N servers. A comma
    # separated STRING, not an array - powershell -File cannot parse array arguments.
    [string]$RepartitionX = "",
    [string]$World = "-150,150,-150,150",
    [string]$OutDir = ""
)
$ErrorActionPreference = "Continue"

$repoRoot = Split-Path -Parent $PSScriptRoot
$deploy = Join-Path $repoRoot "deploy"

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "runs"
}
$runDir = Join-Path $OutDir $Tag

Get-Process -Name EntryPoint -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Remove-Item $runDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$serverExe  = Join-Path $deploy "DistributedPhysicsServer\EntryPoint.exe"
$metricsDir = $runDir -replace '\\','/'

# Recorded alongside the CSVs: determinism is per-configuration, so a dataset
# without its build metadata is not reproducible.
$mode = if ($Ticks -gt 0) { "reproducible" } else { "realtime" }
$haloReliableArg = if ($HaloReliable) { "--halo-reliable" } else { "" }
$repartitionArgs = if ($RepartitionAt -gt 0 -and $RepartitionX -ne "") { "--repartition-at $RepartitionAt --repartition-x $RepartitionX" } else { "" }
$bound = if ($Ticks -gt 0) { "--run-ticks $Ticks" } else { "--run-seconds $Seconds" }

$manifest = [ordered]@{
    tag          = $Tag
    mode         = $mode
    servers      = $Servers
    objects      = $Objects
    seconds      = $Seconds
    ticks        = $Ticks
    seed         = $Seed
    workload     = $Workload
    world        = $World
    haloWidth    = $HaloWidth
    haloLookahead = $HaloLookahead
    physicsThreads = $PhysicsThreads
    repartitionAt = $RepartitionAt
    repartitionX = $RepartitionX
    gitCommit    = (& git -C $repoRoot rev-parse HEAD 2>$null)
    gitDirty     = [bool](& git -C $repoRoot status --porcelain 2>$null)
    machine      = $env:COMPUTERNAME
    os           = (Get-CimInstance Win32_OperatingSystem).Caption
}
$manifest | ConvertTo-Json | Out-File -FilePath (Join-Path $runDir "manifest.json") -Encoding utf8

Write-Host "run=$Tag mode=$mode world=$World servers=$Servers objects=$Objects bound='$bound' seed=$Seed workload=$Workload"

$mgr = Start-Process -PassThru -FilePath (Join-Path $deploy "Manager\EntryPoint.exe") `
    -ArgumentList "--servers $Servers --clients 1 --objects $Objects --port 1234 --world $World --midwares 1 --autostart --headless $repartitionArgs" `
    -WorkingDirectory $deploy -RedirectStandardOutput "$runDir\mgr.log" -RedirectStandardError "$runDir\mgr.err" -WindowStyle Hidden
Start-Sleep -Seconds 3

$mid = Start-Process -PassThru -FilePath (Join-Path $deploy "Midware\EntryPoint.exe") `
    -ArgumentList "--manager-ip 127.0.0.1 --manager-port 1234 --server-exe `"$serverExe`" --headless --fixed-step --seed $Seed --workload $Workload --metrics-dir `"$metricsDir`" --handoff-delay-ticks $HandoffDelayTicks --handoff-lookahead $HandoffLookahead --physics-threads $PhysicsThreads --halo-width $HaloWidth --halo-lookahead $HaloLookahead $haloReliableArg --epoch-align-us $EpochAlignUs $bound" `
    -WorkingDirectory $deploy -RedirectStandardOutput "$runDir\mid.log" -RedirectStandardError "$runDir\mid.err" -WindowStyle Hidden
Start-Sleep -Seconds 4

# The client stops sending well before the servers stop counting, so every command
# it issued has been processed by the time they exit and the I4 tally is exact.
$serverRunSeconds = if ($Ticks -gt 0) { [Math]::Round($Ticks / 120.0) } else { $Seconds }
$clientSeconds = [Math]::Max(5, $serverRunSeconds - 15)

$cli = Start-Process -PassThru -FilePath (Join-Path $deploy "Client\EntryPoint.exe") `
    -ArgumentList "--manager-ip 127.0.0.1 --manager-port 1234 --headless --impulse-test $ImpulseTest --misroute-every $MisrouteEvery --blast-every $BlastEvery --spawn-every $SpawnEvery --blast-offset-x $BlastOffsetX --destroy-every $DestroyEvery --drive-every $DriveEvery --interest-radius $InterestRadius --run-seconds $clientSeconds" `
    -WorkingDirectory $deploy -RedirectStandardOutput "$runDir\cli.log" -RedirectStandardError "$runDir\cli.err" -WindowStyle Hidden

if ($LateClientAfter -gt 0) {
    Start-Sleep -Seconds $LateClientAfter
    Write-Host "Starting late-joining client after ${LateClientAfter}s"
    $lateCli = Start-Process -PassThru -FilePath (Join-Path $deploy "Client\EntryPoint.exe") `
        -ArgumentList "--manager-ip 127.0.0.1 --manager-port 1234 --headless --run-seconds 10" `
        -WorkingDirectory $deploy -RedirectStandardOutput "$runDir\cli-late.log" -RedirectStandardError "$runDir\cli-late.err" -WindowStyle Hidden
}

# Servers self-terminate; allow slack for startup plus flush. Reproducible runs are
# not wall-clock paced (no per-tick sleep), so they finish faster than realtime -
# but how much faster depends on the machine, hence a generous ceiling.
$waitSeconds = if ($Ticks -gt 0) { [Math]::Max(60, $Ticks / 20) } else { $Seconds + 25 }
$deadline = (Get-Date).AddSeconds($waitSeconds)
while ((Get-Date) -lt $deadline) {
    $csvs = Get-ChildItem "$runDir\ticks-server*.csv" -ErrorAction SilentlyContinue
    if ($csvs.Count -ge $Servers) { break }
    Start-Sleep -Seconds 2
}

# The CSV is NOT the completion signal. A server writes its CSV in FlushMetrics and
# prints its @@FINAL line afterwards, so breaking out of the loop above and killing
# the midware immediately can cut the last line off - the run then looks clean, has
# a full set of CSVs, and is silently missing a server from every @@FINAL-based
# invariant check. Observed exactly once in a2 regression testing, on the slower of
# two back-to-back runs.
#
# Wait for one @@FINAL per server, then a short grace for the midware's pipe to
# forward it. Bounded: a genuinely dead server must not hang the harness.
$finalDeadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $finalDeadline) {
    $finals = @(Select-String -Path "$runDir\mid.log" -Pattern "@@FINAL role=server" -ErrorAction SilentlyContinue)
    if ($finals.Count -ge $Servers) { break }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 1

$finals = @(Select-String -Path "$runDir\mid.log" -Pattern "@@FINAL role=server" -ErrorAction SilentlyContinue)
if ($finals.Count -lt $Servers) {
    Write-Host "WARNING: only $($finals.Count) of $Servers servers reported @@FINAL. Invariant totals for this run are incomplete." -ForegroundColor Yellow
}

foreach ($p in @($cli, $mid, $mgr)) {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
Start-Sleep -Seconds 1
Get-Process -Name EntryPoint -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "==================== METRIC FILES ===================="
Get-ChildItem "$runDir\*.csv" -ErrorAction SilentlyContinue | ForEach-Object {
    $rows = (Get-Content $_.FullName | Measure-Object -Line).Lines - 1
    "{0}  {1} rows  {2} bytes" -f $_.Name, $rows, $_.Length
}

Write-Host ""
Write-Host "==================== SINK REPORT ===================="
Select-String -Path "$runDir\mid.log" -Pattern "MetricSink:|Headless run complete" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Line }

Write-Host ""
Write-Host "==================== FINAL TOTALS ===================="
# @@FINAL lines are exact end-of-run totals, unlike the 2 Hz @@STAT samples.
Select-String -Path "$runDir\mid.log", "$runDir\cli.log" -Pattern "@@FINAL" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Line }
