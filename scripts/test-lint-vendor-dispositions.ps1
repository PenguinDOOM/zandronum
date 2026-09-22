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

function Assert-LintInputRejected {
    param(
        [string[]]$Arguments,
        [string]$Expected
    )

    $lintScript = Join-Path $PSScriptRoot 'lint.ps1'
    $output = @(& pwsh -NoProfile -File $lintScript @Arguments 2>&1 | ForEach-Object { $_.ToString() })

    if ($LASTEXITCODE -ne 1) {
        throw "lint.ps1 expected exit code 1 for input rejection, got $LASTEXITCODE."
    }

    if (($output -join [Environment]::NewLine) -notmatch [regex]::Escape($Expected)) {
        throw "lint.ps1 did not report expected input rejection '$Expected'."
    }
}

function Import-LintFunction {
    param(
        [string]$Name
    )

    $lintScript = Join-Path $PSScriptRoot 'lint.ps1'
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($lintScript, [ref]$tokens, [ref]$errors)

    if ($errors.Count -ne 0) {
        throw "Could not parse lint.ps1: $($errors[0].Message)"
    }

    $function = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $Name }, $true) | Select-Object -First 1

    if ($null -eq $function) {
        throw "lint.ps1 function '$Name' was not found."
    }

    $definition = $function.Extent.Text -replace ("(?m)^function\s+" + [regex]::Escape($Name) + '\b'), "function global:$Name"
    Invoke-Expression $definition
}

