# Native OpenAL Soft: Phase 2D-1 Current State

## Scope

This document describes the implemented 2B-1 shared environment state and
numeric adapter, 2B-2 OpenAL EFX environment routing, 2B-3 underwater
pitch/low-pass and virtual-position behavior, the approved 2C source-radius
boundary, and the 2D-1 source-only Doppler implementation. The existing FMOD backend,
`snd_backend` selection, fallback behavior, music selection, CVAR defaults,
compatibility behavior, `AL_NONE` distance model, and manual rolloff remain
unchanged.

2B-1 owns one EFX effect, one auxiliary slot, and the filter capability for the
current OpenAL context. It requests one auxiliary send and records the actual
`ALC_MAX_AUXILIARY_SENDS` value, including zero; `unknown` is reserved for a
failed query. EFX resource availability is independent from filter availability.
Initialization attempts EAX reverb first, then standard reverb, then dry
operation; a failed optional path does not stop ordinary OpenAL playback.
Resource cleanup is tracked for partial allocation failures and remains within
the owning context.

2B-2 adds source send routing on top of those resources. 2B-3 adds the
water-pitch/low-pass approximation and the initial virtual-start position
registration. 2C adds the bounded source-radius behavior described below.
2D-1 adds the standard source-only Doppler behavior described below; FMOD is
unchanged.

This is the current 2D-1 implementation state, not a Phase 2 completion claim.

## 2B-3 Underwater Implementation

The water state is active when

`listener.underwater && snd_waterlp != 0` **or** `SoftwareWater`.

For a pausable SFX source, the effective pitch is computed from its base pitch
without cumulative multiplication:

$$p_{effective}=p_{base}\times0.7937005$$

when water is active, and `$p_{effective}=p_{base}$` otherwise. `SNDF_NOPAUSE`
sources are excluded from both the water pitch and direct low-pass. Music and
encoded/software music remain dry and excluded. `SNDF_NOREVERB` only suppresses
the environmental send; it does not suppress water pitch or the direct
low-pass. With `SoftwareWater`, pitch remains active even when `snd_waterlp`
is zero, while the low-pass is bypassed. `snd_waterreverb` is a no-op in this
backend: the old wet graph, Q=2 behavior, wet-tail EQ, and direct/wet mix are
deferred.

The base pitch is retained and water-state changes rebase live and virtual
positions before applying the new effective pitch. Position resolution keeps
the cached loop position when an OpenAL query is invalid, reports natural
non-loop termination at `Frames` before finalization, and preserves the
pre-stop offset for an explicit stop. Pool eviction rebases the logical
position to the actual cached offset before the source is retired. A virtual
ended frame count is kept distinct from an invalid position, so an invalid
query cannot resurrect a source at an unrelated position. `MarkVirtualStart`
receives the already loaded sound handle, pitch, and flags from `s_sound.cpp`;
OpenAL registers the initial logical position, sample rate, loop bounds,
effective pitch, pause class, and a non-zero owner token. Water changes rebase
both active and virtual positions while preserving the derived pitch. These
are the tested lifecycle boundaries, not an exhaustive guarantee for every
future resolver case.

The low-pass approximation uses the fixed 5000 Hz shelf reference and the
Nyquist-safe cutoff:

$$f_0=\min(5000,0.49F_s),\qquad f_c=\min(\mathit{snd\_waterlp},0.49F_s)$$

$$r=\frac{\tan(\pi f_c/F_s)}{\tan(\pi f_0/F_s)},\quad
t=r^4,\quad
g=\sqrt{\frac{2t}{1+\sqrt{1+8t^2}}}$$

The filter uses `AL_LOWPASS_GAIN=1` and
`AL_LOWPASS_GAINHF=clamp(g, .001, 1)`. At low output rates, when the reduced
reference cannot produce the clamped gain, the filter is bypassed. This is a
clamped approximation, not a claim that the requested low cutoff is physically
reached at every output rate.

