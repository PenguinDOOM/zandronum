# Native OpenAL Soft: Phase 2B-2

## Scope

This document describes the implemented 2B-1 shared environment state and
numeric adapter together with the 2B-2 OpenAL EFX environment routing. The
existing FMOD backend, `snd_backend` selection, fallback behavior, music
selection, CVAR defaults, compatibility behavior, `AL_NONE` distance model,
and manual rolloff remain unchanged.

2B-1 owns one EFX effect, one auxiliary slot, and the filter capability for the
current OpenAL context. It requests one auxiliary send and records the actual
`ALC_MAX_AUXILIARY_SENDS` value, including zero; `unknown` is reserved for a
failed query. EFX resource availability is independent from filter availability.
Initialization attempts EAX reverb first, then standard reverb, then dry
operation; a failed optional path does not stop ordinary OpenAL playback.
Resource cleanup is tracked for partial allocation failures and remains within
the owning context.

2B-2 adds source send routing on top of those resources. Water pitch/low-pass,
virtual-position callbacks, source radius, and Doppler remain later work.

## Shared Environment State

`ForcedEnvironment` has one non-owning definition in the environment owner;
FMOD and OpenAL consume the shared declaration. New environments initialize
`Modified` to true and `SoftwareWater` to false. Flag edits and Revert notify
the same modification state as numeric edits, and replacing or unloading an
environment clears a forced reference before the object is removed. The
environment structure layout, parser ranges, presets, and saved format are
unchanged.

The OpenAL adapter consumes a snapshot. It does not write converted values back
to the shared environment. `ForcedEnvironment` is the sole definition of the
environment owner; there is no per-backend duplicate state.

## Numeric Mapping

Millibel gains use:

$$g(m)=10^{m/2000}.$$

Room, reflections, and late reverb are separate gain fields; Room is not
pre-added to either later gain. Density and Diffusion are the existing SFX
percent values divided by 100. Each target is clamped to its EFX range. NaN
uses the target default, infinities clamp by sign, and non-finite pan becomes
the zero vector.

Pan remains listener-relative XYZ. Components are not independently clamped;
vectors with length greater than one are normalized while preserving direction.
The source coordinate transform, 96-unit distance conversion, and any source
Z reversal are not applied to reverb pan. `EnvSize` is not rescaled into
Density, and size-history flags do not alter time or level for a complete
snapshot. Only reverb flag bit `0x20` maps to the HF decay limit. Unsupported
standard-reverb tokens are not emitted.

`EchoTime` and `EchoDepth` are retained and queryable as API values. In the
validated runtime they are not evidence of an acoustic echo DSP path. The
OpenAL API identity observed locally is `1.1 ALSOFT`; OpenAL Soft 1.25.2 is a
dependency pin in the planned CI provenance, not a proven local runtime
version.

## Capability and Status Semantics

Status keeps these states distinct:

- **Advertised:** the context reports the extension.
- **Callable:** required entry points were resolved.
- **Usable:** the required resource/type operation succeeded.
- **Applied:** runtime source routing has attached the feature.

In 2B-1, EAX and standard reverb application are tested during resource
initialization, while source sends are still unapplied. A device may therefore
report a usable effect resource without claiming that SFX are routed through
it. The status output uses `EAX`, `standard`, or `dry` for reverb selection;
`available`, `failed`, or `absent` for the independent filter state; and `yes`
or `no` for applied routing. A filter can be usable independently of the
reverb result. If both reverb types fail, dry playback remains available. The
lifecycle tests also cover zero sends, missing filter entry points, both
reverb types being rejected, and auxiliary-slot failure. An absent EFX
extension or unavailable device is an early capability path, not a successful
source-routing test. Non-finite reverb pan is converted to zero and warns at
most once per renderer/context.

## Deferred Boundaries

The following are intentionally outside 2B-2:

- `snd_waterlp` pitch/low-pass behavior, `SoftwareWater`, and virtual position
  state (2B-3); and
- source radius and Doppler behavior (2C/2D).

No full acoustic equivalence with FMOD is claimed. In particular, the legacy
water wet graph, Q=2 behavior, wet-tail filtering, listener velocity, and
additional environment routing are not active in this stage.

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

The 2B-1 mapper/query fixture run covered `Off`, `Generic`, `Strong`, and
`Short`: four inputs, eight EAX/standard applications, and 144 parameter
queries. The expected mapper values matched the queried values, including
gain conversion, percentage density/diffusion, range clamping, pan handling,
and the `0x20` flag. The device-free run is `--phase2-unit-only`; the
`--phase2-efx-query` run performs `alGetEffect` queries against an actual
OpenAL device/context. Exit code 77 means that no device/context was
available and is a skip, not a pass. A real-device `--phase2-status` run also
passed and reported `EFX sends: 1`,
`reverb: EAX`, `filter: available`, and `applied: no`.

The Release product/test build and focused CTest records are retained as
validation evidence. Resource-owner fault-injection unit tests also passed.

For 2B-2, the valid Release product and lifecycle-test build, focused CTest
(`2/2`), and direct routing, continuing-source, and error-handling runs are
retained and reused; they were not rerun for the manual resumption. These
automated assertions cover valid-listener forced/listener/Off selection,
modified/retry behavior, 2D `SNDF_NOREVERB`, a wet 3D source,
`SNDF_NOPAUSE`, dry music, source reuse, and failure recovery through the
product setter and error observations described below. The current test file
does not assert invalid-listener hold or a 3D `SNDF_NOREVERB` source; those
remain code-level behavior, not claimed test coverage.