function Assert-ClassifiedFailureEvidence {
    param(
        [string]$FixtureRoot,
        [string]$Error,
        [int]$NewCount,
        [int]$UnresolvedVendorCount
    )

    $evidenceRoot = Join-Path $FixtureRoot ('evidence-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
    $toolFixturePath = Join-Path $evidenceRoot 'fixture-cppcheck.exe'
    Set-Content -LiteralPath $toolFixturePath -Value 'fixture analyzer' -NoNewline
    $toolFixtureHash = (Get-FileHash -LiteralPath $toolFixturePath -Algorithm SHA256).Hash
    @{ ScriptPath = 'lint.ps1'; ScriptSHA256 = 'script'; PolicyPath = 'policy.json'; PolicySHA256 = 'policy'; AnalyzerPath = $toolFixturePath; AnalyzerSHA256 = $toolFixtureHash; AnalyzerVersion = 'Cppcheck fixture' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot 'tool-provenance.json') -NoNewline
    $evidence = [PSCustomObject]@{ Root = $evidenceRoot }
    $vendorDispositionResult = [PSCustomObject]@{ Raw = @('raw'); Accepted = @(); Unaccepted = @('unaccepted') }
    $comparison = [PSCustomObject]@{ Baseline = @('baseline'); Unchanged = @(); BaselineOnly = @(); New = if ($NewCount -eq 0) { @() } else { @(1..$NewCount) }; UnresolvedVendor = if ($UnresolvedVendorCount -eq 0) { @() } else { @(1..$UnresolvedVendorCount) } }
    $finalResultSaved = $false

    try {
        Save-LintEvidenceResult -Evidence $evidence -Status 'failed' -ExitCode 1 -Error $Error -VendorDispositionResult $vendorDispositionResult -Comparison $comparison
        $finalResultSaved = $true
        Write-Error $Error
    }
    catch {
        if ($_.Exception.Message -ne $Error) { throw }
    }
    finally {
        if (-not $finalResultSaved) {
            Save-LintEvidenceResult -Evidence $evidence -Status 'incomplete' -ExitCode 1 -Error 'The gate did not reach final classification.'
        }
    }

    $result = Get-Content -LiteralPath (Join-Path $evidenceRoot 'final-result.json') -Raw | ConvertFrom-Json

    if (($result.Status -ne 'failed') -or ($result.ExitCode -ne 1) -or ($result.Error -ne $Error) -or
        ($result.Classification.New -ne $NewCount) -or ($result.Classification.UnresolvedVendor -ne $UnresolvedVendorCount) -or
        ($result.Analyzer.Path -ne $toolFixturePath) -or ($result.Analyzer.SHA256 -ne $toolFixtureHash) -or
        ($result.Analyzer.Version -ne 'Cppcheck fixture')) {
        throw "Classified failure evidence was overwritten or incomplete for '$Error'."
    }
}

function Assert-FinalizationFailureProcess {
    param(
        [string]$FixtureRoot,
        [string]$FailureKind
    )

    $childScriptPath = Join-Path $FixtureRoot ("finalization-$FailureKind.ps1")
    $resultPath = Join-Path $FixtureRoot ("finalization-$FailureKind.json")
    $definition = (Get-Command Complete-LintFinalization -CommandType Function).Definition
    $childScript = @'
param([string]$FailureKind, [string]$ResultPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Complete-LintFinalization {
'@ + $definition + @'
}
$finalization = Complete-LintFinalization `
    -SaveEvidenceAction { if ($FailureKind -match '^evidence') { throw 'injected evidence failure' } } `
    -CleanupAction { if ($FailureKind -eq 'cleanup') { throw 'injected cleanup failure' } } `
    -SaveResultAction {
        param($status, $exitCode, $errorMessage)
        if ($FailureKind -eq 'evidence-save') { throw 'injected failed evidence save failure' }
        @{ Status = $status; ExitCode = $exitCode; Error = $errorMessage } | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -NoNewline
    }
if ($FailureKind -eq 'evidence-save') {
    if ($finalization.Succeeded -or $finalization.ResultSaved -or ($finalization.ErrorMessage -ne 'injected evidence failure')) {
        exit 3
    }
    exit 1
}
if ($finalization.Succeeded -or (-not $finalization.ResultSaved)) {
    exit 2
}
exit 1
'@
    Set-Content -LiteralPath $childScriptPath -Value $childScript -NoNewline
    & pwsh -NoProfile -File $childScriptPath -FailureKind $FailureKind -ResultPath $resultPath
    if ($LASTEXITCODE -ne 1) {
        throw "Cppcheck $FailureKind finalization process expected exit code 1, got $LASTEXITCODE."
    }

    if ($FailureKind -eq 'evidence-save') {
        return
    }

    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if (($result.Status -ne 'failed') -or ($result.ExitCode -ne 1) -or ($result.Error -notmatch "injected $FailureKind failure")) {
        throw "Cppcheck $FailureKind finalization process did not save a failed result."
    }
}

function Assert-FinalizationSuccessCleanupOnce {
    $cleanupState = [PSCustomObject]@{ Count = 0 }
    $savedState = [PSCustomObject]@{ Result = $null }
    $finalization = Complete-LintFinalization `
        -SaveEvidenceAction {} `
        -CleanupAction { $cleanupState.Count++ } `
        -SaveResultAction { param($status, $exitCode, $errorMessage) $savedState.Result = "$status/$exitCode/$errorMessage" }

    if ((-not $finalization.Succeeded) -or (-not $finalization.ResultSaved) -or ($cleanupState.Count -ne 1) -or ($savedState.Result -ne 'passed/0/')) {
        throw 'Cppcheck successful finalization did not complete cleanup exactly once.'
    }
}

function Assert-LintEvidenceStateSnapshotsCacheConfigurations {
    param([string]$FixtureRoot)

    $sourceBuildRoot = Join-Path $FixtureRoot 'evidence-source-build'
    $baselineBuildRoot = Join-Path $FixtureRoot 'evidence-baseline-build'
    $evidenceRoot = Join-Path $FixtureRoot 'evidence-state'
    $configurationPath = Join-Path $FixtureRoot 'evidence-std.cfg'
    foreach ($buildRoot in @($sourceBuildRoot, $baselineBuildRoot)) {
        New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
        Set-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Value 'fixture-cache' -NoNewline
        Set-Content -LiteralPath (Join-Path $buildRoot 'fixture.vcxproj') -Value '<Project />' -NoNewline
    }
    Set-Content -LiteralPath $configurationPath -Value 'fixture configuration' -NoNewline
    New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
    @{ ScriptPath = 'lint.ps1'; ScriptSHA256 = 'script'; PolicyPath = 'policy.json'; PolicySHA256 = 'policy'; AnalyzerPath = 'cppcheck.exe'; AnalyzerSHA256 = 'analyzer'; AnalyzerVersion = 'Cppcheck fixture' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot 'tool-provenance.json') -NoNewline

    $cacheIdentity = [PSCustomObject]@{
        Key = 'fixture-key'
        Context = [PSCustomObject]@{
            AnalysisContext = [PSCustomObject]@{
                AnalyzerConfigurations = @([PSCustomObject]@{
                    Path = $configurationPath
                    SHA256 = (Get-FileHash -LiteralPath $configurationPath -Algorithm SHA256).Hash
                })
            }
        }
    }
    $evidence = [PSCustomObject]@{ Root = $evidenceRoot }
    Complete-LintFinalization `
        -SaveEvidenceAction {
            Save-LintEvidenceState -Evidence $evidence -AnalysisBuildRoot $sourceBuildRoot -BaselineBuildRoot $baselineBuildRoot -TempRoot $FixtureRoot -AnalysisProjects @() -BaselineProjects @() -Contexts @{} -VerifiedGeneratedBundle @() -CacheIdentity @{ 'head/fixture.vcxproj' = $cacheIdentity } -CachePaths @{} -HeadCommit 'head' -BaseCommit 'base'
        } `
        -CleanupAction {} `
        -SaveResultAction { param($status, $exitCode, $errorMessage) Save-LintEvidenceResult -Evidence $evidence -Status $status -ExitCode $exitCode -Error $errorMessage } | Out-Null

    $snapshotPath = Join-Path $evidenceRoot ('cache-inputs\{0}-evidence-std.cfg' -f $cacheIdentity.Context.AnalysisContext.AnalyzerConfigurations[0].SHA256)
    $cacheContext = Get-Content -LiteralPath (Join-Path $evidenceRoot 'cache-context.json') -Raw | ConvertFrom-Json
    if ((-not (Test-Path -LiteralPath $snapshotPath -PathType Leaf)) -or ($cacheContext.Identity.'head/fixture.vcxproj'.Key -ne 'fixture-key')) {
        throw 'Lint evidence state did not snapshot analyzer configurations from cache identity contexts.'
    }
}

function Assert-LintOuterFinalizationIntegration {
    $lintScript = Join-Path $PSScriptRoot 'lint.ps1'
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($lintScript, [ref]$tokens, [ref]$errors)

    if ($errors.Count -ne 0) {
        throw "Could not parse lint.ps1: $($errors[0].Message)"
    }

    $scriptText = $ast.Extent.Text
    if (($scriptText -notmatch '\$cleanupState = \[PSCustomObject\]@\{ Completed = \$false \}') -or
        ($scriptText -notmatch 'if \(-not \$cleanupState\.Completed\)')) {
        throw 'lint.ps1 outer finalization does not use shared cleanup completion state.'
    }

    $lockFinally = $ast.FindAll({
            param($node)
            ($node -is [System.Management.Automation.Language.TryStatementAst]) -and
            ($null -ne $node.Finally) -and
            ($node.Finally.Extent.Text -match 'Exit-CppcheckCacheLock')
        }, $true) | Select-Object -First 1

    if ($null -eq $lockFinally) {
        throw 'lint.ps1 does not release the Cppcheck cache lock from a finally block.'
    }

    $outerFinalization = $ast.FindAll({
            param($node)
            ($node -is [System.Management.Automation.Language.TryStatementAst]) -and
            ($null -ne $node.Finally) -and
            ($node.Finally.Extent.Text -match 'Remove-CppcheckDisposableStage') -and
            ($node.Finally.FindAll({ param($child) ($child -is [System.Management.Automation.Language.TryStatementAst]) -and ($null -ne $child.Finally) -and ($child.Finally.Extent.Text -match 'Exit-CppcheckCacheLock') }, $true).Count -ne 0)
        }, $true) | Select-Object -First 1

    if ($null -eq $outerFinalization) {
        throw 'lint.ps1 cleanup failure can bypass Cppcheck cache lock release.'
    }
}

function Assert-ManifestDoesNotReconfigureBaseline {
    $lintScript = Join-Path $PSScriptRoot 'lint.ps1'
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($lintScript, [ref]$tokens, [ref]$errors)

    if ($errors.Count -ne 0) {
        throw "Could not parse lint.ps1: $($errors[0].Message)"
    }

    $configureInvocation = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.Extent.Text -match "-Label 'configure-baseline'" }, $true) | Select-Object -First 1

    if ($null -eq $configureInvocation) {
        throw 'lint.ps1 no longer configures the default baseline.'
    }

    $manifestGuard = $configureInvocation
    while (($null -ne $manifestGuard) -and (($manifestGuard -isnot [System.Management.Automation.Language.IfStatementAst]) -or ($manifestGuard.Extent.Text -notmatch '\$null -eq \$inputManifestData'))) {
        $manifestGuard = $manifestGuard.Parent
    }

    if ($null -eq $manifestGuard) {
        throw 'Manifest execution can still invoke the baseline reconfigure path.'
    }
}

function Get-CppcheckCommandEvidence {
    param([string]$EvidenceRoot)

    $commandRoot = Join-Path $EvidenceRoot 'commands'
    $rawPath = (Get-ChildItem -LiteralPath $commandRoot -Filter '*.raw.txt' | Sort-Object Name | Select-Object -Last 1).FullName
    $invocationPath = (Get-ChildItem -LiteralPath $commandRoot -Filter '*.invocation.json' | Sort-Object Name | Select-Object -Last 1).FullName
    return [PSCustomObject]@{
        RawDiagnostics = @(Get-Content -LiteralPath $rawPath | Where-Object { $_ -match "`t\d+`t\d+`t" })
        ExitCode = (Get-Content -LiteralPath $invocationPath -Raw | ConvertFrom-Json).ExitCode
        RawOutput = Get-Content -LiteralPath $rawPath -Raw
    }
}

function Assert-EqualCppcheckRunResult {
    param(
        [object[]]$ExpectedDiagnostics,
        [object]$ExpectedEvidence,
        [object[]]$ActualDiagnostics,
        [object]$ActualEvidence,
        [string]$Description
    )

    if (($ExpectedEvidence.ExitCode -ne $ActualEvidence.ExitCode) -or
        (($ExpectedEvidence.RawDiagnostics -join "`n") -ne ($ActualEvidence.RawDiagnostics -join "`n")) -or
        ((@($ExpectedDiagnostics | ConvertTo-Json -Compress -Depth 8) -join "`n") -ne (@($ActualDiagnostics | ConvertTo-Json -Compress -Depth 8) -join "`n"))) {
        throw "Cppcheck $Description did not preserve raw diagnostic fields, multiplicity, or child exit code."
    }
}

function Assert-NativeCppcheckCacheInvalidation {
    param(
        [string]$FixtureRoot,
        [string]$CacheRoot
    )

    $cppcheck = Get-Command cppcheck -ErrorAction Stop
    $buildRoot = Join-Path $FixtureRoot 'native-cache/build'
    $sourceRoot = Join-Path $buildRoot 'src'
    $sourcePath = Join-Path $sourceRoot 'sample.cpp'
    $headerPath = Join-Path $sourceRoot 'sample.h'
    $projectPath = Join-Path $buildRoot 'compile_commands.json'
    $translationUnit = 'sample.cpp'
    $evidenceRoot = Join-Path $FixtureRoot 'native-cache/evidence'
    New-Item -ItemType Directory -Force -Path (Join-Path $evidenceRoot 'commands') | Out-Null
    New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
    Set-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Value 'CMAKE_GENERATOR_TOOLSET:INTERNAL=v143' -NoNewline
    Set-Content -LiteralPath $headerPath -Value '#define SAMPLE_OFFSET 0' -NoNewline
    Set-Content -LiteralPath $sourcePath -Value ('#include "sample.h"' + "`n" + 'int main() { int value; return value + SAMPLE_OFFSET; }') -NoNewline
    @(@{ directory = $sourceRoot; command = ('cl.exe /I"' + $sourceRoot + '" /c sample.cpp'); file = $sourcePath }) | ConvertTo-Json -Compress -AsArray | Set-Content -LiteralPath $projectPath -NoNewline

    $identity = Get-CppcheckCacheIdentity -AnalyzerVersion (& $cppcheck.Source --version) -AnalyzerSHA256 (Get-FileHash -LiteralPath $cppcheck.Source -Algorithm SHA256).Hash -InputMode 'normal' -Toolset 'v143' -RootIdentity 'native-cache-fixture' -AnalysisContext (Get-CppcheckRegressionAnalysisContext -BuildRoot $buildRoot -RepositoryRoot $buildRoot -ProjectPath $projectPath -InputRootIdentities @('native-cache-fixture') -AnalyzerConfigurationPaths (Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $cppcheck.Source) -AnalyzerOptions @('--project-configuration=Release|x64', '--enable=warning,performance,portability'))
    $cachePath = Get-CppcheckCacheLeaf -CacheRoot $CacheRoot -Namespace 'regression' -Identity $identity -Role 'head' -TargetName 'compile_commands.json' -TranslationUnit $translationUnit
    $freshCachePath = Get-CppcheckCacheLeaf -CacheRoot (Join-Path $FixtureRoot 'native-cache/fresh-cache') -Namespace 'regression' -Identity $identity -Role 'head' -TargetName 'compile_commands.json' -TranslationUnit $translationUnit
    $evidence = [PSCustomObject]@{ Root = $evidenceRoot; Counter = 0 }
    $global:generatedBundlePaths = @()
    $global:utf8 = New-Object System.Text.UTF8Encoding $false

    $cold = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $cachePath -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit $translationUnit -CppcheckJobs 1 -Evidence $evidence -UseProjectConfiguration:$false)
    $coldEvidence = Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot
    $warm = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $cachePath -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit $translationUnit -CppcheckJobs 1 -Evidence $evidence -UseProjectConfiguration:$false)
    $warmEvidence = Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot
    Assert-EqualCppcheckRunResult -ExpectedDiagnostics $cold -ExpectedEvidence $coldEvidence -ActualDiagnostics $warm -ActualEvidence $warmEvidence -Description 'fixture cold/warm route'

    Set-Content -LiteralPath $sourcePath -Value ('#include "sample.h"' + "`n" + 'int main() { int value; return value + SAMPLE_OFFSET + 1; }') -NoNewline
    $sourceChanged = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $cachePath -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit $translationUnit -CppcheckJobs 1 -Evidence $evidence -UseProjectConfiguration:$false)
    $sourceChangedEvidence = Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot
    if ($sourceChangedEvidence.RawOutput -match 'skipping analysis - loaded [0-9]+ cached finding\(s\)') { throw 'Native Cppcheck cache did not invalidate the edited source within its retained leaf.' }
    $sourceFresh = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $freshCachePath -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit $translationUnit -CppcheckJobs 1 -Evidence $evidence -UseProjectConfiguration:$false)
    Assert-EqualCppcheckRunResult -ExpectedDiagnostics $sourceFresh -ExpectedEvidence (Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot) -ActualDiagnostics $sourceChanged -ActualEvidence $sourceChangedEvidence -Description 'source invalidation against fresh cache'

    Set-Content -LiteralPath $headerPath -Value '#define SAMPLE_OFFSET 2' -NoNewline
    $headerChanged = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $cachePath -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit $translationUnit -CppcheckJobs 1 -Evidence $evidence -UseProjectConfiguration:$false)
    $headerChangedEvidence = Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot
    if ($headerChangedEvidence.RawOutput -match 'skipping analysis - loaded [0-9]+ cached finding\(s\)') { throw 'Native Cppcheck cache did not invalidate the edited header within its retained leaf.' }
    $headerFresh = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $freshCachePath -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit $translationUnit -CppcheckJobs 1 -Evidence $evidence -UseProjectConfiguration:$false)
    Assert-EqualCppcheckRunResult -ExpectedDiagnostics $headerFresh -ExpectedEvidence (Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot) -ActualDiagnostics $headerChanged -ActualEvidence $headerChangedEvidence -Description 'header invalidation against fresh cache'
}

