param(
    [string]$BuildDir = "build-v143"
)

$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"

Write-Host "Running Lizard..."

$lizard = Get-Command lizard -ErrorAction SilentlyContinue

if (-not $lizard) {
    Write-Error "Lizard was not found in PATH. Install it before running lint."
    exit 1
}

lizard src `
    -l cpp `
    -C 20 `
    -T nloc=80 `
    --warning-msvs `
    -i -1

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Running Cppcheck..."

$cppcheck = Get-Command cppcheck -ErrorAction SilentlyContinue

if (-not $cppcheck) {
    Write-Error "Cppcheck was not found in PATH. Install Cppcheck and restart your terminal/IDE."
    exit 1
}

$solution = Get-ChildItem $BuildDir -Filter *.sln |
Select-Object -First 1

if (-not $solution) {
    Write-Error @"
No Visual Studio solution found in '$BuildDir'.
Configure the project first:

cmake -S . -B $BuildDir -G "Visual Studio 17 2022" -A x64 -T v143
"@
    exit 1
}

cppcheck `
    --project="$($solution.FullName)" `
    "--project-configuration=Release|x64" `
    "--enable=warning,performance,portability" `
    --error-exitcode=1 `
    --cppcheck-build-dir=".cppcheck-cache" `
    --template=vs

exit $LASTEXITCODE