When the cutoff changes while water remains active, the derived positive
`GainHF` is updated without rebasing source pitch. If the cutoff cannot produce
the minimum useful gain at the output rate, the direct filter is bypassed while
water pitch remains governed by its separate state.

The product lifecycle readback at 44,100 Hz reported
`WATER_FILTER_ACTUAL output_rate=44100 cutoff=250 gainhf=0.00229177088 gain=1`.
The loopback probe used that product-readback `gainhf` as an explicit input; it
did not recompute the coefficient. With a 2 second, 88,200-frame mono 16-bit
sine, the first 44,100 frames were excluded and RMS was measured over the
last 44,100 float32 loopback frames. The measured amplitude ratios were
`0.7770715` at 225 Hz, `0.70710707` at 250 Hz, and `0.6370311` at 275 Hz.
Thus the half-power frequency is bracketed by 225--275 Hz within the stated
10% frequency criterion; the criterion is not an amplitude-error criterion.
The raw reference and filtered files, full inputs, and provenance are retained
under `completes/native-openal-soft-phase-2/2b-3/runtime-probe/raw/measurement-44100-product-readback-final/`.
The measurement used an explicitly loaded DLL whose recorded SHA256 was
`2C44AE1108904B708BDC370EF8703785912BFEE67BB4999ED8D25C28250628A9` and
observed API string `1.1 ALSOFT`; this is historical measurement provenance,
not a claim that the current runtime has the same DLL. An exact OpenAL Soft
`1.25.1` or `1.25.2` runtime is unconfirmed. The loopback is OpenAL Soft DSP
output, not playback through a physical device or a human listening test. The
separate 8,000 Hz fixture output rate is a low-rate bypass decision, not a real
8,000 Hz product measurement.

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

## 2D-1 Source-Only Doppler

For a finite, non-relative 3D source, the renderer configures the OpenAL
context with `alSpeedOfSound(32956.8f)` and `alDopplerFactor(0.5f)`. The
listener velocity is zero. The standard source-only ratio is therefore

$$R=\frac{32956.8}{32956.8-0.5v_{sr}}.$$

The actor's existing world velocity is consumed once after the established
`(X,Y,-Z)` coordinate transform. The implementation does not multiply by
`TICRATE`, derive velocity from position differences, divide by 96 again, or
change `DistanceScale` or manual rolloff. Non-finite velocity becomes zero;
finite extreme velocity is direction-preservingly bounded so that
`D*|v| <= 0.5c`. Relative sources, inner `AREA` sources, 2D sources, and
music use zero velocity, including after source reuse.

`AL_PITCH` remains the base pitch multiplied by the water factor only. Live
source state uses the authoritative OpenAL offset before eviction, save, water
changes, and restart; virtual sources advance with Doppler factor `1`. The
tested water/3D lifecycle case `Test3DUnderwaterEvictionRestart` records
`pitch=1.1905508` for velocity `(3,4,-5)` and verifies the live-to-virtual
offset and restart handoff. The tested live values include offset `450`,
virtual offset `202`, and restart offset `202`.

If the optional Doppler settings are rejected, the renderer verifies factor
zero and continues ordinary playback when that safe state is established. If
the disabled state cannot be established, initialization does not claim
success. No Doppler CVAR or FMOD matching adjustment is added.

The legacy FMOD wet graph, Q=2 behavior, wet-tail filtering, and full FMOD
acoustic equivalence remain outside this work. The historical 2D-0 FMOD
comparison remains FAIL under the original FMOD-equivalence contract; approximately 2% high-
speed divergence from FMOD is expected and is not an OpenAL standard-formula
failure. The accepted loopback OpenAL formula result is the relevant new
criterion.

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

The earlier focused CTest run recorded `2/2` tests passed as historical
evidence. The latest terminal-fix lifecycle run recorded `1/1` test passed;
the device-free unit case
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

