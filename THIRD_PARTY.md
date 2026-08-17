# Third-party components

This repository vendors several third-party libraries in-tree. This file records what they are and
under what terms, so the licensing position is checkable without reading every header.

The project's own code is MIT (see `LICENSE`). Note the copyright holder: the engine this work is
built on is Newcastle University coursework code by Richard Davison, and the MIT grant covers it.
Engine source files additionally carry the header *"Part of Newcastle University's Game Engineering
source code. Use as you see fit!"*

| Component | Location | Licence | Redistributable here? |
|---|---|---|---|
| ENet | `CSC8503CoreClasses/enet/` | MIT | Yes — **licence text not vendored, see below** |
| RecastNavigation (Recast, Detour, DetourTileCache, DebugUtils) | `Recast/`, `Detour/`, `DetourTileCache/`, `DebugUtils/` | zlib | Yes |
| Dear ImGui v1.91.1-WIP | `CSC8503CoreClasses/imgui/` | MIT | Yes |
| glad 2.0.0-beta (generated loader) | `OpenGLRendering/glad/` | Public domain / MIT (generator output) | Yes |
| **FMOD Core API** | `FMODCoreAPI/` | **Proprietary — Firelight Technologies Pty Ltd** | **No — see below** |
| `Assets/` (260 files: 162 png, 31 tga, 43 shaders, 10 spv, 2 msh) | `Assets/` | **Provenance not established** | **Unknown — see below** |

## Open items

These are recorded rather than resolved. Both matter for a public repository and for ACM artifact
evaluation, and both are the author's decision, not a mechanical fix.

### 1. FMOD binaries are committed (highest priority)

`git ls-files` shows **15 tracked FMOD files**, including compiled libraries:

```
FMODCoreAPI/dlls/fmod.dll        FMODCoreAPI/libs/fmod_vc.lib
FMODCoreAPI/dlls/fmodL.dll       FMODCoreAPI/libs/fmodL_vc.lib
FMODCoreAPI/includes/*.h, *.cs   (11 header files)
```

FMOD is proprietary (`Copyright (c), Firelight Technologies Pty, Ltd. 2004-2023`). Redistributing
its headers and binaries in a **public** repository is governed by the FMOD licence, not by this
project's MIT grant. FMOD's free tier permits use under conditions including attribution, but
redistribution of the SDK itself is a separate question that should be checked against the current
licence terms before the repository is cited in a paper.

Relevant mitigation already in place: FMOD is **not used by any distributed role**. The include,
link and DLL-copy steps live only in `CSC8503/` and `EntryPoint/` CMake, and `SoundObject` is inside
`#ifndef DISTRIBUTEDSYSTEMACTIVE`. The three server roles and the headless client build and run
without it. So removing it from version control would not affect the system this dissertation is
about — only the demo-assessment build.

**Options:** (a) confirm redistribution is permitted and add the required attribution; (b) untrack
`FMODCoreAPI/` and document it as a developer-supplied prerequisite; (c) drop the demo audio path.

### 2. `Assets/` provenance is not established

260 tracked files. These came with the Newcastle coursework framework and the removed team game; the
`MaleGuard` and `Security_Camera` textures are leftovers from gameplay that no longer exists. Which
are Newcastle-supplied, which are third-party, and under what terms, is not recorded anywhere.

Assets are **not needed by the distributed system** — the servers are headless and the default
client renderer uses a single untextured cube mesh. `ASSETROOTLOCATION` is baked in at configure
time for the demo build only.

**Options:** (a) establish provenance for what is kept; (b) prune to the minimum the client actually
loads and document that set; (c) drop `Assets/` from the artifact and ship the headless
configuration only.

### 3. ENet carries no copyright notice at all

Vendored version is **1.3.13** (`ENET_VERSION_MAJOR/MINOR/PATCH` in `enet.h`). A grep for
`copyright` and for the upstream author's name across `CSC8503CoreClasses/enet/` returns **nothing**
— there is no licence file and no per-file notice.

This is slightly worse than a missing `LICENSE` file: MIT requires the copyright notice *and* the
permission notice to accompany the software, and neither is currently present anywhere in the
vendored copy. The fix is to copy the `LICENSE` from the upstream ENet 1.3.13 distribution verbatim
into `CSC8503CoreClasses/enet/`.

Deliberately not reconstructed here from memory: an incorrect copyright line in a licence file is a
worse defect than an absent one, so the text must be taken from the upstream release rather than
retyped.

## Note on artifact evaluation

ACM "Artifacts Available" needs an archival DOI (Zenodo or similar) — a GitHub URL alone does not
qualify. Items 1 and 2 should be settled before minting one, because an archived snapshot cannot be
retracted.
