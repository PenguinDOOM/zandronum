# Audio Decoder Third-Party Provenance

This document records the exact third-party decoder sources used by the
decoder-only audio path. The hashes below are for the files currently vendored
in this repository.

## miniaudio

- Upstream: <https://github.com/mackron/miniaudio>
- Pinned tag: `0.11.25`
- Pinned commit: `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`
- Commit URL: <https://github.com/mackron/miniaudio/commit/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d>
- Vendored source: `miniaudio/miniaudio.h`
- Source SHA-256: `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89`
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
- Source SHA-256: `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`
- License file: `stb/LICENSE`
- License SHA-256: `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63`
- Selected license: MIT

The C++ adapter declares stb_vorbis with `STB_VORBIS_HEADER_ONLY`. The
implementation is compiled once from `stb_vorbis.c`.

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

## Verification

Recompute each digest from the file bytes and compare it with the values above.
For a portable command, use `sha256sum <file>` where available, or use
`Get-FileHash -Algorithm SHA256 -LiteralPath <file>` in PowerShell. Replace
`<file>` with each vendored source or license path; no repository-specific
absolute path is required.
