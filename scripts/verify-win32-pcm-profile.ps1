param(
    [Parameter(Mandatory = $true)][string]$WorkspaceRoot,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$Configuration,
    [Parameter(Mandatory = $true)][string]$Architecture,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)

$ErrorActionPreference = 'Stop'
$schema = 'phase2-win32-pcm-profile-v2'
$profileToken = 'msvc-194435229-win32-ia32-fast-release-v1'
$expectedInputs = [ordered]@{
    'tools/testdata/audio/float32_mono.wav' = '314ab5655cea7c25d06c755d631bc9b0a282b0cc2980dc4dcfa4c2452bcc4266'
    'tools/testdata/audio/mp3_mono.mp3' = 'aa63ffa0d4e18fcf9b3b1c314fd7f0e192620596cd65009d965911ca150b3363'
    'src/sound/thirdparty/miniaudio/miniaudio.h' = 'ed718e371508c2c802eb2e6b1495eec42d36a86495f5605595ae2e3a5076d793'
}
$expectedDecoderHash = 'cdccfbec43bc9d5a536cb0c88dc3f87126c9d25bdd2e9c66a6c59daa1685a4fb'
$decisionPath = Join-Path $EvidenceDirectory 'pcm-reference-profile.json'

function Write-Decision {
    param([string]$Decision, [string]$Reason, $Proof, [int]$ExitCode)
    $result = [ordered]@{
        schema = $schema
        decision = $Decision
        reason = $Reason
        architecture = $Architecture
        configuration = $Configuration
        profile = if ($Decision -eq 'accepted') { $profileToken } else { $null }
        evidence = $Proof
    }
    try {
        if (Test-Path -LiteralPath $decisionPath) { throw "Decision path is not fresh: $decisionPath" }
        New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
        $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $decisionPath -Encoding utf8 -NoNewline
    } catch {
        Write-Error "Unable to save PCM profile decision: $($_.Exception.Message)"
        exit 2
    }
    exit $ExitCode
}

