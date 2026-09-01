## Phase 6R-W4 Complete: Windows OpenAL Runtime Package Repair
Corrected the Windows package contract to source the no-router OpenAL Soft runtime from its actual installed filename. The package continues to ship one `OpenAL32.dll` with source-to-package hash validation.

**Files:** `.github/workflows/ci-windows.yml`, `completes/native-openal-soft-phase-1a-phase-6r-w4-complete.md`
**Implementation models:** no-router OpenAL runtime packaging -> GPT-5.6 Terra Threadline (customendpoint)
**Functions / Assets:** None
**Tests / Validations:** YAML parse passed; focused PowerShell runtime candidate/copy/SHA-256 contract passed; VS Code diagnostics passed; `git diff --check -- .github/workflows/ci-windows.yml` passed.
**Review:** APPROVED
**Repository finalization:** Atlas commits this completed phase after writing this artifact when a Git repository contains phase-owned changes; the resulting hash is reported in the checkpoint result.
**Commit message:**
Subject: fix: package Windows OpenAL runtime
Body:
- Copy the no-router OpenAL32.dll install output into Windows packages.
- Validate the corrected runtime package contract with YAML and PowerShell checks.