$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"

$MaxCCN = 20
$MaxNLOC = 80

$lizard = Get-Command lizard -ErrorAction SilentlyContinue

if (-not $lizard) {
    Write-Error "Lizard was not found in PATH. Install it before running lint."
    exit 1
}

# Get staged C/C++ files, preserving the old path for renames.
$entries = @()

$changes = @(
    & git diff `
        --cached `
        --name-status `
        --find-renames `
        --diff-filter=AMR
)

if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to determine staged files."
    exit $LASTEXITCODE
}

foreach ($line in $changes) {
    $parts = $line -split "`t"

    if ($parts.Count -lt 2) {
        continue
    }

    $status = $parts[0]

    if ($status -match '^R') {
        if ($parts.Count -lt 3) {
            continue
        }

        $oldPath = $parts[1]
        $newPath = $parts[2]
    }
    else {
        $oldPath = if ($status -eq "A") { $null } else { $parts[1] }
        $newPath = $parts[1]
    }

    if ($newPath -notmatch '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$') {
        continue
    }

    $entries += [PSCustomObject]@{
        OldPath = $oldPath
        NewPath = $newPath
    }
}

if ($entries.Count -eq 0) {
    Write-Host "No staged C/C++ files require Lizard analysis."
    exit 0
}

$tempRoot = Join-Path `
([System.IO.Path]::GetTempPath()) `
("zandronum-lizard-" + [Guid]::NewGuid().ToString("N"))

New-Item -ItemType Directory -Path $tempRoot | Out-Null

function Write-GitFile {
    param(
        [string]$GitSpec,
        [string]$Destination
    )

    $parent = Split-Path -Parent $Destination

    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $content = @(& git show $GitSpec)

    if ($LASTEXITCODE -ne 0) {
        return $false
    }

    [System.IO.File]::WriteAllLines(
        $Destination,
        [string[]]$content,
        (New-Object System.Text.UTF8Encoding $false)
    )

    return $true
}

function Get-LizardFunctions {
    param(
        [string]$Path
    )

    $csv = @(
        & $lizard.Source `
            -l cpp `
            --csv `
            -V `
            $Path
    )

    if ($LASTEXITCODE -ne 0) {
        throw "Lizard failed while analyzing '$Path'."
    }

    if ($csv.Count -le 1) {
        return @()
    }

    return @($csv | ConvertFrom-Csv)
}

function Get-FunctionKey {
    param(
        $Function
    )

    if (-not [string]::IsNullOrWhiteSpace($Function.long_name)) {
        return $Function.long_name
    }

    return $Function.function
}

$violations = @()

try {
    foreach ($entry in $entries) {
        Write-Host "Checking $($entry.NewPath)..."

        $extension = [System.IO.Path]::GetExtension($entry.NewPath)

        $stagedPath = Join-Path `
            $tempRoot `
        ("staged-" + [Guid]::NewGuid().ToString("N") + $extension)

        if (-not (Write-GitFile -GitSpec ":$($entry.NewPath)" -Destination $stagedPath)) {
            Write-Error "Could not read staged version of '$($entry.NewPath)'."
            exit 1
        }

        $stagedFunctions = @(Get-LizardFunctions $stagedPath)

        $baselineByName = @{}

        if ($null -ne $entry.OldPath) {
            $baselinePath = Join-Path `
                $tempRoot `
            ("baseline-" + [Guid]::NewGuid().ToString("N") + $extension)

            if (Write-GitFile -GitSpec "HEAD:$($entry.OldPath)" -Destination $baselinePath) {
                $baselineFunctions = @(Get-LizardFunctions $baselinePath)

                foreach ($function in $baselineFunctions) {
                    $key = Get-FunctionKey $function

                    $baselineByName[$key] = [PSCustomObject]@{
                        CCN  = [int]$function.CCN
                        NLOC = [int]$function.NLOC
                    }
                }
            }
        }

        foreach ($function in $stagedFunctions) {
            $key = Get-FunctionKey $function

            $newCCN = [int]$function.CCN
            $newNLOC = [int]$function.NLOC

            $baseline = $baselineByName[$key]

            if ($null -eq $baseline) {
                # New function: enforce normal thresholds.
                $badCCN = $newCCN -gt $MaxCCN
                $badNLOC = $newNLOC -gt $MaxNLOC

                if ($badCCN -or $badNLOC) {
                    $violations += [PSCustomObject]@{
                        File     = $entry.NewPath
                        Function = $function.function
                        Line     = [int]$function.start
                        OldCCN   = $null
                        NewCCN   = $newCCN
                        OldNLOC  = $null
                        NewNLOC  = $newNLOC
                        Kind     = "new"
                    }
                }

                continue
            }

            # Existing legacy function:
            #
            # Allow existing technical debt to remain and allow improvements.
            # Block only newly introduced regressions beyond both:
            #
            #   1. the configured threshold, and
            #   2. the previous metric value.
            $badCCN = (
                ($newCCN -gt $MaxCCN) -and
                ($newCCN -gt $baseline.CCN)
            )

            $badNLOC = (
                ($newNLOC -gt $MaxNLOC) -and
                ($newNLOC -gt $baseline.NLOC)
            )

            if ($badCCN -or $badNLOC) {
                $violations += [PSCustomObject]@{
                    File     = $entry.NewPath
                    Function = $function.function
                    Line     = [int]$function.start
                    OldCCN   = $baseline.CCN
                    NewCCN   = $newCCN
                    OldNLOC  = $baseline.NLOC
                    NewNLOC  = $newNLOC
                    Kind     = "regression"
                }
            }
        }
    }
}
finally {
    Remove-Item `
        -LiteralPath $tempRoot `
        -Recurse `
        -Force `
        -ErrorAction SilentlyContinue
}

if ($violations.Count -eq 0) {
    Write-Host "Lizard passed: no new complexity regressions."
    exit 0
}

Write-Host ""
Write-Host "Lizard found new complexity regressions:" -ForegroundColor Red
Write-Host ""

foreach ($violation in $violations | Sort-Object File, Line) {
    if ($violation.Kind -eq "new") {
        Write-Host (
            "{0}:{1}: {2}: new function has CCN {3}, NLOC {4}" -f `
                $violation.File,
            $violation.Line,
            $violation.Function,
            $violation.NewCCN,
            $violation.NewNLOC
        )
    }
    else {
        Write-Host (
            "{0}:{1}: {2}: CCN {3}->{4}, NLOC {5}->{6}" -f `
                $violation.File,
            $violation.Line,
            $violation.Function,
            $violation.OldCCN,
            $violation.NewCCN,
            $violation.OldNLOC,
            $violation.NewNLOC
        )
    }
}

Write-Host ""
Write-Host "Limits: CCN <= $MaxCCN, NLOC <= $MaxNLOC"
exit 1
