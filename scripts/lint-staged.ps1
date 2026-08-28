$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$lizard = Get-Command lizard -ErrorAction SilentlyContinue

if (-not $lizard) {
    Write-Error "Lizard was not found in PATH. Install it before running lint."
    exit 1
}

$files = git diff --cached --name-only --diff-filter=ACMR |
Where-Object { $_ -match '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$' }

if (-not $files) {
    exit 0
}

lizard -l cpp `
    -C 20 `
    -T nloc=80 `
    --warning-msvs `
    $files

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
