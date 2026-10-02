"""Verify pinned Phase 1B decoder source and license provenance without writes."""

import argparse
import hashlib
import pathlib
import sys
import tempfile


DECODERS = (
    {
        "name": "miniaudio",
        "tag": "0.11.25",
        "commit": "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d",
        "source": "src/sound/thirdparty/miniaudio/miniaudio.h",
        "license": "src/sound/thirdparty/miniaudio/LICENSE",
        "license_identifier": "MIT-0",
    },
    {
        "name": "stb_vorbis",
        "commit": "1ee679ca2ef753a528db5ba6801e1067b40481b8",
        "source": "src/sound/thirdparty/stb/stb_vorbis.c",
        "license": "src/sound/thirdparty/stb/LICENSE",
        "license_identifier": "MIT",
    },
)
PROVENANCE = "src/sound/thirdparty/audio-decoders.md"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decoder_records(source_root):
    provenance = source_root / PROVENANCE
    text = provenance.read_text(encoding="utf-8")
    records = []
    for decoder in DECODERS:
        source = source_root / decoder["source"]
        license_file = source_root / decoder["license"]
        source_hash = digest(source)
        license_hash = digest(license_file)
        for value in (decoder["commit"], source_hash, license_hash, decoder["license_identifier"]):
            if value not in text:
                raise ValueError(decoder["name"] + " provenance does not match source files")
        records.append(
            {
                "name": decoder["name"],
                "tag": decoder.get("tag"),
                "commit": decoder["commit"],
                "source": {"path": decoder["source"], "sha256": source_hash},
                "license": {
                    "identifier": decoder["license_identifier"],
                    "path": decoder["license"],
                    "sha256": license_hash,
                },
            }
        )
    return records, provenance


def verify_source(args):
    decoders = decoder_records(args.source_root.resolve())[0]
    print("Phase 1B source provenance passed: " + ", ".join(decoder["name"] for decoder in decoders))


def expect_rejected(action, description):
    try:
        action()
    except (OSError, ValueError):
        return
    raise ValueError("self-test accepted " + description)


def self_test():
    source_root = pathlib.Path(__file__).resolve().parents[1]
    decoders, provenance = decoder_records(source_root)
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        paths = [PROVENANCE] + [decoder[key] for decoder in DECODERS for key in ("source", "license")]
        for relative in paths:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((source_root / relative).read_bytes())
        decoder_records(root)
        for decoder in DECODERS:
            for key in ("source", "license"):
                path = root / decoder[key]
                original = path.read_bytes()
                path.write_bytes(original + b"\nself-test mismatch\n")
                expect_rejected(lambda: decoder_records(root), decoder["name"] + " " + key + " hash mismatch")
                path.unlink()
                expect_rejected(lambda: decoder_records(root), decoder["name"] + " missing " + key)
                path.write_bytes(original)
        text = provenance.read_text(encoding="utf-8")
        for decoder in decoders:
            for value in (decoder["commit"], decoder["source"]["sha256"], decoder["license"]["sha256"], decoder["license"]["identifier"]):
                (root / PROVENANCE).write_text(text.replace(value, "missing-pin"), encoding="utf-8")
                expect_rejected(lambda: decoder_records(root), decoder["name"] + " missing provenance pin")
        (root / PROVENANCE).write_text(text, encoding="utf-8")
        decoder_records(root)
    print("Phase 1B source self-test passed: source/license/hash/provenance mismatches rejected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command")
    verify = commands.add_parser("verify-source")
    verify.add_argument("--source-root", type=pathlib.Path, required=True)
    verify.set_defaults(handler=verify_source)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.command is None:
        parser.error("a command is required unless --self-test is used")
    args.handler(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print("Phase 1B audio gate failed: " + str(error), file=sys.stderr)
        sys.exit(1)