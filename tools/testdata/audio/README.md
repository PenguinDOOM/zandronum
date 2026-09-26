# Audio Decoder Test Corpus

These fixtures are inputs to `tools/audio_decoder_tests.cpp`. Each decodable
file is read through memory, file, and bounded file-slice sources. The test
checks decoded metadata, full PCM16 frame count and hash, partial final read,
EOF, repeated seek, and source parity. Truncation is constructed in test memory;
no deliberately truncated binary fixture is checked in.

## Authored signal and license context

`generate_wav_fixtures.py` creates a mathematically defined 8 kHz PCM16 signal
with 529 frames. It contains no third-party audio material. These generated
bytes are project test fixtures under this repository's licensing context in
`LICENSE.txt`; this statement does not claim a third-party audio license.

The generator SHA-256 is
`a15e5f9ecadb0606b96461f92a9763920c005956800d967faf98fa4ec03c79d1`.
It requires Python 3 and a supplied FFmpeg executable; it neither downloads nor
installs FFmpeg. Reproduce and verify the checked-in files with:

```text
python tools/testdata/audio/generate_wav_fixtures.py --ffmpeg C:\path\to\ffmpeg.exe --verify
```

| Fixture path | SHA-256 | Signal and test use |
| --- | --- | --- |
| `float32_mono.wav` | `314ab5655cea7c25d06c755d631bc9b0a282b0cc2980dc4dcfa4c2452bcc4266` | 529-frame IEEE float mono WAV; PCM16 conversion |
| `flac_mono.flac` | `eff9d6d6802e686d563537d66c76f8f9e8bda3eda7251855dae9829d83c7a8b3` | 529-frame mono FLAC |
| `flac_stereo.flac` | `bfd2e7689a97f2a6a970d2a241d81ae5dd203f530c501a1cdc287b69e92bd31b` | 529-frame stereo FLAC |
| `mp3_mono.mp3` | `aa63ffa0d4e18fcf9b3b1c314fd7f0e192620596cd65009d965911ca150b3363` | ordinary mono MP3; decoder produces 1,728 frames including codec delay/padding |
| `pcm16_stereo.wav` | `d19c52b9c4be0d1f6badbb1d1eb709a4a71dbda1ce8a35449101116745c5f8cb` | 529-frame PCM16 stereo WAV |
| `pcm16_three_channel.wav` | `0dcc7562929e8de9e2cda59b8da1c5c9c54d9eb508e7778b04f391172b2f06d1` | 529-frame PCM16 three-channel WAV; rejection contract |
| `vorbis_mono.ogg` | `170b683a66ac709b14d2678ab3317b47871a5533c498aa12db723020a74f9d88` | 529-frame mono Ogg Vorbis |
| `vorbis_stereo.ogg` | `18afa51033264ac747f0826424e5c65be9a99a2daa5d0ed8991f5d637b60e2c7` | 529-frame stereo Ogg Vorbis |
| `vorbis_three_channel.ogg` | `aeb3085a309c9e97a5f25441b678ba5dfb2373fd1772e6f701471deb1b4d258e` | 529-frame three-channel Ogg Vorbis; rejection contract |

## Phase 1B validation fixtures

`generate_phase1b_music_fixtures.py` deterministically creates the compact,
self-authored music corpus below. It uses `vorbis_mono.ogg` only as the payload
for the generated DUMB Ogg-sample XM; that input is itself generated from the
mathematically defined signal documented above. The SMF, DosBox Raw OPL, MOD, VGM, WAD,
and PK3 layouts are authored by the generator and contain no third-party music.
They are project test fixtures under `LICENSE.txt`; no third-party audio license
is claimed.

Regenerate and verify the fixtures with:

```text
python tools/testdata/audio/generate_phase1b_music_fixtures.py --verify
python tools/testdata/audio/validate_phase1b_manifest.py
```

