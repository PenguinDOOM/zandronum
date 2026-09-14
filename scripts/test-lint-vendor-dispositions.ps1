Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RepositoryRelativePath {
    param(
        [string]$Path,
        [string]$RepositoryRoot,
        [string]$BuildRoot = ''
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')

    if ($fullPath.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $fullPath.Substring($root.Length + 1).Replace('\', '/')
    }

    return $fullPath.Replace('\', '/')
}

function Assert-ExpectedException {
    param(
        [scriptblock]$Action,
        [string]$Expected
    )

    try {
        & $Action
        throw "Expected exception was not thrown: $Expected"
    }
    catch {
        if ($_.Exception.Message -eq "Expected exception was not thrown: $Expected") { throw }
        if ($_.Exception.Message -notmatch [regex]::Escape($Expected)) { throw }
    }
}

. (Join-Path $PSScriptRoot 'cppcheck-vendor-policy.ps1')

$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('zandronum-vendor-policy-' + [guid]::NewGuid().ToString('N'))

try {
    $vendorPath = 'src/sound/thirdparty/miniaudio/miniaudio.h'
    $adapterPath = 'src/sound/audio_decoder_miniaudio.cpp'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $fixtureRoot $vendorPath)) | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $fixtureRoot $adapterPath)) | Out-Null
    Set-Content -LiteralPath (Join-Path $fixtureRoot $vendorPath) -Value 'vendor' -NoNewline
    Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'ma_decoder_init_memory()' -NoNewline
    $vendorHash = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $vendorPath
    $adapterHash = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath
    $context = @{ AnalyzerVersion = 'Cppcheck 2.21.0'; Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'Release' }
    $contexts = @{ zdoom = $context }
    $diagnostic = [PSCustomObject]@{ Target = 'zdoom'; TranslationUnit = $adapterPath; RelativePath = $vendorPath; Line = 10; Column = 4; Severity = 'warning'; Identifier = 'id'; Message = 'message' }
    $policyPath = Join-Path $fixtureRoot 'policy.json'
    $record = @{ Path = $vendorPath; SHA256 = $vendorHash; Line = 10; Column = 4; Severity = 'warning'; Identifier = 'id'; Message = 'message'; Target = 'zdoom'; TranslationUnit = $adapterPath; AnalyzerVersion = 'Cppcheck 2.21.0'; Reason = 'fixture'; Preconditions = @{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'Release'; ApiTokens = @('ma_decoder_init_memory'); ApiRoots = @(@{ Path = $adapterPath; SHA256 = $adapterHash }) } }
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline

    if ((Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts).Accepted.Count -ne 1) { throw 'Exact disposition was not accepted.' }

    $baselineDiagnostic = ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t10`t4`twarning`tid`tmessage") -ExitCode 0 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit $adapterPath
    $otherTranslationUnitDiagnostic = ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t10`t4`twarning`tid`tmessage") -ExitCode 0 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit 'src/other.cpp'
    if ($baselineDiagnostic.Fingerprint -ne $otherTranslationUnitDiagnostic.Fingerprint) { throw 'Baseline fingerprint changed when only the translation unit changed.' }
    if ((Get-CppcheckVendorDispositionResult -Diagnostics @($otherTranslationUnitDiagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts).Accepted.Count -ne 0) { throw 'Disposition accepted a different translation unit.' }
    $movedVendorDiagnostic = ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t11`t4`twarning`tid`tmessage") -ExitCode 1 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit $adapterPath
    $movedComparison = Get-CppcheckBaselineComparisonResult -BaselineDiagnostics @($baselineDiagnostic) -HeadDiagnostics @($movedVendorDiagnostic) -UnacceptedDiagnostics @($movedVendorDiagnostic)
    if (($movedComparison.UnresolvedVendor.Count -ne 1) -or ($movedComparison.New.Count -ne 0)) { throw 'Moved unproven vendor diagnostic was not separated from legacy baseline comparison.' }

    foreach ($mutation in @(@{ Line = 11 }, @{ Column = 5 }, @{ Target = 'other' }, @{ TranslationUnit = 'src/other.cpp' }, @{ RelativePath = $adapterPath }, @{ Identifier = 'local' })) {
        $candidate = [PSCustomObject]@{ Target = $diagnostic.Target; TranslationUnit = $diagnostic.TranslationUnit; RelativePath = $diagnostic.RelativePath; Line = $diagnostic.Line; Column = $diagnostic.Column; Severity = $diagnostic.Severity; Identifier = $diagnostic.Identifier; Message = $diagnostic.Message }
        foreach ($name in $mutation.Keys) { $candidate.$name = $mutation[$name] }
        if ((Get-CppcheckVendorDispositionResult -Diagnostics @($candidate) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts).Accepted.Count -ne 0) { throw "Unexpectedly accepted $($mutation.Keys -join ',')." }
    }

    foreach ($invalid in @(
        @{ Mutate = { Set-Content -LiteralPath (Join-Path $fixtureRoot $vendorPath) -Value 'changed' -NoNewline }; Expected = 'source hash changed' },
        @{ Mutate = { $context.AnalyzerVersion = 'Cppcheck 2.22.0' }; Expected = 'analyzer version changed' },
        @{ Mutate = { $context.Toolset = 'v142' }; Expected = "precondition 'Toolset' does not match" },
        @{ Mutate = { Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'changed_adapter' -NoNewline }; Expected = 'API root hash changed' },
        @{ Mutate = { $context.Defines = 'Debug' }; Expected = "precondition 'Defines' does not match" }
    )) {
        & $invalid.Mutate
        Assert-ExpectedException -Expected $invalid.Expected -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }
        Set-Content -LiteralPath (Join-Path $fixtureRoot $vendorPath) -Value 'vendor' -NoNewline
        Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'ma_decoder_init_memory()' -NoNewline
        $context.AnalyzerVersion = 'Cppcheck 2.21.0'; $context.Toolset = 'v143'; $context.Defines = 'Release'
    }

    Set-Content -LiteralPath (Join-Path $fixtureRoot 'src/other.cpp') -Value 'ma_decoder_init_memory()' -NoNewline
    Assert-ExpectedException -Expected 'API roots changed' -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'src/other.cpp') -Force

    Set-Content -LiteralPath (Join-Path $fixtureRoot 'src/other.h') -Value '#define INITIALIZE_DECODER() ma_decoder_init_memory()' -NoNewline
    Assert-ExpectedException -Expected 'API roots changed' -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'src/other.h') -Force

    Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'ma_decoder_init_memory(); ma_decoder_init_file()' -NoNewline
    $record.Preconditions.ApiRoots[0].SHA256 = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline
    Assert-ExpectedException -Expected 'API tokens changed' -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }
    Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'ma_decoder_init_memory()' -NoNewline
    $record.Preconditions.ApiRoots[0].SHA256 = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline

    Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'ma_decoder_init_memory(); ma_gainer_init()' -NoNewline
    $record.Preconditions.ApiRoots[0].SHA256 = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline
    Assert-ExpectedException -Expected 'API tokens changed' -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }
    Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'ma_decoder_init_memory()' -NoNewline
    $record.Preconditions.ApiRoots[0].SHA256 = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline

    Set-Content -LiteralPath (Join-Path $fixtureRoot 'src/new_api.h') -Value '#define INITIALIZE_GAINER() ma_gainer_init()' -NoNewline
    Assert-ExpectedException -Expected 'API roots changed' -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'src/new_api.h') -Force

    Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'stb_vorbis_open_memory()' -NoNewline
    $record.Preconditions.ApiTokens = @('stb_vorbis_open_memory')
    $record.Preconditions.ApiRoots[0].SHA256 = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline
    Set-Content -LiteralPath (Join-Path $fixtureRoot $adapterPath) -Value 'stb_vorbis_open_memory(); stb_vorbis_test_new_family()' -NoNewline
    $record.Preconditions.ApiRoots[0].SHA256 = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline
    Assert-ExpectedException -Expected 'API tokens changed' -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }

    $localRecord = @{} + $record
    $localRecord.Path = 'src/local.cpp'
    $localRecord.SHA256 = $adapterHash
    $localDiagnostic = [PSCustomObject]@{ Target = 'zdoom'; TranslationUnit = $adapterPath; RelativePath = 'src/local.cpp'; Line = 10; Column = 4; Severity = 'warning'; Identifier = 'id'; Message = 'message' }
    @{ SchemaVersion = 1; Dispositions = @($localRecord) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline
    Assert-ExpectedException -Expected 'unsupported vendor source path' -Action { Get-CppcheckVendorDispositionResult -Diagnostics @($localDiagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null }

    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $policyPath -NoNewline
    $unmatchedDiagnostic = [PSCustomObject]@{ Target = 'unscanned'; TranslationUnit = 'src/local.cpp'; RelativePath = 'src/local.cpp'; Line = 1; Column = 1; Severity = 'warning'; Identifier = 'local'; Message = 'local message' }
    if ((Get-CppcheckVendorDispositionResult -Diagnostics @($unmatchedDiagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts @{}).Unaccepted.Count -ne 1) { throw 'Unmatched local diagnostic did not remain unaccepted without a context.' }

    Set-Content -LiteralPath $policyPath -Value '{' -NoNewline
    try { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null; throw 'Malformed policy was accepted.' } catch { if ($_.Exception.Message -eq 'Malformed policy was accepted.') { throw } }

    foreach ($case in @(
        @{ Output = @("$fixtureRoot/src/missing.h`t1`t1`twarning`tmissingFile`tmissing"); ExitCode = 1; Expected = 'missing' },
        @{ Output = @('error: invalid project'); ExitCode = 1; Expected = 'before producing diagnostics' },
        @{ Output = @("$fixtureRoot/src/vendor.h`t1`t1`twarning`tid`tmessage", 'fatal error: bad configuration'); ExitCode = 1; Expected = 'tool or configuration error' },
        @{ Output = @("$fixtureRoot/src/vendor.h`t1`t1`twarning`tid`tmessage", 'cppcheck: internal failure'); ExitCode = 2; Expected = 'exited unexpectedly' }
    )) {
        try {
            ConvertFrom-CppcheckProjectOutput -Output $case.Output -ExitCode $case.ExitCode -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit 'src/adapter.cpp' | Out-Null
            throw "Cppcheck parser accepted $($case.Expected)."
        }
        catch {
            if ($_.Exception.Message -eq "Cppcheck parser accepted $($case.Expected).") { throw }
            if ($_.Exception.Message -notmatch [regex]::Escape($case.Expected)) { throw }
        }
    }

    $duplicatePolicy = @{ SchemaVersion = 1; Dispositions = @($record, $record) } | ConvertTo-Json -Depth 6
    Set-Content -LiteralPath $policyPath -Value $duplicatePolicy -NoNewline
    try { Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts | Out-Null; throw 'Duplicate policy record was accepted.' } catch { if ($_.Exception.Message -eq 'Duplicate policy record was accepted.') { throw } }

    $projectPath = Join-Path $fixtureRoot 'fixture.vcxproj'
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup><ItemGroup><ClCompile Include="src\sound\audio_decoder_miniaudio.cpp" /></ItemGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    if ((Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0').Defines -ne 'RELEASE') { throw 'Project context was not extracted.' }
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset><PlatformToolset>ClangCL</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    Assert-ExpectedException -Expected 'Could not prove Cppcheck disposition context' -Action { Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0' | Out-Null }
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Debug|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Debug|x64''"><ClCompile><PreprocessorDefinitions>DEBUG</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    if ((Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0').Defines -ne 'RELEASE') { throw 'Known non-Release configuration was not accepted.' }
    $projectXml = '<Project><Import Project="custom.props" /><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    Assert-ExpectedException -Expected 'unsupported import' -Action { Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0' | Out-Null }
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><PropertyGroup Condition="''$(Configuration)''==''Release''"><PlatformToolset>v142</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    Assert-ExpectedException -Expected 'unsupported conditional configuration' -Action { Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0' | Out-Null }
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup><ItemGroup><ClCompile Include="src\sound\audio_decoder_miniaudio.cpp"><PreprocessorDefinitions>MA_NO_SSE2</PreprocessorDefinitions></ClCompile></ItemGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    Assert-ExpectedException -Expected 'Could not prove Cppcheck disposition context' -Action { Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0' | Out-Null }
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions><ForcedIncludeFiles>unverified-decoder-config.h</ForcedIncludeFiles></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    Assert-ExpectedException -Expected 'Could not prove Cppcheck disposition context' -Action { Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0' | Out-Null }
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)'' == ''Release|x64''"><ClCompile><PreprocessorDefinitions>UNVERIFIED_OVERRIDE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    Assert-ExpectedException -Expected 'Could not prove Cppcheck disposition context' -Action { Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0' | Out-Null }
    $projectXml = '<Project><PropertyGroup><CLToolExe>clang-cl.exe</CLToolExe></PropertyGroup><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    Assert-ExpectedException -Expected 'unsupported compiler override' -Action { Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0' | Out-Null }
    Write-Host 'cppcheck vendor disposition tests: PASS'
}
finally {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
}