The current full Cppcheck capture uses the review3-fix working-tree manifest
`completes/native-openal-soft-phase-2/2b-2/cppcheck-input/review3-fix-working-tree-capture-88dcd6fcff83fd3ce5cd8d11e7c00cfdf7697874/manifest.json`
for commit `88dcd6fcff83fd3ce5cd8d11e7c00cfdf7697874`. Today's parent
verification matched the HEAD, all four working-tree hashes, and all four
payload hashes. The completed full result is retained at
`completes/native-openal-soft-phase-2/2b-2/cppcheck-full/run-20260921-032239-3a718c294ede40528a843c3d6811d1e4/final-result.json`:
Cppcheck passed with exit code 0, `Raw=601078`, `Baseline=7503`, `New=0`, and
`UnresolvedVendor=0`. The corresponding raw log ended with zero new
diagnostics, `Cppcheck passed`, `END`, and `EXIT_CODE0`. The formal parent
review3 Lizard result also passed with exit code 0 and
`REAL_INDEX_UNCHANGED=True`. These are verified facts for the current review3
input. Independent round 4 is approved/green, while the commit remains
pending, so this document makes no overall Phase 2 completion claim. The
earlier second-narrowrepair Cppcheck result
and parent Lizard result are historical evidence only; the earlier B1 raw
fixture and query evidence remains valid historical B1 evidence and is not
recast as B2 routing evidence.

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
Doppler is active. The final Release and
NO_SOUND/SERVERONLY records also have valid exit-code-0 results. Historical 2A-1
analyzer and source-hash records remain historical phase evidence and are not
presented as new 2A-2 measurements. Earlier automatic NO_SOUND and SERVERONLY
checks passed independently and are unaffected by the OpenAL-only change; they
are not human listening evidence.
Codacy local analysis is not reported here: the Windows-native MCP path is
unsupported, while the WSL CLI currently lacks the repository configuration.
No setup, reset, or installation was performed for that analysis.

## Environment Routing

For a valid listener, the selected environment is `ForcedEnvironment`, then
the listener environment, then `DefaultEnvironments[0]` (`Off`). An invalid or
missing listener leaves the current environment and source assignments
unchanged; this is verified by the code path but is not covered by the current
lifecycle assertions. `LastAttemptedEnvironment` and `LastAppliedEnvironment`
are kept separate: the former records the selection attempt, while the latter
changes only after the effect parameters and all active source sends succeed.

An environment's `Modified` flag is consumed when its selection is attempted.
An unchanged repeat is not retried; editing the environment, selecting another
environment, or resetting the renderer permits a new attempt. A failed effect
or source-send operation globally latches the renderer to dry routing, records
a failure reason, detaches all sends, and does not mix partially applied
sources. A wet-send failure is checked immediately rather than swallowed. The
failure path is non-reentrant: a persistent EFX environment-drain failure
remains latched until reset or re-selection, while an active source that cannot
be detached is stopped and retired after the finite backend-error cleanup path.
A source that can be made dry remains active. History offsets after failure and
persistent reset/detach behavior are covered by the lifecycle tests.
Re-editing or reselection is the recovery boundary.

Normal 2D and 3D sources use the selected wet environment except when the
selected environment is `Off` or the source has `SNDF_NOREVERB`. `SNDF_NOPAUSE`
does not make a source dry; it remains wet-eligible. Music, encoded streams,
callbacks, and software-generated music remain dry and have no EFX send or
filter. New and reused sources and streams are explicitly reset to dry, with
air absorption, automatic send/filter gain correction, and the direct filter
reset to their dry defaults; the manual source gain is applied once. When the
radius capability is advertised, source radius is capability-guardedly reset
to zero; 2B-2 does not activate radius behavior. `RoomRolloffFactor` is
retained as an EFX
parameter, but it does not reproduce FMOD's distance attenuation in this
`AL_NONE` plus manual-rolloff arrangement. No water processing, virtual-radius
handling, or Doppler is enabled by 2B-2.

The routing tests observe product-side source setter calls and OpenAL error
results, together with runtime lifecycle behavior. OpenAL Soft rejects the
send/filter getter readback used by a more direct inspection, so those setter
and error observations are not presented as getter readback. They establish
routing control flow and failure handling, not acoustic equivalence with FMOD.

The resumed manual session used the hash-verified isolated runtime, with the
launch record retained at
`completes/native-openal-soft-phase-2/2b-2/runtime/launch-resume.json`.
The user confirmed that OpenAL and EFX were usable and that the game was
operable. With `Generic` selected and Test in level enabled, a single pistol
shot had a tail; after switching to `Off` and letting the old tail decay, a
new shot had no tail. After disabling Test in level and closing the editor,
the user confirmed normal SFX and that the forced tail was gone. These
observations cover the Generic-versus-Off tail and forced-clear behavior only;
they are explicit user observation, not an AI measurement or complete
listening validation of the latest executable. A subsequent
`snd_backend fmod`, `snd_reset`, `snd_musicvolume 0.5`, and `snd_status`
check was also confirmed by the user as displaying FMOD with both music and
pistol SFX audible. That is reusable normal FMOD-path evidence, not latest
OpenAL executable listening evidence.

The session ran as PID 33868. The requested `-logfile` did not create a file,
so no new runtime backend-log claim is made. The custom-environment unload
path was not manually exercised; the manual editor-close result is not
claimed as map-change or custom-unload success. The document makes no
acoustic-equivalence claim, and the existing limitation that API `1.1 ALSOFT`
does not prove an OpenAL Soft `1.25.2` runtime remains in force.

## Deferred Work

Later units may add HRTF profile selection or live context reset, water
processing, apply radius, or calibrate and enable source-only Doppler. The
existing `snd_reset` path recreates the renderer and is the current HRTF
application boundary; live `alcResetDeviceSOFT` is not implemented. Full
acoustic equivalence with FMOD, including the distance behavior of
`RoomRolloffFactor`, is not claimed.