| Fixture path | SHA-256 | Purpose |
| --- | --- | --- |
| `phase1b-generated.mid` | `f71c35cad946d4eddcd11bf9b47a7c8048b045926db77c22d91a4ac0b30980fb` | Generated single-track SMF |
| `phase1b-ogg-sample.xm` | `55e6b848d85777f46891322e9504b6f45e76693427c24f2d9ca578b25cb8eed9` | DUMB XM with embedded mono Ogg sample |
| `phase1b-dosbox-raw-opl.dro` | `b35680580629b7032f523bdfeb340effbb344edd33bb870bec3a3b5a500fde1f` | DosBox Raw OPL (`DBRAWOPL`) stream |
| `phase1b-mod.mod` | `30a8ada7ae0ea435e144598e356777c9fe994419d118959765522a18086a8b16` | Four-channel silent MOD |
| `phase1b-gme.vgm` | `18772548b07096ac6635225f44a60ecd46bd60f8f9357a53f97f69b831971f81` | VGM 1.50 stream with `0x66` at the `0x40` command-stream start |
| `phase1b-audio.wad` | `edf900ce2b7abfdc16844ed4e880b81bb4da91e7dcc738b8375dc6c64dcc6c27` | Generated music lumps (`D_MIDI`, `D_XMOGG`, `D_OPL`, `D_MOD`, `D_GME`) |
| `phase1b-fixtures.pk3` | `e3e2b420c7da6b2afb43fb4c9c260fcdd827f5b1da53710b3336ccf24081800e` | Self-contained PK3 with `phase1b-audio.wad` at its root and other generated files under `music/` |

`phase1b-validation.json` is the source-controlled validation contract. The
two local rows first run `cmake --build build-v143-openal --config Release
--target openal_lifecycle_tests`, then run
`build-v143-openal/tools/Release/openal_lifecycle_tests.exe
--phase1b-direct-memory` and `--phase1b-file-slice` against the exact
checked-in Vorbis and WAVE bytes. This is the CMake target's canonical Windows
output location, not a requirement for ignored build output to exist during
source validation.

Generated music rows launch the client with the fixture PK3 and `${STOCK_PK3}`
as a stock package passed to `-file`, then select OpenAL with `+set snd_backend
openal` before initialization. After the startup screen is rendering and the
OpenAL renderer has initialized, the operator opens the console and executes
the row's ordered post-init actions. The MIDI row executes console `stopmus`,
then console `set snd_mididevice -1`, starts a fresh external console/log
capture, and then executes console `changemus D_MIDI`. `stopmus` ensures that
the setting callback cannot restart an already-playing MIDI song. The capture
start is an operator/harness action, not an engine console command: the engine
`clear` command only clears the console display and does not reset diagnostic
history. Count exactly one occurrence of `FMOD MIDI playback is unavailable
with the current sound backend; using OPL instead.` after that capture start
and through the target selection; earlier process output is outside this
observation window. Every non-MIDI runtime row has exactly one post-init
`changemus D_*` action. The WAD is deliberately at the PK3 root as
`phase1b-audio.wad`, so embedded-WAD discovery exposes the `D_*` lumps. The
MIDI, XM, OPL, and MOD rows are local runtime playback checks and do not connect
to a server. Only the final GME compatibility row uses
`${REFERENCE_SERVER}`. All runtime rows remain Phase 6 handoff checks; they are
not claimed as locally executed audio proof. `validate_phase1b_manifest.py`
verifies file, archive-entry, root-WAD, WAD-lump, ordered runtime-action,
focused-target, and VGM stream contracts without a game runtime. Its
`--self-test` option uses temporary manifests to reject negative command-line
settings and broken MIDI stop, setting, observation-window, or selection
ordering.

## Vendored decoder provenance

The decoder sources are byte-pinned imports. Do not trim whitespace, normalize
line endings, or otherwise format them: preserving their upstream bytes is more
important than style-only diagnostics.