For 2B-2, the valid Release product and lifecycle-test build, earlier focused
CTest (`2/2`), and direct routing, continuing-source, and error-handling runs are
retained and reused; they were not rerun for the manual resumption. These
automated assertions cover valid-listener forced/listener/Off selection,
modified/retry behavior, 2D `SNDF_NOREVERB`, a wet 3D source,
`SNDF_NOPAUSE`, dry music, source reuse, and failure recovery through the
product setter and error observations described below. The current test file
does not assert invalid-listener hold or a 3D `SNDF_NOREVERB` source; those
remain code-level behavior, not claimed test coverage.

The review1-lp full Cppcheck gate used the latest final working-tree manifest
at
`completes/native-openal-soft-phase-2/2b-3/review1-lp/review1-complete-final-working-tree-capture-7b1fcf1415953957c7cab69058479480928ab518/manifest.json`.
The manifest verification script is
`completes/native-openal-soft-phase-2/2b-3/review1-lp/verify-final-manifest.ps1`;
it reported `MANIFEST_HASH_MATCH=True`, `FILE_COUNT=6`, and
`CACHE_STABLE=True`. The latest Cppcheck result is retained at
`completes/native-openal-soft-phase-2/2b-3/review1-lp/cppcheck-full-final/run-20260921-104454-1640a1c2e9a6454e8a03bc0cc2987dec/final-result.json`:
it passed with exit code 0, `Raw=601078`, `Baseline=7503`, `New=0`, and
`UnresolvedVendor=0`. The review1-lp Lizard result is
`completes/native-openal-soft-phase-2/2b-3/review1-lp/lizard-final.raw.log`;
it reports no new complexity regressions, `EXIT_CODE=0`, and
`REAL_INDEX_UNCHANGED=True`. The parallel formal parent-review1 Lizard run is
also recorded zero new findings with `REAL_INDEX_UNCHANGED=True`. These
analysis gates apply to the captured working tree; they do not make the
overall Phase 2 complete.

The review1 Release product/test gate and focused CTest gate recorded the
Release build as successful and `2/2` tests passed. The shared `NO_SOUND` and
`SERVERONLY` results remain valid because the shared source is reported
unchanged from the accepted baseline. These are 2B-3 validation results; they
do not make the overall Phase 2 complete.

## 2D-1 Verification Record

The 2D-1 implementation is present in the OpenAL source path, while the final
independent 2D-1 review is approved (GREEN). The latest final integration Cppcheck
result is retained at
`completes/native-openal-soft-phase-2/2d-1/final-integration/3d-water-lifecycle-20260925/cppcheck-full/run-20260925-022237-3df053f636394605bea362013427119a/final-result.json`.
It passed with exit code `0`, `New=0`, and `UnresolvedVendor=0`.

The current focused CTest evidence records `2/2` passed. The added moving
underwater 3D lifecycle case is in the current test input with SHA256
`04C88E9DE154E3082E17684FFB385963C8854E2DA06F78E5EB5915071807C91F`.
The test exercises the live source, virtual Doppler-free progression during
eviction, and restart handoff; it is not a full save-game or renderer-ABSTIME
claim. The reset repair preserves the owner's rolloff through context
replacement; manual dry reset and water reset observations were normal.

D01 through D10 are recorded as manual PASS, including the independent D09
prediction-correction observation. D09 has an approximately 110-second user
observation with audible chainsaw continuity and no anomaly. The independent
raw-log review found actor velocity `8.62236023` world units/second and a
consecutive-position derivative of `20.0143432617`; the consumed actor
velocity, not the correction-spanning position derivative, is the accepted
source velocity. The original producer formula was not accepted as the basis
for this result. D09 remains a bounded observation and does not prove that
every correction came from a server pulse.

