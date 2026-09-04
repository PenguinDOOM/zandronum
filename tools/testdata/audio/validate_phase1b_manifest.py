"""Validate Phase 1B manifest fields, fixture hashes, PK3 entries, and WAD lumps."""

import argparse
import hashlib
import json
import pathlib
import re
import struct
import sys
import tempfile
import zipfile


ROOT = pathlib.Path(__file__).resolve().parent
REPOSITORY_ROOT = ROOT.parents[2]
PLACEHOLDERS = ("${CLIENT_EXE}", "${TESTDATA}", "${STOCK_PK3}", "${REFERENCE_SERVER}")
ROW_FIELDS = ("id", "required", "fixture", "source_mode", "portable_placeholders", "backend", "settings", "commands", "expected", "resource_assertions", "diagnostic", "external_prerequisites")
FOCUSED_BUILD_COMMAND = "cmake --build build-v143-openal --config Release --target openal_lifecycle_tests"
FOCUSED_EXECUTABLE_PATH = pathlib.PurePosixPath("build-v143-openal") / "tools" / "Release" / "openal_lifecycle_tests.exe"
FOCUSED_OPERATIONS = {
    "phase1b-direct-memory": ("direct-memory", "vorbis_mono.ogg", None, "--phase1b-direct-memory"),
    "phase1b-file-slice": ("file-slice", "float32_mono.wav", {"offset": 0, "length": 2160}, "--phase1b-file-slice"),
}
NEGATIVE_CLI_SETTING_PATTERN = re.compile(r"(?:^|\s)\+set\s+\S+\s+-\d+(?=\s|$)")
MIDI_FALLBACK_DIAGNOSTIC_CLASS = "fmod-midi-unavailable-remapped-to-opl"
MIDI_FALLBACK_DIAGNOSTIC_TEXT = "FMOD MIDI playback is unavailable with the current sound backend; using OPL instead."
MIDI_RUNTIME_ACTIONS = [
    {"phase": "after-renderer-initialization", "kind": "engine-console", "command": "stopmus"},
    {"phase": "after-renderer-initialization", "kind": "engine-console", "command": "set snd_mididevice -1"},
    {"phase": "after-renderer-initialization", "kind": "operator-harness", "action": "begin-external-diagnostic-capture", "scope": "subsequent console or log output only; exclude earlier process output"},
    {"phase": "after-renderer-initialization", "kind": "engine-console", "command": "changemus D_MIDI"},
]
MIDI_DIAGNOSTIC_CONTRACT = {
    "count": 1,
    "text_class": MIDI_FALLBACK_DIAGNOSTIC_CLASS,
    "required_substring": MIDI_FALLBACK_DIAGNOSTIC_TEXT,
    "observation_window": {
        "starts_after": "set snd_mididevice -1",
        "starts_with": "begin-external-diagnostic-capture",
        "ends_after": "changemus D_MIDI",
    },
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def command_strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for nested in value.values():
            yield from command_strings(nested)
    elif isinstance(value, list):
        for nested in value:
            yield from command_strings(nested)


def focused_run_command(argument):
    return FOCUSED_EXECUTABLE_PATH.as_posix() + " " + argument


def validate_focused_target_contract():
    cmake_file = REPOSITORY_ROOT / "tools" / "CMakeLists.txt"
    source_file = REPOSITORY_ROOT / "tools" / "openal_renderer_lifecycle_tests.cpp"
    cmake_text = cmake_file.read_text(encoding="utf-8")
    target_pattern = r"add_executable\s*\(\s*openal_lifecycle_tests\s+openal_renderer_lifecycle_tests\.cpp"
    if not source_file.is_file() or not re.search(target_pattern, cmake_text):
        raise ValueError("openal_lifecycle_tests CMake target/source contract is missing")
    executable = REPOSITORY_ROOT / pathlib.Path(FOCUSED_EXECUTABLE_PATH)
    if executable.exists() and not executable.is_file():
        raise ValueError("focused lifecycle target path is not an executable file")


def wad_lumps(data):
    if len(data) < 12 or data[:4] not in (b"IWAD", b"PWAD"):
        raise ValueError("not a WAD")
    count, directory = struct.unpack_from("<II", data, 4)
    if directory + count * 16 > len(data):
        raise ValueError("WAD directory outside file")
    result = {}
    for index in range(count):
        offset, length, name = struct.unpack_from("<II8s", data, directory + index * 16)
        if offset + length > len(data):
            raise ValueError("WAD lump outside file")
        result[name.rstrip(b"\0").decode("ascii")] = data[offset:offset + length]
    return result


def validate_vgm(data):
    if len(data) < 0x41 or data[:4] != b"Vgm " or struct.unpack_from("<I", data, 8)[0] != 0x00000150:
        raise ValueError("invalid VGM header")
    data_offset = struct.unpack_from("<I", data, 0x34)[0]
    stream_offset = 0x40 if data_offset == 0 else 0x34 + data_offset
    if stream_offset >= len(data) or data[stream_offset] != 0x66:
        raise ValueError("VGM end marker is not at the command-stream start")


def validate_fixture(root, fixture):
    path = root / fixture["path"]
    if not path.is_file():
        raise ValueError("missing fixture " + fixture["path"])
    data = path.read_bytes()
    if sha256(data) != fixture["sha256"]:
        raise ValueError("fixture hash mismatch for " + fixture["path"])
    if "slice" in fixture:
        offset = fixture["slice"]["offset"]
        length = fixture["slice"]["length"]
        if offset < 0 or length <= 0 or offset + length > len(data):
            raise ValueError("invalid slice for " + fixture["path"])
    if "archive_entry" not in fixture:
        return
    with zipfile.ZipFile(path) as archive:
        wad_entries = [name for name in archive.namelist() if name.lower().endswith(".wad")]
        if fixture["archive_entry"] != "phase1b-audio.wad" or "/" in fixture["archive_entry"]:
            raise ValueError("embedded WAD must use the PK3 root phase1b-audio.wad path")
        if wad_entries != [fixture["archive_entry"]]:
            raise ValueError("PK3 contains a nested or unexpected embedded WAD path")
        entry = archive.read(fixture["archive_entry"])
    if sha256(entry) != fixture["archive_entry_sha256"]:
        raise ValueError("archive-entry hash mismatch for " + fixture["identity"])
    if "lump" in fixture:
        lumps = wad_lumps(entry)
        if fixture["lump"] not in lumps or sha256(lumps[fixture["lump"]]) != fixture["lump_sha256"]:
            raise ValueError("WAD lump mismatch for " + fixture["identity"])


def validate_midi_runtime_handoff(row, commands, negative_settings, diagnostic):
    actions = commands.get("post_init_actions")
    if "post_init_console_actions" in commands or actions != MIDI_RUNTIME_ACTIONS:
        raise ValueError(row["id"] + " has an invalid MIDI isolation sequence")
    if negative_settings != {"snd_mididevice": -1} or diagnostic != MIDI_DIAGNOSTIC_CONTRACT:
        raise ValueError(row["id"] + " lacks the FMOD-to-OPL fallback diagnostic observation contract")


def validate_row(root, row):
    missing = [field for field in ROW_FIELDS if field not in row]
    if missing:
        raise ValueError(row.get("id", "<unknown>") + " missing " + ", ".join(missing))
    if row["required"] is not True or tuple(row["portable_placeholders"]) != PLACEHOLDERS:
        raise ValueError(row["id"] + " has an incomplete portable contract")
    commands = row["commands"]
    if not isinstance(commands, dict):
        raise ValueError(row["id"] + " has incomplete commands")
    if row["backend"] != "openal" or not isinstance(row["settings"], dict):
        raise ValueError(row["id"] + " has an incomplete OpenAL contract")
    command_text = "\n".join(command_strings(commands))
    if "<" in command_text or ">" in command_text:
        raise ValueError(row["id"] + " uses a pseudo operation or runtime backend switch")
    if NEGATIVE_CLI_SETTING_PATTERN.search(command_text):
        raise ValueError(row["id"] + " uses a negative command-line setting that the engine does not parse")
    for token in re.findall(r"\$\{[^}]+\}", command_text):
        if token not in row["portable_placeholders"]:
            raise ValueError(row["id"] + " uses an undeclared placeholder")
    expected_contract = row["expected"]
    diagnostic = row["diagnostic"]
    if not isinstance(expected_contract, dict) or not all(isinstance(expected_contract.get(field), str) and expected_contract[field] for field in ("observation", "tolerance")):
        raise ValueError(row["id"] + " has an incomplete observation contract")
    if not isinstance(row["resource_assertions"], list) or not row["resource_assertions"] or not all(isinstance(value, str) and value for value in row["resource_assertions"]):
        raise ValueError(row["id"] + " has incomplete resource assertions")
    if not isinstance(diagnostic, dict) or not isinstance(diagnostic.get("count"), int) or diagnostic["count"] < 0 or not isinstance(diagnostic.get("text_class"), str):
        raise ValueError(row["id"] + " has an incomplete diagnostic contract")
    if row["source_mode"] in ("direct-memory", "file-slice"):
        operation = commands.get("focused_test")
        expected = FOCUSED_OPERATIONS.get(row["id"])
        build = operation.get("build") if isinstance(operation, dict) else None
        run = operation.get("run") if isinstance(operation, dict) else None
        if expected is None or row["source_mode"] != expected[0] or row["fixture"].get("path") != expected[1] or row["fixture"].get("slice") != expected[2] or not isinstance(build, dict) or not isinstance(run, dict) or build.get("target") != "openal_lifecycle_tests" or build.get("command") != FOCUSED_BUILD_COMMAND or run.get("executable") != "openal_lifecycle_tests" or run.get("operation") != row["id"] or run.get("command") != focused_run_command(expected[3]):
            raise ValueError(row["id"] + " lacks a runnable focused lifecycle test")
        if "launch" in commands or "console" in commands:
            raise ValueError(row["id"] + " mixes focused and game-runtime actions")
    else:
        launch = commands.get("launch")
        lump = row["fixture"].get("lump")
        actions = commands.get("post_init_actions" if row["id"] == "generated-midi-pk3-wad-lump" else "post_init_console_actions")
        required_launch = "\"${CLIENT_EXE}\" -file \"${TESTDATA}/phase1b-fixtures.pk3\" \"${STOCK_PK3}\" +set snd_backend openal"
        if not isinstance(launch, str) or not lump or not launch.startswith(required_launch) or "+changemus" in launch or not isinstance(actions, list):
            raise ValueError(row["id"] + " has an incomplete post-init OpenAL action contract")
        negative_settings = {setting: value for setting, value in row["settings"].items()
                             if isinstance(value, int) and not isinstance(value, bool) and value < 0}
        if row["id"] == "generated-midi-pk3-wad-lump":
            validate_midi_runtime_handoff(row, commands, negative_settings, diagnostic)
        else:
            expected_actions = [{"phase": "after-renderer-initialization", "command": "set " + setting + " " + str(value)}
                                for setting, value in negative_settings.items()]
            expected_actions.append({"phase": "after-renderer-initialization", "command": "changemus " + lump})
            if len(actions) != 1:
                raise ValueError(row["id"] + " must have exactly one post-init OpenAL changemus action")
            if actions != expected_actions:
                raise ValueError(row["id"] + " must apply negative settings before exactly one post-init OpenAL changemus action")
        for setting, value in row["settings"].items():
            if setting in negative_settings:
                continue
            if "+set " + setting + " " + str(value).lower() not in launch:
                raise ValueError(row["id"] + " does not apply its OpenAL startup setting")
        requires_server = row["id"] == "generated-gme-vgm"
        if ("${REFERENCE_SERVER}" in command_text) != requires_server or ("+connect ${REFERENCE_SERVER}" in launch) != requires_server:
            raise ValueError(row["id"] + " has an invalid local-playback or compatibility-server contract")
        if "focused_test" in commands:
            raise ValueError(row["id"] + " incorrectly claims focused-test coverage")
    validate_fixture(root, row["fixture"])
    if row["id"] == "generated-gme-vgm":
        with zipfile.ZipFile(root / row["fixture"]["path"]) as archive:
            validate_vgm(wad_lumps(archive.read(row["fixture"]["archive_entry"]))[row["fixture"]["lump"]])


def validate_manifest(manifest, testdata):
    document = json.loads(manifest.read_text(encoding="utf-8"))
    if document.get("schema_version") != 3 or tuple(document.get("placeholders", ())) != PLACEHOLDERS:
        raise ValueError("unsupported or incomplete manifest")
    validate_focused_target_contract()
    seen = set()
    for row in document.get("rows", ()):
        if row["id"] in seen:
            raise ValueError("duplicate row " + row["id"])
        seen.add(row["id"])
        validate_row(testdata, row)
    if len(seen) < 7:
        raise ValueError("manifest lacks required Phase 1B rows")
    return len(seen)


def validate_negative_self_tests(manifest, testdata):
    document = json.loads(manifest.read_text(encoding="utf-8"))
    midi_row = next(row for row in document["rows"] if row["id"] == "generated-midi-pk3-wad-lump")
    cases = (
        ("negative-cli-setting", "negative command-line setting", lambda row: row["commands"].update({"launch": row["commands"]["launch"] + " +set snd_mididevice -3"})),
        ("wrong-action-order", "invalid MIDI isolation sequence", lambda row: row["commands"]["post_init_actions"].reverse()),
        ("missing-stop", "invalid MIDI isolation sequence", lambda row: row["commands"]["post_init_actions"].pop(0)),
        ("stop-after-setting", "invalid MIDI isolation sequence", lambda row: row["commands"]["post_init_actions"].__setitem__(slice(0, 2), row["commands"]["post_init_actions"][1::-1])),
        ("observation-starts-too-early", "invalid MIDI isolation sequence", lambda row: row["commands"]["post_init_actions"].__setitem__(slice(0, 3), [row["commands"]["post_init_actions"][2], row["commands"]["post_init_actions"][0], row["commands"]["post_init_actions"][1]])),
        ("missing-observation-start", "invalid MIDI isolation sequence", lambda row: row["commands"]["post_init_actions"].pop(2)),
        ("changemus-before-observation", "invalid MIDI isolation sequence", lambda row: row["commands"]["post_init_actions"].__setitem__(slice(2, 4), row["commands"]["post_init_actions"][3:1:-1])),
    )
    with tempfile.TemporaryDirectory() as directory:
        for name, expected_error, mutate in cases:
            candidate = json.loads(json.dumps(document))
            candidate_row = next(row for row in candidate["rows"] if row["id"] == midi_row["id"])
            mutate(candidate_row)
            candidate_manifest = pathlib.Path(directory) / (name + ".json")
            candidate_manifest.write_text(json.dumps(candidate), encoding="utf-8")
            try:
                validate_manifest(candidate_manifest, testdata)
            except ValueError as error:
                if expected_error in str(error):
                    continue
                raise ValueError("negative self-test rejected " + name + " for an unexpected reason: " + str(error))
            raise ValueError("negative self-test accepted " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=pathlib.Path, default=ROOT / "phase1b-validation.json")
    parser.add_argument("--testdata", type=pathlib.Path, default=ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    row_count = validate_manifest(args.manifest, args.testdata)
    if args.self_test:
        validate_negative_self_tests(args.manifest, args.testdata)
    print("Phase 1B manifest preflight passed: " + str(row_count) + " rows")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        print("Phase 1B manifest preflight failed: " + str(error), file=sys.stderr)
        sys.exit(1)