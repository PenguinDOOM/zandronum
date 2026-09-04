"""Generate deterministic, self-authored Phase 1B music fixture files."""

import argparse
import hashlib
import pathlib
import struct
import sys
import zipfile


ROOT = pathlib.Path(__file__).resolve().parent
VORBIS_SOURCE = "vorbis_mono.ogg"
VORBIS_SHA256 = "170b683a66ac709b14d2678ab3317b47871a5533c498aa12db723020a74f9d88"
FIXTURE_NAMES = (
    "phase1b-generated.mid",
    "phase1b-ogg-sample.xm",
    "phase1b-dosbox-raw-opl.dro",
    "phase1b-mod.mod",
    "phase1b-gme.vgm",
    "phase1b-audio.wad",
    "phase1b-fixtures.pk3",
)
EXPECTED_SHA256 = {
    "phase1b-generated.mid": "f71c35cad946d4eddcd11bf9b47a7c8048b045926db77c22d91a4ac0b30980fb",
    "phase1b-ogg-sample.xm": "55e6b848d85777f46891322e9504b6f45e76693427c24f2d9ca578b25cb8eed9",
    "phase1b-dosbox-raw-opl.dro": "b35680580629b7032f523bdfeb340effbb344edd33bb870bec3a3b5a500fde1f",
    "phase1b-mod.mod": "30a8ada7ae0ea435e144598e356777c9fe994419d118959765522a18086a8b16",
    "phase1b-gme.vgm": "18772548b07096ac6635225f44a60ecd46bd60f8f9357a53f97f69b831971f81",
    "phase1b-audio.wad": "edf900ce2b7abfdc16844ed4e880b81bb4da91e7dcc738b8375dc6c64dcc6c27",
    "phase1b-fixtures.pk3": "e3e2b420c7da6b2afb43fb4c9c260fcdd827f5b1da53710b3336ccf24081800e",
}


def pad(text, length):
    return text.encode("ascii")[:length].ljust(length, b"\0")


def make_midi():
    track = b"\x00\xff\x51\x03\x07\xa1\x20\x00\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 96) + b"MTrk" + struct.pack(">I", len(track)) + track


def make_dosbox_raw_opl():
    commands = bytes((0x00, 0x00, 0x00, 0x00))
    return b"DBRAWOPL" + struct.pack("<HHIII", 0, 1, 0, len(commands), 1) + commands + bytes(4)


def make_mod():
    module = bytearray(1084 + 1024)
    module[:20] = pad("Phase1B MOD", 20)
    module[950] = 1
    module[952:1080] = bytes(128)
    module[1080:1084] = b"M.K."
    return bytes(module)


def make_vgm():
    music = bytearray(0x41)
    music[:4] = b"Vgm "
    struct.pack_into("<I", music, 4, len(music) - 4)
    struct.pack_into("<I", music, 8, 0x00000150)
    music[0x40] = 0x66
    return bytes(music)


def validate_vgm(data):
    if len(data) < 0x41 or data[:4] != b"Vgm ":
        raise RuntimeError("VGM header is invalid")
    if struct.unpack_from("<I", data, 8)[0] != 0x00000150:
        raise RuntimeError("VGM version is not 1.50")
    data_offset = struct.unpack_from("<I", data, 0x34)[0]
    stream_offset = 0x40 if data_offset == 0 else 0x34 + data_offset
    if stream_offset >= len(data) or data[stream_offset] != 0x66:
        raise RuntimeError("VGM command stream does not start with an end marker")


def make_xm(vorbis):
    header = bytearray()
    header += b"Extended Module: " + pad("Phase1B Ogg Sample", 20) + b"\x1a" + pad("Zandronum fixture", 20)
    header += struct.pack("<HI8H", 0x0104, 276, 1, 0, 1, 1, 1, 0, 6, 125)
    header += bytes(256)

    pattern = struct.pack("<IBHH", 9, 0, 1, 5) + bytes((49, 1, 0, 0, 0))
    instrument = bytearray()
    instrument += struct.pack("<I", 243) + pad("Ogg sample", 22) + b"\0" + struct.pack("<H", 1)
    instrument += struct.pack("<I", 40) + bytes(96) + bytes(48 + 48 + 14) + struct.pack("<HH", 0, 0)
    sample_data = struct.pack("<I", 529) + vorbis
    instrument += struct.pack("<IIIBbBBBB22s", len(sample_data), 0, 0, 64, 0, 0, 128, 0, 0, pad("Ogg Vorbis", 22))
    return bytes(header + pattern + instrument + sample_data)


def make_wad(entries):
    data_offset = 12
    directory_offset = data_offset + sum(len(data) for _, data in entries)
    payload = bytearray(b"PWAD" + struct.pack("<II", len(entries), directory_offset))
    directory = bytearray()
    offset = data_offset
    for name, data in entries:
        payload += data
        directory += struct.pack("<II8s", offset, len(data), pad(name, 8))
        offset += len(data)
    return bytes(payload + directory)


def write_archive(path, entries):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in entries:
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_STORED
            archive.writestr(info, data)


def generate(output):
    vorbis_path = output / VORBIS_SOURCE
    if not vorbis_path.is_file():
        raise RuntimeError("missing required source fixture " + VORBIS_SOURCE)
    vorbis = vorbis_path.read_bytes()
    if hashlib.sha256(vorbis).hexdigest() != VORBIS_SHA256:
        raise RuntimeError(VORBIS_SOURCE + " SHA-256 does not match the documented source fixture")

    files = {
        "phase1b-generated.mid": make_midi(),
        "phase1b-ogg-sample.xm": make_xm(vorbis),
        "phase1b-dosbox-raw-opl.dro": make_dosbox_raw_opl(),
        "phase1b-mod.mod": make_mod(),
        "phase1b-gme.vgm": make_vgm(),
    }
    validate_vgm(files["phase1b-gme.vgm"])
    wad_entries = (
        ("D_MIDI", files["phase1b-generated.mid"]),
        ("D_XMOGG", files["phase1b-ogg-sample.xm"]),
        ("D_OPL", files["phase1b-dosbox-raw-opl.dro"]),
        ("D_MOD", files["phase1b-mod.mod"]),
        ("D_GME", files["phase1b-gme.vgm"]),
    )
    files["phase1b-audio.wad"] = make_wad(wad_entries)
    for name, data in files.items():
        (output / name).write_bytes(data)
    archive_entries = tuple(
        (name if name == "phase1b-audio.wad" else "music/" + name, data)
        for name, data in files.items()
    )
    write_archive(output / "phase1b-fixtures.pk3", archive_entries)


def print_hashes(output):
    for name in FIXTURE_NAMES:
        print(name + " " + hashlib.sha256((output / name).read_bytes()).hexdigest())


def verify(output):
    for name, expected in EXPECTED_SHA256.items():
        actual = hashlib.sha256((output / name).read_bytes()).hexdigest()
        if actual != expected:
            raise RuntimeError(name + " SHA-256 is " + actual + ", expected " + expected)
    validate_vgm((output / "phase1b-gme.vgm").read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, default=ROOT, help="Directory for generated fixtures")
    parser.add_argument("--print-hashes", action="store_true", help="Print fixture SHA-256 values")
    parser.add_argument("--verify", action="store_true", help="Verify generated fixtures against checked-in hashes")
    args = parser.parse_args()
    generate(args.output)
    if args.print_hashes:
        print_hashes(args.output)
    if args.verify:
        verify(args.output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print("fixture generation failed: " + str(error), file=sys.stderr)
        sys.exit(1)