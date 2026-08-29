param(
    [string]$BuildDir = "build-v143",
    [string]$BaseSha = "",
    [string]$HeadSha = "HEAD"
)

$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"

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

# When run manually, compare HEAD against its upstream branch.
if ([string]::IsNullOrWhiteSpace($BaseSha)) {
    $upstream = (& git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>$null)

    if (($LASTEXITCODE -ne 0) -or [string]::IsNullOrWhiteSpace($upstream)) {
        Write-Error "No BaseSha was supplied and the current branch has no upstream."
        exit 1
    }

    $BaseSha = (& git merge-base $HeadSha $upstream.Trim())

    if (($LASTEXITCODE -ne 0) -or [string]::IsNullOrWhiteSpace($BaseSha)) {
        Write-Error "Could not determine the merge base with upstream '$($upstream.Trim())'."
        exit 1
    }

    $BaseSha = $BaseSha.Trim()
}

$changedFiles = @(
    & git diff `
        --name-only `
        --diff-filter=ACMR `
        $BaseSha `
        $HeadSha `
        -- `
        '*.c' `
        '*.cc' `
        '*.cpp' `
        '*.cxx'

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to determine changed C/C++ source files."
        exit $LASTEXITCODE
    }
)

$changedFiles = @(
    $changedFiles |
    Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and
        (Test-Path -LiteralPath $_ -PathType Leaf)
    } |
    Sort-Object -Unique
)

if ($changedFiles.Count -eq 0) {
    Write-Host "No changed C/C++ translation units require Cppcheck."
    exit 0
}

Write-Host "Running Cppcheck on $($changedFiles.Count) changed translation unit(s):"

foreach ($file in $changedFiles) {
    Write-Host "  $file"
}

$cppcheckArgs = @(
    "--project=$($solution.FullName)"
    "--project-configuration=Release|x64"
    "--enable=warning,performance,portability"
    "--error-exitcode=1"
    "--cppcheck-build-dir=.cppcheck-cache"
    "--template=vs"
    "--quiet"
)

foreach ($file in $changedFiles) {
    $filter = $file.Replace('\', '/')
    $cppcheckArgs += "--file-filter=$filter"
}

& $cppcheck.Source @cppcheckArgs

$cppcheckExitCode = $LASTEXITCODE

if ($cppcheckExitCode -ne 0) {
    Write-Error "Cppcheck reported an issue in the pushed C/C++ changes."
    exit $cppcheckExitCode
}

Write-Host "Cppcheck passed."
exit 0