function Save-NativeCppcheckCacheEvidence {
    param([string]$FixtureRoot)

    $repositoryRoot = Split-Path -Parent $PSScriptRoot
    $evidenceRoot = Join-Path $repositoryRoot 'completes/lint-cache-key-reuse'
    $runRoot = Join-Path $evidenceRoot ('native-cache-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $FixtureRoot 'native-cache/evidence') -Destination (Join-Path $runRoot 'isolated-fixture') -Recurse
    Copy-Item -LiteralPath (Join-Path $FixtureRoot 'real-project-evidence') -Destination (Join-Path $runRoot 'generated-project') -Recurse
    Copy-Item -LiteralPath (Join-Path $FixtureRoot 'native-cache/build/compile_commands.json') -Destination (Join-Path $runRoot 'compile_commands.json')
}

function Assert-RealCppcheckProjectCacheRoute {
    param(
        [string]$FixtureRoot,
        [string]$CacheRoot
    )

    $cppcheck = Get-Command cppcheck -ErrorAction Stop
    $repositoryRoot = Split-Path -Parent $PSScriptRoot
    $buildRoot = Join-Path $repositoryRoot 'build-v143'
    $projectPath = Join-Path $buildRoot 'src/zdoom.vcxproj'
    $translationUnit = 'src/sound/i_sound.cpp'
    $evidenceRoot = Join-Path $FixtureRoot 'real-project-evidence'
    if ((-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) -or (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot $translationUnit) -PathType Leaf))) {
        throw 'Real generated Cppcheck project fixture is unavailable.'
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $evidenceRoot 'commands') | Out-Null

    $analysisContext = Get-CppcheckRegressionAnalysisContext -BuildRoot $buildRoot -RepositoryRoot $repositoryRoot -ProjectPath $projectPath -InputRootIdentities @('regression-live-worktree') -AnalyzerConfigurationPaths (Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $cppcheck.Source) -AnalyzerOptions @('--project-configuration=Release|x64', '--enable=warning,performance,portability')
    $identity = Get-CppcheckCacheIdentity -AnalyzerVersion (& $cppcheck.Source --version) -AnalyzerSHA256 (Get-FileHash -LiteralPath $cppcheck.Source -Algorithm SHA256).Hash -InputMode 'normal' -Toolset 'v143' -RootIdentity 'regression-live-worktree' -AnalysisContext $analysisContext
    $cachePath = Get-CppcheckCacheLeaf -CacheRoot $CacheRoot -Namespace 'regression' -Identity $identity -Role 'head' -TargetName 'src/zdoom.vcxproj' -TranslationUnit $translationUnit
    $evidence = [PSCustomObject]@{ Root = $evidenceRoot; Counter = 0 }
    $global:generatedBundlePaths = @()
    $global:utf8 = New-Object System.Text.UTF8Encoding $false

    $cold = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $cachePath -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -TargetName 'src/zdoom.vcxproj' -TranslationUnit $translationUnit -CppcheckJobs 1 -Evidence $evidence)
    $coldEvidence = Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot
    $warm = @(Invoke-CppcheckProject -CppcheckPath $cppcheck.Source -ProjectPath $projectPath -CachePath $cachePath -RepositoryRoot $repositoryRoot -BuildRoot $buildRoot -TargetName 'src/zdoom.vcxproj' -TranslationUnit $translationUnit -CppcheckJobs 2 -Evidence $evidence)
    $warmEvidence = Get-CppcheckCommandEvidence -EvidenceRoot $evidenceRoot

    Assert-EqualCppcheckRunResult -ExpectedDiagnostics $cold -ExpectedEvidence $coldEvidence -ActualDiagnostics $warm -ActualEvidence $warmEvidence -Description 'cold/warm route'

    if ($warmEvidence.RawOutput -notmatch 'skipping analysis - loaded [0-9]+ cached finding\(s\)') {
        throw 'Real Cppcheck project route did not report a native warm-cache hit.'
    }
}

