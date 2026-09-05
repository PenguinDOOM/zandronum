"""Validate the schema-version 4 Phase 1B audio manifest."""

import argparse
import copy
import hashlib
import json
import pathlib
import re
import struct
import sys
import zipfile


ROOT = pathlib.Path(__file__).resolve().parent
REPOSITORY_ROOT = ROOT.parents[2]
PLACEHOLDERS = ("${CLIENT_EXE}", "${PACKAGE_EXE}", "${TESTDATA}", "${STOCK_PK3}", "${REFERENCE_SERVER}")
ROW_BACKENDS = {
    "source-ownership-ranges-probe": "cross-backend",
    "required-codec-decode": "cross-backend",
    "partial-final-natural-end": "openal",
    "whole-custom-loop": "cross-backend",
    "seek-lifecycle": "openal",
    "pause-volume-stop": "openal",
    "failure-cleanup": "cross-backend",
    "encoded-sfx": "openal",
    "dumb-vorbis-sample": "openal",
    "midi-compatibility": "cross-backend",
    "memory-lump-integration": "openal",
    "url-deferred": "cross-backend",
    "build-static": "n/a",
    "packaging-provenance": "n/a",
    "multiplayer-compatibility": "cross-backend",
}
ROW_SOURCE_MODES = {
    "source-ownership-ranges-probe": "decoder-direct-memory-and-file-slice",
    "required-codec-decode": "decoder-fixtures",
    "partial-final-natural-end": "decoder-and-openal-lifecycle",
    "whole-custom-loop": "vorbis-loop-tags-and-openal-boundary",
    "seek-lifecycle": "openal-lifecycle-and-fmod-source",
    "pause-volume-stop": "openal-lifecycle-and-fmod-source",
    "failure-cleanup": "decoder-malformed-and-openal-terminal",
    "encoded-sfx": "actual-device-openal-lifecycle-sfx",
    "dumb-vorbis-sample": "PK3-WAD-lump",
    "midi-compatibility": "PK3-WAD-lump-and-midi-policy",
    "memory-lump-integration": "direct-memory-file-slice-and-PK3-WAD-lump",
    "url-deferred": "openal-url-rejection-and-fmod-source",
    "build-static": "build-and-static-validation",
    "packaging-provenance": "package-gate-and-fixture-provenance",
    "multiplayer-compatibility": "reference-server-runtime",
}
COMMAND_KEYS = {
    "source-ownership-ranges-probe": {"automated", "focused"},
    "required-codec-decode": {"automated"},
    "partial-final-natural-end": {"decoder", "openal_lifecycle"},
    "whole-custom-loop": {"decoder_loop_tags", "openal_loop_boundary", "fmod_regression_source"},
    "seek-lifecycle": {"openal_lifecycle", "fmod_regression_source"},
    "pause-volume-stop": {"openal_lifecycle", "fmod_regression_source"},
    "failure-cleanup": {"decoder", "openal_lifecycle"},
    "encoded-sfx": {"openal_lifecycle"},
    "dumb-vorbis-sample": {"openal_lifecycle", "runtime"},
    "midi-compatibility": {"automated_policy", "fmod_preserves_selection", "runtime"},
    "memory-lump-integration": {"focused", "runtime_identity"},
    "url-deferred": {"openal_url_rejection", "fmod_unchanged_source"},
    "build-static": {"client_build", "server_no_sound_build", "focused_ctests", "manifest_preflight", "lizard", "cppcheck", "staged_hook_identity"},
    "packaging-provenance": {"gate_self_test", "manifest_self_test", "package_identity", "actual_package"},
    "multiplayer-compatibility": {"launch", "connection_observation", "additional_connected_operation"},
}
CTESTS = {
    "audio_decoder": "ctest --test-dir build-v143-openal -C Release --output-on-failure -R ^audio_decoder$",
    "openal_pcm": "ctest --test-dir build-v143-openal -C Release --output-on-failure -R ^openal_pcm$",
    "openal_lifecycle": "ctest --test-dir build-v143-openal -C Release --output-on-failure -R ^openal_lifecycle$",
    "midi_device_selection": "ctest --test-dir build-v143-openal -C Release --output-on-failure -R ^midi_device_selection$",
}
FOCUSED_BUILD = "cmake --build build-v143-openal --config Release --target openal_lifecycle_tests"
FOCUSED_EXE = "build-v143-openal/tools/Release/openal_lifecycle_tests.exe"
LIZARD_COMMAND = "lizard -C 19 -T nloc=80 -w src/sound/oalsound.cpp src/sound/audio_decoder.cpp"
LIZARD_EXPECTED = {"exit_code": 0, "max_accepted_ccn": 19, "max_accepted_nloc": 80}
ACTUAL_PACKAGE_COMMAND = 'python scripts/phase1b_audio_gate.py verify-package --client-exe "${PACKAGE_EXE}" --source-root . --sound-enabled'
MIDI_ACTIONS = [
    {"phase": "after-renderer-initialization", "kind": "engine-console", "command": "stopmus"},
    {"phase": "after-renderer-initialization", "kind": "engine-console", "command": "set snd_mididevice -1"},
    {"phase": "after-renderer-initialization", "kind": "operator-harness", "action": "begin-external-diagnostic-capture", "scope": "subsequent console or log output only; exclude earlier process output"},
    {"phase": "after-renderer-initialization", "kind": "engine-console", "command": "changemus D_MIDI"},
]
MIDI_DIAGNOSTIC = {
    "count": 1,
    "text_class": "fmod-midi-unavailable-remapped-to-opl",
    "required_substring": "FMOD MIDI playback is unavailable with the current sound backend; using OPL instead.",
    "observation_window": {
        "starts_after": "set snd_mididevice -1",
        "starts_with": "begin-external-diagnostic-capture",
        "ends_after": "changemus D_MIDI",
    },
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def contained_path(root, value, label):
    require(isinstance(value, str) and value, label + " path is missing")
    candidate = pathlib.Path(value)
    require(not candidate.is_absolute(), label + " path must be relative")
    resolved_root = root.resolve()
    resolved_path = (root / candidate).resolve()
    require(resolved_path.is_relative_to(resolved_root), label + " path escapes its root")
    return resolved_path


def validate_fixture(testdata, fixture):
    require(isinstance(fixture, dict), "fixture must be an object")
    identity = fixture.get("identity")
    path = fixture.get("path")
    require(isinstance(identity, str) and identity, "fixture identity is missing")
    file_path = contained_path(testdata, path, "fixture")
    require(file_path.is_file(), "missing fixture " + path)
    data = file_path.read_bytes()
    require(isinstance(fixture.get("sha256"), str) and sha256(data) == fixture["sha256"], "fixture hash mismatch for " + path)

    slice_value = fixture.get("slice")
    if slice_value is not None:
        require(isinstance(slice_value, dict), "fixture slice must be an object")
        offset = slice_value.get("offset")
        length = slice_value.get("length")
        require(
            isinstance(offset, int)
            and isinstance(length, int)
            and offset >= 0
            and length > 0
            and offset + length <= len(data),
            "invalid slice for " + path,
        )

    if "archive_entry" in fixture:
        validate_archive_fixture(testdata, fixture, data)


def validate_archive_fixture(testdata, fixture, data):
    entry_name = fixture.get("archive_entry")
    entry_hash = fixture.get("archive_entry_sha256")
    lump_name = fixture.get("lump")
    lump_hash = fixture.get("lump_sha256")
    require(entry_name == "phase1b-audio.wad" and isinstance(entry_hash, str), "invalid archive entry contract")
    require(isinstance(lump_name, str) and isinstance(lump_hash, str), "invalid WAD lump contract")
    archive_path = contained_path(testdata, fixture["path"], "fixture")
    with zipfile.ZipFile(archive_path) as archive:
        entries = [name for name in archive.namelist() if name.lower().endswith(".wad")]
        require(entries == [entry_name], "PK3 contains an unexpected embedded WAD path")
        entry_data = archive.read(entry_name)
    require(sha256(entry_data) == entry_hash, "archive-entry hash mismatch for " + fixture["identity"])
    require(sha256(wad_lump(entry_data, lump_name)) == lump_hash, "WAD lump hash mismatch for " + fixture["identity"])


def wad_lump(data, name):
    require(len(data) >= 12 and data[:4] in (b"IWAD", b"PWAD"), "embedded entry is not a WAD")
    count, directory = struct.unpack_from("<II", data, 4)
    require(directory + count * 16 <= len(data), "WAD directory outside file")
    for index in range(count):
        offset, length, raw_name = struct.unpack_from("<II8s", data, directory + index * 16)
        if raw_name.rstrip(b"\0").decode("ascii", "replace") == name:
            require(offset + length <= len(data), "WAD lump outside file")
            return data[offset:offset + length]
    raise ValueError("missing WAD lump " + name)


def validate_artifact(artifact):
    require(isinstance(artifact, (str, dict)), "artifact must be a string or object")
    path = artifact if isinstance(artifact, str) else artifact.get("path")
    expected_hash = None if isinstance(artifact, str) else artifact.get("sha256")
    file_path = contained_path(REPOSITORY_ROOT, path, "artifact")
    require(file_path.is_file(), "missing artifact " + path)
    if expected_hash is not None:
        require(sha256(file_path.read_bytes()) == expected_hash, "artifact hash mismatch for " + path)


def strings_in(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for nested in value.values():
            yield from strings_in(nested)
    elif isinstance(value, list):
        for nested in value:
            yield from strings_in(nested)


def require_exact(value, expected, label):
    require(isinstance(value, str), label + " must be a string")
    require(value == expected, label + " is invalid")


def fixture_identities(row):
    fixtures = row.get("fixtures", [row["fixture"]] if "fixture" in row else [])
    return {fixture.get("identity") for fixture in fixtures if isinstance(fixture, dict)}


def require_focused(commands, label):
    require(isinstance(commands, dict), label + " focused command is invalid")
    require_exact(commands.get("build"), FOCUSED_BUILD, label + " focused build")
    require_exact(commands.get("direct_memory"), FOCUSED_EXE + " --phase1b-direct-memory", label + " direct-memory command")
    require_exact(commands.get("file_slice"), FOCUSED_EXE + " --phase1b-file-slice", label + " file-slice command")


def require_runtime_launch(launch, backend, label):
    require(isinstance(launch, str), label + " runtime launch must be a string")
    for placeholder in ("${CLIENT_EXE}", "${TESTDATA}", "${STOCK_PK3}"):
        require(placeholder in launch, label + " runtime launch lacks " + placeholder)
    match = re.search(r"\+set\s+snd_backend\s+(\S+)", launch)
    require(match is not None, label + " runtime launch lacks snd_backend")
    allowed = {backend} if backend != "cross-backend" else {"openal", "fmod"}
    require(match.group(1) in allowed, label + " runtime launch backend mismatch")


def require_fmod_source(value, tokens, label):
    require(isinstance(value, str) and "src/sound/fmodsound.cpp" in value, label + " requires fmodsound source verification")
    require(all(token in value for token in tokens), label + " has invalid fmodsound source tokens")


def validate_source_row(row):
    commands = row["commands"]
    require_exact(commands["automated"], CTESTS["audio_decoder"], "source audio_decoder CTest")
    require_focused(commands["focused"], "source")


def validate_codec_row(row):
    require_exact(row["commands"]["automated"], CTESTS["audio_decoder"], "codec audio_decoder CTest")
    required = {"vorbis_mono.ogg", "mp3_mono.mp3", "pcm16_stereo.wav", "float32_mono.wav"}
    identities = fixture_identities(row)
    require(required <= identities and any(identity and identity.endswith(".flac") for identity in identities), "codec fixtures must include Vorbis, MP3, FLAC, PCM WAVE, and float WAVE")


def validate_lifecycle_pair(row, decoder_key="decoder"):
    commands = row["commands"]
    require_exact(commands[decoder_key], CTESTS["audio_decoder"], row["id"] + " audio_decoder CTest")
    require_exact(commands["openal_lifecycle"], CTESTS["openal_lifecycle"], row["id"] + " openal_lifecycle CTest")


def validate_loop_row(row):
    commands = row["commands"]
    require_exact(commands["decoder_loop_tags"], CTESTS["audio_decoder"], "loop audio_decoder CTest")
    require_exact(commands["openal_loop_boundary"], CTESTS["openal_lifecycle"], "loop openal_lifecycle CTest")
    require_fmod_source(commands["fmod_regression_source"], ("SetPosition", "SetLoop", "Loop"), "loop")
    require(all(token in row["expected"]["observation"] for token in ("Vorbis loop tags", "loop boundary")), "loop expectations are incomplete")


def validate_fmod_lifecycle_row(row, tokens):
    commands = row["commands"]
    require_exact(commands["openal_lifecycle"], CTESTS["openal_lifecycle"], row["id"] + " openal_lifecycle CTest")
    require_fmod_source(commands["fmod_regression_source"], tokens, row["id"])


def validate_encoded_sfx_row(row):
    commands = row["commands"]
    require_exact(commands["openal_lifecycle"], CTESTS["openal_lifecycle"], "encoded-sfx requires openal_lifecycle")
    identities = fixture_identities(row)
    required = {"vorbis_mono.ogg", "mp3_mono.mp3"}
    require(required <= identities and any(identity and identity.endswith(".flac") for identity in identities), "encoded-sfx requires Vorbis, MP3, and FLAC fixtures")


def validate_dumb_row(row):
    commands = row["commands"]
    require_exact(commands["openal_lifecycle"], CTESTS["openal_lifecycle"], "DUMB openal_lifecycle CTest")
    runtime = commands["runtime"]
    require(isinstance(runtime, dict), "DUMB runtime command is invalid")
    require_runtime_launch(runtime.get("launch"), "openal", "DUMB")
    require(runtime.get("post_init_console_actions") == ["changemus D_XMOGG"], "DUMB runtime must changemus D_XMOGG")
    require(any(identity and identity.endswith(":D_XMOGG") for identity in fixture_identities(row)), "DUMB requires checked D_XMOGG lump")


def validate_midi_row(row):
    commands = row["commands"]
    require_exact(commands["automated_policy"], CTESTS["midi_device_selection"], "MIDI policy CTest")
    preservation = commands["fmod_preserves_selection"]
    require(isinstance(preservation, dict), "MIDI FMOD preservation command is invalid")
    require_exact(preservation.get("command"), CTESTS["midi_device_selection"], "MIDI FMOD preservation CTest")
    require_exact(preservation.get("expected_text"), "FMOD preserves the selected MIDI device when it is available.", "MIDI FMOD preservation text")
    runtime = commands["runtime"]
    require(isinstance(runtime, dict), "MIDI runtime command is invalid")
    require_runtime_launch(runtime.get("launch"), "cross-backend", "MIDI")
    require(not re.search(r"\+set\s+\S+\s+-\d+", runtime["launch"]), "MIDI launch must not set a negative device")
    require(runtime.get("post_init_actions") == MIDI_ACTIONS, "MIDI action sequence is invalid")
    require(row["diagnostic"] == MIDI_DIAGNOSTIC, "MIDI diagnostic contract is invalid")


def validate_memory_row(row):
    commands = row["commands"]
    require_focused(commands["focused"], "memory/lump")
    require_runtime_launch(commands["runtime_identity"], "openal", "memory/lump")
    require(any(identity and ":D_XMOGG" in identity for identity in fixture_identities(row)), "memory/lump requires checked PK3/WAD lump")


def validate_url_row(row):
    commands = row["commands"]
    require_exact(commands["openal_url_rejection"], CTESTS["openal_lifecycle"], "URL openal_lifecycle CTest")
    require("midi_device_selection" not in commands["openal_url_rejection"] and "midi_device_selection" not in commands["fmod_unchanged_source"], "URL validation must not use midi_device_selection")
    require_fmod_source(commands["fmod_unchanged_source"], ("URL|url",), "URL")


def validate_build_row(row):
    commands = row["commands"]
    require_exact(commands["client_build"], "cmake --build build-v143-openal --config Release --target zdoom", "client build")
    require_exact(commands["server_no_sound_build"], "cmake --build build-v143-phase5-serveronly --config Release --target all", "server/no-sound build")
    require(commands["focused_ctests"] == [CTESTS["audio_decoder"], CTESTS["openal_pcm"], CTESTS["midi_device_selection"], CTESTS["openal_lifecycle"], FOCUSED_EXE + " --phase1b-direct-memory", FOCUSED_EXE + " --phase1b-file-slice"], "focused CTest contract is invalid")
    require_exact(commands["manifest_preflight"], "python tools/testdata/audio/validate_phase1b_manifest.py --self-test", "manifest preflight")
    require(isinstance(commands["lizard"], dict), "lizard contract is invalid")
    require_exact(commands["lizard"].get("command"), LIZARD_COMMAND, "lizard command")
    require(commands["lizard"].get("expected") == LIZARD_EXPECTED, "lizard baseline expectation is invalid")
    require("cppcheck" in commands["cppcheck"] and "--project=build-v143-openal/Zandronum.sln" in commands["cppcheck"], "cppcheck command is invalid")
    require_exact(commands["staged_hook_identity"], "pwsh -NoProfile -File scripts/lint-staged.ps1", "staged hook identity")


def validate_packaging_row(row):
    commands = row["commands"]
    require_exact(commands["gate_self_test"], "python scripts/phase1b_audio_gate.py --self-test", "gate self-test")
    require_exact(commands["manifest_self_test"], "python tools/testdata/audio/validate_phase1b_manifest.py --self-test", "manifest self-test")
    require_exact(commands["package_identity"], "python tools/testdata/audio/generate_phase1b_music_fixtures.py --verify", "package identity")
    require_exact(commands["actual_package"], ACTUAL_PACKAGE_COMMAND, "actual package verification")
    artifacts = row.get("artifacts", [])
    identities = {artifact.get("identity") for artifact in artifacts if isinstance(artifact, dict)}
    require({"phase1b-audio-gate", "decoder-manifest-contract", "phase1b-fixture-package"} <= identities, "packaging notice identities are incomplete")
    gate_text = (REPOSITORY_ROOT / "scripts" / "phase1b_audio_gate.py").read_text(encoding="utf-8")
    require(all(token in gate_text for token in ('"name": "miniaudio"', '"name": "stb_vorbis"', "audio-decoders.md", "verify-package", "verify_existing_package", "codec_runtime_assertion", 'add_argument("--package-exe"', '"${PACKAGE_EXE}"', "package executable is unavailable")), "packaging verification support is incomplete")
    require(all(token in row["expected"]["observation"] for token in ("actual package", "notices", "provenance", "codec runtime")), "packaging actual-package expectation is incomplete")


def validate_multiplayer_row(row):
    commands = row["commands"]
    require_runtime_launch(commands["launch"], "cross-backend", "multiplayer")
    require("${REFERENCE_SERVER}" in commands["launch"] and "+connect" in commands["launch"], "multiplayer launch lacks reference connection")
    observation = commands["connection_observation"]
    require(isinstance(observation, list) and all(isinstance(value, str) for value in observation), "multiplayer connection observation is invalid")
    require(all(any(token in value for value in observation) for token in ("join", "play", "disconnect")), "multiplayer requires join/play/disconnect observation")
    prerequisites = row["external_prerequisites"]
    require(any("${STOCK_PK3}" in value for value in prerequisites) and any("${REFERENCE_SERVER}" in value for value in prerequisites), "multiplayer prerequisites lack external placeholders")


def validate_row_semantics(row):
    validators = {
        "source-ownership-ranges-probe": validate_source_row,
        "required-codec-decode": validate_codec_row,
        "partial-final-natural-end": validate_lifecycle_pair,
        "whole-custom-loop": validate_loop_row,
        "seek-lifecycle": lambda value: validate_fmod_lifecycle_row(value, ("SetPosition", "SetTime", "seek")),
        "pause-volume-stop": lambda value: validate_fmod_lifecycle_row(value, ("SetPaused", "snd_musicvolume", "Stop")),
        "failure-cleanup": validate_lifecycle_pair,
        "encoded-sfx": validate_encoded_sfx_row,
        "dumb-vorbis-sample": validate_dumb_row,
        "midi-compatibility": validate_midi_row,
        "memory-lump-integration": validate_memory_row,
        "url-deferred": validate_url_row,
        "build-static": validate_build_row,
        "packaging-provenance": validate_packaging_row,
        "multiplayer-compatibility": validate_multiplayer_row,
    }
    validators[row["id"]](row)


def validate_placeholders(row):
    values = tuple(strings_in(row))
    used = {match for value in values for match in re.findall(r"\$\{[^}]+\}", value)}
    require(used <= set(PLACEHOLDERS), row["id"] + " uses an undeclared placeholder")
    runtime_rows = {"dumb-vorbis-sample", "midi-compatibility", "memory-lump-integration", "multiplayer-compatibility"}
    uses_stock = any("${STOCK_PK3}" in value for value in values if value not in PLACEHOLDERS)
    if row["id"] in runtime_rows:
        require(uses_stock, row["id"] + " runtime requires ${STOCK_PK3}")
    else:
        require(not uses_stock, row["id"] + " must not use ${STOCK_PK3}")
    uses_server = any("${REFERENCE_SERVER}" in value for value in values if value not in PLACEHOLDERS)
    require(uses_server == (row["id"] == "multiplayer-compatibility"), row["id"] + " has invalid ${REFERENCE_SERVER} use")


def validate_row_metadata(row, row_id):
    require(row.get("required") is True, row_id + " must be required")
    require(row.get("backend") == ROW_BACKENDS[row_id], row_id + " has an invalid backend")
    require(row.get("source_mode") == ROW_SOURCE_MODES[row_id], row_id + " has an invalid source_mode")
    require(isinstance(row.get("settings"), dict), row_id + " settings must be an object")
    require(tuple(row.get("portable_placeholders", ())) == PLACEHOLDERS, row_id + " has invalid portable placeholders")


def validate_row_text_contract(row, row_id):
    expected = row.get("expected")
    require(isinstance(expected, dict), row_id + " has an incomplete expected contract")
    require(all(isinstance(expected.get(key), str) and expected[key] for key in ("observation", "tolerance")), row_id + " has an incomplete expected contract")
    diagnostic = row.get("diagnostic")
    require(isinstance(diagnostic, dict), row_id + " has an incomplete diagnostic contract")
    require(isinstance(diagnostic.get("count"), int) and diagnostic["count"] >= 0, row_id + " has an incomplete diagnostic contract")
    require(isinstance(diagnostic.get("text_class"), str) and diagnostic["text_class"], row_id + " has an incomplete diagnostic contract")
    assertions = row.get("resource_assertions")
    require(isinstance(assertions, list) and assertions and all(isinstance(value, str) for value in assertions), row_id + " has incomplete resource assertions")
    prerequisites = row.get("external_prerequisites")
    require(isinstance(prerequisites, list) and all(isinstance(value, str) for value in prerequisites), row_id + " has invalid external prerequisites")


def validate_row_resources(row, row_id, testdata):
    require(not ("fixture" in row and "fixtures" in row), row_id + " cannot use both fixture and fixtures")
    require(not ("artifact" in row and "artifacts" in row), row_id + " cannot use both artifact and artifacts")
    fixtures = row.get("fixtures", [row["fixture"]] if "fixture" in row else [])
    artifacts = row.get("artifacts", [row["artifact"]] if "artifact" in row else [])
    require(fixtures or artifacts, row_id + " requires fixture/fixtures or artifact/artifacts")
    for fixture in fixtures:
        validate_fixture(testdata, fixture)
    for artifact in artifacts:
        validate_artifact(artifact)


def validate_document(document, testdata):
    require(document.get("schema_version") == 4, "unsupported or incomplete manifest")
    require(tuple(document.get("placeholders", ())) == PLACEHOLDERS, "unsupported or incomplete manifest")
    rows = document.get("rows")
    require(isinstance(rows, list), "manifest rows are missing")
    ids = [row.get("id") if isinstance(row, dict) else None for row in rows]
    duplicates = sorted({row_id for row_id in ids if ids.count(row_id) > 1})
    require(not duplicates, "duplicate IDs: " + ", ".join(duplicates))
    require(set(ids) == set(ROW_BACKENDS) and len(rows) == 15, "required ID mismatch")
    for row in rows:
        require(isinstance(row, dict), "manifest row must be an object")
        row_id = row["id"]
        validate_row_metadata(row, row_id)
        validate_row_text_contract(row, row_id)
        validate_row_resources(row, row_id, testdata)
        commands = row.get("commands")
        require(isinstance(commands, dict) and set(commands) == COMMAND_KEYS[row_id], row_id + " has an invalid command set")
        validate_placeholders(row)
        validate_row_semantics(row)
    return len(rows)


def validate_manifest(manifest, testdata):
    return validate_document(json.loads(manifest.read_text(encoding="utf-8")), testdata)


def mutate(document, operation, path, value):
    broken = copy.deepcopy(document)
    target = broken
    for key in path[:-1]:
        target = target[key]
    if operation == "set":
        target[path[-1]] = value
    elif operation == "reverse":
        target[path[-1]].reverse()
    else:
        del target[path[-1]][value]
    return broken


def validate_negative_self_tests(document, testdata):
    float_fixture = copy.deepcopy(document["rows"][1]["fixtures"][-1])
    cases = [
        ("missing ID", "set", ("rows", 0, "id"), "missing-ID", "required ID mismatch"),
        ("unexpected ID", "set", ("rows", 14, "id"), "unexpected-ID", "required ID mismatch"),
        ("duplicate ID", "set", ("rows", 1, "id"), "source-ownership-ranges-probe", "duplicate IDs"),
        ("malformed command", "set", ("rows", 0, "commands", "automated"), [], "must be a string"),
        ("fixture hash", "set", ("rows", 0, "fixtures", 0, "sha256"), "0" * 64, "fixture hash mismatch"),
        ("codec subset", "set", ("rows", 1, "fixtures"), [float_fixture], "codec fixtures must include"),
        ("wrong CTest", "set", ("rows", 1, "commands", "automated"), CTESTS["openal_lifecycle"], "codec audio_decoder CTest is invalid"),
        ("encoded SFX decoder only", "set", ("rows", 7, "commands", "openal_lifecycle"), CTESTS["audio_decoder"], "encoded-sfx requires openal_lifecycle"),
        ("URL MIDI", "set", ("rows", 11, "commands", "fmod_unchanged_source"), "midi_device_selection", "URL validation must not use"),
        ("MIDI action order", "reverse", ("rows", 9, "commands", "runtime", "post_init_actions"), None, "MIDI action sequence"),
        ("MIDI capture", "remove", ("rows", 9, "commands", "runtime", "post_init_actions"), 2, "MIDI action sequence"),
        ("expected tolerance", "set", ("rows", 0, "expected", "tolerance"), "", "incomplete expected contract"),
        ("runtime backend", "set", ("rows", 8, "commands", "runtime", "launch"), '"${CLIENT_EXE}" -file "${TESTDATA}/phase1b-fixtures.pk3" "${STOCK_PK3}" +set snd_backend fmod +set mod_dumb true', "runtime launch backend mismatch"),
        ("staged hook identity", "set", ("rows", 12, "commands", "staged_hook_identity"), "git diff --cached --check", "staged hook identity is invalid"),
        ("missing OpenAL PCM CTest", "set", ("rows", 12, "commands", "focused_ctests"), [CTESTS["audio_decoder"], CTESTS["midi_device_selection"], CTESTS["openal_lifecycle"], FOCUSED_EXE + " --phase1b-direct-memory", FOCUSED_EXE + " --phase1b-file-slice"], "focused CTest contract is invalid"),
        ("obsolete Lizard -L command", "set", ("rows", 12, "commands", "lizard", "command"), "lizard -C 20 -L 80 src/sound/oalsound.cpp src/sound/audio_decoder.cpp", "lizard command is invalid"),
        ("packaging client executable", "set", ("rows", 13, "commands", "actual_package"), 'python scripts/phase1b_audio_gate.py verify-package --client-exe "${CLIENT_EXE}" --source-root . --sound-enabled', "actual package verification is invalid"),
        ("missing package placeholder", "remove", ("placeholders",), 1, "unsupported or incomplete manifest"),
        ("undeclared package placeholder", "remove", ("rows", 13, "portable_placeholders"), 1, "invalid portable placeholders"),
        ("self-test-only package check", "set", ("rows", 13, "commands", "actual_package"), "python scripts/phase1b_audio_gate.py --self-test", "actual package verification is invalid"),
        ("artifact traversal", "set", ("rows", 12, "artifacts", 0), "../CMakeLists.txt", "artifact path escapes"),
        ("fixture traversal", "set", ("rows", 0, "fixtures", 0, "path"), "../phase1b-validation.json", "fixture path escapes"),
    ]
    for label, operation, path, value, expected in cases:
        try:
            validate_document(mutate(document, operation, path, value), testdata)
        except ValueError as error:
            require(expected in str(error), label + " rejected for the wrong reason: " + str(error))
        else:
            raise ValueError(label + " was accepted")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=pathlib.Path, default=ROOT / "phase1b-validation.json")
    parser.add_argument("--testdata", type=pathlib.Path, default=ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    row_count = validate_manifest(args.manifest, args.testdata)
    if args.self_test:
        document = json.loads(args.manifest.read_text(encoding="utf-8"))
        validate_negative_self_tests(document, args.testdata)
    print("Phase 1B manifest preflight passed: " + str(row_count) + " rows")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        print("Phase 1B manifest preflight failed: " + str(error), file=sys.stderr)
        sys.exit(1)