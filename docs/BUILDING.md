# Building

How to get from source to four working programs.

**If you just want to run the system, you do not need to read this.** Run
`tools\build-deploy.ps1` and skip to [Tools](TOOLS.md). This page explains what that script does
and how to do it by hand when you need to.

---

## What you need

| Requirement | Notes |
|---|---|
| **Windows** | The only supported platform. There is no Linux build. |
| **Visual Studio 2022** | With the C++ desktop workload. MSVC, x64. |
| **CMake** | Generates the Visual Studio solution. |
| **.NET SDK** | Only for the launcher UI. Not needed to build the programs. |
| **Python 3** | Only for `analyse.py`. Standard library only — nothing to install. |

The language standard is C++20.

---

## The easy way

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1
```

This builds all four programs, copies them into `deploy/` in the layout the system expects, and
restores your original settings afterwards. It takes a few minutes.

**Use this unless you have a specific reason not to.** The manual path has a step that is easy to
get wrong and fails silently.

---

## The one build switch

There is exactly **one** setting that changes how the libraries are compiled, at the top of
`CMakeLists.txt`:

```cmake
set(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "true")
```

| Value | What you get |
|---|---|
| `"true"` | The three server programs: Manager, Midware, Game Server. |
| `"false"` | The Client, and the non-distributed demo build. |

**One configure builds all three server programs at once.** They share identical libraries and
differ only in a per-target setting, so you do not need to rebuild between them. Only the Client
needs a second configure, because it is the one that flips this switch.

| Switch | CMake target | Program |
|---|---|---|
| `"true"` | `EntryPointManager` | Manager |
| `"true"` | `EntryPointMidware` | Midware |
| `"true"` | `EntryPointServer` | Game Server |
| `"false"` | `EntryPoint` | Client |

> **If you find `CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER` or `CMAKE_BUILD_FOR_PHYSICS_MIDWARE`
> mentioned anywhere, that documentation is out of date.** Those settings were removed. They came
> from an older scheme that needed a full rebuild for each program — four rebuilds to produce four
> binaries, three of which were identical.

---

## Building by hand

### The three server programs

```powershell
# Make sure CMAKE_DISTRIBUTED_SYSTEM_ACTIVE is "true" in CMakeLists.txt, then:
Remove-Item CMakeCache.txt -ErrorAction SilentlyContinue
cmake -G "Visual Studio 17 2022" -A x64 .

$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer `
    /p:Configuration=Release /p:Platform=x64 /m
```

Note the backtick before each `;` — PowerShell needs it.

The binaries appear in `EntryPoint\Release\`.

### The Client

```powershell
# Change CMAKE_DISTRIBUTED_SYSTEM_ACTIVE to "false" first.
Remove-Item CMakeCache.txt -ErrorAction SilentlyContinue
cmake -G "Visual Studio 17 2022" -A x64 .
& $msb DistributedPhysicsSystem.sln /t:EntryPoint /p:Configuration=Release /p:Platform=x64 /m
```

**Change the switch back to `"true"` afterwards** or your next server build will silently produce
the wrong thing.

### Staging into `deploy/`

The Midware launches game servers using a path relative to its own folder, so the layout matters:

```
deploy/Manager/EntryPoint.exe                   <- EntryPointManager.exe
deploy/Midware/EntryPoint.exe                   <- EntryPointMidware.exe
deploy/DistributedPhysicsServer/EntryPoint.exe  <- EntryPointServer.exe
deploy/Client/EntryPoint.exe                    <- EntryPoint.exe
```

Every file is renamed to `EntryPoint.exe`. This is what `build-deploy.ps1` automates, and getting
it wrong produces a "midware connects but no servers appear" failure with no error message.

---

## Building the tests

```powershell
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64
.\tools\InteractionTests\Release\InteractionTests.exe
```

Note the `Tools\` prefix — the target lives in a solution folder. A non-zero exit code means
failures.

---

## Common build problems

| Problem | Fix |
|---|---|
| CMake changes seem to have no effect | Delete `CMakeCache.txt` and configure again. CMake caches aggressively. |
| `msbuild` not found | It is not on PATH by default. Use the full path shown above, or open a Developer Command Prompt. |
| Built fine, but the system behaves as before | You did not copy into `deploy/`. Re-run `build-deploy.ps1`. |
| Copy fails, "file in use" | A previous run is still going. Stop the processes and try again. |
| Server build produces the Client | The switch is `"false"`. Set it back to `"true"`. |

---

## Adding a new source file

Add it to the relevant `CMake*.cmake` file in the module, **not just to disk**. CMake will not
find it otherwise, and the failure appears as a confusing link error rather than a missing file.

---

## Where the assets come from

Asset paths are compiled into the binaries when you configure, pointing at `Assets/`. If you move
the repository after building, re-configure.
