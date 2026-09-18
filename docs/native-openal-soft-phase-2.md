# Native OpenAL Soft: Phase 2A-2

## Scope

This document describes the current 2A-2 capability, HRTF request, and status
layer. EFX, radius, underwater, and Doppler activation remain outside this
unit. The existing FMOD backend, `snd_backend` selection, fallback behavior,
audio feature activation, CVAR defaults, and compatibility behavior remain
unchanged.

2A-2 takes one capability snapshot after the OpenAL context is current. The
snapshot is cached and is used by `snd_status` and the concise renderer stats;
capabilities are not queried every frame. `snd_hrtf` is owned by
`i_sound.cpp`, remains a Bool with default `false`, and retains archive/global
configuration without a callback. FMOD consumes it through `EXTERN_CVAR`.

## What The Status Means

The status output keeps these states separate:

- **Advertised:** the device reports the extension or capability.
- **Callable:** the required function entry points were resolved.
- **Operational / usable:** the capability has passed the checks needed for
  actual use. In 2A-1 this is not an activation step.
- **Applied:** the renderer has applied the feature to runtime audio objects.

2A-2 reports capability information and the initial HRTF request state. It does not create EFX effects,
auxiliary sends, or filters; it does not query EFX send limits; and it does not
apply any EFX path. Consequently, EFX may be reported as advertised/callable
while sends remain `not queried`, usable remains false, and applied remains
false. Radius may be advertised but remains unapplied. Doppler remains
unapplied with the OpenAL Doppler factor at `0`; the existing manual rolloff
and `AL_NONE` distance model remain in force.

The HRTF fields are distinct: the initialization request is the `ALC_HRTF_SOFT`
attribute derived from the `snd_hrtf` value captured at context creation, the
current CVAR is the later user setting, actual active state is the independent
`ALC_HRTF_SOFT` query with its own known/unknown flag, status is the independent
`ALC_HRTF_STATUS_SOFT` query with its own known/unknown flag and numeric reason,
and the specifier is the separate `ALC_HRTF_SPECIFIER_SOFT` string
(`0x1995`). The implementation queries the opened `ALCdevice`, not a null
device. A query failure reports `unknown`, not `off`; an unknown numeric status
also does not imply inactive. The previous enabled-only inference was incorrect:
statuses such as `required` (3) and `headphones-detected` (4) can coexist with
active HRTF, so the manual active gate uses only a known actual-active result.

The status output separates the request captured at initialization from the
current `snd_hrtf` value. Changing the CVAR after initialization is therefore
pending until the existing `snd_reset` recreation boundary is used.

If an optional-attribute context cannot be created or made current, the
failed context/device is closed and a new device/context is attempted once
without the optional HRTF attributes. The status records the retry disposition
(`attribute context creation failed; retried without attributes` or
`attribute context current activation failed; retried without attributes`). If
the basic context also fails, the existing FMOD fallback remains the boundary.
This retry does not prove that HRTF is active.

The implementation does not add HRTF profile selection, live context reset,
EFX objects or sends,
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
described as a fixed OpenAL Soft 1.25.2 runtime. The local executable's PE
version fields are empty. The Windows CI dependency pin at OpenAL Soft 1.25.2
is a separate provenance constraint. Linux distro and local vcpkg OpenAL
versions are not thereby fixed to that CI pin.

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

The earlier 2A-1 status record included `OpenAL Soft`, `1.1 ALSOFT`, HRTF
status `enabled (1)`, EFX `advertised/callable`, sends `not-queried`, and
radius `advertised`; it is historical capability evidence, not proof of the
new 2A-2 request or actual-active state. `--phase2-status` requires a usable
device/context.
Exit code 77 means that no OpenAL device/context was available and the run was
skipped; it is not a successful listening or runtime-activation result.

The focused CTest gate completed successfully (`2/2`) before the final
test-only counter initialization. The parent 2A-2 Release product/test gate
also completed successfully: the Release `zdoom` product and test builds
passed, and the post-constructor Release test rebuild plus
`--phase2-unit-only` passed. The corresponding NO_SOUND and SERVERONLY build
logs also recorded exit code 0. Earlier Debug tests are separate historical
evidence. These checks establish build/lifecycle behavior, not human HRTF
listening results.

## Manual Baseline Procedure

