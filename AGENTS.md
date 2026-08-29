# AGENTS.md

## Project Overview

This repository is a fork of Zandronum based on:

* Zandronum 3.3-alpha-r260112-1855
* Git mirror base commit: `c2e5dbb0dee388cde20eeef964eca2dc11453105`

The primary goal of this fork is to add a native OpenAL Soft audio backend while preserving compatibility with stock Zandronum wherever possible.

Later work may include:

* OpenAL Soft HRTF, EFX, Doppler, and source-radius support
* BSP-aware occlusion, diffraction, and reflection
* Carefully isolated multithreading
* Refactoring voice playback/capture away from direct FMOD coupling

Do not broaden the scope of a task without explicit justification.

---

## Compatibility Baseline

Stock Zandronum compatibility is a primary project constraint.

The reference network-compatible build is:

`3.3-alpha-r260112-1855`

Reference revision:

`1768244130`

Reference `NETGAMEVERSION`:

`162 (0xA2)`

Unless a task explicitly requires a protocol break, preserve compatibility with the reference build.

### Do Not Change Without Explicit Approval

Do not modify any of the following merely as part of audio, rendering, tooling, cleanup, or refactoring work:

* Network protocol commands
* Network serialization formats
* Gameplay simulation semantics
* Demo protocol
* Authentication behavior
* Protected lump contents
* Zstd network dictionary
* `NETGAME_COMPAT_REVISION`
* Stock-server compatibility assumptions

Changes to these areas require explicit review of network compatibility consequences.

---

## Revision Handling

Build provenance and network compatibility are separate concepts.

The revision generator may derive the actual build revision from Mercurial or Git.

The actual Git/Hg revision must not implicitly determine network compatibility for this fork.

Network compatibility remains pinned to the explicitly selected compatibility revision until intentionally changed.

Do not fake or overwrite the current Git build revision merely to make the executable appear identical to a stock build.

---

## `zandronum.pk3`

Development executables intended to connect to stock servers should use the stock `zandronum.pk3` from the matching reference Zandronum build unless a task explicitly concerns PK3 contents.

Do not modify `zandronum.pk3` as part of native audio backend work.

Generated PK3 files may differ in authenticated lump hashes even when generated from closely matching source.

---

## Windows Build Environment

Primary supported development environment:

* Visual Studio 2022
* MSVC v143
* x64
* Current Windows 10/11 SDK
* CMake

Reference configuration:

```cmd
cmake -S . -B build-v143 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -T v143
```

Reference build:

```cmd
cmake --build build-v143 --config Release
```

Do not reintroduce the legacy VS2015 / `v140_xp` / Windows SDK 7.1A toolchain unless explicitly requested.

---

## Architecture Rules

### Preserve Existing Backends

Do not remove FMOD while implementing OpenAL Soft.

The intended architecture is initially:

```text
Sound system
├─ FMOD
└─ OpenAL Soft
```

OpenAL Soft should be implemented as a native backend rather than by restoring the historical FMOD-to-OpenAL output path.

### Audio Boundary

Prefer an explicit state-copy boundary between game state and the audio backend.

Conceptually:

```text
Game state
    ↓
Listener / source state
    ↓
Audio backend
```

Avoid allowing the audio backend to mutate gameplay state.

### Threading

Gameplay/world state should remain single-writer unless a task explicitly establishes a safe alternative.

Worker threads may calculate derived data using immutable or copied state.

OpenAL context ownership should remain on one designated audio thread or owning thread.

Do not issue arbitrary OpenAL calls concurrently from worker threads.

### Voice Chat

Voice chat is not part of the initial OpenAL backend milestone.

Avoid expanding OpenAL implementation work into microphone capture or full voice-chat refactoring unless explicitly requested.

---

## Legacy Code Policy

This repository contains a large amount of legacy code.

Do not perform unrelated cleanup solely because a file is old, unusually styled, large, or fails a newly introduced quality metric.

When editing legacy code:

* Keep changes narrowly scoped.
* Follow surrounding style unless there is a strong reason not to.
* Avoid formatting unrelated lines.
* Avoid mass renames.
* Avoid replacing working legacy constructs merely with newer C++ equivalents.
* Preserve observable behavior unless behavioral change is part of the task.

Lint failures that predate the current change should normally be treated as baseline technical debt rather than automatically fixed.

---

## Static Analysis

The repository uses:

* Lizard for complexity and function-size analysis
* Cppcheck for C/C++ static analysis

Static-analysis tools are advisory for existing legacy code and enforceable for newly introduced regressions.

Do not silence a legitimate warning merely to make a check pass.

Do not add broad suppressions without documenting why the warning is false or unavoidable.

### Lizard

Use Lizard primarily to detect newly introduced excessive complexity.

When practical, new functions should remain:

* Cyclomatic complexity: below 20
* Function NLOC: below 80

Smaller and simpler functions are preferred where decomposition improves clarity.

Do not split functions mechanically when doing so would make control flow harder to understand.

### Cppcheck

Cppcheck should use the generated Visual Studio project/solution where possible so that actual project configuration, defines, and include paths are available.

Recommended categories:

* error
* warning
* performance
* portability

Style findings may be reviewed separately because the upstream codebase contains substantial legacy style debt.

False-positive suppressions should be as narrow as practical.

---

## Validation Requirements

Before considering a change complete, run the checks relevant to that change.

For ordinary C/C++ changes:

1. Build the affected target.
2. Run applicable Lizard checks.
3. Run applicable Cppcheck checks.
4. Check for unintended generated-file changes.

For networking-sensitive changes, additionally verify compatibility implications explicitly.

For audio backend work, test both the existing FMOD path and the OpenAL path when both are affected.

---

## Git Policy

Keep commits focused on one logical concern.

Do not combine:

* revision infrastructure changes
* OpenAL backend implementation
* unrelated cleanup
* network protocol changes

into a single commit.

Do not commit generated build directories or analyzer caches.

Do not bypass a failing hook simply to commit unless the failure is understood and explicitly justified.

---

## Agent Behavior

Agents may without asking:

* Read and search source files
* Inspect history
* Run builds
* Run tests
* Run Lizard
* Run Cppcheck
* Investigate warnings
* Make narrowly scoped changes required by the requested task

Agents should avoid without explicit justification:

* Protocol-breaking changes
* Large unrelated refactors
* Replacing established subsystems wholesale
* Adding unnecessary dependencies
* Modifying stock compatibility data
* Modifying authenticated PK3 content
* Broad warning suppressions
* Disabling lint or tests to obtain a passing result
* Stage files included in .gitignore

When a task reveals a pre-existing defect outside its scope, document it rather than silently expanding the task.