The formal parent Lizard run checked the four phase C++ files in the isolated
index and reported no new complexity regressions. Its raw log is retained at
`completes/native-openal-soft-phase-2/2d-1/final-integration/3d-water-lifecycle-20260925/parent-final-lizard.raw.log`;
it records `LIZARD_EXIT=0` and `REAL_INDEX_UNCHANGED=True`. Saved legacy PK3
Release byte equality also remains unverified.
The local runtime identity is `1.1 ALSOFT`; it is not an exact OpenAL Soft
`1.25.2` claim. No stock-compatibility passing claim is made for 2E, which
remains unapproved.

The latest 2B-3 terminal-repair full Cppcheck input is the six-file working-tree
capture at
`completes/native-openal-soft-phase-2/2b-3/cppcheck-input/review2-terminal-working-tree-capture-7b1fcf1415953957c7cab69058479480928ab518/manifest.json`;
the current six working-tree hashes match that manifest. The actual full
result is retained at the root-side evidence path
`2b3/review2-terminal/cppcheck-full/run-20260922-063359-643079dce90347f7a777043da085ba1d/final-result.json`:
the live gate passed with exit code 0, `New=0`, and `UnresolvedVendor=0`.
Its recorded full-result SHA256 is
`FB9E7C6564F6C511C4285274E1B09958CF4007F011B18E6654C49872AB05CD9A`.
The unexpected root-side `2b3/review2-terminal` raw logs and exit records,
including the Phase 2 test-2 record, are preserved as excluded evidence and
are not moved, edited, or added to the commit. The formal parent-review2
Lizard record at
`completes/native-openal-soft-phase-2/2b-3/parent-review2-lizard.log` passed
with `EXIT_CODE=0` and `REAL_INDEX_UNCHANGED=True`.

The repaired lifecycle boundary covers natural non-loop termination reporting
at `Frames` before finalization and pool eviction rebasing to the cached
offset; the natural callback path does not resurrect a source for one frame.
The Release product/test build passed, and the latest terminal-fix
`openal_lifecycle` run passed (`1/1`); the older terminal-fix CTest record that
reported `2/2` is retained as historical evidence, not as the latest result.
The separate test-2 CTest record reported `No tests were found!!!` and is
excluded from pass evidence. An independent review3 is approved (`GREEN`),
and B3 is committed at `dc669bbb9758e463ecfce33793d8df03da11fd6e`; the
reviewer required no new test or product edit. These records update the
terminal-repair provenance only. All shared/LP manual evidence remains
retained, including normal-executable observations made before the terminal
failure repair; it is not latest post-repair listening evidence. These records
do not replace that retained evidence or make an overall Phase 2 completion
claim.

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

## 2C Source Radius

When the radius extension is advertised, 2C applies radius `32` only to an
`AREA` world source whose world distance is greater than `32`. At distances
less than or equal to `32`, the area source remains head-relative with radius
`0`, preserving the Phase 1 placement behavior. Point sources, 2D sources,
and music use radius `0`. The threshold is in world units and is not scaled by
`DistanceScale`. The position, rolloff calculation, and `AL_GAIN` path are
unchanged. Spatial errors detected before the radius helper are preserved;
radius handling does not discard them. A rejected radius setter follows the
Phase 1 fallback to radius `0`, and source publication still requires a clean
OpenAL error state. Reset paths restore radius `0` and the environment/water
EFX reapplication then reapplies the requested spatial radius, so an EFX reset
does not silently lose an active area radius.

`RadiusApplied` means exactly whether the latest spatial application
successfully set the requested radius. A rejected requested radius `32` that
then falls back to a successfully set `0` remains false. A successful spatial
application whose requested radius is `0` may set it to true. EFX reset,
2D/music reset, and source reuse reset do not update this flag, and the flag is
not per-source proof or proof that radius `32` is currently active. The
position, gain, and radius computations remain intact; they are not replaced by
the status flag.

