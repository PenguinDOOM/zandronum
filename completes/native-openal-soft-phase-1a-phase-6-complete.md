## Phase 6 Complete: Packaging, CI, Documentation, and Static Gates

Completed fail-closed OpenAL runtime packaging for Windows, Linux, and AppImage artifacts, including verified runtime/license inputs and provenance manifests. The console reference now documents backend selection, reset behavior, diagnostics, and OpenAL limitations from the implemented behavior.

**Files:** `.github/workflows/ci-windows.yml`, `.github/workflows/ci-linux.yml`, `.gitlab-ci.yml`, `docs/console.html`, `completes/native-openal-soft-phase-1a-phase-6-complete.md`
**Implementation models:** Phase 6 CI/package wiring -> `GPT-5.6 Luna Threadline (customendpoint)`; Phase 6 console documentation -> `GPT-5.6 Luna Threadline (customendpoint)`
**Functions / Assets:** Windows OpenAL/FMOD runtime and license packaging; Linux/AppImage runtime checks; exact runtime manifest generation and verification; GitLab runner OpenAL provenance binding; console backend configuration and capability reference
**Tests / Validations:** YAML parsing; actionlint; Bash ShellCheck; PowerShell AST parsing; embedded manifest compile and deterministic valid/missing/extra/substituted input fixtures; package and AppImage extraction fixtures; HTML anchor/source fact checks; `git diff --check` passed. Hosted CI artifact creation and clean runtime launch remain Phase 7 validation.
**Review:** APPROVED
**Repository finalization:** Committed as `ea55fb036b7f2bc50b8a9496ff87ae18043485de`. Atlas staged only the four approved workflow/documentation files. Per user instruction, this `completes/` artifact is not staged.
**Commit message:**
Subject: chore: package OpenAL runtime
Body:
- Add fail-closed OpenAL artifact manifests and runtime checks.
- Document backend configuration and OpenAL limitations.

### Static-Gate Repair (Reopened Phase 6)

Repaired the incremental Base/HEAD Cppcheck gate so generated inputs are symmetric and diagnostic identity is stable across isolated build trees. The gate now uses safe internal Cppcheck parallelism while retaining serial target/revision comparison and a fail-on-new-diagnostics policy.

**Files:** `scripts/lint.ps1`
**Implementation model:** Cppcheck static-gate repair -> `GPT-5.6 Terra Threadline (customendpoint)`
**Functions / Assets:** verified protocol-generated input materialization; revision-specific parser/scanner generation; generated diagnostic alias normalization; dynamic Cppcheck job default capped at 12
**Tests / Validations:** full Cppcheck Base/HEAD gate passed with `CppcheckJobs=1` and `CppcheckJobs=12`, each reporting `Baseline-only diagnostics (1)` and `New diagnostics (0)`; PowerShell parse passed; dirty `protocolspec/spec.map.txt` was rejected before generator/Cppcheck execution and restored; `zdoom`, `openal_pcm_tests`, and `openal_lifecycle_tests` built in Release; focused CTest passed 2/2; `lint-staged.ps1` and `git diff --check` passed.
**Review:** APPROVED
**Repository finalization:** Committed separately as `8562f5a7031754c17199e8dfcd5f8ec966ea060c` without amending the original Phase 6 packaging commit. Per user instruction, this `completes/` artifact is not staged.
**Commit message:**
Subject: chore: harden incremental Cppcheck gate
Body:
- Generate equivalent verified analysis inputs for Base and HEAD targets.
- Add bounded Cppcheck internal parallelism and preserve new-diagnostic gating.

### Pre-Push Generated-Path Repair (Reopened Phase 6)

Repaired the generated protocol bundle Git-tree probe for push ranges whose Base predates ignored generated files. Missing generated paths now produce an empty tree lookup, while tracked paths and tree lookup failures continue to fail closed.

**Files:** `scripts/lint.ps1`
**Implementation model:** pre-push generated-path repair -> `GPT-5.6 Luna Threadline (customendpoint)`
**Functions / Assets:** generated bundle Base/HEAD tracking probe in `Get-VerifiedGeneratedBundle`
**Tests / Validations:** PowerShell parse and editor diagnostics passed; both generated bundle paths were absent in Base `a5f1bc3276eff7ca9e06e68d398b7b64016677ef` and HEAD without fatal stderr; a known tracked path was rejected; hook-equivalent full Cppcheck with `CppcheckJobs=12` passed with `New diagnostics (0)`; `git diff --check` passed.
**Review:** APPROVED
**Repository finalization:** Committed separately as `8611444a010be0c2d53df7e37568b6a07ddccdf1` from `8562f5a7031754c17199e8dfcd5f8ec966ea060c`. Per user instruction, this `completes/` artifact is not staged.
**Commit message:**
Subject: fix: allow missing generated lint inputs
Body:
- Treat absent ignored generated paths as untracked Git-tree entries.
- Preserve fail-closed checks for tracked paths and tree lookup errors.

