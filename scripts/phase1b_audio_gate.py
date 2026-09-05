"""Phase 1B package provenance and validation-manifest gates."""

import argparse
import copy
import hashlib
import json
import pathlib
import re
import shutil
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
        "package_license": "miniaudio-LICENSE.txt",
    },
    {
        "name": "stb_vorbis",
        "commit": "1ee679ca2ef753a528db5ba6801e1067b40481b8",
        "source": "src/sound/thirdparty/stb/stb_vorbis.c",
        "license": "src/sound/thirdparty/stb/LICENSE",
        "license_identifier": "MIT",
        "package_license": "stb_vorbis-LICENSE.txt",
    },
)
PROVENANCE = "src/sound/thirdparty/audio-decoders.md"
CODEC_RUNTIME_PATTERN = re.compile(
    r"^(?:(?:lib)?(?:avcodec|avformat|avutil|flac|mad|mp3lame|mpg123|ogg|sndfile|stb[_-]?vorbis|vorbis(?:file)?)|miniaudio)(?:[-_][a-z0-9._-]+)?\.(?:dll|dylib|so(?:\.\d+)*)$",
    re.IGNORECASE,
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def is_relative_to(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def safe_destination(root, user_path, description):
    value = str(user_path)
    posix = pathlib.PurePosixPath(value)
    windows = pathlib.PureWindowsPath(value)
    if posix.is_absolute() or windows.is_absolute() or ".." in posix.parts or ".." in windows.parts:
        raise ValueError(description + " must be a relative path without traversal")
    root = root.resolve()
    destination = (root / pathlib.Path(value)).resolve(strict=False)
    if not is_relative_to(destination, root):
        raise ValueError(description + " escapes its allowed root")
    return destination


def output_destination(user_path):
    return safe_destination(pathlib.Path.cwd(), user_path, "output")


def record(root, path):
    relative = path.relative_to(root).as_posix()
    if pathlib.PurePosixPath(relative).is_absolute() or ".." in pathlib.PurePosixPath(relative).parts:
        raise ValueError("non-relative package path: " + relative)
    return {"path": relative, "sha256": digest(path)}


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


def package_gate(args):
    root = args.package.resolve()
    source_root = args.source_root.resolve()
    manifest_path = safe_destination(root, args.manifest_name, "manifest name")
    notice_directory = safe_destination(root, args.notice_directory, "notice directory")
    if not manifest_path.is_file():
        raise ValueError("missing existing runtime manifest: " + str(manifest_path))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("sound_enabled") is not args.sound_enabled:
        raise ValueError("runtime manifest sound_enabled disagrees with package gate")

    runtime_files = [path for path in root.rglob("*") if path.is_file() and CODEC_RUNTIME_PATTERN.fullmatch(path.name)]
    if runtime_files:
        raise ValueError("unexpected codec runtime: " + ", ".join(path.relative_to(root).as_posix() for path in runtime_files))

    files = manifest.setdefault("files", {})
    if not args.sound_enabled:
        manifest["schema"] = "phase1b-runtime-manifest"
        manifest["schema_version"] = 2
        manifest["decoder_provenance"] = []
        manifest["codec_runtime_assertion"] = {"present": False, "patterns": [CODEC_RUNTIME_PATTERN.pattern]}
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return

    decoders, provenance = decoder_records(source_root)
    notice_directory.mkdir(parents=True, exist_ok=True)
    packaged_notices = []
    for decoder in DECODERS:
        destination = notice_directory / decoder["package_license"]
        shutil.copyfile(source_root / decoder["license"], destination)
        packaged_notices.append(destination)
    provenance_destination = notice_directory / "audio-decoders.md"
    shutil.copyfile(provenance, provenance_destination)
    packaged_notices.append(provenance_destination)

    existing_notices = [root / item["path"] for item in files.get("license_notice", ())]
    notices = existing_notices + packaged_notices
    if any(not path.is_file() for path in notices):
        raise ValueError("runtime manifest references a missing license notice")
    files["license_notice"] = [record(root, path) for path in sorted(set(notices))]
    manifest["schema"] = "phase1b-runtime-manifest"
    manifest["schema_version"] = 2
    manifest["decoder_provenance"] = decoders
    manifest["codec_runtime_assertion"] = {"present": False, "patterns": [CODEC_RUNTIME_PATTERN.pattern]}
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def verify_record(root, value, description):
    if not isinstance(value, dict):
        raise ValueError(description + " record is invalid")
    path = safe_destination(root, value.get("path"), description)
    if not path.is_file() or value.get("sha256") != digest(path):
        raise ValueError(description + " hash mismatch")
    return path


def verify_existing_package(root, source_root, manifest_name, sound_enabled):
    manifest_path = safe_destination(root, manifest_name, "manifest name")
    if not manifest_path.is_file():
        raise ValueError("missing existing runtime manifest: " + str(manifest_path))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("sound_enabled") is not sound_enabled:
        raise ValueError("runtime manifest sound_enabled disagrees with package verification")
    runtime_files = [path for path in root.rglob("*") if path.is_file() and CODEC_RUNTIME_PATTERN.fullmatch(path.name)]
    if runtime_files:
        raise ValueError("unexpected codec runtime: " + ", ".join(path.relative_to(root).as_posix() for path in runtime_files))
    assertion = {"present": False, "patterns": [CODEC_RUNTIME_PATTERN.pattern]}
    if manifest.get("schema") != "phase1b-runtime-manifest" or manifest.get("schema_version") != 2:
        raise ValueError("runtime manifest is not a Phase 1B package manifest")
    if manifest.get("codec_runtime_assertion") != assertion:
        raise ValueError("runtime manifest codec runtime assertion is invalid")
    if not sound_enabled:
        if manifest.get("decoder_provenance") != []:
            raise ValueError("no-sound package declares decoder provenance")
        return

    decoders, provenance = decoder_records(source_root)
    if manifest.get("decoder_provenance") != decoders:
        raise ValueError("runtime manifest decoder provenance is invalid")
    notices = manifest.get("files", {}).get("license_notice")
    if not isinstance(notices, list):
        raise ValueError("runtime manifest license notices are missing")
    expected_notices = {
        decoder["package_license"]: digest(source_root / decoder["license"])
        for decoder in DECODERS
    }
    expected_notices["audio-decoders.md"] = digest(provenance)
    found_notices = {}
    for notice in notices:
        path = verify_record(root, notice, "runtime manifest license notice")
        found_notices[path.name] = notice["sha256"]
    if any(found_notices.get(name) != expected_hash for name, expected_hash in expected_notices.items()):
        raise ValueError("runtime manifest decoder notices or provenance hashes are invalid")


def verify_package(args):
    client = args.client_exe.resolve()
    if not client.is_file():
        raise ValueError("client executable is unavailable: " + str(client))
    verify_existing_package(client.parent, args.source_root.resolve(), args.manifest_name, args.sound_enabled)


def replace_placeholders(value, replacements):
    if isinstance(value, str):
        for placeholder, replacement in replacements.items():
            value = value.replace(placeholder, replacement)
        return value
    if isinstance(value, list):
        return [replace_placeholders(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: replace_placeholders(item, replacements) for key, item in value.items()}
    return value


def resolve_validation(args):
    package_value = getattr(args, "package_exe", None)
    if package_value is None:
        raise ValueError("package executable is required")
    package = package_value.resolve()
    client = args.client_exe.resolve()
    testdata = args.testdata.resolve()
    if not client.is_file():
        raise ValueError("client executable is unavailable: " + str(client))
    if not package.is_file():
        raise ValueError("package executable is unavailable: " + str(package))
    if not testdata.is_dir():
        raise ValueError("testdata directory is unavailable: " + str(testdata))
    source = args.manifest.resolve()
    document = json.loads(source.read_text(encoding="utf-8"))
    resolved = replace_placeholders(copy.deepcopy(document), {
        "${CLIENT_EXE}": str(client),
        "${PACKAGE_EXE}": str(package),
        "${TESTDATA}": str(testdata),
    })
    resolved["resolved_by"] = "scripts/phase1b_audio_gate.py"
    resolved["resolved_placeholders"] = {
        "${CLIENT_EXE}": str(client),
        "${PACKAGE_EXE}": str(package),
        "${TESTDATA}": str(testdata),
        "${STOCK_PK3}": "unresolved: operator input required",
        "${REFERENCE_SERVER}": "unresolved: operator input required",
    }
    output = output_destination(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(resolved, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def validation_inputs(args):
    document = {
        "schema": "phase1b-validation-inputs",
        "schema_version": 1,
        "stock_pk3": {"status": "blocked", "reason": "operator must provide matching stock PK3 path and SHA-256"},
        "reference_server": {"status": "blocked", "reason": "operator must provide reference server invocation or endpoint"},
    }
    output = output_destination(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def expect_rejected(action, description):
    try:
        action()
    except ValueError:
        return
    raise ValueError("self-test accepted " + description)


def validate_codec_runtime_pattern():
    rejected = ("libmpg123.so.0", "libavcodec.so.60", "mpg123.dll", "AVCODEC-60.DLL")
    accepted = ("OpenAL32.dll", "zandronum.pk3", "audio-codec-notes.md", "libopenal.so.1")
    for name in rejected:
        if not CODEC_RUNTIME_PATTERN.fullmatch(name):
            raise ValueError("codec runtime policy missed " + name)
    for name in accepted:
        if CODEC_RUNTIME_PATTERN.fullmatch(name):
            raise ValueError("codec runtime policy rejected harmless " + name)


def validate_path_safety():
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory) / "root"
        root.mkdir()
        outside = pathlib.Path(directory) / "outside"
        expect_rejected(lambda: safe_destination(root, "../outside/file", "test"), "parent traversal")
        expect_rejected(lambda: safe_destination(root, str(outside), "test"), "absolute destination")
        link = root / "link"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError:
            pass
        else:
            expect_rejected(lambda: safe_destination(root, "link/file", "test"), "symlink escape")
        if outside.exists():
            raise ValueError("self-test created an outside directory")


def validate_package_fixture(source_root, decoders, provenance):
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory) / "package"
        root.mkdir()
        client = root / "zandronum.exe"
        client.touch()
        notices = []
        for decoder in DECODERS:
            notice = root / decoder["package_license"]
            shutil.copyfile(source_root / decoder["license"], notice)
            notices.append(record(root, notice))
        provenance_notice = root / "audio-decoders.md"
        shutil.copyfile(provenance, provenance_notice)
        notices.append(record(root, provenance_notice))
        manifest = {
            "schema": "phase1b-runtime-manifest",
            "schema_version": 2,
            "sound_enabled": True,
            "files": {"license_notice": notices},
            "decoder_provenance": decoders,
            "codec_runtime_assertion": {"present": False, "patterns": [CODEC_RUNTIME_PATTERN.pattern]},
        }
        (root / "phase1a-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        verify_existing_package(root, source_root, "phase1a-runtime-manifest.json", True)
        (root / "mpg123.dll").touch()
        expect_rejected(lambda: verify_existing_package(root, source_root, "phase1a-runtime-manifest.json", True), "codec runtime package")
        (root / "mpg123.dll").unlink()
        manifest["files"]["license_notice"][-1]["sha256"] = "0" * 64
        (root / "phase1a-runtime-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        expect_rejected(lambda: verify_existing_package(root, source_root, "phase1a-runtime-manifest.json", True), "notice hash package")


def self_test():
    validate_codec_runtime_pattern()
    expect_rejected(lambda: resolve_validation(argparse.Namespace(package_exe=None)), "missing package executable input")
    validate_path_safety()
    source_root = pathlib.Path(__file__).resolve().parents[1]
    decoders, provenance = decoder_records(source_root)
    validate_package_fixture(source_root, decoders, provenance)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command")
    package = commands.add_parser("package")
    package.add_argument("--package", type=pathlib.Path, required=True)
    package.add_argument("--source-root", type=pathlib.Path, required=True)
    package.add_argument("--manifest-name", default="phase1a-runtime-manifest.json")
    package.add_argument("--notice-directory", default=".")
    package.add_argument("--sound-enabled", action="store_true")
    package.set_defaults(handler=package_gate)
    verify = commands.add_parser("verify-package")
    verify.add_argument("--client-exe", type=pathlib.Path, required=True)
    verify.add_argument("--source-root", type=pathlib.Path, required=True)
    verify.add_argument("--manifest-name", default="phase1a-runtime-manifest.json")
    verify.add_argument("--sound-enabled", action="store_true")
    verify.set_defaults(handler=verify_package)
    resolved = commands.add_parser("resolve-validation")
    resolved.add_argument("--manifest", type=pathlib.Path, required=True)
    resolved.add_argument("--client-exe", type=pathlib.Path, required=True)
    resolved.add_argument("--package-exe", type=pathlib.Path, required=True)
    resolved.add_argument("--testdata", type=pathlib.Path, required=True)
    resolved.add_argument("--output", type=pathlib.Path, required=True)
    resolved.set_defaults(handler=resolve_validation)
    inputs = commands.add_parser("validation-inputs")
    inputs.add_argument("--output", type=pathlib.Path, required=True)
    inputs.set_defaults(handler=validation_inputs)
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
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("Phase 1B audio gate failed: " + str(error), file=sys.stderr)
        sys.exit(1)