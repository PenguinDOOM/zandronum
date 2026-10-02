param(
    [string]$BuildDir = "build-v143",
    [string]$BaseSha = "",
    [string]$HeadSha = "HEAD",
    [string]$InputManifest = "",
    [string]$EvidenceDir = "",
    [switch]$PreflightOnly,
    [ValidateSet('Auto', 'Full')][string]$CppcheckMode = 'Auto',
    [int]$CppcheckJobs = [Math]::Min(12, [Environment]::ProcessorCount)
)

if (($PSVersionTable.PSVersion.Major -lt 7) -or
    ($null -eq [System.Diagnostics.ProcessStartInfo].GetProperty('ArgumentList')) -or
    ($null -eq [System.IO.StreamReader].GetMethod('ReadToEndAsync', [type[]]@()))) {
    throw 'lint.ps1 requires PowerShell 7 and .NET ProcessStartInfo.ArgumentList / StreamReader.ReadToEndAsync.'
}

$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'cppcheck-vendor-policy.ps1')
. (Join-Path $PSScriptRoot 'cppcheck-cache.ps1')

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
$fastSourceGeneratedPaths = @('sqlite/sqlite3.c', 'sqlite/sqlite3.h', 'sqlite/sqlite3ext.h', 'src/gitinfo.h')

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
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        $output = @(& $Path @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
        $stopwatch.Stop()
    }
    finally {
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            Pop-Location
        }
    }

    $result = [PSCustomObject]@{ Output = $output; ExitCode = $exitCode; ElapsedMilliseconds = $stopwatch.ElapsedMilliseconds }
    Save-LintChildEvidence -Path $Path -Arguments $Arguments -Label $Label -Evidence $Evidence -WorkingDirectory $WorkingDirectory -Result $result
    return $result
}

function Save-LintChildEvidence {
    param(
        [string]$Path,
        [string[]]$Arguments,
        [string]$Label,
        [object]$Evidence,
        [string]$WorkingDirectory,
        [object]$Result
    )

    if ($null -ne $Evidence) {
        $Evidence.Counter++
        $prefix = '{0:D4}-{1}' -f $Evidence.Counter, ($Label -replace '[^A-Za-z0-9._-]', '_')
        [System.IO.File]::WriteAllText((Join-Path $Evidence.Root (Join-Path 'commands' ($prefix + '.raw.txt'))), ($Result.Output -join [Environment]::NewLine), $utf8)
        $invocation = [ordered]@{
            Path = $Path
            Arguments = $Arguments
            WorkingDirectory = $WorkingDirectory
            ExitCode = $Result.ExitCode
            ElapsedMilliseconds = $Result.ElapsedMilliseconds
        }
        foreach ($name in @('ProcessId', 'StartedAtUtc', 'EndedAtUtc', 'Stdout', 'Stderr', 'StdoutComplete', 'StderrComplete', 'TransportErrors')) {
            if ($null -ne $Result.PSObject.Properties[$name]) { $invocation[$name] = $Result.$name }
        }
        $invocation | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Evidence.Root (Join-Path 'commands' ($prefix + '.invocation.json'))) -NoNewline
    }
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
        [object[]]$VerifiedGeneratedBundle,
        [object]$CacheIdentity = $null,
        [hashtable]$CachePaths = @{},
        [string]$HeadCommit = '',
        [string]$BaseCommit = ''
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
        Revisions = [PSCustomObject]@{ Head = $HeadCommit; Baseline = $BaseCommit }
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Evidence.Root 'analysis-context.json') -NoNewline

    $cacheInputRoot = Join-Path $Evidence.Root 'cache-inputs'
    New-Item -ItemType Directory -Force -Path $cacheInputRoot | Out-Null
    if ($null -ne $CacheIdentity) {
        $CacheIdentity | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $cacheInputRoot 'identity.json') -NoNewline
        $identities = if ($CacheIdentity -is [hashtable]) { @($CacheIdentity.Values) } else { @($CacheIdentity) }
        foreach ($configuration in @($identities | ForEach-Object { $_.Context.AnalysisContext.AnalyzerConfigurations })) {
            $configurationPath = [string]$configuration.Path
            if (-not [string]::IsNullOrWhiteSpace($configurationPath) -and (Test-Path -LiteralPath $configurationPath -PathType Leaf)) {
                $snapshotName = ('{0}-{1}' -f $configuration.SHA256, (Split-Path -Leaf $configurationPath))
                Copy-Item -LiteralPath $configurationPath -Destination (Join-Path $cacheInputRoot $snapshotName) -Force
            }
        }
    }

    [PSCustomObject]@{
        Identity = $CacheIdentity
        CacheLeaves = @($CachePaths.GetEnumerator() | Sort-Object Key | ForEach-Object { [PSCustomObject]@{ Role = $_.Key; Path = $_.Value } })
        NativeAnalyzerInfo = 'Stored in commands/*.raw.txt for each Cppcheck invocation.'
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $Evidence.Root 'cache-context.json') -NoNewline
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

function Complete-LintTemporaryCleanup {
    param(
        [switch]$BaselineWorktreeCreated,
        [string]$BaselineRoot,
        [string]$TempRoot,
        [string]$CppcheckCacheRoot,
        [string]$StageName
    )

    if ($BaselineWorktreeCreated) {
        & git worktree remove --force $BaselineRoot 2>$null
        if ($LASTEXITCODE -ne 0) {
            throw "Could not remove the isolated baseline worktree '$BaselineRoot'."
        }
    }

    if (Test-Path -LiteralPath $TempRoot) {
        Remove-CppcheckDisposableStage -CacheRoot $CppcheckCacheRoot -StageRoot $TempRoot -Name $StageName
    }
}