If a radius operation fails after a source is active, the finite backend-error
path stops and retires that source. If a source start cannot complete cleanly,
it is not published. A successful fallback to radius `0` is therefore not a
broken-start publication and does not turn a failed requested spatial radius
application into success. The lifecycle tests inspect `AL_SOURCE_RADIUS`
directly and also check relative placement, position, and gain across
distances `0`, `16`, `31.9`, `32`, `32.1`, and `64`, with `DistanceScale`
values `0.5`, `1`, and `2`. They cover area, point, music/2D reset,
rejected-radius fallback, source reuse, and the failure lifecycle. This is
direct OpenAL readback, not human confirmation.

The FMOD comparison boundary is deliberately narrower: FMOD uses its existing
continuous `3DPanLevel` range `0..32`, while OpenAL applies the discrete 2C
boundary above. No exact FMOD equivalence and no final RMS-constant claim are
made. 2C adds no CVAR, inner-radius redesign, or gain compensation; 2D-1
source-only Doppler is documented separately above.

The current 2C review1-fix working-tree capture is
`completes/native-openal-soft-phase-2/2c/review1-fix/working-tree-capture-review1-fix-20260922-101500/manifest.json`.
It contains exactly the three changed files `src/sound/oalsound.cpp`,
`src/sound/oalsound.h`, and `tools/openal_renderer_lifecycle_tests.cpp`.
The manifest records these verified working-tree SHA-256 values:

- `src/sound/oalsound.cpp`:
  `31795C8EFCE3D32EDE0C48CDEC8425E2D751C6D4B2528CCAD2CC2A8DCB7FB493`
- `src/sound/oalsound.h`:
  `D01345279231E70053B316DFE51114881A91CEE570D63E9ADD1D8486F5D98AEC`
- `tools/openal_renderer_lifecycle_tests.cpp`:
  `0141ED074CAA6129C8EA6B24F042C5A6694A327157A76336AFCA41796FACB5BA`

The capture uses Release|x64, Visual Studio 17 2022, v143, with
`BUILD_TESTING=ON`, `NO_SOUND=OFF`, and `DYN_FLUIDSYNTH=ON`; the CMake cache
and both recorded project hashes were stable. The latest full Cppcheck result
is retained at
`completes/native-openal-soft-phase-2/2c/review1-fix/cppcheck-full/run-20260922-100836-4659b8c043bc4e7b8c01db4d73dc230d/final-result.json`.
It passed with exit code `0` and the complete classification
`Raw=601078`, `AcceptedVendor=214`, `Unaccepted=600864`, `Baseline=7503`,
`Unchanged=7481`, `BaselineOnly=22`, `New=0`, and `UnresolvedVendor=0`.
The recorded analyzer is Cppcheck `2.21.0`; the result is for the current
three-file capture, not the historical `radius-split-final2` capture.

The latest valid review1-fix broad Release build record is retained at
`completes/native-openal-soft-phase-2/2c/review1-fix/build-release-review1-fix-final-20260922.raw.log`
and ends with `EXIT_CODE=0`. The canonical lifecycle CTest record is retained
at
`completes/native-openal-soft-phase-2/2c/review1-fix/ctest-openal-lifecycle-review1-fix-canonical-20260922.raw.log`;
it reports `1/1` passed and `EXIT_CODE=0`. The parent formal Lizard result is
recorded separately in
`completes/native-openal-soft-phase-2/2c/review1-fix/parent-final-lizard.raw.log`;
it reports no new complexity regressions, `LIZARD_EXIT_CODE=0`, and
`REAL_INDEX_UNCHANGED=True`. These checks establish the implemented code and
direct test behavior, not human listening or acoustic equivalence.

ROUND2 code and automated review is APPROVED. The Release build and canonical
lifecycle CTest therefore establish the bounded implementation and automated
behavior, but not the manual gate. During evidence recovery, an unrestricted
targetless log-recovery build updated `build-v143/zandronum.pk3` (`updated715`)
and copied it to the Release output. There was no pre-build hash, so restoration
of the original bytes is unconfirmed. This deviation is recorded rather than
described as a no-generated-change result. The generated PK3 remains outside
the commit and is not used for listening; only the independently verified stock
PK3 in the isolated old/new runtimes is used. The isolated stock PK3 hash is
`163C181616F13B6B182F248E1D2617434E23412E77CB16FB77DF68A504105074`.

