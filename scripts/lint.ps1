param(
    [string]$BuildDir = "build-v143",
    [string]$BaseSha = "",
    [string]$HeadSha = "HEAD",
    [string]$InputManifest = "",
    [string]$EvidenceDir = "",
    [switch]$PreflightOnly,
    [int]$CppcheckJobs = [Math]::Min(12, [Environment]::ProcessorCount)
)

$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'cppcheck-vendor-policy.ps1')

if ((-not [string]::IsNullOrWhiteSpace($InputManifest)) -and
    (-not (Test-Path -LiteralPath $InputManifest -PathType Leaf))) {
    Write-Error "InputManifest '$InputManifest' does not exist or is not a file."
    exit 1
}

if (($CppcheckJobs -lt 1) -or ($CppcheckJobs -gt [Environment]::ProcessorCount)) {
    Write-Error "CppcheckJobs must be between 1 and $([Environment]::ProcessorCount); received $CppcheckJobs."
    exit 1
}

$generatedBundlePaths = @(
    'src/network/servercommands.cpp'
    'src/network/servercommands.h'
)

$generatedBuildInputRules = @(
    [PSCustomObject]@{
        Description = 'xlat parser'
        RelativeOutputs = @(
            'src/xlat_parser.c'
            'src/xlat_parser.h'
        )
    }
    [PSCustomObject]@{
        Description = 'sc_man scanner'
        RelativeOutputs = @(
            'src/sc_man_scanner.h'
        )
    }
)

function Initialize-LintEvidence {
    param(
        [string]$RootPath,
        [string]$ManifestPath,
        [string]$CppcheckPath,
        [string]$CppcheckSHA256,
        [string]$CppcheckVersion
    )

    if ([string]::IsNullOrWhiteSpace($RootPath)) {
        return $null
    }

    $runRoot = Join-Path ([System.IO.Path]::GetFullPath($RootPath)) ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path (Join-Path $runRoot 'commands') | Out-Null
    [System.IO.File]::Copy($ManifestPath, (Join-Path $runRoot 'input-manifest.json'), $false)
    $policyPath = Join-Path $PSScriptRoot 'cppcheck-vendor-dispositions.json'
    [PSCustomObject]@{
        ScriptPath = $PSCommandPath
        ScriptSHA256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        PolicyPath = $policyPath
        PolicySHA256 = (Get-FileHash -LiteralPath $policyPath -Algorithm SHA256).Hash
        AnalyzerPath = $CppcheckPath
        AnalyzerSHA256 = $CppcheckSHA256
        AnalyzerVersion = $CppcheckVersion
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'tool-provenance.json') -NoNewline

    return [PSCustomObject]@{ Root = $runRoot; Counter = 0 }
}

function Invoke-LintChild {
    param(
        [string]$Path,
        [string[]]$Arguments,
        [string]$Label,
        [object]$Evidence = $null,
        [string]$WorkingDirectory = ''
    )

    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        Push-Location -LiteralPath $WorkingDirectory
    }

    try {
        $output = @(& $Path @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally {
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            Pop-Location
        }
    }

    if ($null -ne $Evidence) {
        $Evidence.Counter++
        $prefix = '{0:D4}-{1}' -f $Evidence.Counter, ($Label -replace '[^A-Za-z0-9._-]', '_')
        [System.IO.File]::WriteAllText((Join-Path $Evidence.Root (Join-Path 'commands' ($prefix + '.raw.txt'))), ($output -join [Environment]::NewLine), $utf8)
        [PSCustomObject]@{
            Path = $Path
            Arguments = $Arguments
            WorkingDirectory = $WorkingDirectory
            ExitCode = $exitCode
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Evidence.Root (Join-Path 'commands' ($prefix + '.invocation.json'))) -NoNewline
    }

    return [PSCustomObject]@{ Output = $output; ExitCode = $exitCode }
}

function Save-LintEvidenceState {
    param(
        [object]$Evidence,
        [string]$AnalysisBuildRoot,
        [string]$BaselineBuildRoot,
        [string]$TempRoot,
        [object[]]$AnalysisProjects,
        [object[]]$BaselineProjects,
        [hashtable]$Contexts,
        [object[]]$VerifiedGeneratedBundle
    )

    if ($null -eq $Evidence) {
        return
    }

    foreach ($snapshot in @(@{ Name = 'source'; BuildRoot = $AnalysisBuildRoot }, @{ Name = 'baseline'; BuildRoot = $BaselineBuildRoot })) {
        $destination = Join-Path $Evidence.Root (Join-Path 'build-inputs' $snapshot.Name)
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-Item -LiteralPath (Join-Path $snapshot.BuildRoot 'CMakeCache.txt') -Destination (Join-Path $destination 'CMakeCache.txt') -Force
        Get-ChildItem -LiteralPath $snapshot.BuildRoot -Filter *.vcxproj -File -Recurse | ForEach-Object {
            $relativePath = Get-PathRelativeToRoot -Path $_.FullName -RootPath $snapshot.BuildRoot
            $projectDestination = Join-Path $destination $relativePath
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $projectDestination) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $projectDestination -Force
        }
    }

    [PSCustomObject]@{
        Analysis = @($AnalysisProjects | ForEach-Object { [PSCustomObject]@{ Target = $_.RelativeProject; TranslationUnits = @($_.Files) } })
        Baseline = @($BaselineProjects | ForEach-Object { [PSCustomObject]@{ Target = $_.RelativeProject; TranslationUnits = @($_.Sources.Keys | Sort-Object) } })
        Contexts = $Contexts
        GeneratedBundle = $VerifiedGeneratedBundle
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Evidence.Root 'analysis-context.json') -NoNewline

    foreach ($cacheName in @('head-cache', 'baseline-cache')) {
        $cachePath = Join-Path $TempRoot $cacheName
        if (Test-Path -LiteralPath $cachePath -PathType Container) {
            Copy-Item -LiteralPath $cachePath -Destination (Join-Path $Evidence.Root $cacheName) -Recurse -Force
        }
    }
}

function Save-LintEvidenceResult {
    param(
        [object]$Evidence,
        [string]$Status,
        [int]$ExitCode,
        [string]$Error = '',
        [object]$VendorDispositionResult = $null,
        [object]$Comparison = $null
    )

    if ($null -eq $Evidence) {
        return
    }

    $provenance = Get-Content -LiteralPath (Join-Path $Evidence.Root 'tool-provenance.json') -Raw | ConvertFrom-Json
    [PSCustomObject]@{
        ClassificationOrigin = 'live gate execution'
        Status = $Status
        ExitCode = $ExitCode
        Error = $Error
        Script = [PSCustomObject]@{ Path = $provenance.ScriptPath; SHA256 = $provenance.ScriptSHA256 }
        Policy = [PSCustomObject]@{ Path = $provenance.PolicyPath; SHA256 = $provenance.PolicySHA256 }
        Analyzer = [PSCustomObject]@{ Path = $provenance.AnalyzerPath; SHA256 = $provenance.AnalyzerSHA256; Version = $provenance.AnalyzerVersion }
        Classification = [PSCustomObject]@{
            Raw = if ($VendorDispositionResult) { @($VendorDispositionResult.Raw).Count } else { $null }
            AcceptedVendor = if ($VendorDispositionResult) { @($VendorDispositionResult.Accepted).Count } else { $null }
            Unaccepted = if ($VendorDispositionResult) { @($VendorDispositionResult.Unaccepted).Count } else { $null }
            Baseline = if ($Comparison) { @($Comparison.Baseline).Count } else { $null }
            Unchanged = if ($Comparison) { @($Comparison.Unchanged).Count } else { $null }
            BaselineOnly = if ($Comparison) { @($Comparison.BaselineOnly).Count } else { $null }
            New = if ($Comparison) { @($Comparison.New).Count } else { $null }
            UnresolvedVendor = if ($Comparison) { @($Comparison.UnresolvedVendor).Count } else { $null }
        }
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Evidence.Root 'final-result.json') -NoNewline
}