function Normalize-RecordedPath {
    param([string]$Path)
    return ($Path.Replace('/', '\').TrimEnd('\')).ToUpperInvariant()
}

function Get-RecordedRelativePath {
    param([string]$Path, [string]$RelativePath)
    $normalizedPath = Normalize-RecordedPath $Path
    $normalizedRelativePath = Normalize-RecordedPath $RelativePath
    if (!$normalizedPath.EndsWith("\$normalizedRelativePath", [StringComparison]::Ordinal)) { return $null }
    return [ordered]@{
        root = $normalizedPath.Substring(0, $normalizedPath.Length - $normalizedRelativePath.Length).TrimEnd('\')
        relative_path = $normalizedRelativePath
    }
}

function Split-WindowsCommandLine {
    param([string]$Command)
    $arguments = New-Object 'System.Collections.Generic.List[string]'
    $index = 0
    while ($index -lt $Command.Length) {
        while ($index -lt $Command.Length -and [char]::IsWhiteSpace($Command[$index])) { $index++ }
        if ($index -ge $Command.Length) { break }
        $value = New-Object Text.StringBuilder
        $quoted = $false
        while ($index -lt $Command.Length) {
            $slashes = 0
            while ($index -lt $Command.Length -and $Command[$index] -eq '\') { $slashes++; $index++ }
            if ($index -lt $Command.Length -and $Command[$index] -eq '"') {
                [void]$value.Append('\', ($slashes -shr 1))
                if (($slashes % 2) -eq 0) { $quoted = -not $quoted } else { [void]$value.Append('"') }
                $index++
            } else {
                [void]$value.Append('\', $slashes)
                if ($index -ge $Command.Length -or (!$quoted -and [char]::IsWhiteSpace($Command[$index]))) { break }
                [void]$value.Append($Command[$index]); $index++
            }
        }
        $arguments.Add($value.ToString())
    }
    return @($arguments)
}

function Get-Definitions {
    param([string[]]$Arguments)
    $definitions = @()
    for ($index = 0; $index -lt $Arguments.Count; $index++) {
        if ($Arguments[$index] -ceq '/D') {
            if (++$index -ge $Arguments.Count) { throw 'Dangling /D switch.' }
            $definitions += $Arguments[$index]
        } elseif ($Arguments[$index] -cmatch '^/D(.+)$') {
            $definitions += $Matches[1]
        }
    }
    return @($definitions)
}

function Assert-ApprovedCommand {
    param([hashtable]$Record, [string]$ExpectedSource, [string]$ExpectedRoot)
    $arguments = @(Split-WindowsCommandLine $Record.command)
    if ($arguments.Count -eq 0 -or @($arguments | Where-Object { $_.StartsWith('@') -or $_.StartsWith('-') }).Count -ne 0) { throw 'Empty, response-file, or dash-form command rejected.' }
    $required = @('/c', '/Zi', '/nologo', '/W3', '/WX-', '/diagnostics:column', '/O2', '/Ob2', '/Oi', '/Oy', '/GF', '/EHsc', '/MT', '/GS', '/Gy', '/arch:IA32', '/fp:fast', '/Zc:wchar_t', '/Zc:forScope', '/Zc:inline', '/GR-', '/external:W3', '/Gd', '/TP', '/wd4996', '/analyze-')
    foreach ($flag in $required) { if ($arguments -cnotcontains $flag) { throw "Missing required compiler flag $flag." } }
    foreach ($argument in $arguments) {
        if ($argument -cmatch '^/(Oi-|fp:(?!fast$)|O(d|1|x|g)$|Ob(0|1|3)$|arch:(?!IA32$)|D_DEBUG($|=)|D(_MSC|_M_|__AVX|__SSE|_M_FP_FAST|AUDIO_DECODER_PCM_REFERENCE_PROFILE))') { throw "Conflicting compiler option: $argument" }
        if ($argument -cmatch '^/' -and $argument -cnotmatch '^/(c|Zi|nologo|W3|WX-|diagnostics:column|O2|Ob2|Oi|Oy|GF|EHsc|MT|GS|Gy|arch:IA32|fp:fast|Zc:(wchar_t|forScope|inline)|GR-|Fo.+|Fd.+|external:W3|Gd|TP|wd4996|analyze-|D.*|I.*)$') { throw "Unapproved compiler option: $argument" }
    }
    $definitions = @(Get-Definitions $arguments)
    $allowed = '^(?:_MBCS|WIN32|_WINDOWS|NDEBUG|AUDIO_DECODER_TESTING|AUDIO_DECODER_TESTDATA_DIR=.*|CMAKE_INTDIR=\\?"?Release\\?"?)$'
    if (@($definitions | Where-Object { $_ -cnotmatch $allowed }).Count -ne 0) { throw 'Unapproved preprocessor definition.' }
    foreach ($definition in @('WIN32', '_WINDOWS', 'NDEBUG', 'AUDIO_DECODER_TESTING')) { if ($definitions -cnotcontains $definition) { throw "Missing required definition $definition." } }
    if (@($definitions | Where-Object { $_ -cmatch '^CMAKE_INTDIR=\\?"?Release\\?"?$' }).Count -ne 1) { throw 'Missing Release CMAKE_INTDIR definition.' }
    $matchingSource = @($arguments | Where-Object { $_ -cnotmatch '^/' -and (Normalize-RecordedPath $_) -ceq (Normalize-RecordedPath $ExpectedSource) })
    if ($matchingSource.Count -ne 1) { throw "Command does not name exactly $ExpectedSource." }
    $commandSource = Get-RecordedRelativePath $matchingSource[0] (Get-RecordedRelativePath $ExpectedSource $Record.relative_path).relative_path
    if ($null -eq $commandSource -or $commandSource.root -cne $ExpectedRoot) { throw 'Command source uses a conflicting logical source root.' }
    return [ordered]@{ source = $Record.source; command = $Record.command; arguments = $arguments }
}

function Get-ProjectConditionScope {
    param([string]$Condition, [string]$TargetCondition)
    if ([string]::IsNullOrWhiteSpace($Condition) -or $Condition -ceq $TargetCondition) { return 'target' }
    if ($Condition -cmatch "^'\$\(Configuration\)\|\$\(Platform\)'=='Debug\|(Win32|x64)'$") { return 'debug' }
    throw "Unrecognized or ambiguous MSBuild condition: $Condition"
}

function Test-CanonicalProjectConfigurationCondition {
    param([string]$Condition)
    return $Condition -cmatch "^'\$\(Configuration\)\|\$\(Platform\)'=='(Debug|Release|MinSizeRel|RelWithDebInfo)\|Win32'$"
}

function Assert-ExpectedClCompileProperty {
    param([System.Xml.XmlElement]$Element, [hashtable]$ExpectedProperties, [string]$Context, [bool]$RejectUnrecognized)
    $name = $Element.LocalName
    if ($name -ceq 'AdditionalOptions') {
        if (![string]::IsNullOrWhiteSpace($Element.InnerText)) { throw "$Context AdditionalOptions is not empty." }
        return
    }
    if (!$ExpectedProperties.Contains($name)) {
        if ($RejectUnrecognized) { throw "$Context has an unapproved ClCompile property: $name." }
        return
    }
    if ($Element.InnerText -cne $ExpectedProperties[$name]) { throw "$Context $name is not $($ExpectedProperties[$name])." }
}

function Assert-TargetClCompileMetadata {
    param([System.Xml.XmlElement]$ClCompile, [hashtable]$ExpectedProperties, [string]$TargetCondition, [string]$Context, [bool]$RejectUnrecognized)
    $applicableProperties = @{}
    foreach ($child in @($ClCompile.ChildNodes | Where-Object { $_ -is [System.Xml.XmlElement] })) {
        $scope = Get-ProjectConditionScope $child.GetAttribute('Condition') $TargetCondition
        if ($scope -eq 'target') {
            if ($RejectUnrecognized -and $applicableProperties.ContainsKey($child.LocalName)) { throw "$Context has duplicate applicable $($child.LocalName) metadata." }
            $applicableProperties[$child.LocalName] = $true
            Assert-ExpectedClCompileProperty $child $ExpectedProperties $Context $RejectUnrecognized
        }
    }
}

if ($Configuration -cne 'Release') { Write-Decision 'rejected' 'configuration_not_release' $null 1 }
if ($Architecture -ceq 'x64') { Write-Decision 'not_applicable' 'x64_release_profile_not_applicable' $null 0 }
if ($Architecture -cne 'Win32') { Write-Decision 'rejected' 'architecture_not_win32_or_x64' $null 1 }

try {
    $workspace = [IO.Path]::GetFullPath($WorkspaceRoot)
    $build = [IO.Path]::GetFullPath($BuildDirectory)
    $projectCandidates = @(Get-ChildItem -LiteralPath $build -Recurse -File -Filter 'audio_decoder_tests.vcxproj')
    $tlogCandidates = @(Get-ChildItem -LiteralPath $build -Recurse -File -Filter 'CL.command.1.tlog' | Where-Object { $_.FullName -match 'audio_decoder_tests\.dir[\\/]Release' })
    $binaryCandidates = @(Get-ChildItem -LiteralPath (Join-Path $build 'tools\Release') -File -Filter 'audio_decoder_tests.exe')
    if ($projectCandidates.Count -ne 1 -or $tlogCandidates.Count -ne 1 -or $binaryCandidates.Count -ne 1) { throw 'audio_decoder_tests project, Release tlog, or binary is missing or ambiguous.' }
    [xml]$projectXml = Get-Content -LiteralPath $projectCandidates[0].FullName -Raw
    $namespace = New-Object Xml.XmlNamespaceManager $projectXml.NameTable
    $namespace.AddNamespace('msb', 'http://schemas.microsoft.com/developer/msbuild/2003')
    $condition = "'`$(Configuration)|`$(Platform)'=='Release|Win32'"
    $configurationNodes = @($projectXml.SelectNodes("/msb:Project/msb:PropertyGroup[@Label='Configuration']", $namespace))
    $targetConfigurationNodes = @()
    foreach ($node in $configurationNodes) {
        $nodeCondition = $node.GetAttribute('Condition')
        if (!(Test-CanonicalProjectConfigurationCondition $nodeCondition)) { throw "Unrecognized or ambiguous project configuration condition: $nodeCondition" }
        if ($nodeCondition -ceq $condition) { $targetConfigurationNodes += $node }
    }
    if ($targetConfigurationNodes.Count -ne 1 -or $targetConfigurationNodes[0].PlatformToolset -cne 'v143') { throw 'Release|Win32 v143 project configuration is missing or ambiguous.' }
    $expectedRelativeSources = @('tools\audio_decoder_tests.cpp', 'src\sound\audio_decoder_miniaudio.cpp')
    $projectIncludes = @($projectXml.SelectNodes('//msb:ClCompile[@Include]', $namespace) | ForEach-Object { $_.Include })
    $projectRoot = $null
    foreach ($relativeSource in $expectedRelativeSources) {
        $matches = @($projectIncludes | ForEach-Object { Get-RecordedRelativePath $_ $relativeSource } | Where-Object { $null -ne $_ })
        if ($matches.Count -ne 1) { throw "Project source is missing or ambiguous: $relativeSource" }
        if ($null -eq $projectRoot) { $projectRoot = $matches[0].root } elseif ($projectRoot -cne $matches[0].root) { throw 'Project sources use conflicting logical source roots.' }
    }
    $targetItemDefinitionGroups = @()
    foreach ($node in @($projectXml.SelectNodes('/msb:Project/msb:ItemDefinitionGroup', $namespace))) {
        $nodeCondition = $node.GetAttribute('Condition')
        if (!(Test-CanonicalProjectConfigurationCondition $nodeCondition)) { throw "Unrecognized or ambiguous ItemDefinitionGroup condition: $nodeCondition" }
        if ($nodeCondition -ceq $condition) { $targetItemDefinitionGroups += $node }
    }
    if ($targetItemDefinitionGroups.Count -ne 1) { throw 'Release|Win32 ItemDefinitionGroup is missing or ambiguous.' }
    $releaseClCompiles = @($targetItemDefinitionGroups[0].SelectNodes('msb:ClCompile', $namespace))
    if ($releaseClCompiles.Count -ne 1) { throw 'Release|Win32 ClCompile metadata is missing or ambiguous.' }
    $releaseClCompile = $releaseClCompiles[0]
    $expectedProperties = [ordered]@{ Optimization = 'MaxSpeed'; FloatingPointModel = 'Fast'; EnableEnhancedInstructionSet = 'NoExtensions'; RuntimeLibrary = 'MultiThreaded'; OmitFramePointers = 'true'; IntrinsicFunctions = 'true'; PrecompiledHeader = 'NotUsing'; RuntimeTypeInfo = 'false' }
    foreach ($property in $expectedProperties.GetEnumerator()) {
        $properties = @($releaseClCompile.SelectNodes("msb:$($property.Key)", $namespace))
        if ($properties.Count -ne 1 -or $properties[0].InnerText -cne $property.Value) { throw "Release|Win32 ClCompile $($property.Key) is not $($property.Value)." }
    }
    Assert-TargetClCompileMetadata $releaseClCompile $expectedProperties $condition 'Release|Win32 ClCompile' $false
    foreach ($relativeSource in $expectedRelativeSources) {
        $sourceEntries = @($projectXml.SelectNodes('//msb:ClCompile[@Include]', $namespace) | Where-Object {
            $candidate = Get-RecordedRelativePath $_.Include $relativeSource
            $null -ne $candidate
        })
        if ($sourceEntries.Count -ne 1) { throw "Project source entry is missing or ambiguous: $relativeSource" }
        Assert-TargetClCompileMetadata $sourceEntries[0] $expectedProperties $condition "Source ClCompile $relativeSource" $true
    }
    $projectDefinitions = @($releaseClCompile.PreprocessorDefinitions -split ';' | Where-Object { $_ -and $_ -cne '%(PreprocessorDefinitions)' })
    if (@($projectDefinitions | Where-Object { $_ -cmatch '^(?:_DEBUG|_MSC|_M_|__AVX|__SSE|_M_FP_FAST|AUDIO_DECODER_PCM_REFERENCE_PROFILE)' }).Count -ne 0) { throw 'Release|Win32 ClCompile defines a conflicting profile macro.' }
    $lines = @(Get-Content -LiteralPath $tlogCandidates[0].FullName -Encoding Unicode)
    $records = @{}
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if (!$lines[$index].StartsWith('^')) { continue }
        if ($index + 1 -ge $lines.Count -or [string]::IsNullOrWhiteSpace($lines[$index + 1]) -or $lines[$index + 1].StartsWith('^')) { throw 'Malformed tlog source record.' }
        $source = $lines[$index].Substring(1)
        $key = Normalize-RecordedPath $source
        if ($records.ContainsKey($key)) { throw "Duplicate tlog source record: $source" }
        $records[$key] = @{ source = $source; command = $lines[$index + 1] }
        $index++
    }
    $approved = @()
    $recordedRoot = $null
    foreach ($relativeSource in $expectedRelativeSources) {
        $matches = @($records.Keys | ForEach-Object { Get-RecordedRelativePath $_ $relativeSource } | Where-Object { $null -ne $_ })
        if ($matches.Count -ne 1) { throw "Missing or ambiguous tlog record: $relativeSource" }
        $recordKey = Normalize-RecordedPath ($matches[0].root + '\' + $matches[0].relative_path)
        if (!$records.ContainsKey($recordKey)) { throw "Missing tlog source record: $relativeSource" }
        $record = $records[$recordKey]
        $logicalSource = $record.source
        $root = $matches[0].root
        if ($null -eq $recordedRoot) { $recordedRoot = $root } elseif ($recordedRoot -cne $root) { throw 'Tlog records use conflicting logical source roots.' }
        $record['relative_path'] = $relativeSource
        $approved += Assert-ApprovedCommand $record $logicalSource $root
    }
    if ($projectRoot -cne $recordedRoot) { throw 'Project and tlog sources use conflicting logical source roots.' }
    $inputHashes = [ordered]@{}
    foreach ($entry in $expectedInputs.GetEnumerator()) {
        $path = Join-Path $workspace $entry.Key
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -cne $entry.Value) { throw "Pinned input hash mismatch: $($entry.Key)" }
        $inputHashes[$entry.Key] = $hash
    }
    $decoderPath = Join-Path $workspace 'src\sound\audio_decoder_miniaudio.cpp'
    $decoderText = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($decoderPath)).Replace("`r`n", "`n")
    $decoderHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.UTF8Encoding]::new($false).GetBytes($decoderText))).ToLowerInvariant()
    if ($decoderHash -cne $expectedDecoderHash) { throw 'Decoder CRLF-to-LF SHA-256 mismatch.' }
    $proof = [ordered]@{ project = $projectCandidates[0].FullName; tlog = $tlogCandidates[0].FullName; binary = [ordered]@{ path = $binaryCandidates[0].FullName; sha256 = (Get-FileHash -LiteralPath $binaryCandidates[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant() }; recorded_logical_root = $recordedRoot; matched_records = @($approved); decoder_crlf_to_lf_sha256 = $decoderHash; inputs = $inputHashes }
    Write-Decision 'accepted' 'win32_release_profile_proven' $proof 0
} catch {
    Write-Decision 'rejected' $_.Exception.Message $null 1
}