function Assert-ProductionRegressionCacheLeafExcludesGateControls {
    param(
        [string]$FixtureRoot,
        [string]$CacheRoot
    )

    $lintScript = Join-Path $PSScriptRoot 'lint.ps1'
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($lintScript, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "Could not parse lint.ps1: $($errors[0].Message)" }

    $productionNodes = @{}
    foreach ($name in @('headContext', 'headIdentity', 'headCache', 'baselineCacheContext', 'baselineIdentity', 'baselineCache')) {
        $matches = @($ast.FindAll({
                    param($node)
                    ($node -is [System.Management.Automation.Language.AssignmentStatementAst])
                }, $true) | Where-Object { $_.Left -is [System.Management.Automation.Language.VariableExpressionAst] } )
        $matches = @($matches | Where-Object { $_.Left.VariablePath.UserPath -eq $name })
        if ($matches.Count -ne 1) { throw "lint.ps1 does not retain exactly one production assignment for '$name'." }
        $productionNodes[$name] = [scriptblock]::Create($matches[0].Extent.Text)
    }

    $scenarioRoot = Join-Path $FixtureRoot 'excluded-key-inputs'
    $buildRoot = Join-Path $scenarioRoot 'build'
    $projectPath = Join-Path $buildRoot 'fixture.vcxproj'
    $configurationPath = Join-Path $scenarioRoot 'std.cfg'
    $analyzerPath = Join-Path $scenarioRoot 'cppcheck.exe'
    $scriptPath = Join-Path $scenarioRoot 'lint-script.ps1'
    $policyPath = Join-Path $scenarioRoot 'vendor-policy.json'
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $scenarioRoot 'cfg') | Out-Null
    Set-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Value 'CMAKE_GENERATOR_TOOLSET:INTERNAL=v143' -NoNewline
    Set-Content -LiteralPath $projectPath -Value '<Project><PropertyGroup><PlatformToolset>v143</PlatformToolset></PropertyGroup></Project>' -NoNewline
    Set-Content -LiteralPath $configurationPath -Value 'fixture configuration' -NoNewline
    Set-Content -LiteralPath $analyzerPath -Value 'fixture analyzer' -NoNewline
    Set-Content -LiteralPath (Join-Path $scenarioRoot 'cfg\std.cfg') -Value 'fixture standard library' -NoNewline
    $baselineScriptContent = 'Write-Host baseline-script'
    $baselinePolicyContent = '{"SchemaVersion":1,"Dispositions":[]}'
    Set-Content -LiteralPath $scriptPath -Value $baselineScriptContent -NoNewline
    Set-Content -LiteralPath $policyPath -Value $baselinePolicyContent -NoNewline

    $newScenario = {
        param(
            [string]$BaselineRevision,
            [int]$Jobs,
            [string]$DisplayTemplate,
            [int]$ErrorExitCode
        )

        [PSCustomObject]@{
            InvocationAndEvidenceInputs = [PSCustomObject]@{
                ScriptPath = $scriptPath
                ScriptSHA256 = (Get-FileHash -LiteralPath $scriptPath -Algorithm SHA256).Hash
                PolicyPath = $policyPath
                PolicySHA256 = (Get-FileHash -LiteralPath $policyPath -Algorithm SHA256).Hash
                CppcheckJobs = $Jobs
                DisplayTemplate = $DisplayTemplate
                ErrorExitCode = $ErrorExitCode
                BaselineRevision = $BaselineRevision
            }
        }
    }
    $getLeaves = {
        param([object]$Scenario)

        $analysisBuildRoot = $buildRoot
        $baselineBuildRoot = $buildRoot
        $cppcheckCacheRoot = $CacheRoot
        $analysisRepositoryRoot = $scenarioRoot
        $baselineRoot = $scenarioRoot
        $project = [PSCustomObject]@{ ProjectPath = $projectPath }
        $baselineProject = [PSCustomObject]@{ ProjectPath = $projectPath }
        $analysisProjectContext = [PSCustomObject]@{ Configuration = 'Release|x64'; Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64' }
        $baselineProjectContext = [PSCustomObject]@{ Configuration = 'Release|x64'; Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64' }
        $targetName = 'fixture.vcxproj'
        $translationUnit = 'fixture.cpp'
        $cppcheck = [PSCustomObject]@{ Source = $analyzerPath }
        $cppcheckVersion = @('Cppcheck fixture')
        $cppcheckSHA256 = ('a' * 64)
        $inputManifestData = $null
        $CppcheckJobs = $Scenario.InvocationAndEvidenceInputs.CppcheckJobs
        $baseCommit = $Scenario.InvocationAndEvidenceInputs.BaselineRevision
        $vendorDispositionPolicy = $Scenario.InvocationAndEvidenceInputs.PolicyPath
        $scriptContent = Get-Content -LiteralPath $Scenario.InvocationAndEvidenceInputs.ScriptPath -Raw
        $policyContent = Get-Content -LiteralPath $vendorDispositionPolicy -Raw

        . $productionNodes.headContext
        . $productionNodes.headIdentity
        . $productionNodes.headCache
        . $productionNodes.baselineCacheContext
        . $productionNodes.baselineIdentity
        . $productionNodes.baselineCache
        [PSCustomObject]@{ Head = $headCache; Baseline = $baselineCache; HeadIdentity = $headIdentity; BaselineIdentity = $baselineIdentity }
    }

    $baselineRevisionOne = '1111111111111111111111111111111111111111'
    $baselineRevisionTwo = '2222222222222222222222222222222222222222'
    $baselineScenario = & $newScenario $baselineRevisionOne 1 'tabular' 1
    $baselineLeaves = & $getLeaves $baselineScenario
    $excludedFactorMutations = @(
        [PSCustomObject]@{ Name = 'script file content'; Apply = { Set-Content -LiteralPath $scriptPath -Value 'Write-Host changed-script' -NoNewline }; Create = { & $newScenario $baselineRevisionOne 1 'tabular' 1 } },
        [PSCustomObject]@{ Name = 'policy file content'; Apply = { Set-Content -LiteralPath $policyPath -Value '{"SchemaVersion":1,"Dispositions":[{"Reason":"changed"}]}' -NoNewline }; Create = { & $newScenario $baselineRevisionOne 1 'tabular' 1 } },
        [PSCustomObject]@{ Name = 'job count'; Apply = {}; Create = { & $newScenario $baselineRevisionOne 2 'tabular' 1 } },
        [PSCustomObject]@{ Name = 'display and exit controls'; Apply = {}; Create = { & $newScenario $baselineRevisionOne 1 'verbose-tabular' 9 } },
        [PSCustomObject]@{ Name = 'baseline revision'; Apply = {}; Create = { & $newScenario $baselineRevisionTwo 1 'tabular' 1 } }
    )
    foreach ($mutation in $excludedFactorMutations) {
        Set-Content -LiteralPath $scriptPath -Value $baselineScriptContent -NoNewline
        Set-Content -LiteralPath $policyPath -Value $baselinePolicyContent -NoNewline
        & $mutation.Apply
        $changedScenario = & $mutation.Create
        $changedLeaves = & $getLeaves $changedScenario
        if (($changedLeaves.Head -ne $baselineLeaves.Head) -or ($changedLeaves.Baseline -ne $baselineLeaves.Baseline)) {
            throw "Production regression cache leaves changed for excluded factor '$($mutation.Name)'."
        }
        if (($changedScenario.InvocationAndEvidenceInputs | ConvertTo-Json -Compress) -eq ($baselineScenario.InvocationAndEvidenceInputs | ConvertTo-Json -Compress)) {
            throw "Excluded-factor scenario '$($mutation.Name)' did not vary a full invocation or evidence input."
        }
        if (($mutation.Name -eq 'baseline revision') -and ($changedScenario.InvocationAndEvidenceInputs.BaselineRevision -eq $baselineScenario.InvocationAndEvidenceInputs.BaselineRevision)) {
            throw 'Baseline revision scenario did not vary its recorded provenance input.'
        }
        if (($mutation.Name -eq 'script file content') -and ($changedScenario.InvocationAndEvidenceInputs.ScriptSHA256 -eq $baselineScenario.InvocationAndEvidenceInputs.ScriptSHA256)) {
            throw 'Script scenario did not vary its actual file-content hash.'
        }
        if (($mutation.Name -eq 'policy file content') -and ($changedScenario.InvocationAndEvidenceInputs.PolicySHA256 -eq $baselineScenario.InvocationAndEvidenceInputs.PolicySHA256)) {
            throw 'Policy scenario did not vary its actual file-content hash.'
        }
    }

    $negativeHeadCache = [scriptblock]::Create(($productionNodes.headCache.ToString() -replace "-Role 'head'", '-Role $baseCommit'))
    $baseCommit = $baselineRevisionTwo
    $analysisBuildRoot = $buildRoot
    $cppcheckCacheRoot = $CacheRoot
    $analysisRepositoryRoot = $scenarioRoot
    $project = [PSCustomObject]@{ ProjectPath = $projectPath }
    $analysisProjectContext = [PSCustomObject]@{ Configuration = 'Release|x64'; Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64' }
    $targetName = 'fixture.vcxproj'
    $translationUnit = 'fixture.cpp'
    $cppcheck = [PSCustomObject]@{ Source = $analyzerPath }
    $cppcheckVersion = @('Cppcheck fixture')
    $cppcheckSHA256 = ('a' * 64)
    $inputManifestData = $null
    . $productionNodes.headContext
    . $productionNodes.headIdentity
    . $negativeHeadCache
    if ($headCache -eq $baselineLeaves.Head) {
        throw 'Production-node negative control did not detect base commit reintroduction into the cache leaf role.'
    }
}

. (Join-Path $PSScriptRoot 'cppcheck-vendor-policy.ps1')
. (Join-Path $PSScriptRoot 'cppcheck-cache.ps1')
Import-LintFunction -Name 'ConvertTo-NormalizedCppcheckPathValue'
Import-LintFunction -Name 'Get-NormalizedCppcheckAdditionalIncludeDirectories'
Import-LintFunction -Name 'Assert-EqualCppcheckProjectContext'
Import-LintFunction -Name 'Resolve-CppcheckBaselineTarget'
Import-LintFunction -Name 'ConvertTo-ProductionCppcheckVendorDispositionContext'
Import-LintFunction -Name 'Get-RequiredManifestProperty'
Import-LintFunction -Name 'ConvertTo-SafeRelativePath'
Import-LintFunction -Name 'Get-PathRelativeToRoot'
Import-LintFunction -Name 'Get-ByteSHA256'
Import-LintFunction -Name 'Convert-LFBytesToCRLFBytes'
Import-LintFunction -Name 'Resolve-ApiRootRepresentation'
Import-LintFunction -Name 'Get-ManifestApiRoots'
Import-LintFunction -Name 'Assert-ApiRootBlobIdentity'
Import-LintFunction -Name 'Save-LintEvidenceState'
Import-LintFunction -Name 'Save-LintEvidenceResult'
Import-LintFunction -Name 'Complete-LintFinalization'
Import-LintFunction -Name 'Invoke-LintChild'
Import-LintFunction -Name 'Invoke-CppcheckProject'

$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('zandronum-vendor-policy-' + [guid]::NewGuid().ToString('N'))

try {
    $cacheFixtureRoot = Join-Path $fixtureRoot 'cppcheck-cache'
    $contextBuildRoot = Join-Path $fixtureRoot 'context-build'
    $contextProjectPath = Join-Path $contextBuildRoot 'fixture.vcxproj'
    $contextConfigurationPath = Join-Path $fixtureRoot 'cppcheck.cfg'
    New-Item -ItemType Directory -Force -Path $contextBuildRoot | Out-Null
    Set-Content -LiteralPath (Join-Path $contextBuildRoot 'CMakeCache.txt') -Value @('CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022', 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', 'CMAKE_GENERATOR_TOOLSET:INTERNAL=v143', ('Zandronum_BINARY_DIR:STATIC={0}' -f $contextBuildRoot.Replace('\', '/'))) -NoNewline
    Set-Content -LiteralPath $contextProjectPath -Value '<Project><PropertyGroup><ProjectGuid>{A}</ProjectGuid><PlatformToolset>v143</PlatformToolset></PropertyGroup><ProjectReference><Project>{A}</Project></ProjectReference></Project>' -NoNewline
    Set-Content -LiteralPath $contextConfigurationPath -Value 'fixture-one' -NoNewline
    $contextOne = Get-CppcheckAnalysisContext -BuildRoot $contextBuildRoot -ProjectPaths @($contextProjectPath) -InputRootIdentities @('fixture-root') -AnalyzerConfigurationPaths @($contextConfigurationPath) -AnalyzerOptions @('--enable=warning')
    $contextIdentityOne = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'fixture-root' -AnalysisContext $contextOne
    Set-Content -LiteralPath (Join-Path $contextBuildRoot 'CMakeCache.txt') -Value @('CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022', 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', 'CMAKE_GENERATOR_TOOLSET:INTERNAL=v143', ('Zandronum_BINARY_DIR:STATIC={0}' -f $contextBuildRoot)) -NoNewline
    Set-Content -LiteralPath $contextProjectPath -Value '<Project><PropertyGroup><ProjectGuid>{B}</ProjectGuid><PlatformToolset>v143</PlatformToolset></PropertyGroup><ProjectReference><Project>{B}</Project></ProjectReference></Project>' -NoNewline
    $contextTwo = Get-CppcheckAnalysisContext -BuildRoot $contextBuildRoot -ProjectPaths @($contextProjectPath) -InputRootIdentities @('fixture-root') -AnalyzerConfigurationPaths @($contextConfigurationPath) -AnalyzerOptions @('--enable=warning')
    $contextIdentityTwo = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'fixture-root' -AnalysisContext $contextTwo
    Set-Content -LiteralPath $contextProjectPath -Value '<Project><PropertyGroup><ProjectGuid>{B}</ProjectGuid><PlatformToolset>v142</PlatformToolset></PropertyGroup><ProjectReference><Project>{B}</Project></ProjectReference></Project>' -NoNewline
    $contextThree = Get-CppcheckAnalysisContext -BuildRoot $contextBuildRoot -ProjectPaths @($contextProjectPath) -InputRootIdentities @('fixture-root') -AnalyzerConfigurationPaths @($contextConfigurationPath) -AnalyzerOptions @('--enable=warning')
    $contextIdentityThree = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'fixture-root' -AnalysisContext $contextThree
    Set-Content -LiteralPath $contextConfigurationPath -Value 'fixture-two' -NoNewline
    $contextFour = Get-CppcheckAnalysisContext -BuildRoot $contextBuildRoot -ProjectPaths @($contextProjectPath) -InputRootIdentities @('fixture-root') -AnalyzerConfigurationPaths @($contextConfigurationPath) -AnalyzerOptions @('--enable=warning')
    $contextIdentityFour = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'fixture-root' -AnalysisContext $contextFour
    if (($contextIdentityOne.Key -eq $contextIdentityTwo.Key) -or ($contextIdentityTwo.Key -eq $contextIdentityThree.Key) -or ($contextIdentityThree.Key -eq $contextIdentityFour.Key)) { throw 'Cppcheck cache identity did not separate generated project or analyzer configuration changes.' }
    Set-Content -LiteralPath (Join-Path $contextBuildRoot 'CMakeCache.txt') -Value @('CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022', 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', 'CMAKE_GENERATOR_TOOLSET:INTERNAL=v143', ('Zandronum_BINARY_DIR:STATIC={0}' -f $contextBuildRoot.Replace('\', '/'))) -NoNewline
    Set-Content -LiteralPath $contextProjectPath -Value '<Project><PropertyGroup><ProjectGuid>{A}</ProjectGuid><PlatformToolset>v143</PlatformToolset></PropertyGroup><ProjectReference><Project>{A}</Project></ProjectReference></Project>' -NoNewline
    $regressionContextOne = Get-CppcheckRegressionAnalysisContext -BuildRoot $contextBuildRoot -RepositoryRoot $fixtureRoot -ProjectPath $contextProjectPath -InputRootIdentities @('fixture-root') -AnalyzerConfigurationPaths @($contextConfigurationPath) -AnalyzerOptions @('--enable=warning')
    $regressionIdentityOne = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'fixture-root' -AnalysisContext $regressionContextOne
    $unrelatedProjectPath = Join-Path $contextBuildRoot 'unrelated.vcxproj'
    Set-Content -LiteralPath $unrelatedProjectPath -Value '<Project><PropertyGroup><ProjectGuid>{unrelated}</ProjectGuid></PropertyGroup></Project>' -NoNewline
    Set-Content -LiteralPath (Join-Path $contextBuildRoot 'CMakeCache.txt') -Value @('CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022', 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', 'CMAKE_GENERATOR_TOOLSET:INTERNAL=v143', ('Zandronum_BINARY_DIR:STATIC={0}' -f $contextBuildRoot)) -NoNewline
    Set-Content -LiteralPath $contextProjectPath -Value '<Project><PropertyGroup><ProjectGuid>{B}</ProjectGuid><PlatformToolset>v143</PlatformToolset></PropertyGroup><ProjectReference><Project>{B}</Project></ProjectReference></Project>' -NoNewline
    $regressionContextTwo = Get-CppcheckRegressionAnalysisContext -BuildRoot $contextBuildRoot -RepositoryRoot $fixtureRoot -ProjectPath $contextProjectPath -InputRootIdentities @('fixture-root') -AnalyzerConfigurationPaths @($contextConfigurationPath) -AnalyzerOptions @('--enable=warning')
    $regressionIdentityTwo = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'fixture-root' -AnalysisContext $regressionContextTwo
    Set-Content -LiteralPath $contextProjectPath -Value '<Project><PropertyGroup><ProjectGuid>{B}</ProjectGuid><PlatformToolset>v142</PlatformToolset></PropertyGroup><ProjectReference><Project>{B}</Project></ProjectReference></Project>' -NoNewline
    $regressionContextThree = Get-CppcheckRegressionAnalysisContext -BuildRoot $contextBuildRoot -RepositoryRoot $fixtureRoot -ProjectPath $contextProjectPath -InputRootIdentities @('fixture-root') -AnalyzerConfigurationPaths @($contextConfigurationPath) -AnalyzerOptions @('--enable=warning')
    $regressionIdentityThree = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'fixture-root' -AnalysisContext $regressionContextThree
    if (($regressionContextOne.RegressionKeyVersion -ne 1) -or ($regressionIdentityOne.Key -ne $regressionIdentityTwo.Key) -or ($regressionIdentityTwo.Key -eq $regressionIdentityThree.Key)) { throw 'Regression cache key did not normalize generated GUIDs or separate target configuration changes.' }
    $cppcheck = Get-Command cppcheck -ErrorAction Stop
    $installedConfigurationPaths = @(Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $cppcheck.Source)
    if (($installedConfigurationPaths.Count -ne 1) -or ((Split-Path -Leaf $installedConfigurationPaths[0]) -ne 'std.cfg')) { throw 'Cppcheck automatic standard-library configuration was not resolved.' }
    foreach ($failureKind in @('evidence', 'cleanup', 'evidence-save')) { Assert-FinalizationFailureProcess -FixtureRoot $fixtureRoot -FailureKind $failureKind }
    Assert-FinalizationSuccessCleanupOnce
    Assert-LintEvidenceStateSnapshotsCacheConfigurations -FixtureRoot $fixtureRoot
    Assert-LintOuterFinalizationIntegration
    $normalIdentity = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -RootIdentity 'C:\fixture\source'
    $manifestIdentity = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'input-manifest' -RootIdentity 'fixed-regression-manifest-stage'
    $differentToolsetIdentity = Get-CppcheckCacheIdentity -AnalyzerVersion 'Cppcheck 2.21.0' -AnalyzerSHA256 ('a' * 64) -InputMode 'normal' -Toolset 'v142' -RootIdentity 'C:\fixture\source'
    if (($normalIdentity.Key -eq $manifestIdentity.Key) -or ($normalIdentity.Key -eq $differentToolsetIdentity.Key)) { throw 'Cppcheck cache identity did not separate input mode or toolset.' }
    $headCache = Get-CppcheckCacheLeaf -CacheRoot $cacheFixtureRoot -Namespace 'regression' -Identity $normalIdentity -Role 'head' -TargetName 'src/zdoom.vcxproj' -TranslationUnit 'src/example.cpp'
    $baseOneCache = Get-CppcheckCacheLeaf -CacheRoot $cacheFixtureRoot -Namespace 'regression' -Identity $normalIdentity -Role 'baseline' -TargetName 'src/zdoom.vcxproj' -TranslationUnit 'src/example.cpp'
    $baseTwoCache = Get-CppcheckCacheLeaf -CacheRoot $cacheFixtureRoot -Namespace 'regression' -Identity $normalIdentity -Role 'baseline' -TargetName 'src/zdoom.vcxproj' -TranslationUnit 'src/example.cpp'
    if (($headCache -eq $baseOneCache) -or ($baseOneCache -ne $baseTwoCache)) { throw 'Cppcheck cache paths did not retain fixed baseline and separate head roles.' }
    Assert-NativeCppcheckCacheInvalidation -FixtureRoot $fixtureRoot -CacheRoot $cacheFixtureRoot
    Assert-RealCppcheckProjectCacheRoute -FixtureRoot $fixtureRoot -CacheRoot $cacheFixtureRoot
    Assert-ProductionRegressionCacheLeafExcludesGateControls -FixtureRoot $fixtureRoot -CacheRoot $cacheFixtureRoot
    Save-NativeCppcheckCacheEvidence -FixtureRoot $fixtureRoot
    $cacheLock = Enter-CppcheckCacheLock -CacheRoot $cacheFixtureRoot -Name 'fixture'
    try {
        Assert-ExpectedException -Expected 'already running' -Action { Enter-CppcheckCacheLock -CacheRoot $cacheFixtureRoot -Name 'fixture' | Out-Null }
    }
    finally {
        Exit-CppcheckCacheLock -Lock $cacheLock
    }
    $stage = New-CppcheckDisposableStage -CacheRoot $cacheFixtureRoot -Name 'fixture-stage'
    Assert-ExpectedException -Expected 'already exists' -Action { New-CppcheckDisposableStage -CacheRoot $cacheFixtureRoot -Name 'fixture-stage' | Out-Null }
    Remove-CppcheckDisposableStage -CacheRoot $cacheFixtureRoot -StageRoot $stage -Name 'fixture-stage'
    $unknownStage = Join-Path $cacheFixtureRoot 'staging\unknown-stage'
    New-Item -ItemType Directory -Force -Path $unknownStage | Out-Null
    Assert-ExpectedException -Expected 'not the owned' -Action { Remove-CppcheckDisposableStage -CacheRoot $cacheFixtureRoot -StageRoot $unknownStage -Name 'fixture-stage' }
    if (-not (Test-Path -LiteralPath $unknownStage)) { throw 'Cppcheck cleanup removed an unowned staging path.' }
    Assert-ExpectedException -Expected 'not the owned' -Action { Remove-CppcheckDisposableStage -CacheRoot $cacheFixtureRoot -StageRoot $fixtureRoot -Name 'fixture-stage' }
    $outsideStageRoot = Join-Path $fixtureRoot 'outside-stage'
    $stagingPath = Join-Path $cacheFixtureRoot 'staging'
    try {
        New-Item -ItemType Directory -Force -Path $outsideStageRoot | Out-Null
        Set-Content -LiteralPath (Join-Path $outsideStageRoot 'marker.txt') -Value 'keep' -NoNewline
        Remove-Item -LiteralPath $stagingPath -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Junction -Path $stagingPath -Target $outsideStageRoot -ErrorAction Stop | Out-Null
        Assert-ExpectedException -Expected 'reparse-point ancestor' -Action { New-CppcheckDisposableStage -CacheRoot $cacheFixtureRoot -Name 'junction-stage' | Out-Null }
        Assert-ExpectedException -Expected 'reparse-point ancestor' -Action { Remove-CppcheckDisposableStage -CacheRoot $cacheFixtureRoot -StageRoot (Join-Path $stagingPath 'junction-stage') -Name 'junction-stage' }
        if (-not (Test-Path -LiteralPath (Join-Path $outsideStageRoot 'marker.txt') -PathType Leaf)) { throw 'Cppcheck staging cleanup traversed an intermediate junction.' }
    }
    catch [System.UnauthorizedAccessException] {
        Write-Host 'staging junction fixture: skipped (permission unavailable)'
    }
    finally {
        Remove-Item -LiteralPath $stagingPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $outsideStageRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    $lintAllText = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lint-all.ps1') -Raw
    foreach ($directory in @('bzip2', 'jpeg-6b', 'zlib', 'game-music-emu', 'gdtoa', 'dumb', 'lzma', 'sqlite', 'GeoIP', 'rnnoise')) {
        if ($lintAllText -notmatch [regex]::Escape("'$directory'")) { throw "Full Cppcheck exclusion '$directory' is missing." }
    }
    foreach ($path in @('miniaudio', 'stb_vorbis', 'output_sdl', 'upnpnat')) {
        if ($lintAllText -match [regex]::Escape("'$path'")) { throw "Full Cppcheck exclusion unexpectedly includes '$path'." }
    }

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

    $rawApiBytes = [System.Text.Encoding]::ASCII.GetBytes("raw`nbytes`n")
    $rawApiHash = Get-ByteSHA256 -Bytes $rawApiBytes
    $rawApiRepresentation = Resolve-ApiRootRepresentation -Bytes $rawApiBytes -ExpectedSHA256 $rawApiHash -Path $adapterPath
    if (($rawApiRepresentation.Representation -ne 'raw') -or ($rawApiRepresentation.FinalSHA256 -ne $rawApiHash)) { throw 'API root raw representation was not preserved.' }
    $crlfApiBytes = Convert-LFBytesToCRLFBytes -Bytes $rawApiBytes
    $crlfApiHash = Get-ByteSHA256 -Bytes $crlfApiBytes
    $crlfApiRepresentation = Resolve-ApiRootRepresentation -Bytes $rawApiBytes -ExpectedSHA256 $crlfApiHash -Path $adapterPath
    if (($crlfApiRepresentation.Representation -ne 'lf-to-crlf') -or ($crlfApiRepresentation.FinalSHA256 -ne $crlfApiHash)) { throw 'API root LF-to-CRLF representation was not restored.' }
    $isolatedSourceRoot = Join-Path $fixtureRoot 'isolated/source'
    $isolatedBuildRoot = Join-Path $fixtureRoot 'isolated/build'
    $productionSourceRoot = Join-Path $fixtureRoot 'production/source'
    $productionBuildRoot = Join-Path $fixtureRoot 'production/build'
    $isolatedContext = ConvertTo-ProductionCppcheckVendorDispositionContext -Context ([PSCustomObject]@{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; AnalyzerVersion = 'Cppcheck fixture'; Defines = "TESTDATA=`"$($isolatedSourceRoot.Replace('\', '/'))/tools/testdata/audio`";BUILD=`"$($isolatedBuildRoot.Replace('\', '/'))`"" }) -AnalysisRepositoryRoot $isolatedSourceRoot -AnalysisBuildRoot $isolatedBuildRoot -ProductionRepositoryRoot $productionSourceRoot -ProductionBuildRoot $productionBuildRoot
    if ($isolatedContext.Defines -ne "TESTDATA=`"$($productionSourceRoot.Replace('\', '/'))/tools/testdata/audio`";BUILD=`"$($productionBuildRoot.Replace('\', '/'))`"") { throw "Isolated vendor disposition Defines were not normalized to production paths: $($isolatedContext.Defines)" }
    $linuxContext = ConvertTo-ProductionCppcheckVendorDispositionContext -Context ([PSCustomObject]@{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; AnalyzerVersion = 'Cppcheck fixture'; Defines = 'SOURCE="/analysis/source";CHILD=/analysis/source/include;SIBLING=/analysis/source-extra;EMBEDDED=prefix/analysis/source;BUILD="/analysis/build";BUILD_SIBLING=/analysis/build-extra' }) -AnalysisRepositoryRoot '/analysis/source' -AnalysisBuildRoot '/analysis/build' -ProductionRepositoryRoot '/production/source' -ProductionBuildRoot '/production/build'
    if ($linuxContext.Defines -ne 'SOURCE="/production/source";CHILD=/production/source/include;SIBLING=/analysis/source-extra;EMBEDDED=prefix/analysis/source;BUILD="/production/build";BUILD_SIBLING=/analysis/build-extra') { throw "Synthetic Linux vendor disposition Defines were not normalized safely: $($linuxContext.Defines)" }
    Assert-ExpectedException -Expected 'raw or LF-to-CRLF representation' -Action {
        Resolve-ApiRootRepresentation -Bytes $rawApiBytes -ExpectedSHA256 ('0' * 64) -Path $adapterPath | Out-Null
    }
    Assert-ExpectedException -Expected 'differing source and baseline blob IDs' -Action {
        Assert-ApiRootBlobIdentity -Path $adapterPath -SourceBlobId 'source' -BaseBlobId 'baseline'
    }
    $overlapPolicyPath = Join-Path $fixtureRoot 'overlap-policy.json'
    @{ SchemaVersion = 1; Dispositions = @(@{ Preconditions = @{ ApiRoots = @(@{ Path = $adapterPath; SHA256 = $rawApiHash }) } }) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $overlapPolicyPath -NoNewline
    Assert-ExpectedException -Expected 'overlaps an InputManifest payload' -Action {
        Get-ManifestApiRoots -Files @([PSCustomObject]@{ Path = $adapterPath }) -PolicyPath $overlapPolicyPath | Out-Null
    }
    $conflictingPolicyPath = Join-Path $fixtureRoot 'conflicting-policy.json'
    @{ SchemaVersion = 1; Dispositions = @(
        @{ Preconditions = @{ ApiRoots = @(@{ Path = $adapterPath; SHA256 = $rawApiHash }) } },
        @{ Preconditions = @{ ApiRoots = @(@{ Path = $adapterPath; SHA256 = ('1' * 64) }) } }
    ) } | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $conflictingPolicyPath -NoNewline
    Assert-ExpectedException -Expected 'conflicting expected SHA256 values' -Action {
        Get-ManifestApiRoots -Files @() -PolicyPath $conflictingPolicyPath | Out-Null
    }

    Assert-LintInputRejected -Arguments @('-InputManifest', (Join-Path $fixtureRoot 'missing-manifest.json')) -Expected 'does not exist or is not a file'
    $malformedManifestPath = Join-Path $fixtureRoot 'malformed-manifest.json'
    Set-Content -LiteralPath $malformedManifestPath -Value '{' -NoNewline
    Assert-LintInputRejected -Arguments @('-InputManifest', $malformedManifestPath) -Expected 'is not valid JSON'
    $unsafeManifestPath = Join-Path $fixtureRoot 'unsafe-manifest.json'
    @{ SchemaVersion = 1; BaseCommit = 'HEAD'; SourceCommit = 'HEAD'; Files = @(@{ Path = '../outside.cpp'; Operation = 'Modify'; PayloadPath = 'payload.cpp'; SHA256 = ('0' * 64) }) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $unsafeManifestPath -NoNewline
    Assert-LintInputRejected -Arguments @('-InputManifest', $unsafeManifestPath) -Expected 'contains an unsafe path component'
    $payloadPath = Join-Path $fixtureRoot 'payload.cpp'
    Set-Content -LiteralPath $payloadPath -Value 'captured bytes' -NoNewline
    $hashMismatchManifestPath = Join-Path $fixtureRoot 'hash-mismatch-manifest.json'
    @{ SchemaVersion = 1; BaseCommit = 'HEAD'; SourceCommit = 'HEAD'; Files = @(@{ Path = 'src/example.cpp'; Operation = 'Modify'; PayloadPath = 'payload.cpp'; SHA256 = ('0' * 64) }) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $hashMismatchManifestPath -NoNewline
    Assert-LintInputRejected -Arguments @('-InputManifest', $hashMismatchManifestPath) -Expected 'payload hash mismatch'
    $includeFixtureProjectPath = Join-Path $fixtureRoot 'ordered-includes.vcxproj'
    $includeFixtureSourceRoot = 'C:\fixture\source'
    $includeFixtureBuildRoot = 'C:\fixture\build'
    $includeFixtureXml = '<Project><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><AdditionalIncludeDirectories>C:\external\first;C:\external\second;C:\fixture\source\include;C:\fixture\source-extra\header;C:\fixture\build\gdtoa;C:\fixture\source\src\win32;C:\fixture\build;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $includeFixtureProjectPath -Value $includeFixtureXml -NoNewline
    $normalizedIncludes = Get-NormalizedCppcheckAdditionalIncludeDirectories -ProjectPath $includeFixtureProjectPath -RepositoryRoot $includeFixtureSourceRoot -BuildRoot $includeFixtureBuildRoot
    if ($normalizedIncludes -ne 'C:/external/first;C:/external/second;<source>/include;C:/fixture/source-extra/header;<build>/gdtoa;<source>/src/win32;<build>;%(AdditionalIncludeDirectories)') { throw 'Self-contained include fixture did not preserve ordered external, source, build, and inherited entries.' }
    $normalizedQuotedRoots = ConvertTo-NormalizedCppcheckPathValue -Value 'ROOT="C:/fixture/source";BUILD="C:/fixture/build";SOURCE_CHILD="C:/fixture/source/include";BUILD_CHILD=C:/fixture/build/gdtoa;SIBLING=C:/fixture/source-extra;EMBEDDED=prefixC:/fixture/source;SOURCE_SUFFIX=C:/fixture/source.extra' -RepositoryRoot $includeFixtureSourceRoot -BuildRoot $includeFixtureBuildRoot
    if ($normalizedQuotedRoots -ne 'ROOT="<source>";BUILD="<build>";SOURCE_CHILD="<source>/include";BUILD_CHILD=<build>/gdtoa;SIBLING=C:/fixture/source-extra;EMBEDDED=prefixC:/fixture/source;SOURCE_SUFFIX=C:/fixture/source.extra') { throw 'Cppcheck path normalization did not enforce quoted/list root boundaries.' }
    Assert-ExpectedException -Expected 'AdditionalIncludeDirectories' -Action {
        Assert-EqualCppcheckProjectContext -Expected ([PSCustomObject]@{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'RELEASE'; AdditionalIncludeDirectories = 'C:/external/first;C:/external/second' }) -Actual ([PSCustomObject]@{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'RELEASE'; AdditionalIncludeDirectories = 'C:/external/second;C:/external/first' }) -Description 'external include order fixture'
    }

    $defaultAnalysisContext = [PSCustomObject]@{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'CURRENT_ONLY'; AdditionalIncludeDirectories = '<source>/include' }
    $defaultBaselineContext = [PSCustomObject]@{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'BASELINE_ONLY'; AdditionalIncludeDirectories = '<source>/include' }
    if ($null -ne (Resolve-CppcheckBaselineTarget -TargetName 'new-target.vcxproj' -ProductionProject $null -BaselineProject $null -ProductionContext $null -AnalysisContext $defaultAnalysisContext -BaselineContext $null)) {
        throw 'Default target resolution did not preserve a new current-only target.'
    }
    Assert-ExpectedException -Expected 'missing from the production or baseline project set' -Action {
        Resolve-CppcheckBaselineTarget -TargetName 'manifest-target.vcxproj' -ProductionProject ([PSCustomObject]@{}) -BaselineProject $null -ProductionContext $defaultAnalysisContext -AnalysisContext $defaultAnalysisContext -BaselineContext $null -RequireManifestParity | Out-Null
    }
    Assert-ExpectedException -Expected 'Defines' -Action {
        Resolve-CppcheckBaselineTarget -TargetName 'manifest-target.vcxproj' -ProductionProject ([PSCustomObject]@{}) -BaselineProject ([PSCustomObject]@{}) -ProductionContext $defaultAnalysisContext -AnalysisContext $defaultBaselineContext -BaselineContext $defaultBaselineContext -RequireManifestParity | Out-Null
    }

    $invokeBaselineTargetResolution = {
        param(
            [object]$ProductionProject,
            [object]$BaselineProject
        )

        $productionContext = [PSCustomObject]@{ Name = 'production' }
        $analysisContext = [PSCustomObject]@{ Name = 'analysis' }
        $baselineContext = [PSCustomObject]@{ Name = 'baseline' }
        Resolve-CppcheckBaselineTarget `
            -TargetName 'conditional-context-fixture.vcxproj' `
            -ProductionProject $ProductionProject `
            -BaselineProject $BaselineProject `
            -ProductionContext $(if ($ProductionProject) { $productionContext }) `
            -AnalysisContext $analysisContext `
            -BaselineContext $(if ($BaselineProject) { $baselineContext })
    }

    $presentBaselineProject = [PSCustomObject]@{ ProjectPath = 'baseline.vcxproj' }
    $presentBaselineResult = & $invokeBaselineTargetResolution ([PSCustomObject]@{ ProjectPath = 'production.vcxproj' }) $presentBaselineProject
    if ($presentBaselineResult -ne $presentBaselineProject) {
        throw 'Conditional Cppcheck context fixture did not preserve the baseline project.'
    }
    if ($null -ne (& $invokeBaselineTargetResolution $null $null)) {
        throw 'Conditional Cppcheck context fixture did not preserve absent contexts.'
    }

    Assert-ClassifiedFailureEvidence -FixtureRoot $fixtureRoot -Error 'New diagnostic fingerprints.' -NewCount 1 -UnresolvedVendorCount 0
    Assert-ClassifiedFailureEvidence -FixtureRoot $fixtureRoot -Error 'Unresolved vendor diagnostics.' -NewCount 0 -UnresolvedVendorCount 1
    Assert-ManifestDoesNotReconfigureBaseline

    $zeroTargetPayloadPath = Join-Path $fixtureRoot 'zero-target-payload.txt'
    Set-Content -LiteralPath $zeroTargetPayloadPath -Value 'captured bytes' -NoNewline
    $zeroTargetManifestPath = Join-Path $fixtureRoot 'zero-target-manifest.json'
    $zeroTargetManifest = @{
        SchemaVersion = 1
        BaseCommit = 'HEAD'
        SourceCommit = 'HEAD'
        Files = @(@{ Path = 'src/fixture.txt'; Operation = 'Modify'; PayloadPath = 'zero-target-payload.txt'; SHA256 = (Get-FileHash -LiteralPath $zeroTargetPayloadPath -Algorithm SHA256).Hash })
        CMake = @{ Generator = 'fixture'; Platform = 'x64'; Toolset = 'v143'; CacheSHA256 = ('0' * 64); ProductionContext = @{}; Settings = @() }
    }
    $zeroTargetManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $zeroTargetManifestPath -NoNewline
    Assert-LintInputRejected -Arguments @('-InputManifest', $zeroTargetManifestPath, '-PreflightOnly') -Expected 'contains no C/C++ translation units'
    $outsidePayloadRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('zandronum-lint-payload-outside-' + [guid]::NewGuid().ToString('N'))
    $junctionPath = Join-Path $fixtureRoot 'payload-junction'

    try {
        New-Item -ItemType Directory -Force -Path $outsidePayloadRoot | Out-Null
        Set-Content -LiteralPath (Join-Path $outsidePayloadRoot 'payload.cpp') -Value 'captured bytes' -NoNewline
        New-Item -ItemType Junction -Path $junctionPath -Target $outsidePayloadRoot -ErrorAction Stop | Out-Null
        $junctionPayloadHash = (Get-FileHash -LiteralPath (Join-Path $junctionPath 'payload.cpp') -Algorithm SHA256).Hash
        $junctionManifestPath = Join-Path $fixtureRoot 'junction-manifest.json'
        @{ SchemaVersion = 1; BaseCommit = 'HEAD'; SourceCommit = 'HEAD'; Files = @(@{ Path = 'src/example.cpp'; Operation = 'Modify'; PayloadPath = 'payload-junction/payload.cpp'; SHA256 = $junctionPayloadHash }) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $junctionManifestPath -NoNewline
        Assert-LintInputRejected -Arguments @('-InputManifest', $junctionManifestPath) -Expected 'must not traverse a reparse point'
    }
    catch [System.UnauthorizedAccessException] {
        Write-Host 'payload parent junction fixture: skipped (permission unavailable)'
    }
    finally {
        Remove-Item -LiteralPath $junctionPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $outsidePayloadRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ((Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts).Accepted.Count -ne 1) { throw 'Exact disposition was not accepted.' }

    $baselineDiagnostic = ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t10`t4`twarning`tid`tmessage") -ExitCode 0 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit $adapterPath
    $otherTranslationUnitDiagnostic = ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t10`t4`twarning`tid`tmessage") -ExitCode 0 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit 'src/other.cpp'
    if ($baselineDiagnostic.Fingerprint -ne $otherTranslationUnitDiagnostic.Fingerprint) { throw 'Baseline fingerprint changed when only the translation unit changed.' }
    if ((Get-CppcheckVendorDispositionResult -Diagnostics @($otherTranslationUnitDiagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts).Accepted.Count -ne 0) { throw 'Disposition accepted a different translation unit.' }
    $movedVendorDiagnostic = ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t11`t4`twarning`tid`tmessage") -ExitCode 1 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit $adapterPath
    $movedComparison = Get-CppcheckBaselineComparisonResult -BaselineDiagnostics @($baselineDiagnostic) -HeadDiagnostics @($movedVendorDiagnostic) -UnacceptedDiagnostics @($movedVendorDiagnostic)
    if (($movedComparison.UnresolvedVendor.Count -ne 1) -or ($movedComparison.New.Count -ne 0)) { throw 'Moved unproven vendor diagnostic was not separated from legacy baseline comparison.' }

    $emptyParserResult = @(ConvertFrom-CppcheckProjectOutput -Output @() -ExitCode 0 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit $adapterPath)
    if ($emptyParserResult.Count -ne 0) { throw 'Empty Cppcheck output did not return an empty diagnostic collection.' }
    $singleParserResult = @(ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t10`t4`twarning`tsingle`tsingle message") -ExitCode 0 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit $adapterPath)
    if (($singleParserResult.Count -ne 1) -or ($singleParserResult[0].Identifier -ne 'single')) { throw 'Single Cppcheck diagnostic collection shape changed.' }
    $multipleParserResult = @(ConvertFrom-CppcheckProjectOutput -Output @("$(Join-Path $fixtureRoot $vendorPath)`t10`t4`twarning`tfirst`tfirst message", "$(Join-Path $fixtureRoot $vendorPath)`t11`t5`twarning`tsecond`tsecond message", "$(Join-Path $fixtureRoot $vendorPath)`t10`t4`twarning`tfirst`tfirst message") -ExitCode 0 -RepositoryRoot $fixtureRoot -BuildRoot '' -TargetName 'zdoom' -TranslationUnit $adapterPath)
    if ((@($multipleParserResult | ForEach-Object { $_.Identifier }) -join '|') -ne 'first|second|first') { throw 'Multiple Cppcheck diagnostics lost order or duplicates.' }

    $unmatchedDiagnostic = [PSCustomObject]@{ Target = 'unscanned'; TranslationUnit = 'src/local.cpp'; RelativePath = 'src/local.cpp'; Line = 1; Column = 1; Severity = 'warning'; Identifier = 'local'; Message = 'local message' }
    $emptyDispositionResult = Get-CppcheckVendorDispositionResult -Diagnostics @() -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts
    if (($emptyDispositionResult.Raw.Count -ne 0) -or ($emptyDispositionResult.Accepted.Count -ne 0) -or ($emptyDispositionResult.Unaccepted.Count -ne 0)) { throw 'Empty disposition result collection shape changed.' }
    $unmatchedOrderedDiagnostic = [PSCustomObject]@{ Target = 'unscanned'; TranslationUnit = 'src/local.cpp'; RelativePath = 'src/local.cpp'; Line = 2; Column = 1; Severity = 'warning'; Identifier = 'local-second'; Message = 'local message second' }
    $multipleDispositionResult = Get-CppcheckVendorDispositionResult -Diagnostics @($diagnostic, $unmatchedDiagnostic, $diagnostic, $unmatchedOrderedDiagnostic) -PolicyPath $policyPath -RepositoryRoot $fixtureRoot -Contexts $contexts
    if (($multipleDispositionResult.Accepted.Count -ne 2) -or ($multipleDispositionResult.Accepted[0].Diagnostic -ne $diagnostic) -or ($multipleDispositionResult.Accepted[1].Diagnostic -ne $diagnostic)) { throw 'Accepted disposition collection lost order or duplicates.' }
    if ((@($multipleDispositionResult.Unaccepted | ForEach-Object { $_.Identifier }) -join '|') -ne 'local|local-second') { throw 'Unaccepted disposition collection lost order.' }

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
    $projectXml = '<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions><AdditionalIncludeDirectories>C:\isolated\source\include;C:\isolated\source-extra\header;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories></ClCompile></ItemDefinitionGroup></Project>'
    Set-Content -LiteralPath $projectPath -Value $projectXml -NoNewline
    if ((Get-CppcheckVendorDispositionContext -ProjectPath $projectPath -TargetName 'fixture' -AnalyzerVersion 'Cppcheck 2.21.0').Defines -ne 'RELEASE') { throw 'Project context with additional include directories was not extracted.' }
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