The FMOD listening baseline was completed historically during 2A-1 with an
isolated
Release executable (`zandronum.exe` SHA256
`7981E41195CAA0BBA8641F299381ED27A6E94EB1F6ABD54E2A7FB11C2E620553`), not the
older phase-6 executable. The user confirmed that `snd_status` displayed FMOD,
MAP01 music was audible, and weapon sound effects were audible. This is human
listening evidence, not AI audio verification. In the 2A-2 manual listening
feedback, left, right, and front matched the expected directions (M01, M02,
and M03). Back, up, and down (M04, M05, and M06) were difficult to distinguish;
the feedback did not establish that they were reversed, and this was not a
localization pass. Individual acoustic variation is expected, so these results
are listening observations and do not justify sound tuning.

The user also confirmed that the FMOD false/true reset checks displayed FMOD
and that music and SFX were audible. This retains the user-verified FMOD 2A-2
result only; the current fix did not change `fmod` or `i_sound` behavior. The
earlier bug run is not treated as the final pass.

The running FMOD session loaded `resume/runtime/fmodex64.dll` (PE version
`4.44.64`, SHA256
`DA233648ED16DFC0C109447784E55995F8A11156D9C9E7C0F1FEB8E6B523473C`) and
`resume/runtime/OpenAL32.dll` (SHA256
`2C44AE1108904B708BDC370EF8703785912BFEE67BB4999ED8D25C28250628A9`; PE
version fields empty). Loading the regular OpenAL-linked module during the FMOD
session does not prove that the OpenAL backend or HRTF is active. The OpenAL
exact version is not established by this evidence; do not claim a pinned
OpenAL Soft 1.25.2 runtime.

The latest active-query fix keeps the actual-active query and status query
independent, and reports their knownness separately. The earlier human
verification used the corrected isolated Release executable SHA256
`26A5F881614B300A443D5856BB2254302EC6715A33FC7F78A3CFDDFEB4E70002`.
The latest real active-query diagnostic probes instead used the product build
SHA256 `DFE15C7FEA0C591D19B60D2FB0517A3F907911630909A130B067A0F33A7A4228`;
the test CLI performed those probes, rather than the product executable
performing the CLI operation. The same OpenAL32.dll was used in the checked runtime
(SHA256 `2C44AE1108904B708BDC370EF8703785912BFEE67BB4999ED8D25C28250628A9`).
The focused Release probes recorded `attributes=1, request=0,
active-known=1, active=0, status=0` for OFF and `attributes=1, request=1,
active-known=1, active=1, status=1, specifier=SADIE_D02-48000` for ON. The
selected HRTF name is an observed specifier, not an OpenAL library version; an
exact OpenAL Soft 1.25.2 runtime has not been established.

The prior human verification of the corrected OpenAL runtime covered the requested three
stages: OFF, `snd_reset` to ON, and `snd_reset` back to OFF. The expected
state transitions were correct, and music plus weapon SFX were audible in both
states. This is human listening evidence, not AI audio verification. The
latest active-query probe did not require repeating that listening procedure;
the prior OFF/ON/OFF and music/SFX evidence remains valid. The
earlier directional check still supports left, right, and front; back, up, and
down were difficult to distinguish. It does not establish that all six
directions are clear or require a correction for individual listening
variation.

Do not treat this baseline as evidence that EFX, radius, water processing, or
Doppler is active. The final Cppcheck evidence for the corrected source passed
with exit code 0, `New=0`, and `UnresolvedVendor=0`. The final Release and
NO_SOUND/SERVERONLY records also have valid exit-code-0 results. Historical 2A-1
analyzer and source-hash records remain historical phase evidence and are not
presented as new 2A-2 measurements. Earlier automatic NO_SOUND and SERVERONLY
checks passed independently and are unaffected by the OpenAL-only change; they
are not human listening evidence.
Codacy local analysis is not reported here: the Windows-native MCP path is
unsupported, while the WSL CLI currently lacks the repository configuration.
No setup, reset, or installation was performed for that analysis.

## Deferred Work

Later units may add HRTF profile selection or live context reset, create and
apply EFX resources, add water processing, apply radius, or calibrate and
enable source-only Doppler. The existing `snd_reset` path recreates the
renderer and is the current HRTF application boundary; live
`alcResetDeviceSOFT` is not implemented.
