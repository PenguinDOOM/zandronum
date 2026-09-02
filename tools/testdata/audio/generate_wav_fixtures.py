"""Generate the self-authored audio decoder fixture corpus deterministically."""

import argparse
import hashlib
import pathlib
import struct
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent
SAMPLE_RATE = 8000
FRAME_COUNT = 529

EXPECTED_SHA256 = {
    "float32_mono.wav": "314ab5655cea7c25d06c755d631bc9b0a282b0cc2980dc4dcfa4c2452bcc4266",
    "flac_mono.flac": "eff9d6d6802e686d563537d66c76f8f9e8bda3eda7251855dae9829d83c7a8b3",
    "flac_stereo.flac": "bfd2e7689a97f2a6a970d2a241d81ae5dd203f530c501a1cdc287b69e92bd31b",
    "mp3_mono.mp3": "aa63ffa0d4e18fcf9b3b1c314fd7f0e192620596cd65009d965911ca150b3363",
    "pcm16_stereo.wav": "d19c52b9c4be0d1f6badbb1d1eb709a4a71dbda1ce8a35449101116745c5f8cb",
    "pcm16_three_channel.wav": "0dcc7562929e8de9e2cda59b8da1c5c9c54d9eb508e7778b04f391172b2f06d1",
    "vorbis_mono.ogg": "170b683a66ac709b14d2678ab3317b47871a5533c498aa12db723020a74f9d88",
    "vorbis_stereo.ogg": "18afa51033264ac747f0826424e5c65be9a99a2daa5d0ed8991f5d637b60e2c7",
    "vorbis_three_channel.ogg": "aeb3085a309c9e97a5f25441b678ba5dfb2373fd1772e6f701471deb1b4d258e",
}


def pcm16_signal(channels):
    samples = []
    for frame in range(FRAME_COUNT):
        for channel in range(channels):
            samples.append(((frame * (29 + channel * 7) + channel * 101) % 1600) - 800)
    return samples


def pcm16_bytes(channels):
    samples = pcm16_signal(channels)
    return struct.pack("<" + "h" * len(samples), *samples)


def float32_bytes():
    samples = [sample / 1000.0 for sample in pcm16_signal(1)]
    return struct.pack("<" + "f" * len(samples), *samples)


def write_wave(path, format_tag, channels, sample_bytes, bits_per_sample):
    byte_rate = SAMPLE_RATE * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(sample_bytes),
        b"WAVE",
        b"fmt ",
        16,
        format_tag,
        channels,
        SAMPLE_RATE,
        byte_rate,
        block_align,
        bits_per_sample,
        b"data",
        len(sample_bytes),
    )
    path.write_bytes(header + sample_bytes)


def encode(ffmpeg, output, codec, channels):
    command = [
        str(ffmpeg), "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", str(channels), "-i", "pipe:0",
        "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact",
    ]
    if codec == "flac":
        command += ["-c:a", "flac", "-compression_level", "5", "-f", "flac"]
    elif codec == "vorbis":
        command += ["-c:a", "libvorbis", "-q:a", "2", "-f", "ogg"]
    elif codec == "mp3":
        command += ["-c:a", "libmp3lame", "-b:a", "32k", "-write_xing", "0", "-id3v2_version", "0", "-f", "mp3"]
    else:
        raise ValueError("unknown codec: " + codec)
    command.append(str(output))
    result = subprocess.run(command, input=pcm16_bytes(channels), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError("FFmpeg failed for " + output.name + ": " + result.stderr.decode("utf-8", "replace"))


def generate(output, ffmpeg):
    output.mkdir(parents=True, exist_ok=True)
    write_wave(output / "pcm16_stereo.wav", 1, 2, pcm16_bytes(2), 16)
    write_wave(output / "float32_mono.wav", 3, 1, float32_bytes(), 32)
    write_wave(output / "pcm16_three_channel.wav", 1, 3, pcm16_bytes(3), 16)
    encode(ffmpeg, output / "flac_mono.flac", "flac", 1)
    encode(ffmpeg, output / "flac_stereo.flac", "flac", 2)
    encode(ffmpeg, output / "vorbis_mono.ogg", "vorbis", 1)
    encode(ffmpeg, output / "vorbis_stereo.ogg", "vorbis", 2)
    encode(ffmpeg, output / "vorbis_three_channel.ogg", "vorbis", 3)
    encode(ffmpeg, output / "mp3_mono.mp3", "mp3", 1)


def fixture_hashes(output):
    return {name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in sorted(EXPECTED_SHA256)}


def verify(output):
    actual = fixture_hashes(output)
    for name in sorted(EXPECTED_SHA256):
        expected = EXPECTED_SHA256[name]
        if not expected:
            raise RuntimeError("missing expected SHA-256 for " + name)
        if actual[name] != expected:
            raise RuntimeError(name + " SHA-256 is " + actual[name] + ", expected " + expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", type=pathlib.Path, required=True, help="Path to the recorded FFmpeg executable")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT, help="Directory for generated fixtures")
    parser.add_argument("--print-hashes", action="store_true", help="Print SHA-256 values after generation")
    parser.add_argument("--verify", action="store_true", help="Verify generated files against checked-in hashes")
    args = parser.parse_args()
    if not args.ffmpeg.is_file():
        parser.error("--ffmpeg must name an executable file")
    generate(args.output, args.ffmpeg)
    if args.print_hashes:
        for name, value in sorted(fixture_hashes(args.output).items()):
            print(name + " " + value)
    if args.verify:
        verify(args.output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print("fixture generation failed: " + str(error), file=sys.stderr)
        sys.exit(1)