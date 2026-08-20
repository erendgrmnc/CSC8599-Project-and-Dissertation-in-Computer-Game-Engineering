# Shared path handling for the measurement scripts.
#
# This lives in one file because the bug it prevents was caused by it living in two:
# measure.ps1 was fixed to anchor a relative -OutDir and run-experiments.ps1 was not,
# so the same silent metric loss stayed reachable through the other entry point.

function Test-PathFullyQualified {
    param([Parameter(Mandatory = $true)][string]$Path)

    # .NET Framework 4.x (Windows PowerShell 5.1) has no Path.IsPathFullyQualified, and
    # Path.IsPathRooted is not a substitute: it returns true for "C:runs" and for
    # "\runs", both of which are still resolved against a current directory - "C:runs"
    # against whatever the current directory on drive C happens to be. Treating either
    # as absolute passes it through unchanged, which is the silent loss this helper
    # exists to prevent.
    #
    # Written with single-character comparisons rather than a regex because a path
    # separator is a backslash and every layer between here and the file - the shell
    # heredoc, the regex engine's escape rules - gets a vote on how many of them
    # survive. One backslash per literal has no such ambiguity.
    if ($Path.Length -lt 2) {
        return $false
    }

    $first = $Path[0]
    $second = $Path[1]
    $firstIsSep = ($first -eq '\') -or ($first -eq '/')

    # UNC, e.g. \\server\share.
    if ($firstIsSep -and (($second -eq '\') -or ($second -eq '/'))) {
        return $true
    }

    # Drive-qualified, e.g. C:\runs. The separator is the whole point: "C:runs" names a
    # drive but not a directory on it.
    if ($Path.Length -ge 3 -and ($first -match '^[A-Za-z]$') -and $second -eq ':') {
        $third = $Path[2]
        if (($third -eq '\') -or ($third -eq '/')) {
            return $true
        }
    }

    return $false
}

function Resolve-RunOutDir {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$OutDir,
        [Parameter(Mandatory = $true)][string]$RepoRoot
    )

    # A relative -OutDir anchors to the repo root, not to the caller's current
    # directory. The metrics directory is derived from this and handed to the game
    # servers via --metrics-dir; the midware spawns them with lpCurrentDirectory =
    # nullptr (PhysicsServerMidware/ServerMidwareManager.cpp:185), so they inherit
    # deploy/ as their working directory rather than wherever the script was invoked
    # from. A relative metrics path then resolves against deploy/ and silently fails to
    # open the CSV - the run still exits clean and prints @@FINAL, so nothing else
    # about it flags the lost metrics.
    if ([string]::IsNullOrWhiteSpace($OutDir)) {
        return (Join-Path $RepoRoot "runs")
    }
    if (Test-PathFullyQualified $OutDir) {
        return $OutDir
    }
    return (Join-Path $RepoRoot $OutDir)
}