### Pre-Push Runtime Contract Repair (Reopened Phase 6)

Made the tracked pre-push hook require PowerShell 7.3 or later, matching the native-command handling required by the incremental Cppcheck script. The generated bundle probe now accepts only Git's expected untracked exit code while retaining fail-closed behavior for tracked paths and Git errors.

**Files:** `.githooks/pre-push`, `scripts/lint.ps1`
**Implementation model:** pre-push runtime contract repair -> `GPT-5.6 Luna Threadline (customendpoint)`
**Functions / Assets:** PowerShell version gate for the pre-push hook; generated bundle untracked-path probe
**Tests / Validations:** version fixtures rejected 7.2.99, malformed output, and missing `pwsh`, while accepting 7.3.0 and 7.6.5 before consuming ref input; Git-for-Windows shell syntax and LF direct no-op hook invocation passed; PowerShell parse/editor diagnostics and `git diff --check` passed; exact Base/HEAD full Cppcheck with `CppcheckJobs=12` passed with `New diagnostics (0)`.
**Review:** APPROVED
**Repository finalization:** Committed separately as `c9474000e2f28e2fee8e81325486a51ba0cca4ab` from `8611444a010be0c2d53df7e37568b6a07ddccdf1`. Per user instruction, this `completes/` artifact is not staged.
**Commit message:**
Subject: fix: require pwsh for pre-push lint
Body:
- Require PowerShell 7.3 before executing the incremental lint gate.
- Preserve fail-closed generated input checks for push ranges.

### CI Artifact Repair (Reopened Phase 6)

Repaired the Windows and Linux GitHub Actions package paths that blocked runtime-manifest artifact creation. Windows now uses the code-generator Python interpreter without a target-architecture development requirement and builds a pinned ZStd dependency for each target architecture. Linux server-only jobs no longer register the OpenAL lifecycle test, while sound-enabled package validation checks only OpenAL-linked dynamic executables and requires at least one bundled-runtime resolution.

**Files:** `src/CMakeLists.txt`, `tools/CMakeLists.txt`, `.github/workflows/ci-windows.yml`, `.github/workflows/ci-linux.yml`
**Implementation models:** Phase 6R CI/package repair -> `GPT-5.6 Terra Threadline (customendpoint)`
**Functions / Assets:** Python code-generator CMake dependency; architecture-specific ZStd build and CMake hints; server-only lifecycle-test registration; Linux package OpenAL loader verification
**Tests / Validations:** Windows x64/Win32 CMake configure with Python, FMOD, OpenAL, and ZStd passed; fixed-ZStd Win32 build/install passed; WSL server-only configure with OpenAL discovery disabled passed with PCM test retained and lifecycle test absent; `readelf` dynamic executable selection check passed; both GitHub workflows parsed with `yq v4.53.3`; `git diff --check` passed. Exact-diff GitHub Actions artifact jobs and `scripts/lint.ps1` completion remain pending post-commit CI evidence.
**Review:** APPROVED
**Repository finalization:** Atlas will stage only this record and the four reviewed Phase 6R implementation files before creating a separate commit.
**Commit message:**
Subject: fix: repair OpenAL CI packages
Body:
- Restore Windows and Linux package artifact configuration.
- Validate CMake conditions and workflow syntax before CI rerun.

### Windows ZStd Argument Repair (Reopened Phase 6)

Repaired the PowerShell native-command argument that left `$zstdRoot` literal in the Windows ZStd install prefix. The workflow now passes the expanded architecture-specific prefix as one CMake argument, allowing the existing header/library assertion and main configure hints to use the installed dependency.

**Files:** `.github/workflows/ci-windows.yml`
**Implementation models:** Phase 6R-W Windows ZStd argument repair -> `GPT-5.6 Luna Threadline (customendpoint)`
**Functions / Assets:** Windows pinned ZStd CMake install-prefix argument
**Tests / Validations:** Win32/x64 PowerShell native argument-expansion probe passed without literal `$zstdRoot`; workflow parsed with `yq`; targeted `git diff --check` passed. Hosted Windows artifact jobs remain pending post-commit CI evidence.
**Review:** APPROVED
**Repository finalization:** Atlas will stage only this record and the reviewed Windows workflow before creating a separate commit.
**Commit message:**
Subject: fix: expand Windows ZStd prefix
Body:
- Pass the ZStd install root as an expanded CMake argument.
- Verify workflow syntax and PowerShell argument expansion.