function Complete-LintFinalization {
    param(
        [scriptblock]$SaveEvidenceAction,
        [scriptblock]$CleanupAction,
        [scriptblock]$SaveResultAction
    )

    try {
        & $SaveEvidenceAction
        & $CleanupAction
        & $SaveResultAction 'passed' 0 ''
        return [PSCustomObject]@{ Succeeded = $true; ResultSaved = $true; ErrorMessage = '' }
    }
    catch {
        $failureMessage = $_.Exception.Message
        $resultSaved = $false
        try {
            & $SaveResultAction 'failed' 1 $failureMessage
            $resultSaved = $true
        }
        catch {
            Write-Warning "Could not save failed lint evidence: $($_.Exception.Message)" -WarningAction Continue
        }
        return [PSCustomObject]@{ Succeeded = $false; ResultSaved = $resultSaved; ErrorMessage = $failureMessage }
    }
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
        [string]$CppcheckCacheRoot,
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
    $tempRoot = New-CppcheckDisposableStage -CacheRoot $CppcheckCacheRoot -Name 'regression-manifest'
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
            Remove-CppcheckDisposableStage -CacheRoot $CppcheckCacheRoot -StageRoot $tempRoot -Name 'regression-manifest'
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

        $actualBlob = @(& git -c core.autocrlf=true hash-object --path $relativePath -- $sourcePath 2>$null)

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

function Get-CppcheckGitBytes {
    param([string]$RepositoryRoot, [string[]]$Arguments, [byte[]]$InputBytes = $null)

    $process = [Diagnostics.Process]::new()
    $stream = [IO.MemoryStream]::new()
    try {
        $process.StartInfo.FileName = 'git'
        $process.StartInfo.WorkingDirectory = $RepositoryRoot
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.RedirectStandardInput = $null -ne $InputBytes
        foreach ($argument in $Arguments) { $process.StartInfo.ArgumentList.Add($argument) }
        if (-not $process.Start()) { throw 'Could not start Git input reader.' }
        $errorTask = $process.StandardError.ReadToEndAsync()
        $outputTask = $process.StandardOutput.BaseStream.CopyToAsync($stream)
        if ($null -ne $InputBytes) {
            $process.StandardInput.BaseStream.Write($InputBytes, 0, $InputBytes.Length)
            $process.StandardInput.Close()
        }
        $null = $outputTask.GetAwaiter().GetResult()
        $process.WaitForExit()
        $details = $errorTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Git input reader failed ($($process.ExitCode)): $details" }
        return ,$stream.ToArray()
    }
    finally { $stream.Dispose(); $process.Dispose() }
}

function Get-CppcheckTreeChanges {
    param([string]$RepositoryRoot, [string]$BaseCommit, [string]$HeadCommit)

    $bytes = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('diff', '--name-status', '-z', '--no-ext-diff', '--no-textconv', '-M', '-C', $BaseCommit, $HeadCommit, '--')
    $fields = [Text.UTF8Encoding]::new($false, $true).GetString($bytes).Split([char]0)
    for ($index = 0; $index -lt $fields.Length - 1; $index++) {
        $status = $fields[$index]
        if ($status -notmatch '^[ACDMRTUXB][0-9]*$') { throw 'Malformed NUL Git diff status.' }
        $index++
        if ($index -ge $fields.Length - 1) { throw 'Truncated NUL Git diff path.' }
        $path = $fields[$index]
        $oldPath = ''
        if ($status -match '^[RC]') {
            $oldPath = $path
            $index++
            if ($index -ge $fields.Length - 1) { throw 'Truncated NUL Git rename/copy path.' }
            $path = $fields[$index]
        }
        [PSCustomObject]@{ Status = $status; Path = $path; OldPath = $oldPath }
    }
}

function Get-CppcheckSourceRoute {
    param([object[]]$Changes, [bool]$TrackedDirty = $false)

    $sources = @($Changes | Where-Object { $_.Status -match '^[ACMRT]' -and $_.Path -match '\.(c|cc|cpp|cxx)$' } | ForEach-Object { $_.Path } | Sort-Object -Unique)
    $unsupported = @($Changes | Where-Object {
        ($_.Status -eq 'D' -and $_.Path -match '\.(c|cc|cpp|cxx)$') -or
        ($_.Status -match '^[RC]' -and $_.OldPath -match '\.(c|cc|cpp|cxx)$') -or
        $_.Path -match '(^|/)(CMakeLists\.txt|[^/]+\.cmake)$|^(tools|protocolspec)/|(^|/)[^/]+\.(in|re|y)$' -or
        ($_.Status -match '^[RC]' -and $_.OldPath -match '(^|/)(CMakeLists\.txt|[^/]+\.cmake)$|^(tools|protocolspec)/|(^|/)[^/]+\.(in|re|y)$')
    })
    $mode = 'Full'
    $reason = 'receipt validation required'
    if ($sources.Count -eq 0) {
        $mode = if ($unsupported.Count) { 'Error' } else { 'Skip' }
        $reason = if ($unsupported.Count) { 'Build/generator/protocol/deleted-source changes have no selectable TU; this gate does not support an empty analysis.' } else { 'No changed C/C++ TU; headers and documents are not validated by this source gate.' }
    }
    elseif ($TrackedDirty) { $reason = 'tracked index/worktree inputs are dirty; Full retains live-worktree semantics' }
    elseif (@($Changes | Where-Object { $_.Status -ne 'M' -or $_.Path -notmatch '\.(c|cc|cpp|cxx)$' }).Count) { $reason = 'all-diff input is not exclusively existing modified C/C++ TUs' }
    elseif (@($sources | Where-Object { $_ -notmatch '^src/' -or $_ -match '(^|/)(thirdparty|third_party)/|^src/(huffman|oplsynth|timidity)/|^src/network/servercommands\.cpp$' }).Count) { $reason = 'generator/vendor/generated source requires Full' }
    else { $mode = 'Candidate'; $reason = 'existing modified source-only inputs' }
    [PSCustomObject]@{ Mode = $mode; Reason = $reason; Sources = $sources }
}

function Select-CppcheckProjects {
    param(
        [object[]]$Projects,
        [string[]]$ChangedFiles,
        [bool]$IsInputManifest,
        [bool]$IsWindowsVisualStudioBuild,
        [string]$BuildDirectory,
        [scriptblock]$PrepareAnalysis = $null,
        [switch]$SelectedOnly
    )

    $selectedProjects = @()
    $platformNotApplicable = @()
    $platformReasons = @{
        'src/sdl/i_main.cpp' = 'src/CMakeLists.txt selects this SDL entry point only outside Windows; it is not analyzed by the Windows Visual Studio projects.'
        'tools/timidity_pipe_tests.cpp' = 'tools/CMakeLists.txt includes this test only on UNIX; it is not analyzed by the Windows Visual Studio projects.'
    }

    foreach ($file in $ChangedFiles) {
        $projectMatches = @($Projects | Where-Object { $_.Sources.ContainsKey($file) })

        if ($projectMatches.Count -ne 0) {
            foreach ($project in $projectMatches) {
                $project.Files = if ($SelectedOnly) {
                    @($ChangedFiles | Where-Object { $project.Sources.ContainsKey($_) } | Sort-Object -Unique)
                } else {
                    @($project.Sources.Keys | Sort-Object)
                }
                $selectedProjects += $project
            }

            continue
        }

        if ((-not $IsInputManifest) -and $IsWindowsVisualStudioBuild -and $platformReasons.ContainsKey($file)) {
            $platformNotApplicable += [PSCustomObject]@{
                Path = $file
                Reason = $platformReasons[$file]
            }
            continue
        }

        if ($IsInputManifest) {
            throw "InputManifest C/C++ source '$file' is not present in an archive-generated Visual Studio project."
        }

        throw "Changed source '$file' is not present in any generated Visual Studio project. Reconfigure '$BuildDirectory' before linting."
    }

    $analysisPreparation = $null

    if (($selectedProjects.Count -ne 0) -and ($null -ne $PrepareAnalysis)) {
        $analysisPreparation = & $PrepareAnalysis
    }

    return [PSCustomObject]@{
        Projects = @($selectedProjects | Sort-Object RelativeProject -Unique)
        PlatformNotApplicable = @($platformNotApplicable)
        AnalysisPreparation = $analysisPreparation
    }
}

function Assert-CppcheckFastSourceSafety {
    param([string]$RepositoryRoot, [string]$BuildRoot, [string]$HeadCommit, [string[]]$ChangedFiles, [object[]]$Projects, [string]$BaseCommit = '')

    $treeBytes = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('ls-tree', '-rz', '--full-tree', $HeadCommit)
    $entries = [Text.UTF8Encoding]::new($false, $true).GetString($treeBytes).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
        if ($entry -notmatch '^100(644|755) blob [0-9a-f]+\t(?<path>.+)$') { throw [NotSupportedException]::new('Fast tree contains a symlink/submodule or unsupported mode.') }
        if (-not $paths.Add($Matches.path)) { throw [NotSupportedException]::new('Fast tree contains case-colliding paths.') }
    }
    $trackedBytes = [Text.Encoding]::UTF8.GetBytes((@($paths | Sort-Object) -join [char]0) + [char]0)
    $attributeBytes = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('check-attr', '-z', '--all', '--stdin') -InputBytes $trackedBytes
    $attributes = [Text.Encoding]::UTF8.GetString($attributeBytes).Split([char]0)
    for ($index = 0; $index -lt $attributes.Length - 1; $index += 3) {
        if ($attributes[$index + 1] -in @('filter', 'working-tree-encoding', 'export-ignore', 'export-subst', 'ident') -and $attributes[$index + 2] -notin @('unset', 'unspecified')) {
            throw [NotSupportedException]::new("Fast archive/input transformation attribute on '$($attributes[$index])'.")
        }
    }
    $indexBytes = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('ls-files', '-v', '-z')
    if (@([Text.Encoding]::UTF8.GetString($indexBytes).Split([char]0) | Where-Object { $_ -cmatch '^[a-zS] ' }).Count) { throw [NotSupportedException]::new('Fast cannot trust assume-unchanged/sparse tracked inputs.') }
    $configureInputs = @(Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeFiles/generate.stamp.depend') | Where-Object { $_ -notmatch '^#' -and -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { [IO.Path]::GetFullPath($_) })
    foreach ($file in $ChangedFiles) {
        if ((Join-Path $RepositoryRoot $file) -in $configureInputs) { throw [NotSupportedException]::new("Changed TU is a configure input: '$file'.") }
    }
    $topDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $includeDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $definedMacros = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($project in $Projects) {
        $context = Get-CppcheckVendorDispositionContext -ProjectPath $project.ProjectPath -TargetName $project.RelativeProject -AnalyzerVersion ''
        foreach ($definition in $context.Defines.Split(';')) { [void]$definedMacros.Add(($definition -split '=')[0]) }
        foreach ($source in $project.Sources.Keys) {
            if ($source -match '^[^/]+/' -and -not $source.StartsWith(([IO.Path]::GetFileName($BuildRoot) + '/'))) { [void]$topDirectories.Add(($source -split '/')[0]) }
        }
        [xml]$document = [IO.File]::ReadAllText($project.ProjectPath)
        foreach ($node in $document.SelectNodes("//*[local-name()='AdditionalIncludeDirectories']")) {
            foreach ($include in $node.InnerText.Split(';')) {
                if ($include -eq '%(AdditionalIncludeDirectories)' -or [string]::IsNullOrWhiteSpace($include)) { continue }
                if ($include -match '\$\(|%\(') { throw [NotSupportedException]::new("Unknown Fast include macro '$include'.") }
                $fullPath = [IO.Path]::GetFullPath($include, (Split-Path $project.ProjectPath))
                [void]$includeDirectories.Add($fullPath)
                if ($null -ne (Get-PathRelativeToRoot -Path $fullPath -RootPath $BuildRoot)) { continue }
                $relative = Get-PathRelativeToRoot -Path $fullPath -RootPath $RepositoryRoot
                if ($null -ne $relative) {
                    if ([string]::IsNullOrWhiteSpace($relative)) { throw [NotSupportedException]::new('Fast cannot classify source-root-wide untracked includes.') }
                    [void]$topDirectories.Add(($relative -split '/')[0])
                }
            }
        }
    }
    foreach ($ignored in @($false, $true)) {
        $arguments = @('ls-files', '-z', '--others', '--exclude-standard')
        if ($ignored) { $arguments += '--ignored' }
        $bytes = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments ($arguments + @('--') + @($topDirectories | Sort-Object))
        foreach ($path in [Text.Encoding]::UTF8.GetString($bytes).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
            if ($path -match '^sqlite/sqlite-(?:[0-9a-f]{40}|autoconf-[0-9]+)\.tar\.gz$') { continue }
            if ($path -notin ($generatedBundlePaths + $fastSourceGeneratedPaths)) { throw [NotSupportedException]::new("Unclassified untracked Fast analysis input '$path'.") }
        }
    }
    $changedNames = @($ChangedFiles | ForEach-Object { [IO.Path]::GetFileName($_) })
    $inputs = @{}
    $pending = [Collections.Generic.Queue[object]]::new()
    foreach ($path in @($paths | Where-Object { ($_ -split '/')[0] -in $topDirectories -and $_ -match '\.(h|hh|hpp|hxx|inc|c|cc|cpp|cxx|in|re|y)$' })) { $pending.Enqueue(@{ Path = $path; Commit = '' }) }
    foreach ($path in $generatedBundlePaths + $fastSourceGeneratedPaths) {
        if (Test-Path -LiteralPath (Join-Path $RepositoryRoot $path) -PathType Leaf) { $pending.Enqueue(@{ Path = $path; Commit = '' }) }
    }
    if (-not [string]::IsNullOrWhiteSpace($BaseCommit)) {
        foreach ($file in @($Projects | ForEach-Object { $_.Files } | Sort-Object -Unique)) { $pending.Enqueue(@{ Path = $file; Commit = $BaseCommit }) }
    }
    while ($pending.Count) {
        $pendingInput = $pending.Dequeue()
        $path = $pendingInput.Path
        $inputKey = "$($pendingInput.Commit):$path"
        if ($inputs.ContainsKey($inputKey)) { continue }
        $inputPath = if ([IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $RepositoryRoot $path }
        Assert-CppcheckPathHasNoReparseAncestor -Path $inputPath
        $text = if ($pendingInput.Commit) {
            try { [Text.UTF8Encoding]::new($false, $true).GetString((Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('cat-file', 'blob', "$($pendingInput.Commit):$path"))).TrimStart([char]0xFEFF) }
            catch [Text.DecoderFallbackException] { throw [NotSupportedException]::new("Unsupported Fast Base source encoding in '$path'.") }
        } else { [IO.File]::ReadAllText($inputPath) }
        $text = $text -replace '\\\r?\n', ''
        $text = [regex]::Replace($text, '(?ms)(?<literal>"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''|^[ \t]*#[ \t]*include[ \t]*<[^>\r\n]+>)|/\*.*?\*/|//[^\r\n]*', {
            param($match)
            if ($match.Groups['literal'].Success) { return $match.Value }
            return ' ' + ([regex]::Replace($match.Value, '[^\r\n]', ''))
        })
        $inputs[$inputKey] = $text
        foreach ($definition in [regex]::Matches($text, '(?m)^\s*#\s*define\s+(?<name>[A-Za-z_][A-Za-z_0-9]*)')) { [void]$definedMacros.Add($definition.Groups['name'].Value) }
        foreach ($match in [regex]::Matches($text, '(?m)^\s*#\s*include\s*["<](?<path>[^">]+)[">]')) {
            $included = $match.Groups['path'].Value.Replace('\', '/')
            if ($included -match '\.(c|cc|cpp|cxx)$' -and [IO.Path]::GetFileName($included) -in $changedNames) { throw [NotSupportedException]::new("Changed source is textually included by '$path'.") }
            if ([IO.Path]::IsPathRooted($included)) {
                $absolute = [IO.Path]::GetFullPath($included)
                if ($null -ne (Get-PathRelativeToRoot -Path $absolute -RootPath $RepositoryRoot) -and $null -eq (Get-PathRelativeToRoot -Path $absolute -RootPath $BuildRoot)) { throw [NotSupportedException]::new("Literal include reads live source root: '$included' in '$path'.") }
            }
            if ($included -eq 'xlat_parser.c' -and $path -eq 'src/xlat/parse_xlat.cpp') {
                $generated = Join-Path $BuildRoot 'src/xlat_parser.c'
                if (-not (Test-Path -LiteralPath $generated -PathType Leaf)) { throw [NotSupportedException]::new("Missing approved generated source inclusion '$generated'; Full required.") }
                Assert-NormalGeneratedBuildInput -Path $generated -Description 'known generated source inclusion'
                $pending.Enqueue(@{ Path = $generated; Commit = '' })
                continue
            }
            $resolved = $false
            foreach ($directory in @((Split-Path $inputPath)) + @($includeDirectories)) {
                $candidate = [IO.Path]::GetFullPath($included, $directory)
                $relative = Get-PathRelativeToRoot -Path $candidate -RootPath $RepositoryRoot
                if ($null -ne (Get-PathRelativeToRoot -Path $candidate -RootPath $BuildRoot)) {
                    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $resolved = $true; $pending.Enqueue(@{ Path = $candidate; Commit = '' }); break }
                    continue
                }
                if ($null -eq $relative) { continue }
                if (-not $resolved -and $null -ne (Get-PathRelativeToRoot -Path $directory -RootPath $BuildRoot) -and ($paths.Contains($relative) -or (Test-Path -LiteralPath $candidate -PathType Leaf))) {
                    throw [NotSupportedException]::new("Shared build include reads live source root: '$included' in '$path'.")
                }
                if ($paths.Contains($relative)) { $resolved = $true; $pending.Enqueue(@{ Path = $relative; Commit = '' }); break }
                elseif (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    if ($relative -notin ($generatedBundlePaths + $fastSourceGeneratedPaths)) { throw [NotSupportedException]::new("Unclassified include input '$relative' in '$path'.") }
                    $resolved = $true
                    break
                }
            }
            if (-not $resolved -and $included -match '\.(c|cc|cpp|cxx)$') {
                throw [NotSupportedException]::new("Unresolved source inclusion '$included' in '$path'.")
            }
        }
    }
    foreach ($path in $inputs.Keys) {
        $text = $inputs[$path]
        if ($text -match '(?m)^\s*#\s*(include_next|import)\b') { throw [NotSupportedException]::new("Unsupported include directive in '$path'.") }
        foreach ($match in [regex]::Matches($text, '(?m)^\s*#\s*include\s+(?<operand>[^"<\s][^\r\n]*)')) {
            $operand = $match.Groups['operand'].Value.Trim()
            $prefix = $text.Substring(0, $match.Index).TrimEnd()
            $guard = '(?:^|\n)[ \t]*#[ \t]*(?:ifdef[ \t]+' + [regex]::Escape($operand) + '|(?:if|elif)[ \t]+defined[ \t]*\(?[ \t]*' + [regex]::Escape($operand) + '[ \t]*\)?)[ \t]*$'
            if ($operand -match '^[A-Za-z_][A-Za-z_0-9]*$' -and -not $definedMacros.Contains($operand) -and $prefix -match $guard) { continue }
            throw [NotSupportedException]::new("Unresolved macro source/include operand '$operand' in '$path'.")
        }
    }
}

function Get-CppcheckPreparationState {
    param([string]$RepositoryRoot, [string]$BuildRoot, [object[]]$Projects, [string]$AnalyzerVersion)

    $RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
    $BuildRoot = [IO.Path]::GetFullPath($BuildRoot).TrimEnd('\', '/')
    $inputPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $cacheHome = Get-CMakeCacheSetting -CachePath (Join-Path $BuildRoot 'CMakeCache.txt') -Name 'CMAKE_HOME_DIRECTORY'
    if ($null -eq $cacheHome -or [IO.Path]::GetFullPath($cacheHome.Value).TrimEnd('\', '/') -ine $RepositoryRoot.TrimEnd('\', '/')) { throw [NotSupportedException]::new('HEAD build does not belong to the selected repository root.') }
    $tracked = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('ls-files', '-z')
    foreach ($path in [Text.Encoding]::UTF8.GetString($tracked).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
        if ($path -match '(^|/)CMakeLists\.txt$|\.(cmake|in|re|y)$|^(protocolspec|tools/lemon|tools/re2c|tools/updaterevision)/') { [void]$inputPaths.Add((Join-Path $RepositoryRoot $path)) }
    }
    [void]$inputPaths.Add((Join-Path $BuildRoot 'CMakeCache.txt'))
    $dependencyPath = Join-Path $BuildRoot 'CMakeFiles/generate.stamp.depend'
    Assert-NormalGeneratedBuildInput -Path $dependencyPath -Description 'CMake configure dependency inventory'
    [void]$inputPaths.Add($dependencyPath)
    foreach ($path in Get-Content -LiteralPath $dependencyPath) {
        if (-not [string]::IsNullOrWhiteSpace($path) -and $path -notmatch '^#') {
            $fullPath = [IO.Path]::GetFullPath($path)
            if (Test-Path -LiteralPath $fullPath -PathType Leaf) { [void]$inputPaths.Add($fullPath) }
            else { throw "Missing configure input '$fullPath'." }
        }
    }
    $projectStates = @(foreach ($project in @($Projects | Sort-Object RelativeProject)) {
        [void]$inputPaths.Add($project.ProjectPath)
        $context = Get-CppcheckVendorDispositionContext -ProjectPath $project.ProjectPath -TargetName $project.RelativeProject -AnalyzerVersion $AnalyzerVersion
        [PSCustomObject]@{
            Target = $project.RelativeProject
            Context = ConvertTo-NormalizedCppcheckProjectContext -Context $context -ProjectPath $project.ProjectPath -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -ResolveIncludePaths
            TranslationUnits = @($project.Sources.Keys | Sort-Object)
        }
    })
    $commands = @(Get-GeneratedBuildInputCommands -BuildRoot $BuildRoot)
    foreach ($command in $commands) { foreach ($path in $command.InputPaths) { [void]$inputPaths.Add($path) } }
    $generatedPaths = @($commands | ForEach-Object { $_.RelativeOutputs } | ForEach-Object { Join-Path $BuildRoot $_ })
    foreach ($path in $generatedPaths + @(($generatedBundlePaths + $fastSourceGeneratedPaths) | ForEach-Object { Join-Path $RepositoryRoot $_ })) { [void]$inputPaths.Add($path) }
    foreach ($path in @('src/xlat_parser.y', 'tools/lemon/Release/lemon.exe', 'tools/lemon/Release/lempar.c', 'tools/re2c/Release/re2c.exe', 'tools/updaterevision/Release/updaterevision.exe')) {
        $fullPath = Join-Path $BuildRoot $path
        if ($path -like '*lempar.c' -and -not (Test-Path -LiteralPath $fullPath)) { $fullPath = Join-Path $RepositoryRoot 'tools/lemon/lempar.c' }
        [void]$inputPaths.Add($fullPath)
    }
    foreach ($file in Get-ChildItem -LiteralPath $BuildRoot -Recurse -File -Include '*.h', '*.hpp', '*.inc', '*.c', '*.cpp') { [void]$inputPaths.Add($file.FullName) }
    $python = Get-CMakePythonExecutable -CachePath (Join-Path $BuildRoot 'CMakeCache.txt')
    if ([string]::IsNullOrWhiteSpace($python)) { throw 'Fast receipt requires the configured Python executable.' }
    [void]$inputPaths.Add($python)
    [void]$inputPaths.Add((Get-Command cmake -ErrorAction Stop).Source)
    $inputs = @(foreach ($path in @($inputPaths | Sort-Object)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw [NotSupportedException]::new("Missing receipt preparation input '$path'; Full required.") }
        Assert-CppcheckPathHasNoReparseAncestor -Path $path
        Assert-NormalGeneratedBuildInput -Path $path -Description 'Fast preparation input'
        [PSCustomObject]@{ Path = [IO.Path]::GetFullPath($path); SHA256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    })
    $aliases = @(Get-CppcheckGeneratedDiagnosticAliases -GeneratedPaths $generatedPaths -InputPaths @((Join-Path $RepositoryRoot 'src/sc_man_scanner.re'), (Join-Path $RepositoryRoot 'src/xlat/xlat_parser.y')) -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot)
    [PSCustomObject]@{ MappingVersion = 1; Representation = 'git-archive/raw-base-blob-v1'; BuildRoot = $BuildRoot; Projects = $projectStates; Commands = $commands; Inputs = $inputs; Aliases = $aliases }
}

function Assert-CppcheckPreparationState {
    param([object]$Expected, [object]$Actual)

    if (($Expected | ConvertTo-Json -Compress -Depth 16) -cne ($Actual | ConvertTo-Json -Compress -Depth 16)) { throw 'Fast preparation context/input/output changed.' }
}

function New-CppcheckPreparationReceipt {
    param([object]$State, [string]$HeadCommit, [string]$RepositoryRoot, [string]$BuildRoot, [object[]]$Projects, [object[]]$BaselineProjects, [string]$BaselineRoot, [string]$BaselineBuildRoot, [string]$AnalyzerVersion)

    foreach ($project in $Projects) {
        $baseline = @($BaselineProjects | Where-Object RelativeProject -eq $project.RelativeProject)
        if ($baseline.Count -ne 1) { throw 'Receipt requires an exact baseline target.' }
        $expected = Get-CppcheckVendorDispositionContext -ProjectPath $project.ProjectPath -TargetName $project.RelativeProject -AnalyzerVersion $AnalyzerVersion
        $actual = Get-CppcheckVendorDispositionContext -ProjectPath $baseline[0].ProjectPath -TargetName $project.RelativeProject -AnalyzerVersion $AnalyzerVersion
        Assert-EqualCppcheckProjectContext -Expected (ConvertTo-NormalizedCppcheckProjectContext -Context $expected -ProjectPath $project.ProjectPath -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -ResolveIncludePaths) -Actual (ConvertTo-NormalizedCppcheckProjectContext -Context $actual -ProjectPath $baseline[0].ProjectPath -RepositoryRoot $BaselineRoot -BuildRoot $BaselineBuildRoot -ResolveIncludePaths) -Description 'receipt HEAD/baseline parity'
        $headUnits = @($project.Sources.Values | ForEach-Object { Get-RepositoryRelativePath -Path $_ -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot })
        $baseUnits = @($baseline[0].Sources.Values | ForEach-Object { Get-RepositoryRelativePath -Path $_ -RepositoryRoot $BaselineRoot -BuildRoot $BaselineBuildRoot })
        Assert-EqualTranslationUnitSet -Expected $headUnits -Actual $baseUnits -Description 'receipt HEAD/baseline full TU parity'
    }
    foreach ($path in @('sqlite/sqlite3.c', 'sqlite/sqlite3.h', 'sqlite/sqlite3ext.h')) {
        if ((Get-FileHash -LiteralPath (Join-Path $RepositoryRoot $path)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $BaselineRoot $path)).Hash) { throw "Receipt configure-generated input parity differs: '$path'." }
    }
    $revision = [IO.File]::ReadAllText((Join-Path $RepositoryRoot 'src/gitinfo.h'))
    if ($revision -notmatch ('(?m)^// ' + [regex]::Escape($HeadCommit) + '\s*$')) { throw 'Receipt revision input is not generated from clean HEAD.' }
    [PSCustomObject]@{ SchemaVersion = 1; Anchor = $HeadCommit; PreparationVerified = $true; State = $State }
}

function Assert-CppcheckFastLiveHead {
    param([string]$RepositoryRoot, [string]$HeadCommit)

    $head = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('rev-parse', '--verify', 'HEAD^{commit}'))).Trim()
    $dirty = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('status', '--porcelain=v1', '-z', '--untracked-files=no')
    if ($head -ne $HeadCommit -or $dirty.Length) { throw 'Fast live HEAD/index/tracked inputs changed during the run.' }
}

function Invoke-CppcheckFastAnalysis {
    param([string]$RepositoryRoot, [string]$BuildRoot, [object[]]$Projects, [object]$State, [string]$HeadCommit, [string]$BaseCommit, [string]$TempRoot, [string]$CacheRoot, [string]$CppcheckPath, [string]$AnalyzerVersion, [string]$AnalyzerSHA256, [int]$CppcheckJobs, [object]$Evidence = $null)

    $sourceRoot = Join-Path $TempRoot 'source'
    Assert-CppcheckFastLiveHead -RepositoryRoot $RepositoryRoot -HeadCommit $HeadCommit
    Assert-CppcheckPreparationState -Expected $State -Actual (Get-CppcheckPreparationState -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -Projects $Projects -AnalyzerVersion $AnalyzerVersion)
    Push-Location -LiteralPath $RepositoryRoot
    try { Expand-GitArchive -Commit $HeadCommit -Destination $sourceRoot -Evidence $Evidence }
    finally { Pop-Location }
    foreach ($relative in $generatedBundlePaths + $fastSourceGeneratedPaths) {
        $inputPath = Join-Path $RepositoryRoot $relative
        $verifiedInput = @($State.Inputs | Where-Object Path -eq $inputPath)
        if ($verifiedInput.Count -ne 1) { throw "Fast generated input not verified: '$relative'." }
        Copy-VerifiedGeneratedBundle -VerifiedBundle @([PSCustomObject]@{ RelativePath = $relative; GeneratedPath = $inputPath; Hash = $verifiedInput[0].SHA256 }) -DestinationRoot $sourceRoot
    }
    $mappedProjects = @(foreach ($project in $Projects) {
        New-CppcheckFastProject -Project $project -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -StageSourceRoot $sourceRoot -ProjectRoot (Join-Path $TempRoot 'project') -AnalyzerVersion $AnalyzerVersion
    })
    $headBatch = [Collections.Generic.List[object]]::new()
    $baseBatch = [Collections.Generic.List[object]]::new()
    $identities = @{}
    $leaves = @{}
    $contexts = @{}
    $cfgPaths = @(Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $CppcheckPath)
    foreach ($project in $Projects) {
        $target = $project.RelativeProject
        $contexts[$target] = Get-CppcheckVendorDispositionContext -ProjectPath $project.ProjectPath -TargetName $target -AnalyzerVersion $AnalyzerVersion
        $fastContext = Get-CppcheckRegressionAnalysisContext -BuildRoot $BuildRoot -RepositoryRoot $RepositoryRoot -ProjectPath $project.ProjectPath -InputRootIdentities @('regression-fast-head-context') -AnalyzerConfigurationPaths $cfgPaths -AnalyzerOptions @('--project-configuration=Release|x64', '--enable=warning,performance,portability')
        $fastContext | Add-Member -NotePropertyName FastContract -NotePropertyValue ([PSCustomObject]@{ Version = 1; Representation = $State.Representation; SharedInputs = $State.Inputs })
        $fastIdentity = Get-CppcheckCacheIdentity -AnalyzerVersion $AnalyzerVersion -AnalyzerSHA256 $AnalyzerSHA256 -InputMode 'normal-fast-head-context' -Configuration $contexts[$target].Configuration -Compiler $contexts[$target].Compiler -Toolset $contexts[$target].Toolset -Abi $contexts[$target].Abi -RootIdentity 'regression-fast-head-context-v1' -AnalysisContext $fastContext
        Save-CppcheckCacheIdentity -CacheRoot $CacheRoot -Namespace 'regression' -Identity $fastIdentity
        $mapped = $mappedProjects | Where-Object RelativeProject -eq $target | Select-Object -First 1
        foreach ($unit in $project.Files) {
            $headBytes = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('cat-file', 'blob', "${HeadCommit}:$unit")
            if ((Get-ByteSHA256 -Bytes ([IO.File]::ReadAllBytes((Join-Path $sourceRoot $unit)))) -ne (Get-ByteSHA256 -Bytes $headBytes)) { throw "Fast archive TU bytes differ from HEAD: '$unit'." }
            foreach ($role in @('head', 'baseline')) {
                $batch = $headBatch
                if ($role -eq 'baseline') { $batch = $baseBatch }
                $leaf = Get-CppcheckCacheLeaf -CacheRoot $CacheRoot -Namespace 'regression' -Identity $fastIdentity -Role $role -TargetName $target -TranslationUnit $unit
                $descriptor = New-CppcheckDescriptor -CppcheckPath $CppcheckPath -ProjectPath $mapped.ProjectPath -CachePath $leaf -RepositoryRoot $sourceRoot -BuildRoot $BuildRoot -TargetName $target -TranslationUnit $unit -Index $batch.Count
                $descriptor.WorkingDirectory = $sourceRoot
                $descriptor | Add-Member -NotePropertyName DiagnosticFileAliases -NotePropertyValue @($State.Aliases)
                $batch.Add($descriptor)
                $leaves["$role/$target/$unit"] = $leaf
                $identities["$role/$target"] = $fastIdentity
            }
        }
    }
    if (($headBatch | ForEach-Object { "$($_.TargetName)|$($_.TranslationUnit)" } | ConvertTo-Json -Compress) -cne ($baseBatch | ForEach-Object { "$($_.TargetName)|$($_.TranslationUnit)" } | ConvertTo-Json -Compress)) { throw 'Fast selected HEAD/Base descriptor sets differ.' }
    if ($null -ne $Evidence) {
        @{ HEAD = $headBatch.ToArray(); Base = $baseBatch.ToArray(); State = $State; HeadCommit = $HeadCommit; BaseCommit = $BaseCommit } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $Evidence.Root 'fast-inputs.json')
    }
    $preparationElapsed = $script:lintTotalTimer.Elapsed.TotalMilliseconds
    $analyzerTimer = [Diagnostics.Stopwatch]::StartNew()
    $fastHeadDiagnostics = @(Invoke-CppcheckBatch -Descriptors $headBatch.ToArray() -CppcheckJobs $CppcheckJobs -Evidence $Evidence)
    Set-CppcheckBaseBlobs -RepositoryRoot $RepositoryRoot -BaseCommit $BaseCommit -Files @($Projects | ForEach-Object { $_.Files } | Sort-Object -Unique) -StageSourceRoot $sourceRoot
    $fastBaseDiagnostics = @(Invoke-CppcheckBatch -Descriptors $baseBatch.ToArray() -CppcheckJobs $CppcheckJobs -Evidence $Evidence)
    $analyzerTimer.Stop()
    Assert-CppcheckPreparationState -Expected $State -Actual (Get-CppcheckPreparationState -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -Projects $Projects -AnalyzerVersion $AnalyzerVersion)
    Assert-CppcheckFastLiveHead -RepositoryRoot $RepositoryRoot -HeadCommit $HeadCommit
    [PSCustomObject]@{ Head = $fastHeadDiagnostics; Baseline = $fastBaseDiagnostics; Contexts = $contexts; Identities = $identities; CachePaths = $leaves; Projects = $mappedProjects; PreparationMilliseconds = $preparationElapsed; AnalyzerMilliseconds = $analyzerTimer.Elapsed.TotalMilliseconds }
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
        [string]$BuildRoot,
        [switch]$ResolveIncludePaths
    )

    $normalized = [PSCustomObject]@{}

    foreach ($property in @('Compiler', 'Toolset', 'Abi', 'Configuration', 'Defines')) {
        $value = [string]$Context.$property

        if ($property -eq 'Defines') {
            $value = ConvertTo-NormalizedCppcheckPathValue -Value $value -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot
        }

        $normalized | Add-Member -NotePropertyName $property -NotePropertyValue $value
    }

    $normalized | Add-Member -NotePropertyName AdditionalIncludeDirectories -NotePropertyValue (Get-NormalizedCppcheckAdditionalIncludeDirectories -ProjectPath $ProjectPath -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -ResolvePaths:$ResolveIncludePaths)

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
        [string]$BuildRoot,
        [switch]$ResolvePaths
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
        $include = $_
        if ($ResolvePaths -and -not [string]::IsNullOrWhiteSpace($include) -and $include -notmatch '%\(|\$\(') {
            $include = [IO.Path]::GetFullPath($include, (Split-Path $ProjectPath))
        }
        ConvertTo-NormalizedCppcheckPathValue -Value $include -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot
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

function ConvertTo-CppcheckStagePath {
    param([string]$Path, [string]$ProjectDirectory, [string]$RepositoryRoot, [string]$BuildRoot, [string]$StageSourceRoot)

    if ($Path -match '\$\(|%\(') { throw [NotSupportedException]::new("Unknown Fast path macro '$Path'.") }
    $fullPath = [IO.Path]::GetFullPath($Path, $ProjectDirectory)
    $buildRelative = Get-PathRelativeToRoot -Path $fullPath -RootPath $BuildRoot
    if ($null -ne $buildRelative) { return $fullPath }
    $sourceRelative = Get-PathRelativeToRoot -Path $fullPath -RootPath $RepositoryRoot
    if ($null -ne $sourceRelative) { return [IO.Path]::GetFullPath((Join-Path $StageSourceRoot $sourceRelative)) }
    return $fullPath
}

function New-CppcheckFastProject {
    param([object]$Project, [string]$RepositoryRoot, [string]$BuildRoot, [string]$StageSourceRoot, [string]$ProjectRoot, [string]$AnalyzerVersion)

    $originalContext = Get-CppcheckVendorDispositionContext -ProjectPath $Project.ProjectPath -TargetName $Project.RelativeProject -AnalyzerVersion $AnalyzerVersion
    $document = [Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.Load($Project.ProjectPath)
    $projectDirectory = Split-Path -Parent $Project.ProjectPath
    foreach ($item in $document.SelectNodes("//*[local-name()='ItemGroup']/*[local-name()='ClCompile']")) {
        $originalPath = [IO.Path]::GetFullPath($item.GetAttribute('Include'), $projectDirectory)
        if ($null -eq (Get-PathRelativeToRoot -Path $originalPath -RootPath $RepositoryRoot) -and $null -eq (Get-PathRelativeToRoot -Path $originalPath -RootPath $BuildRoot)) { throw [NotSupportedException]::new('Fast cannot isolate an external TU path.') }
        $item.SetAttribute('Include', (ConvertTo-CppcheckStagePath -Path $item.GetAttribute('Include') -ProjectDirectory $projectDirectory -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -StageSourceRoot $StageSourceRoot))
    }
    foreach ($node in $document.SelectNodes("//*[local-name()='ClCompile']/*[local-name()='AdditionalIncludeDirectories']")) {
        $includes = @(foreach ($include in $node.InnerText.Split(';')) {
            if ($include -eq '%(AdditionalIncludeDirectories)' -or [string]::IsNullOrWhiteSpace($include)) { $include; continue }
            ConvertTo-CppcheckStagePath -Path $include -ProjectDirectory $projectDirectory -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -StageSourceRoot $StageSourceRoot
        })
        $node.InnerText = $includes -join ';'
    }
    foreach ($node in $document.SelectNodes("//*[local-name()='ClCompile']/*[local-name()='PreprocessorDefinitions']")) {
        if (($node.InnerText -replace '%\(PreprocessorDefinitions\)', '') -match '\$\(|%\(') { throw 'Unknown Fast define macro.' }
        $value = ConvertTo-NormalizedCppcheckPathValue -Value $node.InnerText -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot
        $node.InnerText = $value.Replace('<source>', $StageSourceRoot.Replace('\', '/')).Replace('<build>', $BuildRoot.Replace('\', '/'))
    }
    $destination = Join-Path $ProjectRoot $Project.RelativeProject
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    $document.Save($destination)
    $mappedContext = Get-CppcheckVendorDispositionContext -ProjectPath $destination -TargetName $Project.RelativeProject -AnalyzerVersion $AnalyzerVersion
    $expectedContext = ConvertTo-NormalizedCppcheckProjectContext -Context $originalContext -ProjectPath $Project.ProjectPath -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -ResolveIncludePaths
    $actualContext = ConvertTo-NormalizedCppcheckProjectContext -Context $mappedContext -ProjectPath $destination -RepositoryRoot $StageSourceRoot -BuildRoot $BuildRoot -ResolveIncludePaths
    Assert-EqualCppcheckProjectContext -Expected $expectedContext -Actual $actualContext -Description 'Fast HEAD XML path mapping'
    $sources = @{}
    foreach ($item in $document.SelectNodes("//*[local-name()='ItemGroup']/*[local-name()='ClCompile']")) {
        $path = $item.GetAttribute('Include')
        $root = if ($null -ne (Get-PathRelativeToRoot -Path $path -RootPath $BuildRoot)) { $RepositoryRoot } else { $StageSourceRoot }
        $sources[(Get-RepositoryRelativePath -Path $path -RepositoryRoot $root)] = $path
    }
    Assert-EqualTranslationUnitSet -Expected @($Project.Sources.Keys) -Actual @($sources.Keys) -Description 'Fast entire project TU mapping'
    [PSCustomObject]@{ RelativeProject = $Project.RelativeProject; ProjectPath = $destination; Sources = $sources; Files = @($Project.Files) }
}

function Get-CppcheckGeneratedDiagnosticAliases {
    param([string[]]$GeneratedPaths, [string[]]$InputPaths, [string]$RepositoryRoot, [string]$BuildRoot)

    $aliases = [Collections.Generic.List[object]]::new()
    foreach ($generatedPath in $GeneratedPaths) {
        Assert-NormalGeneratedBuildInput -Path $generatedPath -Description 'Fast generated diagnostic input'
        foreach ($match in [regex]::Matches([IO.File]::ReadAllText($generatedPath), '(?m)^\s*#\s*(?:line\s+)?[0-9]+\s+"(?<path>[^"]+)"')) {
            $path = $match.Groups['path'].Value
            if (-not [IO.Path]::IsPathRooted($path)) {
                if ($path -in @('xlat_parser.y', 'xlat_parser.c', 'xlat_parser.h', 'sc_man_scanner.h')) { continue }
                throw [NotSupportedException]::new("Unknown generated #line path '$path'.")
            }
            $fullPath = [IO.Path]::GetFullPath($path)
            if ($fullPath -in $GeneratedPaths) { continue }
            $relative = Get-PathRelativeToRoot -Path $fullPath -RootPath $RepositoryRoot
            if ($null -eq $relative -or $fullPath -notin $InputPaths) { throw [NotSupportedException]::new("Unknown generated #line input '$path'.") }
            Assert-NormalGeneratedBuildInput -Path $fullPath -Description 'Fast #line source input'
            $aliases.Add([PSCustomObject]@{
                AbsolutePath = $path; RelativePath = $relative
                InputPath = $fullPath; InputSHA256 = (Get-FileHash -LiteralPath $fullPath).Hash
                GeneratedPath = $generatedPath; GeneratedSHA256 = (Get-FileHash -LiteralPath $generatedPath).Hash
            })
        }
    }
    return @($aliases.ToArray() | Sort-Object AbsolutePath, GeneratedPath -Unique)
}

function ConvertTo-CppcheckAliasedOutput {
    param([string[]]$Output, [object[]]$Aliases, [string]$StageSourceRoot)

    foreach ($line in $Output) {
        $separator = $line.IndexOf("`t")
        if ($separator -gt 0) {
            $file = $line.Substring(0, $separator)
            $alias = @($Aliases | Where-Object { $_.AbsolutePath -ceq $file -or $_.InputPath -ceq $file } | Select-Object -First 1)
            if ($alias.Count) { $line = (Join-Path $StageSourceRoot $alias[0].RelativePath) + $line.Substring($separator) }
        }
        $line
    }
}

function Set-CppcheckBaseBlobs {
    param([string]$RepositoryRoot, [string]$BaseCommit, [string[]]$Files, [string]$StageSourceRoot)

    foreach ($file in @($Files | Sort-Object -Unique)) {
        $relative = ConvertTo-SafeRelativePath -Path $file -Description 'Fast Base TU'
        $destination = Join-Path $StageSourceRoot $relative
        Assert-NormalPathWithinRoot -Path $destination -RootPath $StageSourceRoot -Description 'Fast Base TU overlay'
        Assert-NormalGeneratedBuildInput -Path $destination -Description 'Fast existing TU'
        $bytes = Get-CppcheckGitBytes -RepositoryRoot $RepositoryRoot -Arguments @('cat-file', 'blob', "${BaseCommit}:$relative")
        [IO.File]::WriteAllBytes($destination, $bytes)
        if ((Get-ByteSHA256 -Bytes ([IO.File]::ReadAllBytes($destination))) -ne (Get-ByteSHA256 -Bytes $bytes)) { throw "Fast Base blob overlay failed for '$relative'." }
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
                InputPaths = @($customBuild.ChildNodes | Where-Object {
                    $_.LocalName -eq 'AdditionalInputs' -and $_.GetAttribute('Condition') -eq $configurationCondition
                } | ForEach-Object { $_.InnerText.Split(';') } | Where-Object { $_ -ne '%(AdditionalInputs)' } | ForEach-Object {
                    if ($_ -match '\$\(|%\(') { throw "Unknown generated-input macro '$_'." }
                    [IO.Path]::GetFullPath($_, (Split-Path $projectPath))
                })
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

function New-CppcheckDescriptor {
    param(
        [string]$CppcheckPath,
        [string]$ProjectPath,
        [string]$CachePath,
        [string]$RepositoryRoot,
        [string]$BuildRoot,
        [string]$TargetName,
        [string]$TranslationUnit,
        [int]$Index = 0,
        [bool]$UseProjectConfiguration = $true
    )

    $template = '{file}' + "`t" + '{line}' + "`t" + '{column}' + "`t" + '{severity}' + "`t" + '{id}' + "`t" + '{message}'
    $arguments = @(
        "--project=$ProjectPath"
        "--enable=warning,performance,portability"
        "--error-exitcode=1"
        "--cppcheck-build-dir=$CachePath"
        "--file-filter=$TranslationUnit"
        "--debug-analyzerinfo"
        "--template=$template"
        "--quiet"
        "-j"
        '1'
    )

    if ($UseProjectConfiguration) {
        $arguments += '--project-configuration=Release|x64'
    }

    return [PSCustomObject]@{
        Index = $Index; Path = $CppcheckPath; Arguments = $arguments
        WorkingDirectory = $ExecutionContext.SessionState.Path.CurrentFileSystemLocation.Path
        RepositoryRoot = $RepositoryRoot; BuildRoot = $BuildRoot; TargetName = $TargetName
        TranslationUnit = $TranslationUnit; CachePath = $CachePath
        Label = "cppcheck-$TargetName-$TranslationUnit"
    }
}

function Invoke-CppcheckProcessBatch {
    param([object[]]$Descriptors, [int]$CppcheckJobs, [object]$Evidence = $null)

    if ($CppcheckJobs -lt 1) { throw 'Cppcheck TU workers must be positive.' }
    $leaves = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    for ($index = 0; $index -lt $Descriptors.Count; $index++) {
        if ($Descriptors[$index].Index -ne $index) { throw 'Cppcheck descriptors must have contiguous ordered indices.' }
        if (-not $leaves.Add([System.IO.Path]::GetFullPath($Descriptors[$index].CachePath))) {
            throw "Cppcheck batch contains a duplicate cache leaf '$($Descriptors[$index].CachePath)'."
        }
    }
    $workerCount = [Math]::Min($CppcheckJobs, $Descriptors.Count)
    $owned = [System.Collections.Generic.List[object]]::new()
    $active = [System.Collections.Generic.List[object]]::new()
    $results = [object[]]::new($Descriptors.Count)
    $failures = [System.Collections.Generic.List[object]]::new()
    $nextIndex = 0
    try {
        while (($nextIndex -lt $Descriptors.Count) -or ($active.Count -gt 0)) {
            while (($nextIndex -lt $Descriptors.Count) -and ($active.Count -lt $workerCount)) {
                $descriptor = $Descriptors[$nextIndex]
                New-Item -ItemType Directory -Force -Path $descriptor.CachePath | Out-Null
                $process = [System.Diagnostics.Process]::new()
                $state = [PSCustomObject]@{
                    Descriptor = $descriptor; Process = $process; Started = $false
                    Stdout = $null; Stderr = $null; Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
                }
                $owned.Add($state)
                $process.StartInfo.FileName = $descriptor.Path
                $process.StartInfo.WorkingDirectory = $descriptor.WorkingDirectory
                $process.StartInfo.UseShellExecute = $false
                $process.StartInfo.RedirectStandardOutput = $true
                $process.StartInfo.RedirectStandardError = $true
                $process.StartInfo.StandardOutputEncoding = [Console]::OutputEncoding
                $process.StartInfo.StandardErrorEncoding = [Console]::OutputEncoding
                foreach ($argument in $descriptor.Arguments) { $process.StartInfo.ArgumentList.Add($argument) }
                $state.Started = $process.Start()
                if (-not $state.Started) { throw "Could not start '$($descriptor.Path)'." }
                $state.Stdout = $process.StandardOutput.ReadToEndAsync()
                $state.Stderr = $process.StandardError.ReadToEndAsync()
                $active.Add($state)
                $nextIndex++
            }
            foreach ($state in @($active.ToArray())) {
                if ($state.Stdout.IsFaulted) { $null = $state.Stdout.GetAwaiter().GetResult() }
                if ($state.Stderr.IsFaulted) { $null = $state.Stderr.GetAwaiter().GetResult() }
                if ($state.Process.HasExited -and $state.Stdout.IsCompleted -and $state.Stderr.IsCompleted) {
                    $state.Stopwatch.Stop()
                    $null = $active.Remove($state)
                }
            }
            if (($active.Count -gt 0) -and (($nextIndex -ge $Descriptors.Count) -or ($active.Count -eq $workerCount))) {
                $waiting = $active[0]
                if (-not $waiting.Process.HasExited) { $null = $waiting.Process.WaitForExit(25) }
                elseif (-not $waiting.Stdout.IsCompleted) { $null = $waiting.Stdout.Wait(25) }
                elseif (-not $waiting.Stderr.IsCompleted) { $null = $waiting.Stderr.Wait(25) }
            }
        }
    }
    catch { $failures.Add($_) }
    finally {
        foreach ($state in $owned) {
            if ($state.Started) {
                try {
                    if (-not $state.Process.HasExited) { $state.Process.Kill($true) }
                }
                catch { $failures.Add($_) }
            }
        }
        foreach ($state in $owned) {
            try {
                if ($state.Started) {
                    $transportErrors = [System.Collections.Generic.List[string]]::new()
                    $result = [PSCustomObject]@{
                        Index = $state.Descriptor.Index; Output = @(); ExitCode = $null
                        ElapsedMilliseconds = $null; ProcessId = $null; StartedAtUtc = $null; EndedAtUtc = $null
                        Stdout = $null; Stderr = $null; StdoutComplete = $false; StderrComplete = $false
                        TransportErrors = @()
                    }
                    try {
                        $state.Process.WaitForExit()
                        $result.ExitCode = $state.Process.ExitCode
                        $result.ProcessId = $state.Process.Id
                        $result.StartedAtUtc = $state.Process.StartTime.ToUniversalTime().ToString('o')
                        $result.EndedAtUtc = $state.Process.ExitTime.ToUniversalTime().ToString('o')
                    }
                    catch { $failures.Add($_); $transportErrors.Add($_.Exception.Message) }
                    foreach ($stream in @('Stdout', 'Stderr')) {
                        try {
                            if ($null -eq $state.$stream) {
                                $reader = if ($stream -eq 'Stdout') { $state.Process.StandardOutput } else { $state.Process.StandardError }
                                $state.$stream = $reader.ReadToEndAsync()
                            }
                            $result.$stream = $state.$stream.GetAwaiter().GetResult()
                            $result.($stream + 'Complete') = $true
                        }
                        catch { $failures.Add($_); $transportErrors.Add($_.Exception.Message) }
                    }
                    $state.Stopwatch.Stop()
                    $result.ElapsedMilliseconds = $state.Stopwatch.ElapsedMilliseconds
                    $result.TransportErrors = $transportErrors.ToArray()
                    $result.Output = @(foreach ($text in @($result.Stdout, $result.Stderr)) {
                        if (-not [string]::IsNullOrEmpty($text)) {
                            $lines = [regex]::Split($text, '\r\n|\n|\r')
                            $count = $lines.Count
                            if ($lines[-1] -eq '') { $count-- }
                            for ($lineIndex = 0; $lineIndex -lt $count; $lineIndex++) { $lines[$lineIndex] }
                        }
                    })
                    $results[$state.Descriptor.Index] = $result
                }
            }
            catch { $failures.Add($_) }
            finally {
                try { $state.Process.Dispose() }
                catch { $failures.Add($_) }
            }
        }
        foreach ($descriptor in $Descriptors) {
            if ($null -ne $results[$descriptor.Index]) {
                try {
                    Save-LintChildEvidence -Path $descriptor.Path -Arguments $descriptor.Arguments -Label $descriptor.Label -WorkingDirectory $descriptor.WorkingDirectory -Evidence $Evidence -Result $results[$descriptor.Index]
                }
                catch { $failures.Add($_) }
            }
        }
    }
    if ($failures.Count -gt 0) {
        $message = $failures[0].Exception.Message
        if ($failures.Count -gt 1) { $message += ' Secondary failures: ' + (($failures | Select-Object -Skip 1 | ForEach-Object { $_.Exception.Message }) -join '; ') }
        throw [System.Exception]::new($message, $failures[0].Exception)
    }
    return $results
}

function Invoke-CppcheckBatch {
    param([object[]]$Descriptors, [int]$CppcheckJobs, [object]$Evidence = $null)

    $results = @(Invoke-CppcheckProcessBatch -Descriptors $Descriptors -CppcheckJobs $CppcheckJobs -Evidence $Evidence)
    foreach ($descriptor in $Descriptors) {
        $result = $results[$descriptor.Index]
        $output = $result.Output
        if ($null -ne $descriptor.PSObject.Properties['DiagnosticFileAliases']) {
            $output = @(ConvertTo-CppcheckAliasedOutput -Output $output -Aliases $descriptor.DiagnosticFileAliases -StageSourceRoot $descriptor.RepositoryRoot)
        }
        ConvertFrom-CppcheckProjectOutput -Output $output -ExitCode $result.ExitCode -RepositoryRoot $descriptor.RepositoryRoot -BuildRoot $descriptor.BuildRoot -TargetName $descriptor.TargetName -TranslationUnit $descriptor.TranslationUnit -AllowedMissingFiles $generatedBundlePaths
    }
}

function Invoke-CppcheckProject {
    param(
        [string]$CppcheckPath, [string]$ProjectPath, [string]$CachePath,
        [string]$RepositoryRoot, [string]$BuildRoot, [string]$TargetName,
        [string]$TranslationUnit, [int]$CppcheckJobs, [object]$Evidence = $null,
        [bool]$UseProjectConfiguration = $true
    )

    $descriptor = New-CppcheckDescriptor -CppcheckPath $CppcheckPath -ProjectPath $ProjectPath -CachePath $CachePath -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot -TargetName $TargetName -TranslationUnit $TranslationUnit -UseProjectConfiguration $UseProjectConfiguration
    Invoke-CppcheckBatch -Descriptors @($descriptor) -CppcheckJobs $CppcheckJobs -Evidence $Evidence
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

$localCppcheckPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'cppcheck/cppcheck.exe'
if (Test-Path -LiteralPath $localCppcheckPath -PathType Leaf) {
    $cppcheck = Get-Command $localCppcheckPath -ErrorAction Stop
}
else {
    $cppcheck = Get-Command cppcheck -ErrorAction SilentlyContinue
}
$cmake = Get-Command cmake -ErrorAction SilentlyContinue

if (-not $cppcheck) {
    Write-Error "Cppcheck was not found at '$localCppcheckPath' or in PATH. Place Cppcheck at that path or install it in PATH and restart your terminal/IDE."
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
$cppcheckCacheRoot = Join-Path $repositoryRoot '.cppcheck-cache'
$cacheLock = $null
$evidence = $null
$inputManifestRoots = $null
$tempRoot = $null
$cppcheckSHA256 = (Get-FileHash -LiteralPath $cppcheck.Source -Algorithm SHA256).Hash

try {
if ($null -ne $inputManifestData) {
    $cacheLock = Enter-CppcheckCacheLock -CacheRoot $cppcheckCacheRoot -Name 'regression'
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
        $inputManifestRoots = Invoke-InputManifestPreflight -Manifest $inputManifestData -RepositoryRoot $repositoryRoot -CachePath $preflightCachePath -CMakePath $cmake.Source -CppcheckCacheRoot $cppcheckCacheRoot -KeepTemporaryRoot -Evidence $evidence
    }
    catch {
        Write-Error $_.Exception.Message
        exit 1
    }

    if ($PreflightOnly) {
        Remove-CppcheckDisposableStage -CacheRoot $cppcheckCacheRoot -StageRoot $inputManifestRoots.TempRoot -Name 'regression-manifest'
        exit 0
    }
}

Write-Host "Cppcheck parallel jobs: $CppcheckJobs"
$script:lintTotalTimer = [Diagnostics.Stopwatch]::StartNew()
$preparationMilliseconds = 0.0
$analyzerMilliseconds = 0.0
$analysisProjects = @()

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

$generatorSetting = Get-CMakeCacheSetting -CachePath $cacheFile -Name 'CMAKE_GENERATOR'
$platformSetting = Get-CMakeCacheSetting -CachePath $cacheFile -Name 'CMAKE_GENERATOR_PLATFORM'
$toolsetSetting = Get-CMakeCacheSetting -CachePath $cacheFile -Name 'CMAKE_GENERATOR_TOOLSET'
$cachePaths = @{}
$cacheIdentities = @{}

$changedFiles = @()
$sourceRoute = $null
$treeChanges = @()

if ($null -ne $inputManifestData) {
    $changedFiles = @($inputManifestData.Files | Where-Object { $_.Path -match '\.(c|cc|cpp|cxx)$' } | ForEach-Object { $_.Path })
}
else {
    $treeChanges = @(Get-CppcheckTreeChanges -RepositoryRoot $repositoryRoot -BaseCommit $baseCommit -HeadCommit $headCommit)
    $dirtyBytes = Get-CppcheckGitBytes -RepositoryRoot $repositoryRoot -Arguments @('status', '--porcelain=v1', '-z', '--untracked-files=no')
    $sourceRoute = Get-CppcheckSourceRoute -Changes $treeChanges -TrackedDirty:($dirtyBytes.Length -ne 0)
    $changedFiles = $sourceRoute.Sources
    if ($sourceRoute.Mode -eq 'Error') { throw $sourceRoute.Reason }
}

$changedFiles = @($changedFiles | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)

if ($changedFiles.Count -eq 0) {
    if ($null -ne $inputManifestData) {
        Write-Error 'InputManifest contains no C/C++ translation units for the full Cppcheck gate.'
        exit 1
    }

    Write-Host $sourceRoute.Reason
    exit 0
}

$headProjects = Get-ProjectSources -BuildRoot $analysisBuildRoot -RepositoryRoot $analysisRepositoryRoot
$isWindowsVisualStudioBuild = ($null -ne $generatorSetting) -and
    ($generatorSetting.Value -match '^Visual Studio ') -and
    ($null -ne $platformSetting) -and
    (-not [string]::IsNullOrWhiteSpace($platformSetting.Value))
$useFast = $false
$fastState = $null
$receipt = $null
$routeReason = if ($null -ne $inputManifestData) { 'InputManifest retains its independent Full context' } elseif ($CppcheckMode -eq 'Full') { 'explicit Full mode' } else { $sourceRoute.Reason }
if ($null -eq $inputManifestData -and $sourceRoute.Mode -eq 'Candidate') {
    $cacheLock = Enter-CppcheckCacheLock -CacheRoot $cppcheckCacheRoot -Name 'regression'
    if ($CppcheckMode -eq 'Auto') {
        $receipt = Get-CppcheckPreparationReceipt -CacheRoot $cppcheckCacheRoot -BuildRoot $buildRoot
        if ($null -eq $receipt) { $routeReason = 'no verified preparation receipt; cold Full required' }
        elseif ($null -eq $receipt.PSObject.Properties['Anchor'] -or $null -eq $receipt.PSObject.Properties['State'] -or $null -eq $receipt.PSObject.Properties['PreparationVerified'] -or -not $receipt.PreparationVerified) { $routeReason = 'incomplete preparation receipt' }
        else {
            $anchorChanges = @(Get-CppcheckTreeChanges -RepositoryRoot $repositoryRoot -BaseCommit $receipt.Anchor -HeadCommit $headCommit)
            $anchorRoute = Get-CppcheckSourceRoute -Changes $anchorChanges
            if ($anchorChanges.Count -and $anchorRoute.Mode -ne 'Candidate') { $routeReason = 'receipt anchor has non-source-only changes' }
            else {
                $candidateSelection = Select-CppcheckProjects -Projects $headProjects -ChangedFiles $changedFiles -IsInputManifest:$false -IsWindowsVisualStudioBuild:$isWindowsVisualStudioBuild -BuildDirectory $BuildDir -SelectedOnly
                if ($candidateSelection.Projects.Count) {
                    try {
                        Assert-CppcheckFastSourceSafety -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -HeadCommit $headCommit -ChangedFiles @($changedFiles + $anchorRoute.Sources | Sort-Object -Unique) -Projects $candidateSelection.Projects -BaseCommit $baseCommit
                        $fastState = Get-CppcheckPreparationState -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -Projects $candidateSelection.Projects -AnalyzerVersion $cppcheckVersion[0]
                        if (($receipt.State | ConvertTo-Json -Depth 16 -Compress) -ceq ($fastState | ConvertTo-Json -Depth 16 -Compress)) { $useFast = $true; $routeReason = 'verified source-only receipt reuse' }
                        else { $routeReason = 'preparation receipt context/input/output mismatch' }
                    }
                    catch [NotSupportedException] { $routeReason = $_.Exception.Message }
                }
            }
        }
    }
}
$stageName = if ($null -ne $inputManifestData) { 'regression-manifest' } elseif ($useFast) { 'regression-fast' } else { 'regression-normal' }
Write-Host "Cppcheck mode: $(if ($useFast) { 'Fast' } else { 'Full' }); reason: $routeReason."
if ($useFast) { Write-Host 'Scope: selected TUs only, both roles share verified HEAD context; not whole-project/Base-build equivalence. Do not run a concurrent build.' }
$selection = Select-CppcheckProjects `
    -Projects $headProjects `
    -ChangedFiles $changedFiles `
    -IsInputManifest:($null -ne $inputManifestData) `
    -IsWindowsVisualStudioBuild:$isWindowsVisualStudioBuild `
    -BuildDirectory $BuildDir `
    -SelectedOnly:$useFast `
    -PrepareAnalysis {
        $acquiredCacheLock = $null
        $preparedCacheLock = $cacheLock

        if ($null -eq $preparedCacheLock) {
            $acquiredCacheLock = Enter-CppcheckCacheLock -CacheRoot $cppcheckCacheRoot -Name 'regression'
            $preparedCacheLock = $acquiredCacheLock
        }

        try {
            [PSCustomObject]@{
                CacheLock = $preparedCacheLock
                TempRoot = if ($null -ne $inputManifestData) { $inputManifestRoots.TempRoot } else { New-CppcheckDisposableStage -CacheRoot $cppcheckCacheRoot -Name $stageName }
            }
        }
        catch {
            if ($null -ne $acquiredCacheLock) {
                Exit-CppcheckCacheLock -Lock $acquiredCacheLock
            }

            throw
        }
    }
$analysisProjects = $selection.Projects

foreach ($disposition in $selection.PlatformNotApplicable) {
    Write-Host "Platform-not-applicable source '$($disposition.Path)': $($disposition.Reason)"
}

if ($analysisProjects.Count -eq 0) {
    Write-Host 'No changed C/C++ translation units are analyzed by this Windows Visual Studio configuration.'
    exit 0
}

$cacheLock = $selection.AnalysisPreparation.CacheLock
$tempRoot = $selection.AnalysisPreparation.TempRoot
$baselineRoot = if ($null -ne $inputManifestData) { $inputManifestRoots.BaselineRoot } else { Join-Path $tempRoot 'baseline' }
$baselineBuildRoot = if ($null -ne $inputManifestData) { $inputManifestRoots.BaselineBuildRoot } else { Join-Path $baselineRoot 'build' }
$baselineWorktreeCreated = $false
$baselineProjects = @()
$vendorDispositionContexts = @{}
$verifiedGeneratedBundle = @()
$finalResultSaved = $false
$cleanupState = [PSCustomObject]@{ Completed = $false }
$gateFailure = ''

try {
    $receiptStartState = $null
    $receiptCandidate = $null
    if ($useFast) {
        $fastResult = Invoke-CppcheckFastAnalysis -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -Projects $analysisProjects -State $fastState -HeadCommit $headCommit -BaseCommit $baseCommit -TempRoot $tempRoot -CacheRoot $cppcheckCacheRoot -CppcheckPath $cppcheck.Source -AnalyzerVersion $cppcheckVersion[0] -AnalyzerSHA256 $cppcheckSHA256 -CppcheckJobs $CppcheckJobs -Evidence $evidence
        $headDiagnostics = $fastResult.Head
        $baselineDiagnostics = $fastResult.Baseline
        $vendorDispositionContexts = $fastResult.Contexts
        $cacheIdentities = $fastResult.Identities
        $cachePaths = $fastResult.CachePaths
        $preparationMilliseconds = $fastResult.PreparationMilliseconds
        $analyzerMilliseconds = $fastResult.AnalyzerMilliseconds
    }
    else {
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
        if ($sourceRoute.Mode -eq 'Candidate') {
            $refresh = Invoke-LintChild -Path $cmake.Source -Arguments @('-S', $repositoryRoot, '-B', $buildRoot) -Label 'receipt-refresh-head' -Evidence $evidence
            if ($refresh.ExitCode -ne 0) { throw 'Cold receipt HEAD CMake refresh failed.' }
            $headProjects = Get-ProjectSources -BuildRoot $buildRoot -RepositoryRoot $repositoryRoot
            $analysisProjects = (Select-CppcheckProjects -Projects $headProjects -ChangedFiles $changedFiles -IsInputManifest:$false -IsWindowsVisualStudioBuild:$isWindowsVisualStudioBuild -BuildDirectory $BuildDir).Projects
        }
    }

    if ($null -ne $inputManifestData) {
        $verifiedGeneratedBundle = $inputManifestRoots.VerifiedGeneratedBundle
    }
    else {
        Invoke-GeneratedBuildInputPreparation -CMakePath $cmake.Source -BuildRoot $analysisBuildRoot -RevisionName 'source' -Evidence $evidence
        if ($sourceRoute.Mode -eq 'Candidate') {
            $revisionResult = Invoke-LintChild -Path $cmake.Source -Arguments @('--build', $buildRoot, '--config', 'Release', '--target', 'revision_check') -Label 'receipt-source-revision' -Evidence $evidence
            if ($revisionResult.ExitCode -ne 0) { throw 'Cold receipt revision preparation failed.' }
            Invoke-ProtocolspecGeneration -CMakePath $cmake.Source -BuildRoot $buildRoot -RevisionName 'source' -Evidence $evidence
        }
    }

    if ($null -ne $inputManifestData) { Write-Host 'Protocolspec provenance equality: confirmed.' }

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
        Write-Host 'Protocolspec provenance and generated output equality: confirmed.'
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

    if ($null -ne $inputManifestData) { Write-Host "Cppcheck context parity: confirmed for $($analysisProjects.Count) target(s) (InputManifest)." }
    else { Write-Host 'Full uses independently configured HEAD/live-worktree and Base projects; context/TU parity is not implied.' }
    if ($null -eq $inputManifestData -and $sourceRoute.Mode -eq 'Candidate') {
        try {
            Assert-CppcheckFastSourceSafety -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -HeadCommit $headCommit -ChangedFiles $changedFiles -Projects $analysisProjects
            $receiptStartState = Get-CppcheckPreparationState -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -Projects $analysisProjects -AnalyzerVersion $cppcheckVersion[0]
            $null = @(foreach ($project in $analysisProjects) { New-CppcheckFastProject -Project $project -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -StageSourceRoot (Join-Path $tempRoot 'receipt-proof/source') -ProjectRoot (Join-Path $tempRoot 'receipt-proof/project') -AnalyzerVersion $cppcheckVersion[0] })
            $receiptCandidate = New-CppcheckPreparationReceipt -State $receiptStartState -HeadCommit $headCommit -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -Projects $analysisProjects -BaselineProjects $baselineProjects -BaselineRoot $baselineRoot -BaselineBuildRoot $baselineBuildRoot -AnalyzerVersion $cppcheckVersion[0]
            Write-Host 'Receipt preparation: real HEAD/Base context and entire TU parity verified.'
        }
        catch {
            if ($_.Exception -isnot [NotSupportedException] -and $_.Exception.Message -notmatch '^Cppcheck project context mismatch|^Cppcheck translation-unit selection mismatch|^Receipt ') { throw }
            Write-Host "Receipt not issued: $($_.Exception.Message)"
            $receiptCandidate = $null
        }
    }
    Save-LintEvidenceState -Evidence $evidence -AnalysisBuildRoot $analysisBuildRoot -BaselineBuildRoot $baselineBuildRoot -TempRoot $tempRoot -AnalysisProjects $analysisProjects -BaselineProjects $baselineProjects -Contexts $vendorDispositionContexts -VerifiedGeneratedBundle $verifiedGeneratedBundle -CacheIdentity $cacheIdentities -CachePaths $cachePaths -HeadCommit $headCommit -BaseCommit $baseCommit
    Write-Host "Running isolated Cppcheck analysis for $($analysisProjects.Count) target(s):"
    $preparationMilliseconds = $script:lintTotalTimer.Elapsed.TotalMilliseconds
    $fullAnalyzerTimer = [Diagnostics.Stopwatch]::StartNew()

    foreach ($project in $analysisProjects) {
        $targetName = $project.RelativeProject
        $analysisProjectContext = Get-CppcheckVendorDispositionContext `
            -ProjectPath $project.ProjectPath `
            -TargetName $targetName `
            -AnalyzerVersion $cppcheckVersion[0]
        $headContext = Get-CppcheckRegressionAnalysisContext `
            -BuildRoot $analysisBuildRoot `
            -RepositoryRoot $analysisRepositoryRoot `
            -ProjectPath $project.ProjectPath `
            -InputRootIdentities @($(if ($null -eq $inputManifestData) { 'regression-live-worktree' } else { 'regression-fixed-manifest-stage' })) `
            -AnalyzerConfigurationPaths (Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $cppcheck.Source) `
            -AnalyzerOptions @('--project-configuration=Release|x64', '--enable=warning,performance,portability')
        $headIdentity = Get-CppcheckCacheIdentity `
            -AnalyzerVersion $cppcheckVersion[0] `
            -AnalyzerSHA256 $cppcheckSHA256 `
            -InputMode $(if ($null -eq $inputManifestData) { 'normal' } else { 'input-manifest' }) `
            -Configuration $analysisProjectContext.Configuration `
            -Compiler $analysisProjectContext.Compiler `
            -Toolset $analysisProjectContext.Toolset `
            -Abi $analysisProjectContext.Abi `
            -RootIdentity $(if ($null -eq $inputManifestData) { 'regression-live-worktree' } else { 'regression-fixed-manifest-stage' }) `
            -AnalysisContext $headContext
        $cacheIdentities["head/$targetName"] = $headIdentity
        Save-CppcheckCacheIdentity -CacheRoot $cppcheckCacheRoot -Namespace 'regression' -Identity $headIdentity
        Write-Host "  $targetName"

        $headDescriptors = [System.Collections.Generic.List[object]]::new()
        foreach ($translationUnit in $project.Files) {
            $headCache = Get-CppcheckCacheLeaf -CacheRoot $cppcheckCacheRoot -Namespace 'regression' -Identity $headIdentity -Role 'head' -TargetName $targetName -TranslationUnit $translationUnit
            $cachePaths["head/$targetName/$translationUnit"] = $headCache
            $headDescriptors.Add((New-CppcheckDescriptor `
                -CppcheckPath $cppcheck.Source `
                -ProjectPath $project.ProjectPath `
                -CachePath $headCache `
                -RepositoryRoot $analysisRepositoryRoot `
                -BuildRoot $analysisBuildRoot `
                -TargetName $targetName `
                -TranslationUnit $translationUnit `
                -Index $headDescriptors.Count))
        }
        Write-Host "Cppcheck TU workers: $([Math]::Min($CppcheckJobs, $headDescriptors.Count)) ($targetName/head)"
        [void]$headDiagnostics.AddRange(@(Invoke-CppcheckBatch -Descriptors $headDescriptors.ToArray() -CppcheckJobs $CppcheckJobs -Evidence $evidence))

        $baselineProject = $baselineProjectsByTarget[$targetName]

        if ($baselineProject) {
            $baselineProjectContext = Get-CppcheckVendorDispositionContext `
                -ProjectPath $baselineProject.ProjectPath `
                -TargetName $targetName `
                -AnalyzerVersion $cppcheckVersion[0]
            $baselineCacheContext = Get-CppcheckRegressionAnalysisContext `
                -BuildRoot $baselineBuildRoot `
                -RepositoryRoot $baselineRoot `
                -ProjectPath $baselineProject.ProjectPath `
                -InputRootIdentities @($(if ($null -eq $inputManifestData) { 'regression-live-worktree' } else { 'regression-fixed-manifest-stage' })) `
                -AnalyzerConfigurationPaths (Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $cppcheck.Source) `
                -AnalyzerOptions @('--project-configuration=Release|x64', '--enable=warning,performance,portability')
            $baselineIdentity = Get-CppcheckCacheIdentity `
                -AnalyzerVersion $cppcheckVersion[0] `
                -AnalyzerSHA256 $cppcheckSHA256 `
                -InputMode $(if ($null -eq $inputManifestData) { 'normal' } else { 'input-manifest' }) `
                -Configuration $baselineProjectContext.Configuration `
                -Compiler $baselineProjectContext.Compiler `
                -Toolset $baselineProjectContext.Toolset `
                -Abi $baselineProjectContext.Abi `
                -RootIdentity $(if ($null -eq $inputManifestData) { 'regression-live-worktree' } else { 'regression-fixed-manifest-stage' }) `
                -AnalysisContext $baselineCacheContext
            $cacheIdentities["baseline/$targetName"] = $baselineIdentity
            Save-CppcheckCacheIdentity -CacheRoot $cppcheckCacheRoot -Namespace 'regression' -Identity $baselineIdentity
            $baselineDescriptors = [System.Collections.Generic.List[object]]::new()
            foreach ($translationUnit in @($baselineProject.Sources.Keys | Sort-Object)) {
                $baselineCache = Get-CppcheckCacheLeaf -CacheRoot $cppcheckCacheRoot -Namespace 'regression' -Identity $baselineIdentity -Role 'baseline' -TargetName $targetName -TranslationUnit $translationUnit
                $cachePaths["baseline/$baseCommit/$targetName/$translationUnit"] = $baselineCache
                $baselineDescriptors.Add((New-CppcheckDescriptor `
                    -CppcheckPath $cppcheck.Source `
                    -ProjectPath $baselineProject.ProjectPath `
                    -CachePath $baselineCache `
                    -RepositoryRoot $baselineRoot `
                    -BuildRoot $baselineBuildRoot `
                    -TargetName $targetName `
                    -TranslationUnit $translationUnit `
                        -Index $baselineDescriptors.Count))
            }
                    Write-Host "Cppcheck TU workers: $([Math]::Min($CppcheckJobs, $baselineDescriptors.Count)) ($targetName/baseline)"
                    [void]$baselineDiagnostics.AddRange(@(Invoke-CppcheckBatch -Descriptors $baselineDescriptors.ToArray() -CppcheckJobs $CppcheckJobs -Evidence $evidence))
        }
    }

    $baselineDiagnostics = $baselineDiagnostics.ToArray()
    $headDiagnostics = $headDiagnostics.ToArray()
    $fullAnalyzerTimer.Stop()
    $analyzerMilliseconds = $fullAnalyzerTimer.Elapsed.TotalMilliseconds
    if ($null -ne $receiptCandidate) {
        Assert-CppcheckPreparationState -Expected $receiptStartState -Actual (Get-CppcheckPreparationState -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -Projects $analysisProjects -AnalyzerVersion $cppcheckVersion[0])
        Assert-CppcheckFastLiveHead -RepositoryRoot $repositoryRoot -HeadCommit $headCommit
        Save-CppcheckPreparationReceipt -CacheRoot $cppcheckCacheRoot -BuildRoot $buildRoot -Receipt $receiptCandidate
        Write-Host 'Verified preparation receipt published (independent of diagnostic classification).'
    }
    }
    $vendorDispositionPolicy = Join-Path $PSScriptRoot 'cppcheck-vendor-dispositions.json'
    Write-Host "Raw HEAD diagnostics ($($headDiagnostics.Count)):"
    $headDiagnostics | Sort-Object Display | ForEach-Object { Write-Host "  $($_.Display)" }
    $vendorDispositionResult = Get-CppcheckVendorDispositionResult `
        -Diagnostics $headDiagnostics `
        -PolicyPath $vendorDispositionPolicy `
        -RepositoryRoot $(if ($useFast) { $repositoryRoot } else { $analysisRepositoryRoot }) `
        -Contexts $vendorDispositionContexts `
        -NormalizeTrackedTextNewlines:($null -eq $inputManifestData)
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
    $finalization = Complete-LintFinalization `
        -SaveEvidenceAction {
            Save-LintEvidenceState -Evidence $evidence -AnalysisBuildRoot $analysisBuildRoot -BaselineBuildRoot $baselineBuildRoot -TempRoot $tempRoot -AnalysisProjects $analysisProjects -BaselineProjects $baselineProjects -Contexts $vendorDispositionContexts -VerifiedGeneratedBundle $verifiedGeneratedBundle -CacheIdentity $cacheIdentities -CachePaths $cachePaths -HeadCommit $headCommit -BaseCommit $baseCommit
        } `
        -CleanupAction {
            Complete-LintTemporaryCleanup -BaselineWorktreeCreated:$baselineWorktreeCreated -BaselineRoot $baselineRoot -TempRoot $tempRoot -CppcheckCacheRoot $cppcheckCacheRoot -StageName $stageName
            $cleanupState.Completed = $true
        } `
        -SaveResultAction {
            param($status, $exitCode, $errorMessage)
            Save-LintEvidenceResult -Evidence $evidence -Status $status -ExitCode $exitCode -Error $errorMessage -VendorDispositionResult $vendorDispositionResult -Comparison $comparison
        }
    $finalResultSaved = $finalization.ResultSaved
    if (-not $finalization.Succeeded) {
        throw $finalization.ErrorMessage
    }
}
catch {
    $gateFailure = $_.Exception.Message
    throw
}
finally {
    if (-not $cleanupState.Completed) {
        try {
            Complete-LintTemporaryCleanup -BaselineWorktreeCreated:$baselineWorktreeCreated -BaselineRoot $baselineRoot -TempRoot $tempRoot -CppcheckCacheRoot $cppcheckCacheRoot -StageName $stageName
            $cleanupState.Completed = $true
        }
        catch {
            if ([string]::IsNullOrWhiteSpace($gateFailure)) {
                $gateFailure = $_.Exception.Message
            }
            else {
                Write-Warning "Could not complete lint temporary cleanup: $($_.Exception.Message)" -WarningAction Continue
            }
        }
    }

    if (-not $finalResultSaved) {
        $finalizationError = if ([string]::IsNullOrWhiteSpace($gateFailure)) { 'The gate did not reach final classification.' } else { $gateFailure }
        try {
            Save-LintEvidenceResult -Evidence $evidence -Status 'failed' -ExitCode 1 -Error $finalizationError
        }
        catch {
            Write-Warning "Could not save failed lint evidence: $($_.Exception.Message)" -WarningAction Continue
        }
    }

}

}

finally {
    if ($null -ne (Get-Variable lintTotalTimer -Scope Script -ErrorAction SilentlyContinue)) {
        $script:lintTotalTimer.Stop()
        Write-Host ("Cppcheck timing (ms): preparation={0:F3}; analyzer={1:F3}; total={2:F3}; targets={3}; selected TUs={4}." -f $preparationMilliseconds, $analyzerMilliseconds, $script:lintTotalTimer.Elapsed.TotalMilliseconds, $analysisProjects.Count, @($analysisProjects | ForEach-Object { $_.Files }).Count)
    }
    try {
        if (($null -ne $inputManifestRoots) -and ($null -eq $tempRoot) -and (Test-Path -LiteralPath $inputManifestRoots.TempRoot)) {
            Remove-CppcheckDisposableStage -CacheRoot $cppcheckCacheRoot -StageRoot $inputManifestRoots.TempRoot -Name 'regression-manifest'
        }
    }
    finally {
        Exit-CppcheckCacheLock -Lock $cacheLock
    }
}

exit 0
