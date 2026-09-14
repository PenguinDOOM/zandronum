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
- Current locally patched source SHA-256: `ed718e371508c2c802eb2e6b1495eec42d36a86495f5605595ae2e3a5076d793`
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
- The CUESHEET patch validates the `36`-byte track and `12`-byte index wire
  records, including remaining-length, count/product/sum, alignment-padding,
  and overflow bounds. Scalar values are copied into typed locals before
  endian conversion, and all public fields and reserved bytes are initialized.
- CUESHEET raw storage is separate from the typed track and index storage. The
  typed arrays use one checked, aligned allocation with C++ byte backing and
  placement array construction where required; zero indexes produce a `NULL`
  index pointer. Raw and typed storage remain alive through the metadata
  callback and are released exactly once after callback use.
- The producer and iterator use the same typed representation. Iterator
  exhaustion returns `MA_FALSE` and advances no further. The private `pTrackData`
  representation changed; old external packed construction or direct
  interpretation is not supported across this revision, while public vendor
  structures, function signatures, FLAC wire data, and PCM output remain
  unchanged.
- The CUESHEET implementation is covered by the existing four-slot V2R
  allocation/lifetime checks plus a dedicated fifth probe for native Ogg
  comment-first input. The dedicated adapter and test hooks are verification
  support only and do not change the decoder's public ABI.

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
- The current Lizard review reported 47 non-legacy new or changed functions;
  all satisfy the inclusive review thresholds of CCN <= 20 and NLOC <= 80,
  including functions with CCN 20. The legacy parser changed from `391` to
  `370` NLOC and from `83` to `87` CCN; it remains explicitly above the
  repository's advisory thresholds and was not cleaned. These figures are
  advisory and are not a claim of a clean whole-repository total.
- The V2B vendor-disposition policy is source-, API-, and configuration-bound.
  The saved six-target/TU evidence contains `322` raw diagnostics: `321` are
  accepted by exact records and one local `memleak` at
  `src/sound/audio_decoder_miniaudio.cpp:439:3` remains unaccepted. The local
  diagnostic is retained in the raw result and is not treated as a vendor
  filter. The accepted records are `195` `dangerousTypeCast`, `96`
  `invalidPointerCast`, `15` `memsetClassFloat`, `3`
  `arrayIndexOutOfBoundsCond`, `6` `shiftNegativeLHS`, and `6` `uninitvar`
  occurrences. These are not a claim that general casts are safe: the 65
  distinct dangerous casts comprise 9 configuration-limited object-
  representation/typed-storage cases and 56 cases unreachable under the
  verified PCM16, original-rate/channel, identity-map test paths.
- A disposition is keyed by target, translation unit, source path, line,
  column, severity, identifier, and message. It also requires the exact
  vendor-source hash, API-consumer root hashes and exact token sets, analyzer version, and
  compiler context (`Cppcheck 2.21.0`, MSVC `v143`, x64, `Release|x64`, and
  the recorded defines). The current API closure covers 53 `ma_` tokens in
  one consumer root and 17 `stb_vorbis_` tokens in three consumer roots.
  Preconditions are fail-closed: source/API/configuration/analyzer-version
  changes, unknown vendor occurrences, malformed records, missing or fatal
  input, and tool exits outside 0/1 reject the result. The original
  `Target|Path|Severity|ID|Message` fingerprint remains the ordinary local
  baseline-comparison key.
- Only `src/sound/thirdparty/miniaudio/miniaudio.h` and
  `src/sound/thirdparty/stb/stb_vorbis.c` can receive a disposition. The
  context extractor proves only the supported generated-project subset; it is
  not a full MSBuild evaluator or portable-ABI proof. Unsupported imports,
  compiler or translation-unit overrides, forced includes, and duplicate or
  otherwise ambiguous toolsets are rejected. Per-TU analysis retains the
  entire selected project TU set (the verified project counts are `455`, `6`,
  and `8`), and raw diagnostics remain separate from accepted reasons.
- The focused `scripts/test-lint-vendor-dispositions.ps1` check passed, as did
  the normal three-project context check, including rejection of duplicate
  `v143`/`ClangCL` and empty or missing toolset cases. This is independent
  code approval evidence, not a V3 full-gate result. The earlier `105`/`106`/
  `105` Cppcheck counts and the broader record of 48 pre-push failures are
  historical raw-scan/gate labels; they are not the current V2B count or a
  declaration that the push gate is green. These results do not establish push
  readiness or that all platforms are clean.

For the user-facing build and test path, use the repository's normal commands:

```text
cmake -S . -B build-v143 -G "Visual Studio 17 2022" -A x64 -T v143
cmake --build build-v143 --config Release
ctest --test-dir build-v143 -C Release -R "^audio_decoder$" --output-on-failure
```

The additional decoder test hooks are enabled only by `AUDIO_DECODER_TESTING`
and are not part of the normal build. The current Release rebuild and the
`audio_decoder` test passed. Separate C11/C++11-oriented MSVC and WSL probes
also passed for the production iterator, raw/live/value/alignment guards, and
the two-allocation/two-free lifetime check. The primary ABI probe passed with
the unchanged public layout and compatibility revision. These are targeted
checks, not a claim that every compiler mode or warning is clean.

## Hash Verification

Recompute each digest from the file bytes and compare it with the values above.
For a portable command, use `sha256sum <file>` where available, or use
`Get-FileHash -Algorithm SHA256 -LiteralPath <file>` in PowerShell. Replace
`<file>` with each vendored source or license path; no repository-specific
absolute path is required. Compare the current source digest with the
"Current locally patched source SHA-256" value and the original source digest
with the separately recorded original pin; do not treat them as the same
artifact. The license digests above were rechecked and remain unchanged.
