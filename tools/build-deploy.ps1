<#
.SYNOPSIS
    Builds all four distributed-system roles (one per CMake toggle configuration)
    and stages them into a deploy/ folder the GUI launcher consumes.

.DESCRIPTION
    A single EntryPoint.exe is produced per CMake toggle configuration, so the four
    roles (Manager / Midware / Game Server / Client) are four separate builds of the
    same solution. This script flips the toggles in CMakeLists.txt, regenerates,
    builds EntryPoint, and copies the resulting EntryPoint.exe plus its DLLs into:

        deploy/Manager/EntryPoint.exe
        deploy/Midware/EntryPoint.exe
        deploy/Client/EntryPoint.exe
        deploy/DistributedPhysicsServer/EntryPoint.exe   (spawned by the midware)
        deploy/*.dll                                      (shared, e.g. FMOD)

    ASSETROOTLOCATION is baked as an absolute path at configure time, so the deployed
    exes resolve assets regardless of working directory. The original toggle state
    (true/false/true) is restored at the end.

.PARAMETER Config
    MSBuild configuration (Debug or Release). Defaults to Debug.

.PARAMETER Roles
    Subset of roles to build. Defaults to all four.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1
    powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [ValidateSet("Manager", "Midware", "Client", "GameServer")]
    [string[]]$Roles = @("Manager", "Midware", "Client", "GameServer")
)

$ErrorActionPreference = "Continue"

# --- Paths -------------------------------------------------------------------
$repo = Split-Path -Parent $PSScriptRoot   # tools/ -> repo root
$cml = Join-Path $repo "CMakeLists.txt"
$deploy = Join-Path $repo "deploy"
# Per-role exe names now differ (EntryPointManager / EntryPointMidware /
# EntryPointServer / EntryPoint); Stage-Role resolves each from $builtDir.
$builtDir = Join-Path $repo "EntryPoint\$Config"

$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
    $found = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022" -Recurse -Filter "MSBuild.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\Bin\\MSBuild.exe$" } | Select-Object -First 1
    if ($found) { $msbuild = $found.FullName } else { Write-Host "ERROR: MSBuild.exe not found." -ForegroundColor Red; exit 1 }
}

# The three distributed server roles now differ only by a per-target compile
# definition, so ONE configure + ONE build produces all three. Only the Client flips
# DISTRIBUTEDSYSTEMACTIVE, which changes library code, so it needs its own configure.
# This is two configures instead of the previous four full rebuilds.
#
# role -> (cmake target, deploy subfolder)
$distributedRoles = @{
    "Manager"    = @{ Target = "EntryPointManager"; Out = "Manager" }
    "Midware"    = @{ Target = "EntryPointMidware"; Out = "Midware" }
    "GameServer" = @{ Target = "EntryPointServer";  Out = "DistributedPhysicsServer" }
}
$clientRole = @{ Target = "EntryPoint"; Out = "Client" }

function Set-Toggle([string]$active) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    $text = [System.IO.File]::ReadAllText($cml)
    $text = [regex]::Replace($text, 'set\(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "[^"]*"\)', "set(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE `"$active`")")
    [System.IO.File]::WriteAllText($cml, $text, $enc)
}

# Stages one built exe into deploy/<Out>/EntryPoint.exe, carrying any sibling DLLs.
function Stage-Role([string]$targetName, [string]$outName, [ref]$resultList) {
    $src = Join-Path $builtDir "$targetName.exe"
    if (-not (Test-Path $src)) { $resultList.Value += "$outName : EXE MISSING ($targetName.exe)"; return }

    $outDir = Join-Path $deploy $outName
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    try {
        Copy-Item $src (Join-Path $outDir "EntryPoint.exe") -Force -ErrorAction Stop
    }
    catch {
        $resultList.Value += "$outName : COPY FAILED (exe in use? close running role windows)"
        return
    }

    foreach ($dll in (Get-ChildItem $builtDir -Filter "*.dll" -ErrorAction SilentlyContinue)) {
        Copy-Item $dll.FullName (Join-Path $outDir $dll.Name) -Force -ErrorAction SilentlyContinue
        Copy-Item $dll.FullName (Join-Path $deploy $dll.Name) -Force -ErrorAction SilentlyContinue
    }
    $resultList.Value += "$outName : OK -> deploy\$outName\EntryPoint.exe"
}

Set-Location $repo
New-Item -ItemType Directory -Force -Path $deploy | Out-Null
$results = @()

# Free any deployed role exes still running (e.g. a previous launch), otherwise
# the copy below fails with a file lock.
Get-Process -Name EntryPoint -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($deploy, [System.StringComparison]::OrdinalIgnoreCase) } |
    ForEach-Object { Write-Host "Stopping running role: $($_.Path)" -ForegroundColor Yellow; Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 600

# ---- Pass 1: the three distributed server roles, one configure + one build --------
$wantedDistributed = @($Roles | Where-Object { $distributedRoles.ContainsKey($_) })
if ($wantedDistributed.Count -gt 0) {
    Write-Host "==================== DEPLOY: $($wantedDistributed -join ', ') (distributed configure) ====================" -ForegroundColor Cyan

    Set-Toggle "true"
    Remove-Item (Join-Path $repo "CMakeCache.txt") -ErrorAction SilentlyContinue
    & cmake -G "Visual Studio 17 2022" -A x64 . | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $results += "distributed roles : CMAKE FAILED"
    }
    else {
        $targets = ($wantedDistributed | ForEach-Object { $distributedRoles[$_].Target }) -join ";"
        & $msbuild "DistributedPhysicsSystem.sln" /t:$targets /p:Configuration=$Config /p:Platform=x64 /m /v:minimal /nologo
        if ($LASTEXITCODE -ne 0) {
            $results += "distributed roles : BUILD FAILED"
        }
        else {
            foreach ($role in $wantedDistributed) {
                Stage-Role $distributedRoles[$role].Target $distributedRoles[$role].Out ([ref]$results)
            }
        }
    }
}

# ---- Pass 2: the client, which needs DISTRIBUTEDSYSTEMACTIVE off ------------------
if ($Roles -contains "Client") {
    Write-Host "==================== DEPLOY: Client (non-distributed configure) ====================" -ForegroundColor Cyan

    Set-Toggle "false"
    Remove-Item (Join-Path $repo "CMakeCache.txt") -ErrorAction SilentlyContinue
    & cmake -G "Visual Studio 17 2022" -A x64 . | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $results += "Client : CMAKE FAILED"
    }
    else {
        & $msbuild "DistributedPhysicsSystem.sln" /t:$($clientRole.Target) /p:Configuration=$Config /p:Platform=x64 /m /v:minimal /nologo
        if ($LASTEXITCODE -ne 0) { $results += "Client : BUILD FAILED" }
        else { Stage-Role $clientRole.Target $clientRole.Out ([ref]$results) }
    }
}

# Publish the .NET launcher into deploy/Launcher so a remote machine's deploy/
# folder is self-contained (controller GUI + agent mode), and drop a run-agent.bat.
$launcherProj = Join-Path $repo "tools\DistributedLauncher\DistributedLauncher.csproj"
if (Test-Path $launcherProj) {
    Write-Host "==================== PUBLISH: Launcher ====================" -ForegroundColor Cyan
    $launcherOut = Join-Path $deploy "Launcher"
    & dotnet publish $launcherProj -c Release -o $launcherOut --nologo | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $batPath = Join-Path $deploy "run-agent.bat"
        $batBody = "@echo off`r`nREM Starts the launcher in headless agent mode for remote midware machines.`r`n`"%~dp0Launcher\DistributedLauncher.exe`" --agent --port 5099`r`n"
        [System.IO.File]::WriteAllText($batPath, $batBody, (New-Object System.Text.UTF8Encoding($false)))
        $results += "Launcher : OK -> deploy\Launcher\ (+ run-agent.bat)"
    }
    else {
        $results += "Launcher : PUBLISH FAILED"
    }
}

# Restore the default (distributed) toggle so the working tree is unchanged, and
# REGENERATE. Restoring the toggle alone left DistributedPhysicsSystem.sln holding the
# last pass's configuration (the Client), so a subsequent plain
# `msbuild DistributedPhysicsSystem.sln` would build the wrong role set against
# libraries from a different configure and fail to link.
Set-Toggle "true"
Remove-Item (Join-Path $repo "CMakeCache.txt") -ErrorAction SilentlyContinue
& cmake -G "Visual Studio 17 2022" -A x64 . | Out-Null
if ($LASTEXITCODE -ne 0) { $results += "restore configure : CMAKE FAILED" }

Write-Host "==================== SUMMARY ====================" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host $_ }
Write-Host "Deploy folder: $deploy"
