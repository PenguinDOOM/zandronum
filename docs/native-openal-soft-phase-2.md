# Native OpenAL Soft: Phase 2A-1

## Scope

This document describes the current 2A-1 capability and status layer. It does
not claim that the later HRTF, EFX, radius, underwater, or Doppler work is
implemented. The existing FMOD backend, `snd_backend` selection, fallback
behavior, audio feature activation, CVAR defaults, and compatibility behavior
remain unchanged.

2A-1 takes one capability snapshot after the OpenAL context is current. The
snapshot is cached and is used by `snd_status` and the concise renderer stats;
capabilities are not queried every frame.

## What The Status Means

The status output keeps these states separate:

- **Advertised:** the device reports the extension or capability.
- **Callable:** the required function entry points were resolved.
- **Operational / usable:** the capability has passed the checks needed for
  actual use. In 2A-1 this is not an activation step.
- **Applied:** the renderer has applied the feature to runtime audio objects.

2A-1 reports capability information only. It does not create EFX effects,
auxiliary sends, or filters; it does not query EFX send limits; and it does not
apply any EFX path. Consequently, EFX may be reported as advertised/callable
while sends remain `not queried`, usable remains false, and applied remains
false. Radius may be advertised but remains unapplied. Doppler remains
unapplied with the OpenAL Doppler factor at `0`; the existing manual rolloff
and `AL_NONE` distance model remain in force.

HRTF status is a runtime status query, not proof that 2A-1 requested HRTF or
changed `snd_hrtf`. Context attributes are not changed here. OpenAL Soft's
runtime default may therefore report HRTF enabled even though 2A-1 did not
request it. Known numeric statuses are preserved, including unknown values.

The implementation does not add HRTF profile selection, EFX objects or sends,
water filtering/reverb, source radius application, Doppler motion, or new
activation/fallback rules. Those are later work and are not enabled by this
unit.

## Runtime Identity

The validated Windows build used MSVC Visual Studio 17 2022, x64, Release,
with the vcpkg OpenAL headers and library:

```text
C:/Users/Penguin/vcpkg/installed/x64-windows/include
C:/Users/Penguin/vcpkg/installed/x64-windows/lib/OpenAL32.lib
```

The observed runtime identity was:

```text
AL_VERSION = 1.1 ALSOFT
```

This is the runtime string from the local vcpkg-backed build; it must not be
described as a fixed OpenAL Soft 1.25.2 runtime. The Windows CI dependency pin
at OpenAL Soft 1.25.2 is a separate provenance constraint. Linux distro and
local vcpkg OpenAL versions are not thereby fixed to that CI pin.

The product CMake target is `zdoom`; its Release output is
`zandronum.exe`. Do not substitute a nominal `zandronum` target in the build
command.

## Reproducible Checks

From the repository root, configure or reuse the v143 build tree, then build
the product and lifecycle test target:

```powershell
cmake --build build-v143 --config Release --target zdoom openal_lifecycle_tests
ctest --test-dir build-v143 -C Release --output-on-failure -R "^(openal_phase2_unit|openal_lifecycle)$"
```

The focused CTest run recorded `2/2` tests passed. The device-free unit case
is `openal_phase2_unit`, which runs:

```powershell
build-v143\tools\Release\openal_lifecycle_tests.exe --phase2-unit-only
```

For an actual OpenAL context and status snapshot, run:

```powershell
build-v143\tools\Release\openal_lifecycle_tests.exe --phase2-status
```

The recorded status included `OpenAL Soft`, `1.1 ALSOFT`, HRTF status
`enabled (1)`, EFX `advertised/callable`, sends `not-queried`, and radius
`advertised`. `--phase2-status` requires a usable device/context. Exit code
77 means that no OpenAL device/context was available and the run was skipped;
it is not a successful listening or runtime-activation result.

## Manual Baseline Procedure

The FMOD portion of this baseline was completed with the current isolated
Release executable (`zandronum.exe` SHA256
`7981E41195CAA0BBA8641F299381ED27A6E94EB1F6ABD54E2A7FB11C2E620553`), not the
older phase-6 executable. The user confirmed that `snd_status` displayed FMOD,
MAP01 music was audible, and weapon sound effects were audible. This is human
listening evidence, not AI audio verification. The optional OpenAL follow-up
procedure remains: select the OpenAL backend, run `snd_reset`, play a short
sound effect and local music, and use `snd_status` to capture the backend
identity. OpenAL-backend listening was not performed in this session.

The running FMOD session loaded `resume/runtime/fmodex64.dll` (PE version
`4.44.64`, SHA256
`DA233648ED16DFC0C109447784E55995F8A11156D9C9E7C0F1FEB8E6B523473C`) and
`resume/runtime/OpenAL32.dll` (SHA256
`2C44AE1108904B708BDC370EF8703785912BFEE67BB4999ED8D25C28250628A9`; PE
version fields empty). Loading the regular OpenAL-linked module during the FMOD
session does not prove that the OpenAL backend or HRTF is active. The OpenAL
exact version is not established by this evidence; do not claim a pinned
OpenAL Soft 1.25.2 runtime.

Do not treat this baseline as evidence that HRTF, EFX, radius, water processing,
or Doppler is active. Lizard raw19, focused CTest raw18 (`2/2` passed), product
raw28, and the full Cppcheck run `run20260918-014804...` passed with `new=0`
and `unresolved=0`; the Cppcheck result was independently approved after the
G1 fixes and configuration equivalence check. The original eight source hashes
matched the immutable snapshot taken before this documentation edit. Source
code is unchanged; this edit invalidates only the captured whole-document hash,
not the C++ checks.
Codacy local analysis is not reported here: the Windows-native MCP path is
unsupported, while the WSL CLI currently lacks the repository configuration.
No setup, reset, or installation was performed for that analysis.

## Deferred Work

Later units may request HRTF context attributes and move the shared `snd_hrtf`
boundary, create and apply EFX resources, add water processing, apply radius,
or calibrate and enable source-only Doppler. None of those enhancements is
part of the current operational state.