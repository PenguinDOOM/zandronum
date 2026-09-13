# Audio Decoder Third-Party Provenance

This document records the third-party decoder pins and the bounded local
patches used by the decoder-only audio path. The upstream hashes and the
current local hashes are intentionally listed separately: the vendored files
are locally patched and are not claimed to be byte-identical to upstream.

## miniaudio

- Upstream: <https://github.com/mackron/miniaudio>
- Pinned tag: `0.11.25`
- Pinned commit: `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`
- Commit URL: <https://github.com/mackron/miniaudio/commit/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d>
- Vendored source: `miniaudio/miniaudio.h`
- Original upstream source SHA-256: `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89`
- Current locally patched source SHA-256: `99925510ea204a3642703a52a10e6136a4dd7986e32a1e73ca31787b5058cb34`
- License file: `miniaudio/LICENSE`
- License SHA-256: `457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb`
- Selected license: MIT-0 (MIT No Attribution)

The miniaudio source is used only for audio decoding and data conversion. It
does not provide device I/O, an audio engine, or a resource manager in this
path.

## stb_vorbis

- Upstream: <https://github.com/nothings/stb>
- Pinned commit: `1ee679ca2ef753a528db5ba6801e1067b40481b8`
- Commit URL: <https://github.com/nothings/stb/commit/1ee679ca2ef753a528db5ba6801e1067b40481b8>
- Vendored source: `stb/stb_vorbis.c`
- Original upstream source SHA-256: `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`
- Current locally patched source SHA-256: `c031782ea9521739dfc540cff340bffe8d3e3e670fe6e696abe2642d12305d31`
- License file: `stb/LICENSE`
- License SHA-256: `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63`
- Selected license: MIT

The C++ adapter declares stb_vorbis with `STB_VORBIS_HEADER_ONLY`. The
implementation is compiled once from `stb_vorbis.c`.

## Bounded Local Patches

The local changes are limited to the decoder path and its test hooks. They do
not change the network protocol, FMOD behavior, or the OpenAL backend.

### Patch Details: miniaudio

- WAV PCM and IEEE conversion helpers read scalar samples with byte copies
  before conversion, preserving the existing conversion, clipping, rounding,
  endian, and partial-read behavior.
- MP3's typed frame cache preserves the old `9216`-byte capacity in both the
  integer and float compile modes.
- The redundant first WAV probe condition was removed while WAV, FLAC, and MP3
  fallback behavior remains ordered and observable.
- FLAC uses one parent allocation. Its checked layout keeps the parent object,
  decoded samples, seekpoint array, and internal Ogg state within that raw
  allocation. C++ array lifetimes are explicitly constructed, and the returned
  typed pointers are retained. Layout arithmetic, alignment, overflow, and the
  `18`-byte seekpoint wire record are checked; malformed non-`18`-byte
  seektable records are rejected.
- Valid native FLAC and Ogg-FLAC input remains covered by the decoder tests,
  including seektable metadata, callback ownership, Ogg state transfer, OOM,
  short reads, seek failures, and seek/PCM checks.

### Patch Details: stb_vorbis

- Memory seek offsets are checked as integer lengths before pointer formation;
  zero, `length - 1`, `length`, `length + 1`, `UINT_MAX`, EOF, and recovery
  after a rejected seek are covered by test hooks.
- Residue and IMDCT scratch requirements are checked with integer bounds and
  alignment-aware sizing. Without an external arena, the bounded scratch is
  allocated once before the first decode and released once at close; this is a
  scratch bound, not a claim about total decoder heap usage.
- Residue type 0, 1, and 2 checks distinguish logical partitions from rounded
  classification storage, preserving current-frame bounds and arena canaries.
- The existing type-0 PCM limitation in the Vorbis scratch fixtures was
  identified but not fixed. This document does not claim a complete vendor
  security audit or make a new-CVE claim.

These are local patches on the pinned sources, not upstream changes. A future
upstream update must be compared against the original pin and reviewed for
reapplication of these patches.

## Decoder-Only Compile Configuration

The miniaudio translation unit uses exactly this decoder-only macro set:

```c
MINIAUDIO_IMPLEMENTATION
MA_NO_DEVICE_IO
MA_NO_ENCODING
MA_NO_ENGINE
MA_NO_GENERATION
MA_NO_NODE_GRAPH
MA_NO_RESOURCE_MANAGER
MA_NO_VORBIS
```

The supported use is local WAV PCM/float, FLAC, MP3, and Ogg Vorbis decoding
with PCM16 output. This configuration includes no miniaudio device, engine, or
resource manager, and requires no codec runtime DLL or SO.

## Verification Scope

The verification evidence is configuration-specific and does not establish
that every warning or every vendor build mode is clean.

- The affected production and test paths were exercised with MSVC x64 and
  with C++11-oriented WSL builds; Ogg variants were included where the
  configuration enabled them.
- The `MA_DR_FLAC_NO_STDIO` baseline was checked in C and C++ probe builds.
  The current C probe succeeded. The corresponding C++ probe failed because
  the standalone probe did not declare its file wrapper; that probe result is
  recorded as unsupported for that harness, not as proof that all modes
  compile.
- Lizard evidence for the current files reported 3,387 functions, including
  78 new and 21 changed functions. The changed residue helpers include
  functions above the repository's advisory thresholds; this is reported
  rather than described as a warning-free result.
- Current Cppcheck evidence is non-zero: miniaudio diagnostics were reported
  as `108/109/108` and stb diagnostics as `2/2/2` across the recorded runs.
  The latest recorded run exited `1`; these diagnostics are not claimed to be
  fully resolved.
- The broader pre-push gate was still failing with 48 failures, including
  unfinished V2B work. This document therefore does not declare the push gate
  green.

For the user-facing build and test path, use the repository's normal commands:

```text
cmake -S . -B build-v143 -G "Visual Studio 17 2022" -A x64 -T v143
cmake --build build-v143 --config Release
ctest --test-dir build-v143 -C Release -R "^(audio_decoder|midi_device_selection|openal_lifecycle)$" --output-on-failure
```

The additional decoder test hooks are enabled only by `AUDIO_DECODER_TESTING`
and are not part of the normal build.

## Hash Verification

Recompute each digest from the file bytes and compare it with the values above.
For a portable command, use `sha256sum <file>` where available, or use
`Get-FileHash -Algorithm SHA256 -LiteralPath <file>` in PowerShell. Replace
`<file>` with each vendored source or license path; no repository-specific
absolute path is required. Compare the current source digest with the
"Current locally patched source SHA-256" value and the original source digest
with the separately recorded original pin; do not treat them as the same
artifact. The license digests above were rechecked and remain unchanged.
