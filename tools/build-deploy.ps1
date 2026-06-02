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
$builtExe = Join-Path $repo "EntryPoint\$Config\EntryPoint.exe"
$builtDir = Join-Path $repo "EntryPoint\$Config"

$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
    $found = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022" -Recurse -Filter "MSBuild.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\Bin\\MSBuild.exe$" } | Select-Object -First 1
    if ($found) { $msbuild = $found.FullName } else { Write-Host "ERROR: MSBuild.exe not found." -ForegroundColor Red; exit 1 }
}

# role -> (toggle triple, deploy subfolder)
$matrix = @{
    "Manager"    = @{ Toggle = @("true", "true", "false");  Out = "Manager" }
    "Midware"    = @{ Toggle = @("true", "false", "true");  Out = "Midware" }
    "Client"     = @{ Toggle = @("false", "false", "true"); Out = "Client" }
    "GameServer" = @{ Toggle = @("true", "false", "false"); Out = "DistributedPhysicsServer" }
}

function Set-Toggle([string]$active, [string]$manager, [string]$midware) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    $text = [System.IO.File]::ReadAllText($cml)
    $text = [regex]::Replace($text, 'set\(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "[^"]*"\)', "set(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE `"$active`")")
    $text = [regex]::Replace($text, 'set\(CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER "[^"]*"\)', "set(CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER `"$manager`")")
    $text = [regex]::Replace($text, 'set\(CMAKE_BUILD_FOR_PHYSICS_MIDWARE "[^"]*"\)', "set(CMAKE_BUILD_FOR_PHYSICS_MIDWARE `"$midware`")")
    [System.IO.File]::WriteAllText($cml, $text, $enc)
}

Set-Location $repo
New-Item -ItemType Directory -Force -Path $deploy | Out-Null
$results = @()

foreach ($role in $Roles) {
    $info = $matrix[$role]
    $t = $info.Toggle
    Write-Host "==================== DEPLOY: $role ($($t -join '/')) ====================" -ForegroundColor Cyan

    Set-Toggle $t[0] $t[1] $t[2]
    Remove-Item (Join-Path $repo "CMakeCache.txt") -ErrorAction SilentlyContinue
    & cmake -G "Visual Studio 17 2022" -A x64 . | Out-Null
    if ($LASTEXITCODE -ne 0) { $results += "$role : CMAKE FAILED"; continue }

    & $msbuild "DistributedPhysicsSystem.sln" /t:EntryPoint /p:Configuration=$Config /p:Platform=x64 /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { $results += "$role : BUILD FAILED"; continue }
    if (-not (Test-Path $builtExe)) { $results += "$role : EXE MISSING"; continue }

    $outDir = Join-Path $deploy $info.Out
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    Copy-Item $builtExe (Join-Path $outDir "EntryPoint.exe") -Force

    # Carry any DLLs (FMOD etc.) that CMake copied next to the exe; place them
    # both next to the role exe and at the deploy root for convenience.
    $dlls = Get-ChildItem $builtDir -Filter "*.dll" -ErrorAction SilentlyContinue
    foreach ($dll in $dlls) {
        Copy-Item $dll.FullName (Join-Path $outDir $dll.Name) -Force
        Copy-Item $dll.FullName (Join-Path $deploy $dll.Name) -Force
    }

    $results += "$role : OK -> deploy\$($info.Out)\EntryPoint.exe"
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

# Restore the original (midware) toggle so the working tree is unchanged.
Set-Toggle "true" "false" "true"
Remove-Item (Join-Path $repo "CMakeCache.txt") -ErrorAction SilentlyContinue

Write-Host "==================== SUMMARY ====================" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host $_ }
Write-Host "Deploy folder: $deploy"
