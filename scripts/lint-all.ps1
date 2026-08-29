param(
    [string]$BuildDir = "build-v143"
)

$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"

$lizard = Get-Command lizard -ErrorAction SilentlyContinue

if (-not $lizard) {
    Write-Error "Lizard was not found in PATH. Install it before running full lint."
    exit 1
}

$cppcheck = Get-Command cppcheck -ErrorAction SilentlyContinue

if (-not $cppcheck) {
    Write-Error "Cppcheck was not found in PATH. Install Cppcheck and restart your terminal/IDE."
    exit 1
}

$solution = Get-ChildItem -LiteralPath $BuildDir -Filter *.sln |
Select-Object -First 1

if (-not $solution) {
    Write-Error @"
No Visual Studio solution found in '$BuildDir'.
Configure the project first:

cmake -S . -B $BuildDir -G "Visual Studio 17 2022" -A x64 -T v143
"@
    exit 1
}

$logDir = Join-Path ".cppcheck-cache" "logs"

New-Item `
    -ItemType Directory `
    -Force `
    -Path $logDir |
Out-Null

$lizardLog = Join-Path $logDir "lizard-full.log"
$cppcheckLog = Join-Path $logDir "cppcheck-full.log"

Write-Host "Running full Lizard analysis..."
Write-Host "  Output: $lizardLog"

& $lizard.Source `
    src `
    -l cpp `
    -C 20 `
    -T nloc=80 `
    --warning-msvs `
    -i -1 `
    *> $lizardLog

$lizardExitCode = $LASTEXITCODE

if ($lizardExitCode -ne 0) {
    Write-Error "Lizard failed to execute correctly. See '$lizardLog'."
    exit $lizardExitCode
}

Write-Host "Running full Cppcheck analysis..."
Write-Host "  Output: $cppcheckLog"

$cppcheckArgs = @(
    "--project=$($solution.FullName)"
    "--project-configuration=Release|x64"
    "--enable=warning,performance,portability"
    "--cppcheck-build-dir=.cppcheck-cache"
    "--template=vs"
    "--quiet"
)

& $cppcheck.Source @cppcheckArgs *> $cppcheckLog

$cppcheckExitCode = $LASTEXITCODE

if ($cppcheckExitCode -ne 0) {
    Write-Error "Cppcheck failed to execute correctly. See '$cppcheckLog'."
    exit $cppcheckExitCode
}

Write-Host ""
Write-Host "Full static-analysis reports generated:"
Write-Host "  Lizard:   $lizardLog"
Write-Host "  Cppcheck: $cppcheckLog"
Write-Host ""
Write-Host "Full-project findings are report-only and do not block commits or pushes."

exit 0
