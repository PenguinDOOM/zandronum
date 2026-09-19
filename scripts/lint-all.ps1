param(
    [string]$BuildDir = "build-v143"
)

$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'cppcheck-cache.ps1')

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

$cacheRoot = Join-Path $PSScriptRoot '..\.cppcheck-cache'
$cacheLock = Enter-CppcheckCacheLock -CacheRoot $cacheRoot -Name 'full'

try {
    $logDir = Join-Path $cacheRoot "logs"
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $lizardLog = Join-Path $logDir "lizard-full.log"
    $cppcheckLog = Join-Path $logDir "cppcheck-full.log"

    Write-Host "Running full Lizard analysis..."
    Write-Host "  Output: $lizardLog"
    & $lizard.Source src -l cpp -C 20 -T nloc=80 --warning-msvs -i -1 *> $lizardLog
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Lizard failed to execute correctly. See '$lizardLog'."
        exit $LASTEXITCODE
    }

    Write-Host "Running full Cppcheck analysis..."
    Write-Host "  Output: $cppcheckLog"
    $cppcheckVersion = @(& $cppcheck.Source --version)
    if (($LASTEXITCODE -ne 0) -or ($cppcheckVersion.Count -ne 1)) {
        Write-Error 'Could not determine the Cppcheck version for the full cache identity.'
        exit 1
    }
    $cppcheckArgs = @(
        "--project=$($solution.FullName)"
        '--project-configuration=Release|x64'
        '--enable=warning,performance,portability'
        '--template=vs'
        '--quiet'
    )
    foreach ($excludedDirectory in @('bzip2', 'jpeg-6b', 'zlib', 'game-music-emu', 'gdtoa', 'dumb', 'lzma', 'sqlite', 'GeoIP', 'rnnoise')) {
        $cppcheckArgs += "-i$([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\\$excludedDirectory")))"
    }
    $analysisContext = Get-CppcheckAnalysisContext `
        -BuildRoot $BuildDir `
        -ProjectPaths @(Get-ChildItem -LiteralPath $BuildDir -Filter *.vcxproj -File -Recurse | ForEach-Object { $_.FullName }) `
        -InputRootIdentities @('full-live-worktree') `
        -AnalyzerConfigurationPaths (@($PSCommandPath, (Join-Path $PSScriptRoot 'cppcheck-cache.ps1')) + @(Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $cppcheck.Source)) `
        -AnalyzerOptions $cppcheckArgs
    $fullIdentity = Get-CppcheckCacheIdentity `
        -AnalyzerVersion $cppcheckVersion[0] `
        -AnalyzerSHA256 (Get-FileHash -LiteralPath $cppcheck.Source -Algorithm SHA256).Hash `
        -InputMode 'full-project' `
        -RootIdentity 'full-live-worktree' `
        -AnalysisContext $analysisContext
    Save-CppcheckCacheIdentity -CacheRoot $cacheRoot -Namespace 'full' -Identity $fullIdentity
    $cppcheckCache = Get-CppcheckCacheLeaf -CacheRoot $cacheRoot -Namespace 'full' -Identity $fullIdentity -Role 'analysis'
    New-Item -ItemType Directory -Force -Path $cppcheckCache | Out-Null
    $cppcheckArgs += "--cppcheck-build-dir=$cppcheckCache"
    & $cppcheck.Source @cppcheckArgs *> $cppcheckLog
    $cppcheckExitCode = $LASTEXITCODE
}
finally {
    Exit-CppcheckCacheLock -Lock $cacheLock
}

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
