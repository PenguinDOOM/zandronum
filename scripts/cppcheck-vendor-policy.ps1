Set-StrictMode -Version Latest

function Get-VendorPolicyRecordKey {
    param([object]$Record)

    return "$($Record.Target)|$($Record.TranslationUnit)|$($Record.RelativePath)|$($Record.Line)|$($Record.Column)|$($Record.Severity)|$($Record.Identifier)|$($Record.Message)"
}

function Get-VendorPolicyFileHash {
    param(
        [string]$RepositoryRoot,
        [string]$RelativePath
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or [System.IO.Path]::IsPathRooted($RelativePath) -or $RelativePath.Contains('..')) {
        throw "Vendor disposition has an invalid relative path '$RelativePath'."
    }

    $path = Join-Path $RepositoryRoot $RelativePath

    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Vendor disposition source is missing: $RelativePath"
    }

    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Assert-VendorDispositionPath {
    param([string]$RelativePath)

    if ($RelativePath -notin @(
        'src/sound/thirdparty/miniaudio/miniaudio.h',
        'src/sound/thirdparty/stb/stb_vorbis.c'
    )) {
        throw "Vendor disposition has an unsupported vendor source path '$RelativePath'."
    }
}

function Assert-VendorPolicyPreconditions {
    param(
        [object]$Disposition,
        [string]$RepositoryRoot,
        [hashtable]$Context
    )

    $required = @('Path', 'SHA256', 'Line', 'Column', 'Severity', 'Identifier', 'Message', 'Target', 'TranslationUnit', 'AnalyzerVersion', 'Reason', 'Preconditions')

    foreach ($name in $required) {
        if ($null -eq $Disposition.PSObject.Properties[$name] -or [string]::IsNullOrWhiteSpace([string]$Disposition.$name)) {
            throw "Vendor disposition is missing required '$name' data."
        }
    }

    $actualHash = Get-VendorPolicyFileHash -RepositoryRoot $RepositoryRoot -RelativePath $Disposition.Path

    if ($actualHash -ne $Disposition.SHA256) {
        throw "Vendor disposition source hash changed: $($Disposition.Path)"
    }

    if ($Context.AnalyzerVersion -ne $Disposition.AnalyzerVersion) {
        throw "Vendor disposition analyzer version changed for $($Disposition.Path)."
    }

    foreach ($property in @('Compiler', 'Toolset', 'Abi', 'Configuration', 'Defines')) {
        if ($null -eq $Disposition.Preconditions.PSObject.Properties[$property] -or $Context[$property] -ne $Disposition.Preconditions.$property) {
            throw "Vendor disposition precondition '$property' does not match for $($Disposition.Path)."
        }
    }

    if ($null -eq $Disposition.Preconditions.PSObject.Properties['ApiRoots'] -or @($Disposition.Preconditions.ApiRoots).Count -eq 0) {
        throw "Vendor disposition has no verified API roots for $($Disposition.Path)."
    }

    foreach ($apiRoot in @($Disposition.Preconditions.ApiRoots)) {
        if ($null -eq $apiRoot.PSObject.Properties['Path'] -or $null -eq $apiRoot.PSObject.Properties['SHA256']) {
            throw "Vendor disposition has a malformed API root for $($Disposition.Path)."
        }

        $apiHash = Get-VendorPolicyFileHash -RepositoryRoot $RepositoryRoot -RelativePath $apiRoot.Path

        if ($apiHash -ne $apiRoot.SHA256) {
            throw "Vendor disposition API root hash changed: $($apiRoot.Path)"
        }
    }

    if ($null -eq $Disposition.Preconditions.PSObject.Properties['ApiTokens'] -or @($Disposition.Preconditions.ApiTokens).Count -eq 0) {
        throw "Vendor disposition has no API token coverage for $($Disposition.Path)."
    }

    $apiNamespaces = @($Disposition.Preconditions.ApiTokens | ForEach-Object {
        if ($_ -match '^ma_') {
            return 'ma_'
        }

        if ($_ -match '^stb_vorbis_') {
            return 'stb_vorbis_'
        }

        throw "Vendor disposition API token has no supported library namespace: $_"
    } | Sort-Object -Unique)
    $expectedApiTokens = @($Disposition.Preconditions.ApiTokens | Sort-Object -Unique)
    $expectedApiRoots = @($Disposition.Preconditions.ApiRoots | ForEach-Object { $_.Path } | Sort-Object -Unique)
    $apiConsumers = @(
        @('src', 'tools') | ForEach-Object { Join-Path $RepositoryRoot $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Recurse -File -Include '*.c', '*.cc', '*.cpp', '*.cxx', '*.h', '*.hh', '*.hpp', '*.hxx' } |
        Where-Object { $_.FullName.Replace('\', '/') -notmatch '/thirdparty/' } |
        Where-Object {
            $source = Get-Content -LiteralPath $_.FullName -Raw
            @($apiNamespaces | Where-Object { $source -match ('\b' + [regex]::Escape($_) + '[A-Za-z0-9_]*\b') }).Count -ne 0
        } |
        ForEach-Object {
            [PSCustomObject]@{
                Path = $_.FullName.Substring($RepositoryRoot.TrimEnd('\', '/').Length + 1).Replace('\', '/')
                Source = Get-Content -LiteralPath $_.FullName -Raw
            }
        }
    )

    $actualApiRoots = @($apiConsumers | ForEach-Object { $_.Path } | Sort-Object -Unique)
    $actualApiTokens = @($apiConsumers | ForEach-Object {
        foreach ($namespace in $apiNamespaces) {
            [regex]::Matches($_.Source, ('\b' + [regex]::Escape($namespace) + '[A-Za-z0-9_]*\b')) | ForEach-Object { $_.Value }
        }
    } | Sort-Object -Unique)

    if (@(Compare-Object -ReferenceObject $expectedApiRoots -DifferenceObject $actualApiRoots).Count -ne 0) {
        throw "Vendor disposition API roots changed for $($Disposition.Path). Expected: $($expectedApiRoots -join ', '). Actual: $($actualApiRoots -join ', ')."
    }

    if (@(Compare-Object -ReferenceObject $expectedApiTokens -DifferenceObject $actualApiTokens).Count -ne 0) {
        throw "Vendor disposition API tokens changed for $($Disposition.Path)."
    }
}

function ConvertFrom-CppcheckProjectOutput {
    param(
        [string[]]$Output,
        [int]$ExitCode,
        [string]$RepositoryRoot,
        [string]$BuildRoot,
        [string]$TargetName,
        [string]$TranslationUnit,
        [string[]]$AllowedMissingFiles = @()
    )

    $diagnostics = @()
    $unexpectedOutput = @()

    foreach ($line in $Output) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $match = [regex]::Match($line, '^(?<file>.+?)\t(?<line>\d+)\t(?<column>\d+)\t(?<severity>[^\t]+)\t(?<id>[^\t]+)\t(?<message>.*)$')

        if (-not $match.Success) {
            $unexpectedOutput += $line
            continue
        }

        $relativePath = Get-RepositoryRelativePath -Path $match.Groups['file'].Value -RepositoryRoot $RepositoryRoot -BuildRoot $BuildRoot
        $severity = $match.Groups['severity'].Value
        $identifier = $match.Groups['id'].Value
        $message = $match.Groups['message'].Value
        $diagnostics += [PSCustomObject]@{
            Fingerprint = "$TargetName|$relativePath|$severity|$identifier|$message"
            Display = "${TargetName}: ${TranslationUnit}: ${relativePath}:$($match.Groups['line'].Value):$($match.Groups['column'].Value) [$severity/$identifier] $message"
            Target = $TargetName
            TranslationUnit = $TranslationUnit
            RelativePath = $relativePath
            Line = [int]$match.Groups['line'].Value
            Column = [int]$match.Groups['column'].Value
            Severity = $severity
            Identifier = $identifier
            Message = $message
        }
    }

    $missingFileDiagnostics = @($diagnostics | Where-Object { $_.Identifier -eq 'missingFile' })

    if ($missingFileDiagnostics.Count -ne 0) {
        $missingPaths = @($missingFileDiagnostics | ForEach-Object { $_.RelativePath } | Sort-Object -Unique)
        $unsupportedPaths = @($missingPaths | Where-Object { $_ -notin $AllowedMissingFiles })

        if ($unsupportedPaths.Count -ne 0) {
            throw "Cppcheck reported unsupported missing input(s): $($unsupportedPaths -join ', ')."
        }

        throw "Cppcheck reported missing verified generated input(s): $($missingPaths -join ', ')."
    }

    if (($ExitCode -ne 0) -and ($ExitCode -ne 1)) {
        $details = ($unexpectedOutput -join [Environment]::NewLine).Replace($RepositoryRoot, '<repo>')
        throw "Cppcheck exited unexpectedly for '$TargetName' with code ${ExitCode}: $details"
    }

    if (($ExitCode -ne 0) -and ($diagnostics.Count -eq 0)) {
        $details = ($unexpectedOutput -join [Environment]::NewLine).Replace($RepositoryRoot, '<repo>')
        throw "Cppcheck failed before producing diagnostics for '$TargetName': $details"
    }

    $fatalOutput = @($unexpectedOutput | Where-Object { $_ -match '(?i)(^|:\s*)(fatal )?error:' })

    if ($fatalOutput.Count -ne 0) {
        $details = ($fatalOutput -join [Environment]::NewLine).Replace($RepositoryRoot, '<repo>')
        throw "Cppcheck reported a tool or configuration error for '$TargetName': $details"
    }

    return $diagnostics
}

function Get-CppcheckVendorDispositionResult {
    param(
        [object[]]$Diagnostics,
        [string]$PolicyPath,
        [string]$RepositoryRoot,
        [hashtable]$Contexts
    )

    if (-not (Test-Path -LiteralPath $PolicyPath -PathType Leaf)) {
        throw "Vendor disposition policy is missing: $PolicyPath"
    }

    try {
        $policy = Get-Content -LiteralPath $PolicyPath -Raw | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Vendor disposition policy is malformed: $($_.Exception.Message)"
    }

    if ($null -eq $policy.PSObject.Properties['SchemaVersion'] -or $policy.SchemaVersion -ne 1 -or $null -eq $policy.PSObject.Properties['Dispositions']) {
        throw 'Vendor disposition policy has an unsupported schema.'
    }

    $dispositionsByKey = @{}

    foreach ($disposition in @($policy.Dispositions)) {
        $key = "$($disposition.Target)|$($disposition.TranslationUnit)|$($disposition.Path)|$($disposition.Line)|$($disposition.Column)|$($disposition.Severity)|$($disposition.Identifier)|$($disposition.Message)"

        if ($dispositionsByKey.ContainsKey($key)) {
            throw "Vendor disposition policy contains a duplicate record: $key"
        }

        $dispositionsByKey[$key] = $disposition
    }

    $accepted = @()
    $unaccepted = @()

    foreach ($diagnostic in $Diagnostics) {
        $key = Get-VendorPolicyRecordKey -Record $diagnostic

        if ($dispositionsByKey.ContainsKey($key)) {
            $disposition = $dispositionsByKey[$key]

            Assert-VendorDispositionPath -RelativePath $disposition.Path

            if (-not $Contexts.ContainsKey($diagnostic.Target)) {
                throw "Vendor disposition target context is missing: $($diagnostic.Target)"
            }

            Assert-VendorPolicyPreconditions -Disposition $disposition -RepositoryRoot $RepositoryRoot -Context $Contexts[$diagnostic.Target]
            $accepted += [PSCustomObject]@{ Diagnostic = $diagnostic; Disposition = $disposition }
        }
        else {
            $unaccepted += $diagnostic
        }
    }

    return [PSCustomObject]@{
        Raw = @($Diagnostics)
        Accepted = $accepted
        Unaccepted = $unaccepted
    }
}

function Test-VendorDispositionPath {
    param([string]$RelativePath)

    return $RelativePath -in @(
        'src/sound/thirdparty/miniaudio/miniaudio.h',
        'src/sound/thirdparty/stb/stb_vorbis.c'
    )
}

function Get-CppcheckBaselineComparisonResult {
    param(
        [object[]]$BaselineDiagnostics,
        [object[]]$HeadDiagnostics,
        [object[]]$UnacceptedDiagnostics
    )

    $unresolvedVendor = @($UnacceptedDiagnostics | Where-Object { Test-VendorDispositionPath -RelativePath $_.RelativePath })
    $baselineByFingerprint = @{}
    $headByFingerprint = @{}

    foreach ($diagnostic in $BaselineDiagnostics) { $baselineByFingerprint[$diagnostic.Fingerprint] = $diagnostic }
    foreach ($diagnostic in $UnacceptedDiagnostics) { $headByFingerprint[$diagnostic.Fingerprint] = $diagnostic }

    return [PSCustomObject]@{
        UnresolvedVendor = $unresolvedVendor
        Baseline = @($baselineByFingerprint.Values)
        Unchanged = @($headByFingerprint.Keys | Where-Object { $baselineByFingerprint.ContainsKey($_) } | Sort-Object | ForEach-Object { $headByFingerprint[$_] })
        New = @($headByFingerprint.Keys | Where-Object { -not $baselineByFingerprint.ContainsKey($_) } | Sort-Object | ForEach-Object { $headByFingerprint[$_] })
        BaselineOnly = @($baselineByFingerprint.Keys | Where-Object { -not $headByFingerprint.ContainsKey($_) } | Sort-Object | ForEach-Object { $baselineByFingerprint[$_] })
    }
}

function Get-CppcheckVendorDispositionContext {
    param(
        [string]$ProjectPath,
        [string]$TargetName,
        [string]$AnalyzerVersion
    )

    $configuration = 'Release|x64'
    $condition = "'`$(Configuration)|`$(Platform)'=='$configuration'"
    $configurationProperty = "'`$(Configuration)|`$(Platform)'"
    $configurationConditionPattern = '^' + [regex]::Escape($configurationProperty) + "=='([^']+)'$"
    $project = New-Object System.Xml.XmlDocument
    $project.Load($ProjectPath)

    $allowedImports = @(
        '$(VCTargetsPath)\Microsoft.Cpp.Default.props',
        '$(VCTargetsPath)\Microsoft.Cpp.props',
        '$(VCTargetsPath)\Microsoft.Cpp.targets',
        'do_not_import_user.props'
    )

    foreach ($import in @($project.SelectNodes('//*[local-name()="Import"]'))) {
        $importPath = $import.GetAttribute('Project').Trim()

        if ($importPath -notin $allowedImports) {
            throw "Could not prove Cppcheck disposition context from '$TargetName': unsupported import '$importPath'."
        }

        if (($importPath -eq 'do_not_import_user.props') -and (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $ProjectPath) $importPath))) {
            throw "Could not prove Cppcheck disposition context from '$TargetName': user configuration import."
        }
    }

    $compilerOverrides = @($project.SelectNodes("//*[local-name()='PropertyGroup']/*[local-name()='CLToolExe' or local-name()='CLToolPath' or local-name()='CLToolArchitecture']"))
    if ($compilerOverrides.Count -ne 0) {
        throw "Could not prove Cppcheck disposition context from '$TargetName': unsupported compiler override."
    }

    foreach ($configurationNode in @($project.SelectNodes("//*[local-name()='PropertyGroup' or local-name()='ItemDefinitionGroup']"))) {
        $configurationCondition = $configurationNode.GetAttribute('Condition').Trim()
        $normalizedConfigurationCondition = $configurationCondition -replace '\s+', ''
        $isReleaseConfiguration = $normalizedConfigurationCondition -eq $condition
        $isOtherConfiguration = ($normalizedConfigurationCondition -match $configurationConditionPattern) -and ($Matches[1] -ne $configuration)
        $changesContext = @($configurationNode.SelectNodes(".//*[local-name()='PlatformToolset' or local-name()='Compiler' or local-name()='PreprocessorDefinitions' or local-name()='AdditionalOptions' or local-name()='ForcedIncludeFiles' or local-name()='AdditionalIncludeDirectories']")).Count -ne 0
        $invalidPlatform = @($configurationNode.SelectNodes(".//*[local-name()='Platform']") | Where-Object { ($configurationCondition -ne '') -or ($_.InnerText.Trim() -ne 'x64') }).Count -ne 0

        if (($changesContext -or $invalidPlatform) -and -not $isReleaseConfiguration -and -not $isOtherConfiguration) {
            throw "Could not prove Cppcheck disposition context from '$TargetName': unsupported conditional configuration."
        }
    }

    $releasePropertyGroups = @($project.SelectNodes("//*[local-name()='PropertyGroup']") | Where-Object { ($_.GetAttribute('Condition').Trim() -replace '\s+', '') -eq $condition })
    $releaseItemDefinitionGroups = @($project.SelectNodes("//*[local-name()='ItemDefinitionGroup']") | Where-Object { ($_.GetAttribute('Condition').Trim() -replace '\s+', '') -eq $condition })
    $toolsets = @($releasePropertyGroups | ForEach-Object { $_.SelectNodes("*[local-name()='PlatformToolset']") } | Where-Object { $null -ne $_ } | ForEach-Object { $_.InnerText.Trim() })
    $defines = @($releaseItemDefinitionGroups | ForEach-Object { $_.SelectNodes("*[local-name()='ClCompile']/*[local-name()='PreprocessorDefinitions']") } | Where-Object { $null -ne $_ } | ForEach-Object { $_.InnerText.Trim() } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $unsupportedCompilerInputs = @($releaseItemDefinitionGroups | ForEach-Object { $_.SelectNodes("*[local-name()='ClCompile']/*[local-name()='AdditionalOptions' or local-name()='ForcedIncludeFiles']") } | Where-Object { -not [string]::IsNullOrWhiteSpace($_.InnerText) })
    $tuOverrides = @($project.SelectNodes("//*[local-name()='ItemGroup']/*[local-name()='ClCompile']/*[local-name()='PreprocessorDefinitions' or local-name()='AdditionalOptions' or local-name()='ForcedIncludeFiles' or local-name()='AdditionalIncludeDirectories']"))

    if (($toolsets.Count -ne 1) -or [string]::IsNullOrWhiteSpace($toolsets[0]) -or ($defines.Count -ne 1) -or ($unsupportedCompilerInputs.Count -ne 0) -or ($tuOverrides.Count -ne 0)) {
        throw "Could not prove Cppcheck disposition context from '$TargetName' for $configuration."
    }

    return @{
        AnalyzerVersion = $AnalyzerVersion
        Compiler = 'MSVC'
        Toolset = $toolsets[0]
        Abi = 'x64'
        Configuration = $configuration
        Defines = $defines[0]
    }
}