function Get-PathRelativeToRoot {
    param(
        [string]$Path,
        [string]$RootPath
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRootPath = [System.IO.Path]::GetFullPath($RootPath).TrimEnd('\', '/')

    if ($fullPath.Equals($fullRootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return ''
    }

    foreach ($separator in @('\', '/')) {
        if ($fullPath.StartsWith($fullRootPath + $separator, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $fullPath.Substring($fullRootPath.Length + 1).Replace('\', '/')
        }
    }

    return $null
}

function Assert-NormalPathWithinRoot {
    param(
        [string]$Path,
        [string]$RootPath,
        [string]$Description
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRootPath = [System.IO.Path]::GetFullPath($RootPath).TrimEnd('\', '/')
    $relativePath = Get-PathRelativeToRoot -Path $fullPath -RootPath $fullRootPath

    if ($null -eq $relativePath) {
        throw "$Description escapes the manifest directory."
    }

    $currentPath = $fullRootPath

    foreach ($component in ($relativePath -split '[\\/]' | Where-Object { $_ -ne '' })) {
        $currentPath = Join-Path $currentPath $component
        $item = Get-Item -LiteralPath $currentPath -Force

        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description must not traverse a reparse point."
        }
    }
}

function Get-KnownCppcheckBarePathAlias {
    param(
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or [System.IO.Path]::IsPathRooted($Path) -or ($Path -match '[\\/]')) {
        return $null
    }

    switch ($Path) {
        'xlat_parser.c' { return '<build>/src/xlat_parser.c' }
        'xlat_parser.h' { return '<build>/src/xlat_parser.h' }
        'sc_man_scanner.h' { return '<build>/src/sc_man_scanner.h' }
        'xlat_parser.y' { return 'src/xlat/xlat_parser.y' }
    }

    return $null
}

function Get-RepositoryRelativePath {
    param(
        [string]$Path,
        [string]$RepositoryRoot,
        [string]$BuildRoot = ''
    )

    $knownAlias = Get-KnownCppcheckBarePathAlias -Path $Path

    if ($null -ne $knownAlias) {
        return $knownAlias
    }

    $fullPath = [System.IO.Path]::GetFullPath($Path)

    if (-not [string]::IsNullOrWhiteSpace($BuildRoot)) {
        $buildRelativePath = Get-PathRelativeToRoot -Path $fullPath -RootPath $BuildRoot

        if ($null -ne $buildRelativePath) {
            return '<build>/' + $buildRelativePath
        }
    }

    $repositoryRelativePath = Get-PathRelativeToRoot -Path $fullPath -RootPath $RepositoryRoot

    if ($null -ne $repositoryRelativePath) {
        return $repositoryRelativePath
    }

    return $fullPath.Replace('\', '/')
}

function Get-CMakeCacheSetting {
    param(
        [string]$CachePath,
        [string]$Name
    )

    $match = Select-String -LiteralPath $CachePath -Pattern ("^{0}:([^=]+)=(.*)$" -f [regex]::Escape($Name)) |
    Select-Object -First 1

    if (-not $match) {
        return $null
    }

    $parts = [regex]::Match($match.Line, "^[^:]+:(?<type>[^=]+)=(?<value>.*)$")
    return [PSCustomObject]@{
        Type = $parts.Groups['type'].Value
        Value = $parts.Groups['value'].Value
    }
}

function Get-CMakePythonExecutable {
    param([string]$CachePath)

    foreach ($name in @('PYTHON_EXECUTABLE', 'Python3_EXECUTABLE', '_Python3_EXECUTABLE')) {
        $setting = Get-CMakeCacheSetting -CachePath $CachePath -Name $name

        if ($setting -and -not [string]::IsNullOrWhiteSpace($setting.Value) -and
            (Test-Path -LiteralPath $setting.Value -PathType Leaf)) {
            return $setting.Value
        }
    }

    return $null
}

function Get-RequiredManifestProperty {
    param(
        [object]$Object,
        [string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]

    if (($null -eq $property) -or ($null -eq $property.Value) -or
        (($property.Value -is [string]) -and [string]::IsNullOrWhiteSpace($property.Value))) {
        throw "InputManifest is missing required property '$Name'."
    }

    return $property.Value
}

function ConvertTo-SafeRelativePath {
    param(
        [string]$Path,
        [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or [System.IO.Path]::IsPathRooted($Path) -or
        ($Path -match '^[\\/]') -or ($Path -match '^[a-zA-Z]:')) {
        throw "$Description must be a non-empty relative path."
    }

    $normalized = $Path.Replace('\', '/')

    if (($normalized -split '/') | Where-Object { ($_ -eq '') -or ($_ -eq '.') -or ($_ -eq '..') }) {
        throw "$Description contains an unsafe path component."
    }

    return $normalized
}

function Get-InputManifest {
    param([string]$ManifestPath)

    try {
        $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    }
    catch {
        throw "InputManifest '$ManifestPath' is not valid JSON: $($_.Exception.Message)"
    }

    if ($null -eq $manifest) {
        throw "InputManifest '$ManifestPath' is empty."
    }

    if ([int](Get-RequiredManifestProperty -Object $manifest -Name 'SchemaVersion') -ne 1) {
        throw "InputManifest '$ManifestPath' has an unsupported SchemaVersion."
    }

    $files = @($manifest.Files)

    if ($files.Count -eq 0) {
        throw "InputManifest '$ManifestPath' contains no file operations."
    }

    $manifestDirectory = Split-Path -Parent ([System.IO.Path]::GetFullPath($ManifestPath))
    $seenPaths = @{}
    $validatedFiles = @()

    foreach ($file in $files) {
        $relativePath = ConvertTo-SafeRelativePath -Path ([string](Get-RequiredManifestProperty -Object $file -Name 'Path')) -Description 'InputManifest file path'

        if ($seenPaths.ContainsKey($relativePath)) {
            throw "InputManifest contains duplicate file operation '$relativePath'."
        }

        $seenPaths[$relativePath] = $true
        $operation = [string](Get-RequiredManifestProperty -Object $file -Name 'Operation')

        if ($operation -notin @('Add', 'Modify')) {
            throw "InputManifest file '$relativePath' has unsupported operation '$operation'."
        }

        $payloadRelativePath = ConvertTo-SafeRelativePath -Path ([string](Get-RequiredManifestProperty -Object $file -Name 'PayloadPath')) -Description "InputManifest payload path for '$relativePath'"
        $payloadPath = [System.IO.Path]::GetFullPath((Join-Path $manifestDirectory $payloadRelativePath))

        if (-not [System.IO.File]::Exists($payloadPath)) {
            throw "InputManifest payload '$payloadRelativePath' for '$relativePath' does not exist or is not a normal file."
        }

        Assert-NormalPathWithinRoot -Path $payloadPath -RootPath $manifestDirectory -Description "InputManifest payload '$payloadRelativePath' for '$relativePath'"

        $payloadItem = Get-Item -LiteralPath $payloadPath -Force

        if (($payloadItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "InputManifest payload '$payloadRelativePath' for '$relativePath' must not be a reparse point."
        }

        $expectedHash = [string](Get-RequiredManifestProperty -Object $file -Name 'SHA256')

        if ($expectedHash -notmatch '^[0-9a-fA-F]{64}$') {
            throw "InputManifest file '$relativePath' has an invalid SHA256."
        }

        $actualHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash

        if (-not $actualHash.Equals($expectedHash, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "InputManifest payload hash mismatch for '$relativePath'."
        }

        $validatedFiles += [PSCustomObject]@{
            Path = $relativePath
            Operation = $operation
            PayloadPath = $payloadPath
            SHA256 = $actualHash
        }
    }

    $cmake = Get-RequiredManifestProperty -Object $manifest -Name 'CMake'
    foreach ($name in @('Generator', 'Platform', 'Toolset', 'CacheSHA256', 'ProductionContext', 'Settings')) {
        [void](Get-RequiredManifestProperty -Object $cmake -Name $name)
    }

    return [PSCustomObject]@{
        Path = [System.IO.Path]::GetFullPath($ManifestPath)
        BaseCommit = [string](Get-RequiredManifestProperty -Object $manifest -Name 'BaseCommit')
        SourceCommit = [string](Get-RequiredManifestProperty -Object $manifest -Name 'SourceCommit')
        Files = $validatedFiles
        CMake = $cmake
    }
}

function Expand-GitArchive {
    param(
        [string]$Commit,
        [string]$Destination,
        [object]$Evidence = $null
    )

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $archivePath = Join-Path ([System.IO.Path]::GetTempPath()) ('zandronum-input-' + [guid]::NewGuid().ToString('N') + '.tar')

    try {
        $archiveResult = Invoke-LintChild -Path 'git' -Arguments @('archive', '--format=tar', "--output=$archivePath", $Commit) -Label "archive-$Commit" -Evidence $Evidence

        if ($archiveResult.ExitCode -ne 0) {
            throw "Could not archive Git commit '$Commit'."
        }

        $extractResult = Invoke-LintChild -Path 'tar' -Arguments @('-xf', $archivePath, '-C', $Destination) -Label "extract-$Commit" -Evidence $Evidence

        if ($extractResult.ExitCode -ne 0) {
            throw "Could not extract Git archive for '$Commit'."
        }
    }
    finally {
        Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
    }
}

function Copy-InputManifestFiles {
    param(
        [object[]]$Files,
        [string]$SourceRoot
    )

    foreach ($file in $Files) {
        $destination = Join-Path $SourceRoot $file.Path
        $exists = [System.IO.File]::Exists($destination)

        if ((($file.Operation -eq 'Add') -and $exists) -or (($file.Operation -eq 'Modify') -and (-not $exists))) {
            throw "InputManifest operation '$($file.Operation)' does not match archived source state for '$($file.Path)'."
        }

        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $file.PayloadPath -Destination $destination -Force

        $actualHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash

        if ($actualHash -ne $file.SHA256) {
            throw "InputManifest materialization hash mismatch for '$($file.Path)'."
        }
    }
}

function Get-ByteSHA256 {
    param([byte[]]$Bytes)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()

    try {
        return ([BitConverter]::ToString($sha256.ComputeHash($Bytes))).Replace('-', '')
    }
    finally {
        $sha256.Dispose()
    }
}

function Convert-LFBytesToCRLFBytes {
    param([byte[]]$Bytes)

    if ($Bytes -contains [byte]13) {
        throw 'API root bytes contain CR and cannot be represented as CRLF safely.'
    }

    $stream = New-Object System.IO.MemoryStream

    try {
        foreach ($byte in $Bytes) {
            if ($byte -eq 10) {
                $stream.WriteByte(13)
            }

            $stream.WriteByte($byte)
        }

        return $stream.ToArray()
    }
    finally {
        $stream.Dispose()
    }
}

function Resolve-ApiRootRepresentation {
    param(
        [byte[]]$Bytes,
        [string]$ExpectedSHA256,
        [string]$Path
    )

    $rawHash = Get-ByteSHA256 -Bytes $Bytes

    if ($rawHash.Equals($ExpectedSHA256, [System.StringComparison]::OrdinalIgnoreCase)) {
        return [PSCustomObject]@{ Bytes = $Bytes; OriginalSHA256 = $rawHash; FinalSHA256 = $rawHash; Representation = 'raw' }
    }

    if ($Bytes -contains [byte]13) {
        throw "API root '$Path' does not match its policy SHA256 and cannot be converted because it contains CR bytes."
    }

    $candidate = Convert-LFBytesToCRLFBytes -Bytes $Bytes
    $candidateHash = Get-ByteSHA256 -Bytes $candidate

    if (-not $candidateHash.Equals($ExpectedSHA256, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "API root '$Path' does not match its policy SHA256 in raw or LF-to-CRLF representation."
    }

    return [PSCustomObject]@{ Bytes = $candidate; OriginalSHA256 = $rawHash; FinalSHA256 = $candidateHash; Representation = 'lf-to-crlf' }
}

function Get-ManifestApiRoots {
    param(
        [object[]]$Files,
        [string]$PolicyPath
    )

    $policy = Get-Content -LiteralPath $PolicyPath -Raw | ConvertFrom-Json
    $roots = @{}
    $payloadPaths = @($Files | ForEach-Object { $_.Path })

    foreach ($disposition in @($policy.Dispositions)) {
        foreach ($apiRoot in @($disposition.Preconditions.ApiRoots)) {
            $path = ConvertTo-SafeRelativePath -Path ([string](Get-RequiredManifestProperty -Object $apiRoot -Name 'Path')) -Description 'Vendor disposition API root path'
            $expectedHash = [string](Get-RequiredManifestProperty -Object $apiRoot -Name 'SHA256')

            if ($expectedHash -notmatch '^[0-9a-fA-F]{64}$') {
                throw "Vendor disposition API root '$path' has an invalid SHA256."
            }

            if ($payloadPaths -contains $path) {
                throw "Vendor disposition API root '$path' overlaps an InputManifest payload."
            }

            if ($roots.ContainsKey($path)) {
                if (-not $roots[$path].SHA256.Equals($expectedHash, [System.StringComparison]::OrdinalIgnoreCase)) {
                    throw "Vendor disposition API root '$path' has conflicting expected SHA256 values."
                }

                continue
            }

            $roots[$path] = [PSCustomObject]@{ Path = $path; SHA256 = $expectedHash.ToUpperInvariant() }
        }
    }

    return @($roots.Values | Sort-Object Path)
}

function Assert-ApiRootBlobIdentity {
    param(
        [string]$Path,
        [string]$SourceBlobId,
        [string]$BaseBlobId
    )

    if ([string]::IsNullOrWhiteSpace($SourceBlobId) -or [string]::IsNullOrWhiteSpace($BaseBlobId) -or ($SourceBlobId -ne $BaseBlobId)) {
        throw "Vendor disposition API root '$Path' has differing source and baseline blob IDs."
    }
}

function Restore-ManifestApiRoots {
    param(
        [object[]]$Files,
        [string]$PolicyPath,
        [string]$SourceCommit,
        [string]$BaseCommit,
        [string]$SourceRoot,
        [string]$BaselineRoot,
        [object]$Evidence = $null
    )

    $records = @()

    foreach ($apiRoot in @(Get-ManifestApiRoots -Files $Files -PolicyPath $PolicyPath)) {
        $sourceBlobId = Get-GitSingleLine -Arguments @('rev-parse', "$SourceCommit`:$($apiRoot.Path)") -ErrorMessage "Could not resolve source blob for API root '$($apiRoot.Path)'."
        $baseBlobId = Get-GitSingleLine -Arguments @('rev-parse', "$BaseCommit`:$($apiRoot.Path)") -ErrorMessage "Could not resolve baseline blob for API root '$($apiRoot.Path)'."
        Assert-ApiRootBlobIdentity -Path $apiRoot.Path -SourceBlobId $sourceBlobId -BaseBlobId $baseBlobId
        $sourcePath = Join-Path $SourceRoot $apiRoot.Path
        $baselinePath = Join-Path $BaselineRoot $apiRoot.Path

        if ((-not [System.IO.File]::Exists($sourcePath)) -or (-not [System.IO.File]::Exists($baselinePath))) {
            throw "Vendor disposition API root '$($apiRoot.Path)' is missing from an isolated archive."
        }

        $sourceBytes = [System.IO.File]::ReadAllBytes($sourcePath)
        $baselineBytes = [System.IO.File]::ReadAllBytes($baselinePath)

        if ((Get-GitSingleLine -Arguments @('hash-object', '--', $sourcePath) -ErrorMessage "Could not hash archived source API root '$($apiRoot.Path)'.") -ne $sourceBlobId -or
            (Get-GitSingleLine -Arguments @('hash-object', '--', $baselinePath) -ErrorMessage "Could not hash archived baseline API root '$($apiRoot.Path)'.") -ne $baseBlobId) {
            throw "Vendor disposition API root '$($apiRoot.Path)' archive bytes do not match their committed blobs."
        }

        $sourceRepresentation = Resolve-ApiRootRepresentation -Bytes $sourceBytes -ExpectedSHA256 $apiRoot.SHA256 -Path $apiRoot.Path
        $baselineRepresentation = Resolve-ApiRootRepresentation -Bytes $baselineBytes -ExpectedSHA256 $apiRoot.SHA256 -Path $apiRoot.Path
        [System.IO.File]::WriteAllBytes($sourcePath, $sourceRepresentation.Bytes)
        [System.IO.File]::WriteAllBytes($baselinePath, $baselineRepresentation.Bytes)
        $records += [PSCustomObject]@{
            Path = $apiRoot.Path
            ExpectedSHA256 = $apiRoot.SHA256
            SourceBlobId = $sourceBlobId
            BaseBlobId = $baseBlobId
            SourceOriginalSHA256 = $sourceRepresentation.OriginalSHA256
            BaselineOriginalSHA256 = $baselineRepresentation.OriginalSHA256
            SourceRepresentation = $sourceRepresentation.Representation
            BaselineRepresentation = $baselineRepresentation.Representation
            SourceFinalSHA256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
            BaselineFinalSHA256 = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash
        }
    }

    if ($null -ne $Evidence) {
        [PSCustomObject]@{
            PolicyPath = $PolicyPath
            PolicySHA256 = (Get-FileHash -LiteralPath $PolicyPath -Algorithm SHA256).Hash
            ApiRoots = @($records)
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Evidence.Root 'api-root-representation.json') -NoNewline
    }

    return @($records)
}

function Get-InputManifestCMakeSettings {
    param([object]$CMake)

    $settings = @($CMake.Settings)
    $seenNames = @{}
    $validated = @()

    foreach ($setting in $settings) {
        $name = [string](Get-RequiredManifestProperty -Object $setting -Name 'Name')
        $type = [string](Get-RequiredManifestProperty -Object $setting -Name 'Type')
        $value = [string](Get-RequiredManifestProperty -Object $setting -Name 'Value')

        if (($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') -or $seenNames.ContainsKey($name)) {
            throw "InputManifest CMake setting '$name' is invalid or duplicated."
        }

        if ($type -notin @('BOOL', 'FILEPATH', 'PATH', 'STRING')) {
            throw "InputManifest CMake setting '$name' has unsupported type '$type'."
        }

        $seenNames[$name] = $true
        $validated += [PSCustomObject]@{ Name = $name; Type = $type; Value = $value }
    }

    foreach ($requiredName in @('BUILD_TESTING', 'DYN_FLUIDSYNTH', 'NO_SOUND', 'FMOD_INCLUDE_DIR', 'FMOD_LIBRARY', 'OPENAL_INCLUDE_DIR', 'OPENAL_LIBRARY', 'OPUS_INCLUDE_DIR', 'OPUS_LIBRARIES', 'ZSTD_INCLUDE_DIR', 'ZSTD_LIBRARY', 'FLUIDSYNTH_INCLUDE_DIR', 'FLUIDSYNTH_LIBRARIES')) {
        if (-not $seenNames.ContainsKey($requiredName)) {
            throw "InputManifest CMake settings are missing required '$requiredName'."
        }
    }

    return $validated
}

function Assert-InputManifestCMakeCapture {
    param(
        [object]$Manifest,
        [string]$CachePath,
        [switch]$SkipCacheHash
    )

    if (-not $SkipCacheHash) {
        $cacheHash = (Get-FileHash -LiteralPath $CachePath -Algorithm SHA256).Hash

        if (-not $cacheHash.Equals([string]$Manifest.CMake.CacheSHA256, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'InputManifest CMakeCache SHA256 does not match the captured production cache.'
        }
    }

    foreach ($pair in @(@{ Name = 'CMAKE_GENERATOR'; Value = [string]$Manifest.CMake.Generator }, @{ Name = 'CMAKE_GENERATOR_PLATFORM'; Value = [string]$Manifest.CMake.Platform }, @{ Name = 'CMAKE_GENERATOR_TOOLSET'; Value = [string]$Manifest.CMake.Toolset })) {
        $actual = Get-CMakeCacheSetting -CachePath $CachePath -Name $pair.Name
        $actualValue = if ($actual) { $actual.Value } else { '' }

        if ($actualValue -ne $pair.Value) {
            throw "InputManifest CMake capture does not match '$($pair.Name)'."
        }
    }

    foreach ($setting in Get-InputManifestCMakeSettings -CMake $Manifest.CMake) {
        $actual = Get-CMakeCacheSetting -CachePath $CachePath -Name $setting.Name

        if (($null -eq $actual) -or ($actual.Type -ne $setting.Type) -or ($actual.Value -ne $setting.Value)) {
            throw "InputManifest CMake capture does not match '$($setting.Name)'."
        }
    }
}

function Assert-InputManifestProductionContext {
    param(
        [object]$Manifest,
        [string]$RepositoryRoot
    )

    $projects = @(Get-RequiredManifestProperty -Object $Manifest.CMake.ProductionContext -Name 'Projects')

    if ($projects.Count -eq 0) {
        throw 'InputManifest ProductionContext contains no project records.'
    }

    $seenPaths = @{}

    foreach ($project in $projects) {
        $relativePath = ConvertTo-SafeRelativePath -Path ([string](Get-RequiredManifestProperty -Object $project -Name 'Path')) -Description 'InputManifest ProductionContext project path'
        $expectedHash = [string](Get-RequiredManifestProperty -Object $project -Name 'SHA256')

        if (($expectedHash -notmatch '^[0-9a-fA-F]{64}$') -or $seenPaths.ContainsKey($relativePath)) {
            throw "InputManifest ProductionContext project '$relativePath' has an invalid or duplicate SHA256."
        }

        $seenPaths[$relativePath] = $true
        $projectPath = Join-Path $RepositoryRoot $relativePath

        if (-not [System.IO.File]::Exists($projectPath)) {
            throw "InputManifest ProductionContext project '$relativePath' is missing or is not a normal file."
        }

        $projectItem = Get-Item -LiteralPath $projectPath -Force

        if (($projectItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "InputManifest ProductionContext project '$relativePath' must not be a reparse point."
        }

        $actualHash = (Get-FileHash -LiteralPath $projectPath -Algorithm SHA256).Hash

        if (-not $actualHash.Equals($expectedHash, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "InputManifest ProductionContext project '$relativePath' does not match the captured SHA256."
        }
    }
}

function Invoke-InputManifestPreflight {
    param(
        [object]$Manifest,
        [string]$RepositoryRoot,
        [string]$CachePath,
        [string]$CMakePath,
        [switch]$KeepTemporaryRoot,
        [object]$Evidence = $null
    )

    $changedSources = @($Manifest.Files | Where-Object { $_.Path -match '\.(c|cc|cpp|cxx)$' } | ForEach-Object { $_.Path })

    if ($changedSources.Count -eq 0) {
        throw 'InputManifest contains no C/C++ translation units for the full Cppcheck gate.'
    }

    Assert-InputManifestCMakeCapture -Manifest $Manifest -CachePath $CachePath
    Assert-InputManifestProductionContext -Manifest $Manifest -RepositoryRoot $RepositoryRoot
    $baseCommit = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', "$($Manifest.BaseCommit)^{commit}") -ErrorMessage "InputManifest BaseCommit '$($Manifest.BaseCommit)' does not resolve to a commit."
    $sourceCommit = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', "$($Manifest.SourceCommit)^{commit}") -ErrorMessage "InputManifest SourceCommit '$($Manifest.SourceCommit)' does not resolve to a commit."
    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('zandronum-input-preflight-' + [guid]::NewGuid().ToString('N'))
    $sourceRoot = Join-Path $tempRoot 'source'
    $baselineRoot = Join-Path $tempRoot 'baseline'
    $sourceBuildRoot = Join-Path $tempRoot 'source-build'
    $baselineBuildRoot = Join-Path $tempRoot 'baseline-build'
    $result = $null

    try {
        Expand-GitArchive -Commit $sourceCommit -Destination $sourceRoot -Evidence $Evidence
        Expand-GitArchive -Commit $baseCommit -Destination $baselineRoot -Evidence $Evidence
        [void](Restore-ManifestApiRoots `
            -Files $Manifest.Files `
            -PolicyPath (Join-Path $PSScriptRoot 'cppcheck-vendor-dispositions.json') `
            -SourceCommit $sourceCommit `
            -BaseCommit $baseCommit `
            -SourceRoot $sourceRoot `
            -BaselineRoot $baselineRoot `
            -Evidence $Evidence)
        Copy-InputManifestFiles -Files $Manifest.Files -SourceRoot $sourceRoot

        foreach ($file in $Manifest.Files) {
            $sourcePath = Join-Path $sourceRoot $file.Path
            $baselinePath = Join-Path $baselineRoot $file.Path
            $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash

            if ($sourceHash -ne $file.SHA256) {
                throw "InputManifest realized source does not match selected payload for '$($file.Path)'."
            }

            if (($file.Operation -eq 'Modify') -and ((Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash -eq $sourceHash)) {
                throw "InputManifest realized diff does not contain required modification '$($file.Path)'."
            }
        }

        $configureArguments = @('-G', [string]$Manifest.CMake.Generator)

        if (-not [string]::IsNullOrWhiteSpace([string]$Manifest.CMake.Platform)) {
            $configureArguments += @('-A', [string]$Manifest.CMake.Platform)
        }

        if (-not [string]::IsNullOrWhiteSpace([string]$Manifest.CMake.Toolset)) {
            $configureArguments += @('-T', [string]$Manifest.CMake.Toolset)
        }

        foreach ($setting in Get-InputManifestCMakeSettings -CMake $Manifest.CMake) {
            $configureArguments += "-D$($setting.Name):$($setting.Type)=$($setting.Value)"
        }

        foreach ($configuration in @(@{ Root = $sourceRoot; Build = $sourceBuildRoot; Name = 'source' }, @{ Root = $baselineRoot; Build = $baselineBuildRoot; Name = 'baseline' })) {
            $configureResult = Invoke-LintChild -Path $CMakePath -Arguments (@('-S', $configuration.Root, '-B', $configuration.Build) + $configureArguments) -Label "configure-$($configuration.Name)" -Evidence $Evidence
            $output = $configureResult.Output

            if ($configureResult.ExitCode -ne 0) {
                throw "InputManifest $($configuration.Name) configure failed: $($output -join [Environment]::NewLine)"
            }

            Assert-InputManifestCMakeCapture -Manifest $Manifest -CachePath (Join-Path $configuration.Build 'CMakeCache.txt') -SkipCacheHash
        }

        Invoke-GeneratedBuildInputPreparation -CMakePath $CMakePath -BuildRoot $sourceBuildRoot -RevisionName 'source' -Evidence $Evidence
        Invoke-GeneratedBuildInputPreparation -CMakePath $CMakePath -BuildRoot $baselineBuildRoot -RevisionName 'baseline' -Evidence $Evidence
        Invoke-ProtocolspecGeneration -CMakePath $CMakePath -BuildRoot $sourceBuildRoot -RevisionName 'source' -Evidence $Evidence
        $verifiedGeneratedBundle = Get-VerifiedGeneratedBundle `
            -SourceRoot $sourceRoot `
            -GeneratedOutputRoot $sourceRoot `
            -BaseCommit $baseCommit `
            -HeadCommit $sourceCommit `
            -TempRoot $tempRoot `
            -RelativePaths $generatedBundlePaths `
            -PythonPath (Get-CMakePythonExecutable -CachePath (Join-Path $sourceBuildRoot 'CMakeCache.txt')) `
            -Evidence $Evidence
        Copy-VerifiedGeneratedBundle -VerifiedBundle $verifiedGeneratedBundle -DestinationRoot $baselineRoot

        $productionBuildRoot = Split-Path -Parent $CachePath
        $productionProjects = Get-ProjectSources -BuildRoot $productionBuildRoot -RepositoryRoot $RepositoryRoot
        $sourceProjects = Get-ProjectSources -BuildRoot $sourceBuildRoot -RepositoryRoot $sourceRoot
        $baselineProjects = Get-ProjectSources -BuildRoot $baselineBuildRoot -RepositoryRoot $baselineRoot
        $selectedProjects = @()

        foreach ($sourcePath in $changedSources) {
            $projectMatches = @($sourceProjects | Where-Object { $_.Sources.ContainsKey($sourcePath) })

            if ($projectMatches.Count -eq 0) {
                throw "InputManifest C/C++ source '$sourcePath' is not present in an archive-generated Visual Studio project."
            }

            foreach ($project in $projectMatches) {
                $project.Files = @($project.Sources.Keys | Sort-Object)
                $selectedProjects += $project
            }
        }

        $selectedProjects = @($selectedProjects | Sort-Object RelativeProject -Unique)

        if ($selectedProjects.Count -eq 0) {
            throw 'InputManifest contains no C/C++ translation units for the full Cppcheck gate.'
        }

        Save-LintEvidenceState -Evidence $Evidence -AnalysisBuildRoot $sourceBuildRoot -BaselineBuildRoot $baselineBuildRoot -TempRoot $tempRoot -AnalysisProjects $selectedProjects -BaselineProjects $baselineProjects -Contexts @{} -VerifiedGeneratedBundle $verifiedGeneratedBundle

        foreach ($project in $selectedProjects) {
            $productionProject = $productionProjects | Where-Object { $_.RelativeProject -eq $project.RelativeProject } | Select-Object -First 1
            $baselineProject = $baselineProjects | Where-Object { $_.RelativeProject -eq $project.RelativeProject } | Select-Object -First 1

            if (($null -eq $productionProject) -or ($null -eq $baselineProject)) {
                throw "InputManifest target '$($project.RelativeProject)' is missing from the production or baseline project set."
            }

            $sourceContext = Get-CppcheckVendorDispositionContext -ProjectPath $project.ProjectPath -TargetName $project.RelativeProject -AnalyzerVersion 'preflight'
            $productionContext = Get-CppcheckVendorDispositionContext -ProjectPath $productionProject.ProjectPath -TargetName $project.RelativeProject -AnalyzerVersion 'preflight'
            $baselineContext = Get-CppcheckVendorDispositionContext -ProjectPath $baselineProject.ProjectPath -TargetName $project.RelativeProject -AnalyzerVersion 'preflight'
            Assert-EqualCppcheckProjectContext -Expected (ConvertTo-NormalizedCppcheckProjectContext -Context $productionContext -ProjectPath $productionProject.ProjectPath -RepositoryRoot $RepositoryRoot -BuildRoot $productionBuildRoot) -Actual (ConvertTo-NormalizedCppcheckProjectContext -Context $sourceContext -ProjectPath $project.ProjectPath -RepositoryRoot $sourceRoot -BuildRoot $sourceBuildRoot) -Description "production and source target '$($project.RelativeProject)'"
            Assert-EqualCppcheckProjectContext -Expected (ConvertTo-NormalizedCppcheckProjectContext -Context $sourceContext -ProjectPath $project.ProjectPath -RepositoryRoot $sourceRoot -BuildRoot $sourceBuildRoot) -Actual (ConvertTo-NormalizedCppcheckProjectContext -Context $baselineContext -ProjectPath $baselineProject.ProjectPath -RepositoryRoot $baselineRoot -BuildRoot $baselineBuildRoot) -Description "source and baseline target '$($project.RelativeProject)'"
            Assert-EqualTranslationUnitSet -Expected @($productionProject.Sources.Keys) -Actual @($project.Sources.Keys) -Description "production and source target '$($project.RelativeProject)'"
            Assert-EqualTranslationUnitSet -Expected @($project.Sources.Keys) -Actual @($baselineProject.Sources.Keys) -Description "source and baseline target '$($project.RelativeProject)'"
            Write-Host "InputManifest selected target: $($project.RelativeProject) ($($project.Files.Count) source TU(s), $($baselineProject.Sources.Count) baseline TU(s))."
        }

        Write-Host "InputManifest preflight passed: source $sourceCommit, baseline $baseCommit, $($Manifest.Files.Count) verified file operation(s)."
        $result = [PSCustomObject]@{
            TempRoot = $tempRoot
            SourceRoot = $sourceRoot
            SourceBuildRoot = $sourceBuildRoot
            BaselineRoot = $baselineRoot
            BaselineBuildRoot = $baselineBuildRoot
            VerifiedGeneratedBundle = $verifiedGeneratedBundle
        }
    }
    finally {
        if (((-not $KeepTemporaryRoot) -or ($null -eq $result)) -and (Test-Path -LiteralPath $tempRoot)) {
            Remove-Item -LiteralPath $tempRoot -Force -Recurse -ErrorAction SilentlyContinue
        }
    }

    return $result
}

function Get-GitSingleLine {
    param(
        [string[]]$Arguments,
        [string]$ErrorMessage
    )

    $output = @(& git @Arguments 2>$null)

    if (($LASTEXITCODE -ne 0) -or ($output.Count -ne 1) -or [string]::IsNullOrWhiteSpace($output[0])) {
        Write-Error $ErrorMessage
        exit 1
    }

    return $output[0].Trim()
}

function Get-ProtocolspecProvenanceInputs {
    param(
        [string]$Commit
    )

    $inputs = @{}
    $entries = @(& git ls-tree -r --full-tree $Commit -- protocolspec)

    if ($LASTEXITCODE -ne 0) {
        throw "Could not inspect protocolspec provenance for '$Commit'."
    }

    foreach ($entry in $entries) {
        $match = [regex]::Match($entry, '^[0-9]+ blob (?<blob>[0-9a-f]+)\t(?<path>.+)$')

        if ($match.Success) {
            $inputs[$match.Groups['path'].Value] = $match.Groups['blob'].Value
        }
    }

    if ($inputs.Count -eq 0) {
        throw "Protocolspec provenance for '$Commit' contains no generator or specification inputs."
    }

    return $inputs
}

function Assert-EqualProtocolspecProvenance {
    param(
        [string]$BaseCommit,
        [string]$HeadCommit
    )

    $baseInputs = Get-ProtocolspecProvenanceInputs -Commit $BaseCommit
    $headInputs = Get-ProtocolspecProvenanceInputs -Commit $HeadCommit
    $differentPaths = @()

    foreach ($relativePath in @($baseInputs.Keys + $headInputs.Keys | Sort-Object -Unique)) {
        if ((-not $baseInputs.ContainsKey($relativePath)) -or
            (-not $headInputs.ContainsKey($relativePath)) -or
            ($baseInputs[$relativePath] -ne $headInputs[$relativePath])) {
            $differentPaths += $relativePath
        }
    }

    if ($differentPaths.Count -ne 0) {
        throw "Generated bundle cannot be materialized because protocolspec provenance differs: $($differentPaths -join ', ')."
    }

    return $headInputs
}

function Assert-MaterializedProtocolspecProvenance {
    param(
        [string]$SourceRoot,
        [hashtable]$ExpectedInputs
    )

    $differentPaths = @()

    foreach ($relativePath in @($ExpectedInputs.Keys | Sort-Object)) {
        $sourcePath = Join-Path $SourceRoot $relativePath

        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            $differentPaths += $relativePath
            continue
        }

        $sourceItem = Get-Item -LiteralPath $sourcePath -Force

        if (($sourceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            $differentPaths += $relativePath
            continue
        }

        $actualBlob = @(& git hash-object -- $sourcePath 2>$null)

        if (($LASTEXITCODE -ne 0) -or ($actualBlob.Count -ne 1) -or ($actualBlob[0].Trim() -ne $ExpectedInputs[$relativePath])) {
            $differentPaths += $relativePath
        }
    }

    if ($differentPaths.Count -ne 0) {
        throw "Generated bundle cannot be materialized because protocolspec inputs differ from the archived commit: $($differentPaths -join ', ')."
    }
}

function Get-VerifiedGeneratedBundle {
    param(
        [string]$SourceRoot,
        [string]$GeneratedOutputRoot,
        [string]$BaseCommit,
        [string]$HeadCommit,
        [string]$TempRoot,
        [string[]]$RelativePaths,
        [string]$PythonPath,
        [object]$Evidence = $null
    )

    if ([string]::IsNullOrWhiteSpace($PythonPath) -or (-not (Test-Path -LiteralPath $PythonPath -PathType Leaf))) {
        throw 'The current CMake cache does not provide a usable PYTHON_EXECUTABLE for protocolspec generation.'
    }

    foreach ($relativePath in $RelativePaths) {
        foreach ($commit in @($BaseCommit, $HeadCommit)) {
            $trackedPaths = @(& git ls-tree --full-tree --name-only $commit -- $relativePath 2>&1)

            if ($LASTEXITCODE -ne 0) {
                $details = ($trackedPaths -join [Environment]::NewLine).Trim()
                throw "Could not inspect generated bundle path '$relativePath' in Git tree '$commit': $details"
            }

            if ($trackedPaths.Count -ne 0) {
                throw "Generated bundle path '$relativePath' is tracked by a Git input tree."
            }
        }

        $sourcePath = Join-Path $SourceRoot $relativePath

        if (-not [System.IO.File]::Exists($sourcePath)) {
            throw "Generated bundle path '$relativePath' is missing or is not a normal source file."
        }

        $sourceItem = Get-Item -LiteralPath $sourcePath -Force

        if (($sourceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Generated bundle path '$relativePath' must be a normal source file, not a reparse point."
        }
    }

    $headInputs = Assert-EqualProtocolspecProvenance -BaseCommit $BaseCommit -HeadCommit $HeadCommit
    Assert-MaterializedProtocolspecProvenance -SourceRoot $SourceRoot -ExpectedInputs $headInputs
    $generatedRoot = Join-Path $TempRoot 'generated'
    $sourceOutput = Join-Path $generatedRoot $RelativePaths[0]
    $headerOutput = Join-Path $generatedRoot $RelativePaths[1]
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sourceOutput) | Out-Null

    $generatorPath = Join-Path $SourceRoot 'protocolspec/generator/codegenerator.py'
    $specPath = Join-Path $SourceRoot 'protocolspec/spec.txt'
    $generatorDirectory = Split-Path -Parent $generatorPath
    $generatorResult = Invoke-LintChild -Path $PythonPath -Arguments @($generatorPath, '--spec', $specPath, '--source', $sourceOutput, '--header', $headerOutput) -Label 'protocolspec-generator' -Evidence $Evidence -WorkingDirectory $generatorDirectory
    $generatorOutput = $generatorResult.Output

    if ($generatorResult.ExitCode -ne 0) {
        $details = ($generatorOutput -join [Environment]::NewLine).Replace($SourceRoot, '<source>').Replace($TempRoot, '<temp>')
        throw "Protocolspec generator failed: $details"
    }

    $verifiedBundle = @()

    foreach ($relativePath in $RelativePaths) {
        $generatedPath = Join-Path $generatedRoot $relativePath
        $outputPath = Join-Path $GeneratedOutputRoot $relativePath

        if ((-not [System.IO.File]::Exists($generatedPath)) -or (-not [System.IO.File]::Exists($outputPath))) {
            throw "Protocolspec generator did not produce expected bundle path '$relativePath'."
        }

        $generatedHash = (Get-FileHash -LiteralPath $generatedPath -Algorithm SHA256).Hash
        $outputHash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash

        if ($generatedHash -ne $outputHash) {
            throw "Generated bundle verification failed for '$relativePath': generated build output SHA-256 does not match generator output."
        }

        $evidencePath = ''

        if ($null -ne $Evidence) {
            $evidencePath = Join-Path 'generated-bundle/original' $relativePath
            $evidenceOutputPath = Join-Path $Evidence.Root $evidencePath
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $evidenceOutputPath) | Out-Null
            [System.IO.File]::Copy($generatedPath, $evidenceOutputPath, $true)

            if ((Get-FileHash -LiteralPath $evidenceOutputPath -Algorithm SHA256).Hash -ne $generatedHash) {
                throw "Generated bundle evidence copy failed for '$relativePath': copied SHA-256 does not match verified generator output."
            }
        }

        $verifiedBundle += [PSCustomObject]@{
            RelativePath = $relativePath
            GeneratedPath = $generatedPath
            Hash = $generatedHash
            EvidencePath = $evidencePath
            EvidenceKind = if ($null -ne $Evidence) { 'verified-generator-output' } else { '' }
        }
    }

    return $verifiedBundle
}

function Get-ProjectSources {
    param(
        [string]$BuildRoot,
        [string]$RepositoryRoot
    )

    $projects = @()

    foreach ($projectFile in Get-ChildItem -LiteralPath $BuildRoot -Filter *.vcxproj -File -Recurse) {
        $sources = @{}
        $projectText = [System.IO.File]::ReadAllText($projectFile.FullName)

        foreach ($match in [regex]::Matches($projectText, '<ClCompile Include="(?<path>[^"]+)"')) {
            $sourcePath = $match.Groups['path'].Value

            if ($sourcePath -notmatch '^\$\(') {
                $sources[(Get-RepositoryRelativePath -Path $sourcePath -RepositoryRoot $RepositoryRoot)] = $sourcePath
            }
        }

        if ($sources.Count -ne 0) {
            $projects += [PSCustomObject]@{
                RelativeProject = Get-RepositoryRelativePath -Path $projectFile.FullName -RepositoryRoot $BuildRoot
                ProjectPath = $projectFile.FullName
                Sources = $sources
                Files = @()
            }
        }
    }

    return $projects
}

function Assert-EqualCppcheckProjectContext {
    param(
        [object]$Expected,
        [object]$Actual,
        [string]$Description
    )

    foreach ($property in @('Compiler', 'Toolset', 'Abi', 'Configuration', 'Defines', 'AdditionalIncludeDirectories')) {
        if ($Expected.$property -ne $Actual.$property) {
            throw "Cppcheck project context mismatch for ${Description}: '$property' differs."
        }
    }
}

function ConvertTo-NormalizedCppcheckProjectContext {
    param(
        [object]$Context,
        [string]$ProjectPath,
        [string]$RepositoryRoot,
        [string]$BuildRoot
    )

    $normalized = [PSCustomObject]@{}

    foreach ($property in @('Compiler', 'Toolset', 'Abi', 'Configuration', 'Defines')) {
        $value = [string]$Context.$property

        if ($property -eq 'Defines') {
            $value = ConvertTo-NormalizedCppcheckPathValue -Value $value -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot
        }

        $normalized | Add-Member -NotePropertyName $property -NotePropertyValue $value
    }

    $normalized | Add-Member -NotePropertyName AdditionalIncludeDirectories -NotePropertyValue (Get-NormalizedCppcheckAdditionalIncludeDirectories -ProjectPath $ProjectPath -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot)

    return $normalized
}

function ConvertTo-NormalizedCppcheckPathValue {
    param(
        [string]$Value,
        [string]$RepositoryRoot,
        [string]$BuildRoot
    )

    foreach ($root in @(@{ Path = $BuildRoot; Token = '<build>' }, @{ Path = $RepositoryRoot; Token = '<source>' })) {
        $normalizedRoot = [System.IO.Path]::GetFullPath($root.Path).TrimEnd('\', '/').Replace('\', '/')
        $pattern = '(?i)(?<![A-Za-z0-9_.+/-])' + [regex]::Escape($normalizedRoot) + '(?=($|[\\/;"'']))'
        $Value = [regex]::Replace($Value.Replace('\', '/'), $pattern, $root.Token)
    }

    return $Value
}

function Get-NormalizedCppcheckAdditionalIncludeDirectories {
    param(
        [string]$ProjectPath,
        [string]$RepositoryRoot,
        [string]$BuildRoot
    )

    $project = New-Object System.Xml.XmlDocument
    $project.Load($ProjectPath)
    $condition = "'`$(Configuration)|`$(Platform)'=='Release|x64'"
    $includeDirectories = @($project.SelectNodes("//*[local-name()='ItemDefinitionGroup']") | Where-Object {
        ($_.GetAttribute('Condition').Trim() -replace '\s+', '') -eq $condition
    } | ForEach-Object {
        $_.SelectNodes("*[local-name()='ClCompile']/*[local-name()='AdditionalIncludeDirectories']")
    } | Where-Object { $null -ne $_ } | ForEach-Object { $_.InnerText.Trim() })

    if ($includeDirectories.Count -gt 1) {
        throw "Could not prove Cppcheck additional include directories from '$ProjectPath'."
    }

    $value = if ($includeDirectories.Count -eq 1) { $includeDirectories[0] } else { '' }

    return (@($value -split ';' | ForEach-Object {
        ConvertTo-NormalizedCppcheckPathValue -Value $_ -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot
    }) -join ';')
}

function ConvertTo-ProductionCppcheckVendorDispositionContext {
    param(
        [object]$Context,
        [string]$AnalysisRepositoryRoot,
        [string]$AnalysisBuildRoot,
        [string]$ProductionRepositoryRoot,
        [string]$ProductionBuildRoot
    )

    $normalized = @{}

    foreach ($property in @('Compiler', 'Toolset', 'Abi', 'Configuration', 'AnalyzerVersion')) {
        $normalized[$property] = $Context.$property
    }

    $defines = [string]$Context.Defines

    foreach ($root in @(@{ Analysis = $AnalysisBuildRoot; Production = $ProductionBuildRoot }, @{ Analysis = $AnalysisRepositoryRoot; Production = $ProductionRepositoryRoot })) {
        $analysisPath = $root.Analysis.TrimEnd('\', '/').Replace('\', '/')
        $productionPath = $root.Production.TrimEnd('\', '/').Replace('\', '/')
        $defines = [regex]::Replace($defines.Replace('\', '/'), '(?i)(?<![A-Za-z0-9_.+/-])' + [regex]::Escape($analysisPath) + '(?=($|[\\/;"'']))', $productionPath)
    }

    $normalized['Defines'] = $defines

    return $normalized
}

function Assert-EqualTranslationUnitSet {
    param(
        [string[]]$Expected,
        [string[]]$Actual,
        [string]$Description
    )

    $difference = @(Compare-Object -ReferenceObject @($Expected | Sort-Object -Unique) -DifferenceObject @($Actual | Sort-Object -Unique))

    if ($difference.Count -ne 0) {
        throw "Cppcheck translation-unit selection mismatch for $Description."
    }
}

function Resolve-CppcheckBaselineTarget {
    param(
        [string]$TargetName,
        [object]$ProductionProject,
        [object]$BaselineProject,
        [object]$ProductionContext,
        [object]$AnalysisContext,
        [object]$BaselineContext,
        [switch]$RequireManifestParity
    )

    if (-not $RequireManifestParity) {
        return $BaselineProject
    }

    if (($null -eq $ProductionProject) -or ($null -eq $BaselineProject)) {
        throw "InputManifest target '$TargetName' is missing from the production or baseline project set."
    }

    Assert-EqualCppcheckProjectContext -Expected $ProductionContext -Actual $AnalysisContext -Description "production and source target '$TargetName'"
    Assert-EqualCppcheckProjectContext -Expected $AnalysisContext -Actual $BaselineContext -Description "source and baseline target '$TargetName'"
    return $BaselineProject
}

function Assert-NormalGeneratedBuildInput {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not [System.IO.File]::Exists($Path)) {
        throw "Generated $Description is missing or is not a normal file: '$Path'."
    }

    $item = Get-Item -LiteralPath $Path -Force

    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Generated $Description must be a normal file, not a reparse point: '$Path'."
    }
}

function Get-GeneratedBuildInputCommands {
    param(
        [string]$BuildRoot
    )

    $projectPath = Join-Path $BuildRoot 'src/zdoom.vcxproj'
    Assert-NormalGeneratedBuildInput -Path $projectPath -Description 'zdoom project file'

    $project = New-Object System.Xml.XmlDocument
    $project.Load($projectPath)
    $configurationCondition = "'`$(Configuration)|`$(Platform)'=='Release|x64'"
    $commands = @()

    foreach ($rule in $generatedBuildInputRules) {
        $expectedOutputs = @($rule.RelativeOutputs | ForEach-Object { [System.IO.Path]::GetFullPath((Join-Path $BuildRoot $_)) })
        $matchingRules = @()

        foreach ($customBuild in $project.SelectNodes('//*[local-name()="CustomBuild"]')) {
            $outputNode = @($customBuild.ChildNodes | Where-Object {
                ($_.LocalName -eq 'Outputs') -and ($_.GetAttribute('Condition') -eq $configurationCondition)
            } | Select-Object -First 1)
            $commandNode = @($customBuild.ChildNodes | Where-Object {
                ($_.LocalName -eq 'Command') -and ($_.GetAttribute('Condition') -eq $configurationCondition)
            } | Select-Object -First 1)

            if (($outputNode.Count -eq 0) -or ($commandNode.Count -eq 0)) {
                continue
            }

            $actualOutputs = @($outputNode[0].InnerText.Split(';') | ForEach-Object { [System.IO.Path]::GetFullPath($_) })

            if (($actualOutputs.Count -ne $expectedOutputs.Count) -or
                (@($actualOutputs | Where-Object { $_ -notin $expectedOutputs }).Count -ne 0) -or
                (@($expectedOutputs | Where-Object { $_ -notin $actualOutputs }).Count -ne 0)) {
                continue
            }

            $matchingRules += [PSCustomObject]@{
                Description = $rule.Description
                Command = $commandNode[0].InnerText
                RelativeOutputs = $rule.RelativeOutputs
            }
        }

        if ($matchingRules.Count -ne 1) {
            throw "CMake project '$projectPath' must define exactly one Release|x64 custom build rule for generated $($rule.Description) inputs."
        }

        $commands += $matchingRules[0]
    }

    return $commands
}

function Invoke-GeneratedBuildInputPreparation {
    param(
        [string]$CMakePath,
        [string]$BuildRoot,
        [string]$RevisionName,
        [object]$Evidence = $null
    )

    $toolResult = Invoke-LintChild -Path $CMakePath -Arguments @('--build', $BuildRoot, '--config', 'Release', '--target', 'lemon', 're2c') -Label "$RevisionName-generated-tools" -Evidence $Evidence
    $toolOutput = $toolResult.Output

    if ($toolResult.ExitCode -ne 0) {
        $details = $toolOutput -join [Environment]::NewLine
        throw "Failed to build generated-input tools for ${RevisionName}: $details"
    }

    foreach ($command in Get-GeneratedBuildInputCommands -BuildRoot $BuildRoot) {
        $batchPath = Join-Path ([System.IO.Path]::GetTempPath()) ("zandronum-cppcheck-generator-" + [guid]::NewGuid().ToString('N') + '.cmd')
        $projectDirectory = Join-Path $BuildRoot 'src'

        try {
            [System.IO.File]::WriteAllText($batchPath, "@echo off`r`n" + $command.Command, $utf8)
            $commandResult = Invoke-LintChild -Path $env:ComSpec -Arguments @('/d', '/c', $batchPath) -Label "$RevisionName-$($command.Description)" -Evidence $Evidence -WorkingDirectory $projectDirectory
            $commandOutput = $commandResult.Output

            if ($commandResult.ExitCode -ne 0) {
                $details = $commandOutput -join [Environment]::NewLine
                throw "Failed to generate $($command.Description) inputs for ${RevisionName}: $details"
            }
        }
        finally {
            Remove-Item -LiteralPath $batchPath -Force -ErrorAction SilentlyContinue
        }

        foreach ($relativeOutput in $command.RelativeOutputs) {
            Assert-NormalGeneratedBuildInput -Path (Join-Path $BuildRoot $relativeOutput) -Description $relativeOutput
        }
    }
}

function Invoke-ProtocolspecGeneration {
    param(
        [string]$CMakePath,
        [string]$BuildRoot,
        [string]$RevisionName,
        [object]$Evidence = $null
    )

    $toolResult = Invoke-LintChild -Path $CMakePath -Arguments @('--build', $BuildRoot, '--config', 'Release', '--target', 'protocolspec') -Label "$RevisionName-protocolspec" -Evidence $Evidence
    $toolOutput = $toolResult.Output

    if ($toolResult.ExitCode -ne 0) {
        $details = $toolOutput -join [Environment]::NewLine
        throw "Failed to generate protocolspec inputs for ${RevisionName}: $details"
    }
}

function Copy-VerifiedGeneratedBundle {
    param(
        [object[]]$VerifiedBundle,
        [string]$DestinationRoot
    )

    foreach ($generatedFile in $VerifiedBundle) {
        $destinationPath = Join-Path $DestinationRoot $generatedFile.RelativePath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destinationPath) | Out-Null
        Copy-Item -LiteralPath $generatedFile.GeneratedPath -Destination $destinationPath -Force
        $destinationHash = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash

        if ($destinationHash -ne $generatedFile.Hash) {
            throw "Generated bundle materialization failed for '$($generatedFile.RelativePath)': destination SHA-256 does not match verified generator output."
        }
    }
}

function Invoke-CppcheckProject {
    param(
        [string]$CppcheckPath,
        [string]$ProjectPath,
        [string]$CachePath,
        [string]$RepositoryRoot,
        [string]$BuildRoot,
        [string]$TargetName,
        [string]$TranslationUnit,
        [int]$CppcheckJobs,
        [object]$Evidence = $null
    )

    New-Item -ItemType Directory -Force -Path $CachePath | Out-Null

    $template = '{file}' + "`t" + '{line}' + "`t" + '{column}' + "`t" + '{severity}' + "`t" + '{id}' + "`t" + '{message}'
    $arguments = @(
        "--project=$ProjectPath"
        "--project-configuration=Release|x64"
        "--enable=warning,performance,portability"
        "--error-exitcode=1"
        "--cppcheck-build-dir=$CachePath"
        "--file-filter=$TranslationUnit"
        "--template=$template"
        "--quiet"
        "-j"
        "$CppcheckJobs"
    )

    $result = Invoke-LintChild -Path $CppcheckPath -Arguments $arguments -Label "cppcheck-$TargetName-$TranslationUnit" -Evidence $Evidence
    $output = $result.Output
    $exitCode = $result.ExitCode
    return ConvertFrom-CppcheckProjectOutput `
        -Output $output `
        -ExitCode $exitCode `
        -RepositoryRoot $RepositoryRoot `
        -BuildRoot $BuildRoot `
        -TargetName $TargetName `
        -TranslationUnit $TranslationUnit `
        -AllowedMissingFiles $generatedBundlePaths
}

$inputManifestData = $null

if (-not [string]::IsNullOrWhiteSpace($InputManifest)) {
    try {
        $inputManifestData = Get-InputManifest -ManifestPath $InputManifest
    }
    catch {
        Write-Error $_.Exception.Message
        exit 1
    }
}

$cppcheck = Get-Command cppcheck -ErrorAction SilentlyContinue
$cmake = Get-Command cmake -ErrorAction SilentlyContinue

if (-not $cppcheck) {
    Write-Error "Cppcheck was not found in PATH. Install Cppcheck and restart your terminal/IDE."
    exit 1
}

if (-not $cmake) {
    Write-Error "CMake was not found in PATH. Install CMake and restart your terminal/IDE."
    exit 1
}

if ($PreflightOnly -and ($null -eq $inputManifestData)) {
    Write-Error 'PreflightOnly requires InputManifest.'
    exit 1
}

$repositoryRoot = Get-GitSingleLine -Arguments @('rev-parse', '--show-toplevel') -ErrorMessage 'This script must run inside a Git worktree.'
$evidence = $null
$cppcheckSHA256 = (Get-FileHash -LiteralPath $cppcheck.Source -Algorithm SHA256).Hash

if ($null -ne $inputManifestData) {
    $effectiveEvidenceDir = if ([string]::IsNullOrWhiteSpace($EvidenceDir)) { Join-Path $repositoryRoot 'completes/openal-full-gate-preparation/evidence' } else { $EvidenceDir }
    $evidence = Initialize-LintEvidence -RootPath $effectiveEvidenceDir -ManifestPath $inputManifestData.Path -CppcheckPath $cppcheck.Source -CppcheckSHA256 $cppcheckSHA256 -CppcheckVersion ''
}

$cppcheckVersionResult = Invoke-LintChild -Path $cppcheck.Source -Arguments @('--version') -Label 'cppcheck-version' -Evidence $evidence
$cppcheckVersion = $cppcheckVersionResult.Output

if (($cppcheckVersionResult.ExitCode -ne 0) -or ($cppcheckVersion.Count -ne 1) -or [string]::IsNullOrWhiteSpace($cppcheckVersion[0])) {
    Write-Error "Could not determine the Cppcheck version for vendor disposition validation."
    exit 1
}

if ($null -ne $evidence) {
    $policyPath = Join-Path $PSScriptRoot 'cppcheck-vendor-dispositions.json'
    [PSCustomObject]@{
        ScriptPath = $PSCommandPath
        ScriptSHA256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        PolicyPath = $policyPath
        PolicySHA256 = (Get-FileHash -LiteralPath $policyPath -Algorithm SHA256).Hash
        AnalyzerPath = $cppcheck.Source
        AnalyzerSHA256 = $cppcheckSHA256
        AnalyzerVersion = $cppcheckVersion[0]
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence.Root 'tool-provenance.json') -NoNewline
}

if ($null -ne $inputManifestData) {
    $preflightCachePath = Join-Path ([System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDir))) 'CMakeCache.txt'

    try {
        $inputManifestRoots = Invoke-InputManifestPreflight -Manifest $inputManifestData -RepositoryRoot $repositoryRoot -CachePath $preflightCachePath -CMakePath $cmake.Source -KeepTemporaryRoot -Evidence $evidence
    }
    catch {
        Write-Error $_.Exception.Message
        exit 1
    }

    if ($PreflightOnly) {
        Remove-Item -LiteralPath $inputManifestRoots.TempRoot -Force -Recurse -ErrorAction SilentlyContinue
        exit 0
    }
}

Write-Host "Cppcheck parallel jobs: $CppcheckJobs"

$workspaceHead = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', 'HEAD^{commit}') -ErrorMessage 'Could not resolve the current worktree HEAD.'
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDir))
$cacheFile = Join-Path $buildRoot 'CMakeCache.txt'

if ($null -ne $inputManifestData) {
    $headCommit = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', "$($inputManifestData.SourceCommit)^{commit}") -ErrorMessage "InputManifest SourceCommit '$($inputManifestData.SourceCommit)' does not resolve to a commit."
    $baseCommit = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', "$($inputManifestData.BaseCommit)^{commit}") -ErrorMessage "InputManifest BaseCommit '$($inputManifestData.BaseCommit)' does not resolve to a commit."

    if ($headCommit -ne $workspaceHead) {
        Write-Error "InputManifest SourceCommit '$($inputManifestData.SourceCommit)' must resolve to the current worktree HEAD."
        exit 1
    }

    $analysisRepositoryRoot = $inputManifestRoots.SourceRoot
    $analysisBuildRoot = $inputManifestRoots.SourceBuildRoot
}
else {
    $headCommit = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', "$HeadSha^{commit}") -ErrorMessage "HeadSha '$HeadSha' does not resolve to a commit."

    if ($headCommit -ne $workspaceHead) {
        Write-Error "HeadSha '$HeadSha' must resolve to the current worktree HEAD for this generated build directory."
        exit 1
    }

    # When run manually, compare HEAD against its upstream branch.
    if ([string]::IsNullOrWhiteSpace($BaseSha)) {
        $upstream = (& git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>$null)

        if (($LASTEXITCODE -ne 0) -or [string]::IsNullOrWhiteSpace($upstream)) {
            Write-Error "No BaseSha was supplied and the current branch has no upstream."
            exit 1
        }

        $BaseSha = Get-GitSingleLine -Arguments @('merge-base', $headCommit, $upstream.Trim()) -ErrorMessage "Could not determine the merge base with upstream '$($upstream.Trim())'."
    }

    $baseCommit = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', "$BaseSha^{commit}") -ErrorMessage "BaseSha '$BaseSha' does not resolve to a commit."
    $analysisRepositoryRoot = $repositoryRoot
    $analysisBuildRoot = $buildRoot
}

if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) {
    Write-Error "No CMake cache found in '$BuildDir'. Configure the project first."
    exit 1
}

$changedFiles = @()

if ($null -ne $inputManifestData) {
    $changedFiles = @($inputManifestData.Files | Where-Object { $_.Path -match '\.(c|cc|cpp|cxx)$' } | ForEach-Object { $_.Path })
}
else {
    $changedFiles = @(
        & git diff --name-only --diff-filter=ACMR $baseCommit $headCommit -- '*.c' '*.cc' '*.cpp' '*.cxx'

        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to determine changed C/C++ source files."
            exit $LASTEXITCODE
        }
    )
}

$changedFiles = @($changedFiles | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)

if ($changedFiles.Count -eq 0) {
    if ($null -ne $inputManifestData) {
        Write-Error 'InputManifest contains no C/C++ translation units for the full Cppcheck gate.'
        exit 1
    }

    Write-Host "No changed C/C++ translation units require Cppcheck."
    exit 0
}

$headProjects = Get-ProjectSources -BuildRoot $analysisBuildRoot -RepositoryRoot $analysisRepositoryRoot

foreach ($file in $changedFiles) {
    $projectMatches = @($headProjects | Where-Object { $_.Sources.ContainsKey($file) })

    if ($projectMatches.Count -eq 0) {
        Write-Error "Changed source '$file' is not present in any generated Visual Studio project. Reconfigure '$BuildDir' before linting."
        exit 1
    }

    foreach ($project in $projectMatches) {
        $project.Files = @($project.Sources.Keys | Sort-Object)
    }
}

$analysisProjects = @($headProjects | Where-Object { $_.Files.Count -ne 0 })
$tempRoot = if ($null -ne $inputManifestData) { $inputManifestRoots.TempRoot } else { Join-Path ([System.IO.Path]::GetTempPath()) ("zandronum-cppcheck-" + [guid]::NewGuid().ToString('N')) }
$baselineRoot = if ($null -ne $inputManifestData) { $inputManifestRoots.BaselineRoot } else { Join-Path $tempRoot 'baseline' }
$baselineBuildRoot = if ($null -ne $inputManifestData) { $inputManifestRoots.BaselineBuildRoot } else { Join-Path $baselineRoot 'build' }
$baselineWorktreeCreated = $false
$baselineProjects = @()
$vendorDispositionContexts = @{}
$verifiedGeneratedBundle = @()
$finalResultSaved = $false

try {
    if ($null -eq $inputManifestData) {
        New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
        & git worktree add --detach $baselineRoot $baseCommit | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Could not create an isolated baseline worktree for '$baseCommit'."
        }

        $baselineWorktreeCreated = $true
        $generator = Get-CMakeCacheSetting -CachePath $cacheFile -Name 'CMAKE_GENERATOR'
        $platform = Get-CMakeCacheSetting -CachePath $cacheFile -Name 'CMAKE_GENERATOR_PLATFORM'
        $toolset = Get-CMakeCacheSetting -CachePath $cacheFile -Name 'CMAKE_GENERATOR_TOOLSET'
        $python = Get-CMakePythonExecutable -CachePath $cacheFile

    }

    if ($null -ne $inputManifestData) {
        $verifiedGeneratedBundle = $inputManifestRoots.VerifiedGeneratedBundle
    }
    else {
        Invoke-GeneratedBuildInputPreparation -CMakePath $cmake.Source -BuildRoot $analysisBuildRoot -RevisionName 'source' -Evidence $evidence
    }

    Write-Host 'Protocolspec provenance equality: confirmed.'

    foreach ($generatedFile in $verifiedGeneratedBundle) {
        Write-Host "Generated baseline input: $($generatedFile.RelativePath) SHA-256 $($generatedFile.Hash)"
    }

    if ($null -eq $inputManifestData) {
        if (-not $generator) {
            throw "The current CMake cache does not record CMAKE_GENERATOR."
        }

        $configureArguments = @('-S', $baselineRoot, '-B', $baselineBuildRoot, '-G', $generator.Value)

        if ($platform -and -not [string]::IsNullOrWhiteSpace($platform.Value)) {
            $configureArguments += @('-A', $platform.Value)
        }

        if ($toolset -and -not [string]::IsNullOrWhiteSpace($toolset.Value)) {
            $configureArguments += @('-T', $toolset.Value)
        }

        foreach ($name in @('BUILD_TESTING', 'DYN_FLUIDSYNTH', 'NO_SOUND', 'FMOD_INCLUDE_DIR', 'FMOD_LIBRARY', 'OPENAL_INCLUDE_DIR', 'OPENAL_LIBRARY', 'OPUS_INCLUDE_DIR', 'OPUS_LIBRARIES', 'ZSTD_INCLUDE_DIR', 'ZSTD_LIBRARY', 'FLUIDSYNTH_INCLUDE_DIR', 'FLUIDSYNTH_LIBRARIES')) {
            $setting = Get-CMakeCacheSetting -CachePath $cacheFile -Name $name

            if ($setting -and ($setting.Value -notmatch 'NOTFOUND')) {
                $configureArguments += "-D$($name):$($setting.Type)=$($setting.Value)"
            }
        }

        $configureResult = Invoke-LintChild -Path $cmake.Source -Arguments $configureArguments -Label 'configure-baseline' -Evidence $evidence
        $configureOutput = $configureResult.Output

        if ($configureResult.ExitCode -ne 0) {
            $details = ($configureOutput -join [Environment]::NewLine).Replace($baselineRoot, '<baseline>').Replace($repositoryRoot, '<repo>')
            throw "Failed to configure the isolated baseline worktree: $details"
        }
    }

    if ($null -eq $inputManifestData) {
        Invoke-GeneratedBuildInputPreparation -CMakePath $cmake.Source -BuildRoot $baselineBuildRoot -RevisionName 'baseline' -Evidence $evidence
        Invoke-ProtocolspecGeneration -CMakePath $cmake.Source -BuildRoot $baselineBuildRoot -RevisionName 'baseline' -Evidence $evidence
        $verifiedGeneratedBundle = Get-VerifiedGeneratedBundle `
            -SourceRoot $analysisRepositoryRoot `
            -GeneratedOutputRoot $analysisRepositoryRoot `
            -BaseCommit $baseCommit `
            -HeadCommit $headCommit `
            -TempRoot $tempRoot `
            -RelativePaths $generatedBundlePaths `
            -PythonPath $python `
            -Evidence $evidence
        Copy-VerifiedGeneratedBundle -VerifiedBundle $verifiedGeneratedBundle -DestinationRoot $baselineRoot
    }

    $baselineProjects = Get-ProjectSources -BuildRoot $baselineBuildRoot -RepositoryRoot $baselineRoot
    $productionProjects = Get-ProjectSources -BuildRoot $buildRoot -RepositoryRoot $repositoryRoot
    $baselineDiagnostics = [System.Collections.Generic.List[object]]::new()
    $headDiagnostics = [System.Collections.Generic.List[object]]::new()
    $baselineProjectsByTarget = @{}

    foreach ($project in $analysisProjects) {
        $targetName = $project.RelativeProject
        $productionProject = $productionProjects | Where-Object { $_.RelativeProject -eq $targetName } | Select-Object -First 1
        $baselineProject = $baselineProjects | Where-Object { $_.RelativeProject -eq $targetName } | Select-Object -First 1

        $analysisContext = Get-CppcheckVendorDispositionContext `
            -ProjectPath $project.ProjectPath `
            -TargetName $targetName `
            -AnalyzerVersion $cppcheckVersion[0]
        $vendorDispositionContexts[$targetName] = ConvertTo-ProductionCppcheckVendorDispositionContext `
            -Context $analysisContext `
            -AnalysisRepositoryRoot $analysisRepositoryRoot `
            -AnalysisBuildRoot $analysisBuildRoot `
            -ProductionRepositoryRoot $repositoryRoot `
            -ProductionBuildRoot $buildRoot
        $productionContext = if ($productionProject) {
            Get-CppcheckVendorDispositionContext `
                -ProjectPath $productionProject.ProjectPath `
                -TargetName $targetName `
                -AnalyzerVersion $cppcheckVersion[0]
        }
        $baselineContext = if ($baselineProject) {
            Get-CppcheckVendorDispositionContext `
                -ProjectPath $baselineProject.ProjectPath `
                -TargetName $targetName `
                -AnalyzerVersion $cppcheckVersion[0]
        }

        $baselineProject = Resolve-CppcheckBaselineTarget `
            -TargetName $targetName `
            -ProductionProject $productionProject `
            -BaselineProject $baselineProject `
            -ProductionContext $(if ($productionProject) { ConvertTo-NormalizedCppcheckProjectContext -Context $productionContext -ProjectPath $productionProject.ProjectPath -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot }) `
            -AnalysisContext (ConvertTo-NormalizedCppcheckProjectContext -Context $analysisContext -ProjectPath $project.ProjectPath -RepositoryRoot $analysisRepositoryRoot -BuildRoot $analysisBuildRoot) `
            -BaselineContext $(if ($baselineProject) { ConvertTo-NormalizedCppcheckProjectContext -Context $baselineContext -ProjectPath $baselineProject.ProjectPath -RepositoryRoot $baselineRoot -BuildRoot $baselineBuildRoot }) `
            -RequireManifestParity:($null -ne $inputManifestData)

        if ($baselineProject) {
            $baselineProjectsByTarget[$targetName] = $baselineProject
        }
    }

    Write-Host "Cppcheck context and full translation-unit equality: confirmed for $($analysisProjects.Count) target(s)."
    Save-LintEvidenceState -Evidence $evidence -AnalysisBuildRoot $analysisBuildRoot -BaselineBuildRoot $baselineBuildRoot -TempRoot $tempRoot -AnalysisProjects $analysisProjects -BaselineProjects $baselineProjects -Contexts $vendorDispositionContexts -VerifiedGeneratedBundle $verifiedGeneratedBundle
    Write-Host "Running isolated Cppcheck analysis for $($analysisProjects.Count) target(s):"

    foreach ($project in $analysisProjects) {
        $targetName = $project.RelativeProject
        Write-Host "  $targetName"

        foreach ($translationUnit in $project.Files) {
            $headCache = Join-Path $tempRoot (Join-Path 'head-cache' ($targetName.Replace('/', '_') + '-' + $translationUnit.Replace('/', '_')))
            [void]$headDiagnostics.AddRange(@(Invoke-CppcheckProject `
                -CppcheckPath $cppcheck.Source `
                -ProjectPath $project.ProjectPath `
                -CachePath $headCache `
                -RepositoryRoot $analysisRepositoryRoot `
                -BuildRoot $analysisBuildRoot `
                -TargetName $targetName `
                -TranslationUnit $translationUnit `
                -CppcheckJobs $CppcheckJobs `
                -Evidence $evidence))
        }

        $baselineProject = $baselineProjectsByTarget[$targetName]

        if ($baselineProject) {
            foreach ($translationUnit in @($baselineProject.Sources.Keys | Sort-Object)) {
                $baselineCache = Join-Path $tempRoot (Join-Path 'baseline-cache' ($targetName.Replace('/', '_') + '-' + $translationUnit.Replace('/', '_')))
                [void]$baselineDiagnostics.AddRange(@(Invoke-CppcheckProject `
                    -CppcheckPath $cppcheck.Source `
                    -ProjectPath $baselineProject.ProjectPath `
                    -CachePath $baselineCache `
                    -RepositoryRoot $baselineRoot `
                    -BuildRoot $baselineBuildRoot `
                    -TargetName $targetName `
                    -TranslationUnit $translationUnit `
                    -CppcheckJobs $CppcheckJobs `
                    -Evidence $evidence))
            }
        }
    }

    $baselineDiagnostics = $baselineDiagnostics.ToArray()
    $headDiagnostics = $headDiagnostics.ToArray()
    $vendorDispositionPolicy = Join-Path $PSScriptRoot 'cppcheck-vendor-dispositions.json'
    Write-Host "Raw HEAD diagnostics ($($headDiagnostics.Count)):"
    $headDiagnostics | Sort-Object Display | ForEach-Object { Write-Host "  $($_.Display)" }
    $vendorDispositionResult = Get-CppcheckVendorDispositionResult `
        -Diagnostics $headDiagnostics `
        -PolicyPath $vendorDispositionPolicy `
        -RepositoryRoot $analysisRepositoryRoot `
        -Contexts $vendorDispositionContexts
    $headDiagnosticsForComparison = @($vendorDispositionResult.Unaccepted)

    $comparison = Get-CppcheckBaselineComparisonResult `
        -BaselineDiagnostics $baselineDiagnostics `
        -HeadDiagnostics $headDiagnostics `
        -UnacceptedDiagnostics $headDiagnosticsForComparison

    Write-Host "Accepted vendor dispositions ($($vendorDispositionResult.Accepted.Count)):"
    $vendorDispositionResult.Accepted | Sort-Object { $_.Diagnostic.Display } | ForEach-Object { Write-Host "  $($_.Diagnostic.Display) [$($_.Disposition.Reason)]" }

    $unchangedDiagnostics = $comparison.Unchanged
    $newDiagnostics = $comparison.New
    $baselineOnlyDiagnostics = $comparison.BaselineOnly

    Write-Host "Baseline diagnostics ($($baselineDiagnostics.Count)):"
    $comparison.Baseline | Sort-Object Display | ForEach-Object { Write-Host "  $($_.Display)" }
    Write-Host "Unchanged baseline diagnostics ($($unchangedDiagnostics.Count)):"
    $unchangedDiagnostics | ForEach-Object { Write-Host "  $($_.Display)" }
    Write-Host "Baseline-only diagnostics ($($baselineOnlyDiagnostics.Count)):"
    $baselineOnlyDiagnostics | ForEach-Object { Write-Host "  $($_.Display)" }
    Write-Host "New diagnostics ($($newDiagnostics.Count)):"
    $newDiagnostics | ForEach-Object { Write-Host "  $($_.Display)" }

    if ($comparison.UnresolvedVendor.Count -ne 0) {
        Save-LintEvidenceResult -Evidence $evidence -Status 'failed' -ExitCode 1 -Error 'Unresolved vendor diagnostics.' -VendorDispositionResult $vendorDispositionResult -Comparison $comparison
        $finalResultSaved = $true
        Write-Error "Cppcheck reported $($comparison.UnresolvedVendor.Count) unresolved vendor diagnostic(s)."
        $comparison.UnresolvedVendor | Sort-Object Display | ForEach-Object { Write-Host "  $($_.Display)" }
        exit 1
    }

    if ($newDiagnostics.Count -ne 0) {
        Save-LintEvidenceResult -Evidence $evidence -Status 'failed' -ExitCode 1 -Error 'New diagnostic fingerprints.' -VendorDispositionResult $vendorDispositionResult -Comparison $comparison
        $finalResultSaved = $true
        Write-Error "Cppcheck reported $($newDiagnostics.Count) new diagnostic fingerprint(s)."
        exit 1
    }

    Write-Host "Cppcheck passed: no new diagnostic fingerprints."
    Save-LintEvidenceResult -Evidence $evidence -Status 'passed' -ExitCode 0 -VendorDispositionResult $vendorDispositionResult -Comparison $comparison
    $finalResultSaved = $true
}
finally {
    if (-not $finalResultSaved) {
        Save-LintEvidenceResult -Evidence $evidence -Status 'incomplete' -ExitCode 1 -Error 'The gate did not reach final classification.'
    }

    Save-LintEvidenceState -Evidence $evidence -AnalysisBuildRoot $analysisBuildRoot -BaselineBuildRoot $baselineBuildRoot -TempRoot $tempRoot -AnalysisProjects $analysisProjects -BaselineProjects $baselineProjects -Contexts $vendorDispositionContexts -VerifiedGeneratedBundle $verifiedGeneratedBundle

    if ($baselineWorktreeCreated) {
        & git worktree remove --force $baselineRoot 2>$null
    }

    if (Test-Path -LiteralPath $tempRoot) {
        try {
            Remove-Item -LiteralPath $tempRoot -Force -Recurse
        }
        catch {
            Write-Warning "Could not remove Cppcheck temporary directory '$tempRoot': $($_.Exception.Message)"
        }
    }
}

exit 0