The manual comparison used the fixture with SHA256
`6B7323378003F3530A53B1AFE687283FC196EB89A8B5D9A5E12DB98FC255FF91` and the
runtime provenance at
`completes/native-openal-soft-phase-2/2c/runtime/provenance-isolated-runtimes.json`.
The old executable hash was
`93D922BD475EDCE29A561AE60D555FAE9B0730B185C5D2EF2F609B0C460D8C21`; the new
executable hash was
`62056E3957B3871404D1991E06F51620AE45E54F874A36800806FD3699F767EA`.
Both used the same verified stock PK3 and OpenAL DLL
`2C44AE1108904B708BDC370EF8703785912BFEE67BB4999ED8D25C28250628A9`; the
provenance record contains the exact hashes for all assets.

The old and new startup logs are
`completes/native-openal-soft-phase-2/2c/runtime/isolated/old-b3/logs/old-off-startup.log`
and
`completes/native-openal-soft-phase-2/2c/runtime/isolated/new-2c/logs/new-off-startup.log`.
Parent verification found HRTF inactive in OLD-OFF and active in OLD-ON and
NEW-ON, with `SADIE_D02-48000`; NEW-ON also logged FMOD `4.44.64` during the
FMOD comparison. These are runtime/status observations, not a claim of
OpenAL Soft `1.25.2`.

For the user listening results, OLD-OFF confirmed continuous sound and the
31.9/32/32.1 boundary baseline; OLD-ON confirmed active HRTF. The user quit
the old process and then launched the new one. NEW-OFF had no clear worsening
against OLD-OFF for localization, volume, or clicks. NEW-ON was active and had
no clear worsening against OLD-ON. NEW-NEAR confirmed left/right reversal at
`warp 0,-64` and `0,64` and continuous near-boundary behavior. The FMOD
`snd_backend fmod`, HRTF-off reset/status comparison found both MAP01 AREA and
MAP02 point normal, with the same mono DSSAWIDL source, coordinates, and
distance-16 check. These are human listening observations, not machine audio
assertions; sector-source semantics, an RMS-constant claim, and a per-source
32 global status claim were not tested. The F5/F6/F7 bindings and warp
commands are logged, but not every key execution is logged.

The current game PID `22748` remains running for the user; the old PID `46304`
was quit by the user. The independent ROUND3 review is approved (`GREEN`);
code hashes are unchanged, all required 2C manual results are accepted, the
user-accepted PK3 past-incident exception remains accepted, and no findings
were reported. The historical 2C checkpoint was committed as
`ab7c308238146a59bf5329860a39c664dda22808`; Phase 2 is not yet complete
overall, and the 2D-1 final review is approved (GREEN). Codacy local analysis remains unavailable on
this Windows native path and no setup was performed.

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

Do not treat this baseline as evidence that radius or 2D-1 Doppler is active. The final Release and
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
A source that can be made dry remains active, and later active sources continue
through the dry fallback. Direct water low-pass application is independent of
reverb-send draining, so a reverb property failure does not remove an otherwise
successful direct filter. The lifecycle tests cover these bounded recovery and
retirement cases; they do not establish unrestricted acoustic equivalence.
Re-editing or reselection is the recovery boundary.