| Dependency | Upstream pin and license choice | Imported file SHA-256 |
| --- | --- | --- |
| miniaudio | `0.11.25`, `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`; [release](https://github.com/mackron/miniaudio/releases/tag/0.11.25); **MIT-0 selected** (upstream also offers public domain) | `miniaudio.h`: `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89`; `LICENSE`: `457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb` |
| stb_vorbis | `1.22`, `1ee679ca2ef753a528db5ba6801e1067b40481b8`; [source](https://github.com/nothings/stb/blob/1ee679ca2ef753a528db5ba6801e1067b40481b8/stb_vorbis.c); **MIT selected** (upstream also offers public domain) | `stb_vorbis.c`: `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`; `LICENSE`: `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63` |

`src/sound/audio_decoder_miniaudio.cpp` is the decoder-only miniaudio integration. It defines
`MINIAUDIO_IMPLEMENTATION`, `MA_NO_DEVICE_IO`, `MA_NO_ENCODING`, `MA_NO_ENGINE`,
`MA_NO_GENERATION`, `MA_NO_NODE_GRAPH`, `MA_NO_RESOURCE_MANAGER`, and `MA_NO_VORBIS`
before including `miniaudio.h`. Vorbis is supplied separately: `audio_decoder_vorbis.cpp`
includes `stb_vorbis.c` with `STB_VORBIS_HEADER_ONLY`, while the vendored
`stb_vorbis.c` is compiled as the single implementation translation unit by the source and
test targets.

## Win32 PCM reference profile

The test-only `AUDIO_DECODER_PCM_REFERENCE_PROFILE` variable records a reviewed
reference profile for two fixture hashes. It does not describe product audio
behavior, require an IA32 CPU, or change the decoder's file, memory, or bounded
file-slice inputs. The existing metadata, full PCM comparison, partial final
read, EOF, repeated seek, and source-parity checks remain unchanged.

The variable is absent or empty by default, which selects `default` and retains
the existing golden hashes. The only reviewed profile is
`msvc-194435229-win32-ia32-fast-release-v1`. It is valid only when the runtime
test guard sees MSVC full version `_MSC_FULL_VER == 194435229`, `_M_IX86`, not
`_M_X64`, 32-bit pointers, `_M_IX86_FP == 0`, and no `_DEBUG`. It selects the
reviewed full-PCM hash for `float32_mono.wav` and exactly these 64-bit integer
hash values:

| Fixture | Default golden hash | Reviewed profile hash |
| --- | ---: | ---: |
| `float32_mono.wav` | `5804898877390266` | `660772755265263697` |
| `mp3_mono.mp3` | `13849218400374428564` | `5678258263728297510` |

There is no +/-1 tolerance, fallback, or multiple-hash admission. An unknown
profile, or a known profile used by an incompatible binary such as x64, is a
failure. A compiler update is therefore blocked until a new profile has a
separately reviewed complete-PCM reference and compile-condition record.

The JSON evidence emitted for the two profile fixtures uses the fields
`pcmReferenceProfile` and `pcmReferenceProfileSelected` to identify selection.
The surrounding fields `actualRate`, `channels`, `sampleCount`, `frameCount`,
`byteCount`, `actualHash`, and `expectedHash` describe the decoded result;
`dataHashAlgorithm` is `HashPCM16` and `dataHashIncludesMetadata` is `false`.
The `testTU` object records `compiler`, `architecture`, `pointerWidth`,
`mIx86Fp`, and `decoderCompileFlags`; the last value is explicitly metadata,
not proof of the decoder translation unit's actual flags.

The direct gate is `scripts/verify-win32-pcm-profile.ps1`. It is called with
explicit `WorkspaceRoot`, `BuildDirectory`, `Configuration`, `Architecture`,
and `EvidenceDirectory` roots:

```powershell
pwsh -NoProfile -File scripts/verify-win32-pcm-profile.ps1 `
-WorkspaceRoot C:\path\to\zandronum `
-BuildDirectory C:\path\to\build `
-Configuration Release `
-Architecture Win32 `
-EvidenceDirectory C:\path\to\evidence
```

Each invocation writes a fresh `pcm-reference-profile.json` with schema
`phase2-win32-pcm-profile-v2`, `decision`, `reason`, `architecture`,
`configuration`, `profile`, and evidence. A Release/Win32 build is
`accepted` only after the `audio_decoder_tests` project, its Release tlog, and
its binary are matched to the actual
`src/sound/audio_decoder_miniaudio.cpp` and test TU. The gate also checks the
v143 XML configuration, source-specific tlog records, `/O2`, `/fp:fast`,
`/arch:IA32`, approved definitions, the pinned fixture/header hashes, and the
decoder's normalized-EOL source hash. An x64 Release build is
`not_applicable` with `profile: null`; missing, conflicting, or stale evidence
is `rejected`, and a rejected or unsaved decision is nonzero.

The Windows workflow caller separately checks the process exit code, JSON
shape and schema, architecture/configuration, decision, and the exact profile
token before passing an accepted token to CTest. It does not reuse an old
accepted JSON or persist the token through `GITHUB_ENV`; `not_applicable`
leaves the profile unset and `rejected` stops before CTest. The direct gate
does not reimplement the decoder's PCM decision and does not infer the
compiler's full `CL.exe` version from project metadata. The recorded
`_MSC_FULL_VER == 194435229` condition is supported by the test guard and
profile contract; a raw full-version measurement of `CL.exe` is not claimed.

The 2026-09-26 positive evidence at
`completes/native-openal-soft-phase-2/2e/win32-pcm-profile/direct-gate/20260926-positive/atlas-keyfix-check/`
is a saved direct-gate run with `NATIVE_EXIT=0`, decision `accepted`, and the
fixed profile token. Its copied project XML and UTF-16 tlog are byte-identical
recorded inputs for the current read-only facts; the placeholder EXE was not
executed, so its SHA-256 is only a property hash and not proof of Win32
runtime execution. The round2b refusal and caller-validation result is at
`completes/native-openal-soft-phase-2/2e/win32-pcm-profile/direct-gate/refusal-validation-round2b/result-matrix.json`:
all 22/22 cases passed. The matrix covers x64 `not_applicable`, x64-as-Win32
rejection, missing and duplicate source records, unrelated-target exclusion,
conflicting flags and definitions, pinned-input and toolset/source failures,
stale or unwritable output, source-specific XML overrides, and duplicate
applicable conditional metadata. The workflow caller also rejects typed JSON
arrays and invalid architecture/decision/profile combinations while accepting
the exact Win32 Release combination. These are semantic gate checks, not
evidence of actual Win32 execution.

The two default/profile golden pairs remain strict exact 64-bit matches: there
is no one-LSB acceptance, automatic fallback, or hash adoption. The default
x64 test pass remains valid. The selected Win32 profile was not run on a new
remote Win32 binary; current CI was not run for this evidence, and native
OpenAL lifecycle validation remains unresolved. None of these is claimed as
complete Phase 2 evidence.

## FFmpeg provenance

Retrieved on 2026-09-03 from the final URL
`https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip` into a
repository-external temporary directory. Archive SHA-256:
`fec81ae03971d9dd4be3ebe02e263bd2ec1d789483f931bdba5f5715e65da2e9`.

```text
ffmpeg version 9.0.1-essentials_build-www.gyan.dev Copyright (c) 2000-2026 the FFmpeg developers
built with gcc 16.1.0 (Rev2, Built by MSYS2 project)
configuration: --enable-gpl --enable-version3 --enable-static --disable-w32threads --disable-autodetect --enable-cairo --enable-fontconfig --enable-iconv --enable-gnutls --enable-libxml2 --enable-gmp --enable-bzlib --enable-lzma --enable-zlib --enable-libsrt --enable-libssh --enable-libzmq --enable-avisynth --enable-sdl2 --enable-libwebp --enable-libx264 --enable-libx265 --enable-libxvid --enable-libaom --enable-libopenjpeg --enable-libvpx --enable-mediafoundation --enable-libass --enable-libfreetype --enable-libfribidi --enable-libharfbuzz --enable-libvidstab --enable-libvmaf --enable-libzimg --enable-amf --enable-cuda-llvm --enable-cuvid --enable-dxva2 --enable-d3d11va --enable-d3d12va --enable-ffnvcodec --enable-libvpl --enable-nvdec --enable-nvenc --enable-vaapi --enable-openal --enable-libgme --enable-libopenmpt --enable-libopencore-amrwb --enable-libmp3lame --enable-libtheora --enable-libvo-amrwbenc --enable-libgsm --enable-libopencore-amrnb --enable-libopus --enable-libspeex --enable-libvorbis --enable-librubberband
libavutil      61.  1.101 / 61.  1.101
libavcodec     63.  1.101 / 63.  1.101
libavformat    63.  1.101 / 63.  1.101
libavdevice    63.  1.101 / 63.  1.101
libavfilter    12.  1.101 / 12.  1.101
libswscale     10.  1.101 / 10.  1.101
libswresample   7.  1.101 /  7.  1.101
```
