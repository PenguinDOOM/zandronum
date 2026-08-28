param(
    [string]$BuildDir = "build-v143"
)

$ErrorActionPreference = "Stop"

Write-Host "Running Lizard..."

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