Normal 2D and 3D sources use the selected wet environment except when the
selected environment is `Off` or the source has `SNDF_NOREVERB`. `SNDF_NOPAUSE`
does not make a source dry; it remains wet-eligible. Music, encoded streams,
callbacks, and software-generated music remain dry and have no EFX send or
filter. New and reused sources and streams are explicitly reset to dry, with
air absorption, automatic send/filter gain correction, and the direct filter
reset to their dry defaults; the manual source gain is applied once. When
`AL_EXT_SOURCE_RADIUS` is advertised, an area/world source uses an OpenAL
source radius of `32` only after its world distance is greater than 32. At
distances up to and including 32, it remains head-relative with radius `0`.
Point sources, 2D sources, and music remain at radius `0`; point-source
placement is unchanged. The radius decision uses world distance and does not
multiply the 32-unit boundary by `DistanceScale`. Position, manual rolloff,
and `AL_GAIN` remain unchanged. A rejected radius setter falls back to radius
`0` through the Phase 1 failure path, but a successful fallback does not make
the requested spatial application successful. `RadiusApplied` means exactly
whether the latest spatial application successfully set its requested radius;
EFX reset, 2D/music reset, and source reuse reset do not update it. It is not
per-source proof that radius `32` is active. `RoomRolloffFactor` is retained as an EFX
parameter, but it does not reproduce FMOD's distance attenuation in this
`AL_NONE` plus manual-rolloff arrangement. The 2D-1 standard Doppler settings
are independent of the implemented
water pitch/low-pass and virtual-position behavior.

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
so that historical session has no new runtime backend-log claim. The later
2B-3 resumed manual run used PID 4476 and the isolated executable
`completes/native-openal-soft-phase-2/2b-3/runtime-new/zandronum.exe` with
SHA256
`93D922BD475EDCE29A561AE60D555FAE9B0730B185C5D2EF2F609B0C460D8C21`.
Its launch record is
`completes/native-openal-soft-phase-2/2b-3/runtime-new/launch-resume.json`,
and the `+logfile` output exists at
`completes/native-openal-soft-phase-2/2b-3/runtime-new/logs/water-manual-20260922-060739.log`.
The runtime provenance records all 13 source/runtime assets and the IWAD as
verified; the fresh private configuration selected OpenAL,
`snd_waterlp=250`, and `snd_waterreverb=true`.

The user confirmed that the OpenAL status was usable. With the Test in level
editor's Builtin `DSP Water` selected, the forced `SoftwareWater` path produced
lower pitch and audible low-pass muffling at `snd_waterlp=250`. With
`snd_waterlp=0`, pitch remained lowered while the low-pass was bypassed; after
restoring `250` and selecting `Off`, pitch returned to normal. This route
demonstrates forced `SoftwareWater`, not physical underwater level-3 detection.
The user also confirmed normal music and SFX in the tested states.

The separate FMOD run confirmed lowered pitch and a normal restore to `Off`,
but no muffling. This is expected for the selected FMOD SDK
`FMOD_VERSION=0x00044464`: the current water path applies pitch, while the
legacy water low-pass/reverb branch is unavailable for this SDK. This is a
current FMOD implementation boundary, not a claim that FMOD generally lacks
low-pass capability, and no FMOD muffling pass is claimed. FMOD pause/resume
was also user-confirmed as normal, with music and SFX normal. These are user
listening observations, not AI or instrumented acoustic measurements.

The custom-environment unload path was not manually exercised; the manual
editor-close result is not claimed as map-change or custom-unload success. The
document makes no acoustic-equivalence claim, and the existing limitation that
API `1.1 ALSOFT` does not prove an OpenAL Soft `1.25.2` runtime remains in
force.

## Deferred Work

Later units may add HRTF profile selection or live context reset. The
existing `snd_reset` path recreates the renderer and is the current HRTF
application boundary; live `alcResetDeviceSOFT` is not implemented. Full
acoustic equivalence with FMOD, including the distance behavior of
`RoomRolloffFactor`, is not claimed.

The manual 2B-3 editor route used selecting `DSP Water` in Test in level. That
route exercises `SoftwareWater`; it is not evidence of the built-in underwater
listener flag or actual player submersion. The latest 2B-3 manual and
provenance evidence is recorded above. B3 independent review round three is
approved (`GREEN`), and B3 is committed at
`dc669bbb9758e463ecfce33793d8df03da11fd6e`; no overall Phase 2 completion
claim is made, and no unverified pause console command is
prescribed here.
