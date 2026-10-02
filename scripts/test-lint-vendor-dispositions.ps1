param([string]$PreparedNativeFixtureRoot = '', [string]$PreparedNativeCommitFile = 'input-commits.json')

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
    $output = @(& pwsh -NoProfile -File $lintScript @Arguments 2>&1 | Out-String -Width 4096)

    if ($LASTEXITCODE -ne 1) {
        throw "lint.ps1 expected exit code 1 for input rejection, got $LASTEXITCODE."
    }

    if (((($output -join [Environment]::NewLine) -replace '\|\s*', '') -replace '\s+', ' ') -notmatch [regex]::Escape($Expected)) {
        throw "lint.ps1 did not report expected input rejection '$Expected'."
    }
}

function Import-LintFunction {
    param(
        [string]$Name,
        [string]$LintScript = (Join-Path $PSScriptRoot 'lint.ps1'),
        [string]$ImportedName = $Name
    )

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

    $definition = $function.Extent.Text -replace ("(?m)^function\s+" + [regex]::Escape($Name) + '\b'), "function global:$ImportedName"
    Invoke-Expression $definition
}

function New-CppcheckSchedulerFixture {
    param([string]$FixtureRoot, [int[]]$Delays = @(1000, 100, 400), [string]$Role = 'head')

    $root = Join-Path $FixtureRoot $Role
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $childPath = Join-Path $root 'fake analyzer.ps1'
    $child = @'
param([string]$ConfigPath, [string]$Sentinel)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
@{ PID = $PID; Start = [DateTime]::UtcNow.ToString('o'); Cwd = (Get-Location).Path; Sentinel = $Sentinel } | ConvertTo-Json | Set-Content -LiteralPath $config.StartPath
[Console]::Out.Write($config.Stdout)
[Console]::Error.Write($config.Stderr)
$null = [Threading.Tasks.Task]::Delay([int]$config.Delay).GetAwaiter().GetResult()
@{ PID = $PID; End = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath $config.EndPath
exit ([int]$config.ExitCode)
'@
    Set-Content -LiteralPath $childPath -Value $child -NoNewline
    $hostPath = (Get-Process -Id $PID).Path
    $sentinel = 'space "quote"' + "`t" + [char]0x65e5 + [char]0x672c
    for ($index = 0; $index -lt $Delays.Count; $index++) {
        $unit = "unit-$index.cpp"
        $message = "message-$index " + [char]0x65e5 + [char]0x672c
        $line = (Join-Path $root $unit) + "`t1`t2`twarning`tfixture`t$message"
        $configPath = Join-Path $root "config-$index.json"
        @{ Delay = $Delays[$index]; Stdout = "progress-$index`n`n"; Stderr = "$line`n$line"; ExitCode = 1; StartPath = (Join-Path $root "start-$index.json"); EndPath = (Join-Path $root "end-$index.json") } | ConvertTo-Json | Set-Content -LiteralPath $configPath
        $descriptor = New-CppcheckDescriptor -CppcheckPath $hostPath -ProjectPath (Join-Path $root 'compile_commands.json') -CachePath (Join-Path $root "cache/$unit") -RepositoryRoot $root -BuildRoot $root -TargetName 'fixture' -TranslationUnit $unit -Index $index -UseProjectConfiguration:$false
        $descriptor.Path = $hostPath
        $descriptor.Arguments = @('-NoProfile', '-File', $childPath, '-ConfigPath', $configPath, '-Sentinel', $sentinel)
        $descriptor.WorkingDirectory = $root
        $descriptor
    }
}

function Assert-CppcheckProcessIntervals {
    param([object[]]$Invocations, [int]$Limit, [int]$Minimum = 1)

    $events = @(foreach ($invocation in $Invocations) {
        if (($null -eq $invocation.ProcessId) -or (-not $invocation.StdoutComplete) -or (-not $invocation.StderrComplete)) { throw 'Incomplete native invocation.' }
        [PSCustomObject]@{ Time = ([DateTime]$invocation.StartedAtUtc).ToUniversalTime(); Change = 1 }
        [PSCustomObject]@{ Time = ([DateTime]$invocation.EndedAtUtc).ToUniversalTime(); Change = -1 }
        $liveProcess = Get-Process -Id $invocation.ProcessId -ErrorAction SilentlyContinue
        if ($null -ne $liveProcess) {
            try {
                $liveStartTime = $liveProcess.StartTime
                if ($null -ne $liveStartTime -and -not $liveProcess.HasExited -and $liveStartTime.ToUniversalTime() -eq ([DateTime]$invocation.StartedAtUtc).ToUniversalTime()) {
                    throw "Owned PID $($invocation.ProcessId) survived the batch."
                }
            }
            finally { $liveProcess.Dispose() }
        }
    })
    $count = 0
    $maximum = 0
    foreach ($processEvent in @($events | Sort-Object Time, Change)) {
        $count += $processEvent.Change
        $maximum = [Math]::Max($maximum, $count)
    }
    if (($maximum -lt $Minimum) -or ($maximum -gt $Limit) -or ($count -ne 0)) { throw "Observed process overlap $maximum outside $Minimum..$Limit." }
    return $maximum
}

function Assert-CppcheckSchedulerTransport {
    param([string]$FixtureRoot, [string]$OldDefinitionsPath = '')

    $global:utf8 = [Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = $utf8
    $global:generatedBundlePaths = @()
    $root = Join-Path $FixtureRoot ('scheduler space ' + [char]0x65e5)
    $descriptors = @(New-CppcheckSchedulerFixture -FixtureRoot $root)
    $expected = $null
    $completionOrders = @()
    foreach ($jobs in @(1, 2, 9)) {
        $evidenceRoot = Join-Path $root "jobs-$jobs"
        New-Item -ItemType Directory -Force -Path (Join-Path $evidenceRoot 'commands') | Out-Null
        $evidence = [PSCustomObject]@{ Root = $evidenceRoot; Counter = 40 }
        $results = @(Invoke-CppcheckProcessBatch -Descriptors $descriptors -CppcheckJobs $jobs -Evidence $evidence)
        $diagnostics = @(foreach ($descriptor in $descriptors) {
            $result = $results[$descriptor.Index]
            ConvertFrom-CppcheckProjectOutput -Output $result.Output -ExitCode $result.ExitCode -RepositoryRoot $descriptor.RepositoryRoot -BuildRoot $descriptor.BuildRoot -TargetName $descriptor.TargetName -TranslationUnit $descriptor.TranslationUnit
        })
        @{ Results = $results; Diagnostics = $diagnostics } | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $evidenceRoot 'results.json')
        $actual = $diagnostics | ConvertTo-Json -Depth 8 -Compress
        if ($jobs -eq 1) { $expected = $actual }
        elseif ($actual -cne $expected) { throw 'Worker count changed full diagnostic fields/order/duplicates.' }
        $invocations = @(Get-ChildItem (Join-Path $evidenceRoot 'commands') -Filter '*.invocation.json' | Sort-Object Name | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
        $null = Assert-CppcheckProcessIntervals -Invocations $invocations -Limit ([Math]::Min($jobs, 3)) -Minimum $(if ($jobs -eq 1) { 1 } else { 2 })
        if (($evidence.Counter -ne 43) -or ($invocations.Count -ne 3) -or (@(Get-ChildItem (Join-Path $evidenceRoot 'commands') -Filter '*.raw.txt').Count -ne 3)) { throw 'Counter/raw/invocation mapping failed.' }
        foreach ($descriptor in $descriptors) {
            $index = $descriptor.Index
            $invocation = $invocations[$index]
            $config = Get-Content $descriptor.Arguments[4] -Raw | ConvertFrom-Json
            $start = Get-Content $config.StartPath -Raw | ConvertFrom-Json
            if (($invocation.Stdout -cne $config.Stdout) -or ($invocation.Stderr -cne $config.Stderr) -or ($start.Sentinel -cne $descriptor.Arguments[6]) -or ($start.Cwd -cne $descriptor.WorkingDirectory)) { throw 'Stream/ArgumentList/cwd corruption.' }
            $raw = Get-Content (Get-ChildItem (Join-Path $evidenceRoot 'commands') -Filter '*.raw.txt' | Sort-Object Name)[$index].FullName -Raw
            if ($raw -cne ($results[$index].Output -join [Environment]::NewLine)) { throw 'TU raw output was misassociated.' }
        }
        $completionOrders += ,@($results | Sort-Object EndedAtUtc | ForEach-Object { $_.Index })
    }
    if (($completionOrders[0] -join ',') -eq ($completionOrders[1] -join ',')) { throw 'Fixture did not produce a different completion order.' }
    $reverse = @(New-CppcheckSchedulerFixture -FixtureRoot (Join-Path $root 'reverse') -Delays @(100, 1000, 400))
    $reverseResults = @(Invoke-CppcheckProcessBatch -Descriptors $reverse -CppcheckJobs 2)
    $reverseOrder = @($reverseResults | Sort-Object EndedAtUtc | ForEach-Object { $_.Index })
    if (($reverseOrder -join ',') -eq ($completionOrders[1] -join ',')) { throw 'Swapping delays did not change completion order.' }
    @{ Orders = $completionOrders; Reverse = $reverseResults } | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $root 'completion-orders.json')
    $withoutEvidence = @(Invoke-CppcheckBatch -Descriptors $descriptors -CppcheckJobs 2)
    if (($withoutEvidence | ConvertTo-Json -Depth 8 -Compress) -cne $expected) { throw 'Evidence-disabled result differs.' }
    $single = @(Invoke-CppcheckProcessBatch -Descriptors @($descriptors[0]) -CppcheckJobs 9)
    $null = Assert-CppcheckProcessIntervals -Invocations $single -Limit 1
    if (@(Invoke-CppcheckProcessBatch -Descriptors @() -CppcheckJobs 2).Count -ne 0) { throw 'Empty batch started a child.' }
    Assert-ExpectedException -Expected 'duplicate cache leaf' -Action {
        $duplicate = $descriptors[1].PSObject.Copy()
        $duplicate.CachePath = $descriptors[0].CachePath
        Invoke-CppcheckProcessBatch -Descriptors @($descriptors[0], $duplicate) -CppcheckJobs 2 | Out-Null
    }
    if (-not [string]::IsNullOrWhiteSpace($OldDefinitionsPath)) {
        Import-LintFunction -Name 'Invoke-LintChild' -LintScript $OldDefinitionsPath -ImportedName 'Invoke-OldLintChild'
        $oldResults = @(foreach ($descriptor in $descriptors) { Invoke-OldLintChild -Path $descriptor.Path -Arguments $descriptor.Arguments -Label $descriptor.Label -WorkingDirectory $descriptor.WorkingDirectory })
        $newResults = @(Invoke-CppcheckProcessBatch -Descriptors $descriptors -CppcheckJobs 1)
        @{ Old = $oldResults; New = $newResults } | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $root 'V1-three-TU.json')
        for ($index = 0; $index -lt 3; $index++) {
            $oldDiagnostics = @(ConvertFrom-CppcheckProjectOutput -Output $oldResults[$index].Output -ExitCode $oldResults[$index].ExitCode -RepositoryRoot $descriptors[$index].RepositoryRoot -BuildRoot $descriptors[$index].BuildRoot -TargetName 'fixture' -TranslationUnit $descriptors[$index].TranslationUnit)
            $newDiagnostics = @(ConvertFrom-CppcheckProjectOutput -Output $newResults[$index].Output -ExitCode $newResults[$index].ExitCode -RepositoryRoot $descriptors[$index].RepositoryRoot -BuildRoot $descriptors[$index].BuildRoot -TargetName 'fixture' -TranslationUnit $descriptors[$index].TranslationUnit)
            Assert-EqualCppcheckRunResult -ExpectedDiagnostics $oldDiagnostics -ActualDiagnostics $newDiagnostics -ExpectedEvidence ([PSCustomObject]@{ ExitCode = $oldResults[$index].ExitCode; RawDiagnostics = @($oldResults[$index].Output | Where-Object { $_ -match "`t\d+`t\d+`t" }) }) -ActualEvidence ([PSCustomObject]@{ ExitCode = $newResults[$index].ExitCode; RawDiagnostics = @($newResults[$index].Output | Where-Object { $_ -match "`t\d+`t\d+`t" }) }) -Description 'old/new three-TU serial transport'
        }
    }
    $large = @(New-CppcheckSchedulerFixture -FixtureRoot (Join-Path $root 'large') -Delays @(0))
    $configPath = $large[0].Arguments[4]
    $config = Get-Content $configPath -Raw | ConvertFrom-Json
    $config.Stdout = (('out ' + "`t" + [char]0x65e5 + "`n`n") * 20000) + 'last stdout'
    $config.Stderr = (('err ' + "`t" + [char]0x672c + "`n") * 20000) + $config.Stderr
    $config | ConvertTo-Json | Set-Content $configPath
    $largeEvidenceRoot = Join-Path $root 'large/evidence'
    New-Item -ItemType Directory -Force -Path (Join-Path $largeEvidenceRoot 'commands') | Out-Null
    $largeResults = @(Invoke-CppcheckProcessBatch -Descriptors $large -CppcheckJobs 2 -Evidence ([PSCustomObject]@{ Root = $largeEvidenceRoot; Counter = 0 }))
    if (($largeResults[0].Stdout -cne $config.Stdout) -or ($largeResults[0].Stderr -cne $config.Stderr)) { throw 'Pipe-buffer output was truncated or corrupted.' }
    $largeDiagnostics = @(ConvertFrom-CppcheckProjectOutput -Output $largeResults[0].Output -ExitCode 1 -RepositoryRoot $large[0].RepositoryRoot -BuildRoot $large[0].BuildRoot -TargetName 'fixture' -TranslationUnit $large[0].TranslationUnit)
    if ($largeDiagnostics.Count -ne 2 -or $largeDiagnostics[0].Message -cne $largeDiagnostics[1].Message) { throw 'Large output lost duplicate diagnostics.' }
    $config.Stdout = 'cppcheck: error: injected stdout fatal'
    $config.ExitCode = 0
    $config | ConvertTo-Json | Set-Content $configPath
    Assert-ExpectedException -Expected 'tool or configuration error' -Action { Invoke-CppcheckBatch -Descriptors $large -CppcheckJobs 2 | Out-Null }
    Write-Host 'V1/V2/V4/V5 scheduler transport: passed.'
}

function Assert-CppcheckBatchIntegration {
    param([string]$LintScript = (Join-Path $PSScriptRoot 'lint.ps1'))

    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($LintScript, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw $errors[0].Message }
    $targetLoop = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.ForEachStatementAst] -and $node.Variable.VariablePath.UserPath -eq 'project' -and $node.Condition.Extent.Text -eq '$analysisProjects' }, $true) | Select-Object -Last 1
    if ($null -eq $targetLoop) { throw 'Production target loop missing.' }
    $calls = @($targetLoop.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Invoke-CppcheckBatch' }, $true))
    if ($calls.Count -ne 2 -or $calls[0].Extent.Text -notmatch '\$headDescriptors.ToArray\(\)' -or $calls[1].Extent.Text -notmatch '\$baselineDescriptors.ToArray\(\)') { throw 'Production HEAD/baseline batch connections missing or reordered.' }
    $loops = @($targetLoop.FindAll({ param($node) $node -is [System.Management.Automation.Language.ForEachStatementAst] -and $node.Variable.VariablePath.UserPath -eq 'translationUnit' }, $true))
    if ($loops.Count -ne 2 -or $loops[0].Condition.Extent.Text -ne '$project.Files' -or $loops[1].Condition.Extent.Text -ne '@($baselineProject.Sources.Keys | Sort-Object)') { throw 'Production TU selection/order changed.' }
    if (($loops[0].Extent.EndOffset -ge $calls[0].Extent.StartOffset) -or ($calls[0].Extent.EndOffset -ge $loops[1].Extent.StartOffset) -or ($loops[1].Extent.EndOffset -ge $calls[1].Extent.StartOffset)) { throw 'Production role barrier moved inside a TU loop.' }
    foreach ($role in @('head', 'baseline')) {
        $loop = $loops[$(if ($role -eq 'head') { 0 } else { 1 })]
        $text = $loop.Extent.Text
        if ($text -notmatch 'New-CppcheckDescriptor' -or $text -match 'Invoke-CppcheckProject|Invoke-CppcheckBatch' -or $text -notmatch ([regex]::Escape('-CachePath $' + $role + 'Cache'))) { throw "Production $role descriptor preparation differs." }
    }
    Write-Host 'V3 production AST connections/role barrier: passed.'
}

function Assert-CppcheckRoleClassification {
    param([string]$FixtureRoot)

    $root = Join-Path $FixtureRoot 'classification'
    $head = @(New-CppcheckSchedulerFixture -FixtureRoot $root -Delays @(300, 0, 100) -Role 'head')
    $baseline = @(New-CppcheckSchedulerFixture -FixtureRoot $root -Delays @(0, 300, 100) -Role 'baseline')
    $vendorPath = 'src/sound/thirdparty/miniaudio/miniaudio.h'
    $adapterPath = 'src/sound/audio_decoder_miniaudio.cpp'
    foreach ($role in @($head, $baseline)) {
        New-Item -ItemType Directory -Force -Path (Split-Path (Join-Path $role[0].RepositoryRoot $vendorPath)) | Out-Null
        Set-Content (Join-Path $role[0].RepositoryRoot $vendorPath) 'vendor' -NoNewline
        Set-Content (Join-Path $role[0].RepositoryRoot $adapterPath) 'ma_decoder_init_memory()' -NoNewline
        foreach ($descriptor in $role) { $descriptor.TargetName = 'zdoom' }
        $role[0].TranslationUnit = $adapterPath
    }
    $policyPath = Join-Path $root 'policy.json'
    $record = @{ Path = $vendorPath; SHA256 = (Get-VendorPolicyFileHash -RepositoryRoot $head[0].RepositoryRoot -RelativePath $vendorPath); Line = 10; Column = 4; Severity = 'warning'; Identifier = 'id'; Message = 'message'; Target = 'zdoom'; TranslationUnit = $adapterPath; AnalyzerVersion = 'Cppcheck fixture'; Reason = 'fixture'; Preconditions = @{ Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'Release'; ApiTokens = @('ma_decoder_init_memory'); ApiRoots = @(@{ Path = $adapterPath; SHA256 = (Get-VendorPolicyFileHash -RepositoryRoot $head[0].RepositoryRoot -RelativePath $adapterPath) }) } }
    @{ SchemaVersion = 1; Dispositions = @($record) } | ConvertTo-Json -Depth 8 | Set-Content $policyPath
    $contexts = @{ zdoom = @{ AnalyzerVersion = 'Cppcheck fixture'; Compiler = 'MSVC'; Toolset = 'v143'; Abi = 'x64'; Configuration = 'Release|x64'; Defines = 'Release' } }
    foreach ($role in @($head, $baseline)) {
        for ($index = 0; $index -lt 3; $index++) {
            $descriptor = $role[$index]
            $config = Get-Content $descriptor.Arguments[4] -Raw | ConvertFrom-Json
            $roleRoot = $descriptor.RepositoryRoot
            if ($index -eq 0) { $config.Stderr = (Join-Path $roleRoot $vendorPath) + "`t10`t4`twarning`tid`tmessage" }
            elseif ($index -eq 1) {
                $config.Stderr = (Join-Path $roleRoot 'src/local.cpp') + "`t2`t1`twarning`tcommon`tunchanged"
                if ($roleRoot -eq $head[0].RepositoryRoot) { $config.Stderr += "`n" + (Join-Path $roleRoot 'src/local.cpp') + "`t3`t1`twarning`tnew`tnew message" }
            }
            elseif ($roleRoot -eq $head[0].RepositoryRoot) { $config.Stderr = (Join-Path $roleRoot $vendorPath) + "`t11`t4`twarning`tunresolved`tunproven vendor" }
            else { $config.Stderr = (Join-Path $roleRoot 'src/local.cpp') + "`t4`t1`twarning`tremoved`tbaseline only" }
            $config | ConvertTo-Json | Set-Content $descriptor.Arguments[4]
        }
    }
    $expected = $null
    foreach ($jobs in @(1, 2)) {
        $evidenceRoot = Join-Path $root "jobs-$jobs"
        New-Item -ItemType Directory -Force -Path (Join-Path $evidenceRoot 'commands') | Out-Null
        $evidence = [PSCustomObject]@{ Root = $evidenceRoot; Counter = 0 }
        $headDiagnostics = @(Invoke-CppcheckBatch -Descriptors $head -CppcheckJobs $jobs -Evidence $evidence)
        $baselineDiagnostics = @(Invoke-CppcheckBatch -Descriptors $baseline -CppcheckJobs $jobs -Evidence $evidence)
        $dispositions = Get-CppcheckVendorDispositionResult -Diagnostics $headDiagnostics -PolicyPath $policyPath -RepositoryRoot $head[0].RepositoryRoot -Contexts $contexts
        $comparison = Get-CppcheckBaselineComparisonResult -BaselineDiagnostics $baselineDiagnostics -HeadDiagnostics $headDiagnostics -UnacceptedDiagnostics $dispositions.Unaccepted
        $classification = @{ RawHead = $headDiagnostics; AcceptedVendor = $dispositions.Accepted; BaselineInput = $baselineDiagnostics; Comparison = $comparison; Gate = $(if ($comparison.UnresolvedVendor.Count) { 'failed-unresolved-vendor' } elseif ($comparison.New.Count) { 'failed-new' } else { 'passed' }) }
        $classification | ConvertTo-Json -Depth 15 | Set-Content (Join-Path $evidenceRoot 'classification.json')
        $actual = $classification | ConvertTo-Json -Depth 15 -Compress
        if ($jobs -eq 1) { $expected = $actual }
        elseif ($expected -cne $actual) { throw 'Role/classification full content changed with Jobs.' }
        if ($headDiagnostics.Count -ne 4 -or $dispositions.Accepted.Count -ne 1 -or $comparison.Baseline.Count -ne 3 -or $comparison.Unchanged.Count -ne 1 -or $comparison.BaselineOnly.Count -ne 2 -or $comparison.New.Count -ne 2 -or $comparison.UnresolvedVendor.Count -ne 1) { throw 'Classification fixture did not cover every category.' }
        $invocations = @(Get-ChildItem (Join-Path $evidenceRoot 'commands') -Filter '*.invocation.json' | Sort-Object Name | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
        $headEnd = ($invocations[0..2] | Sort-Object EndedAtUtc | Select-Object -Last 1).EndedAtUtc
        $baselineStart = ($invocations[3..5] | Sort-Object StartedAtUtc | Select-Object -First 1).StartedAtUtc
        if ([DateTime]$baselineStart -lt [DateTime]$headEnd) { throw 'HEAD/baseline process lifetimes overlapped.' }
    }
    Write-Host 'V3 full role/classification comparison: passed.'
}

function Assert-CppcheckSchedulerFailures {
    param([string]$FixtureRoot, [string]$LintScript = (Join-Path $PSScriptRoot 'lint.ps1'), [string]$TestScript = $PSCommandPath)

    $root = Join-Path $FixtureRoot 'scheduler-failures'
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $childPath = Join-Path $root 'outer-finalization.ps1'
    $child = @'
param([string]$Case, [string]$Root, [string]$LintScript, [string]$TestScript)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($TestScript, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw $errors[0].Message }
$import = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Import-LintFunction' }, $true) | Select-Object -First 1
Invoke-Expression $import.Extent.Text
Import-LintFunction -Name 'New-CppcheckSchedulerFixture' -LintScript $TestScript
foreach ($name in @('New-CppcheckDescriptor', 'Invoke-CppcheckProcessBatch', 'Invoke-CppcheckBatch', 'Save-LintChildEvidence', 'Save-LintEvidenceResult', 'Complete-LintFinalization', 'Complete-LintTemporaryCleanup')) { Import-LintFunction -Name $name -LintScript $LintScript }
$scripts = Split-Path $LintScript
. (Join-Path $scripts 'cppcheck-cache.ps1')
. (Join-Path $scripts 'cppcheck-vendor-policy.ps1')
Import-LintFunction -Name 'Get-RepositoryRelativePath' -LintScript $TestScript
$global:utf8 = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $utf8
$global:generatedBundlePaths = @()
$evidenceRoot = Join-Path $Root 'evidence'
New-Item -ItemType Directory -Force -Path (Join-Path $evidenceRoot 'commands') | Out-Null
$analyzer = (Get-Command cppcheck).Source
@{ ScriptPath = $LintScript; ScriptSHA256 = (Get-FileHash $LintScript).Hash; PolicyPath = (Join-Path $scripts 'cppcheck-vendor-dispositions.json'); PolicySHA256 = (Get-FileHash (Join-Path $scripts 'cppcheck-vendor-dispositions.json')).Hash; AnalyzerPath = $analyzer; AnalyzerSHA256 = (Get-FileHash $analyzer).Hash; AnalyzerVersion = (& $analyzer --version) } | ConvertTo-Json | Set-Content (Join-Path $evidenceRoot 'tool-provenance.json')
$evidence = [PSCustomObject]@{ Root = $evidenceRoot; Counter = 0 }
$cacheRoot = Join-Path $Root 'cache'
$lock = Enter-CppcheckCacheLock -CacheRoot $cacheRoot -Name 'failure-fixture'
$stage = New-CppcheckDisposableStage -CacheRoot $cacheRoot -Name 'failure-stage'
$repo = Join-Path $Root 'repo'
New-Item -ItemType Directory -Force -Path $repo | Out-Null
& git -C $repo init -q
if ($LASTEXITCODE) { throw 'Fixture git init failed.' }
Set-Content (Join-Path $repo 'input.txt') 'fixture'
& git -C $repo add input.txt
& git -C $repo -c user.name=Fixture -c user.email=fixture@example.invalid commit -qm fixture
if ($LASTEXITCODE) { throw 'Fixture git commit failed.' }
$baselineRoot = Join-Path $stage 'baseline'
& git -C $repo worktree add --detach -q $baselineRoot HEAD
if ($LASTEXITCODE) { throw 'Fixture worktree add failed.' }
$descriptors = @(New-CppcheckSchedulerFixture -FixtureRoot (Join-Path $stage 'inputs') -Delays @(100, 1200, 800))
$configPath = $descriptors[0].Arguments[4]
$config = Get-Content $configPath -Raw | ConvertFrom-Json
switch ($Case) {
    'exit2' { $config.ExitCode = 2; $config.Stderr = ''; $config.Stdout = 'injected exit 2' }
    'exit1-empty' { $config.ExitCode = 1; $config.Stderr = '' }
    'fatal' { $config.ExitCode = 0; $config.Stdout = 'cppcheck: error: injected stdout fatal' }
    'missing' { $config.Stderr = (Join-Path $descriptors[0].RepositoryRoot 'unsupported.cpp') + "`t1`t1`terror`tmissingFile`tinjected missing input" }
    'start' { $config.Delay = 8000; $descriptors[1].Path = Join-Path $Root 'not-an-analyzer.exe' }
    'evidence' {
        foreach ($index in @(0, 1)) {
            $prefix = '{0:D4}-{1}' -f ($index + 1), $descriptors[$index].Label
            New-Item -ItemType Directory -Path (Join-Path $evidenceRoot "commands/$prefix.raw.txt") | Out-Null
        }
    }
}
$config | ConvertTo-Json | Set-Content $configPath
$lifecycle = [Collections.Generic.List[object]]::new()
$cleanupState = [PSCustomObject]@{ Completed = $false }
$cleanupAction = {
    $invocations = @(Get-ChildItem (Join-Path $evidenceRoot 'commands') -Filter '*.invocation.json' -File | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
    $starts = @(Get-ChildItem (Join-Path $stage 'inputs') -Filter 'start-*.json' -Recurse -File | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
    $ownedPids = @(@($invocations | ForEach-Object { $_.ProcessId }) + @($starts | ForEach-Object { $_.PID }) | Sort-Object -Unique)
    foreach ($ownedPid in $ownedPids) {
        if (Get-Process -Id $ownedPid -ErrorAction SilentlyContinue) { throw "Owned PID $ownedPid survived before outer cleanup." }
    }
    $lifecycle.Add(@{ Event = 'owned-stopped-before-cleanup'; Time = [DateTime]::UtcNow.ToString('o'); PIDs = $ownedPids; Invocations = $invocations; Starts = $starts })
    Push-Location $repo
    try { Complete-LintTemporaryCleanup -BaselineWorktreeCreated -BaselineRoot $baselineRoot -TempRoot $stage -CppcheckCacheRoot $cacheRoot -StageName 'failure-stage' }
    finally { Pop-Location }
    $cleanupState.Completed = $true
    $lifecycle.Add(@{ Event = 'cleanup'; Time = [DateTime]::UtcNow.ToString('o'); StageExists = (Test-Path $stage); BaselineExists = (Test-Path $baselineRoot); Worktrees = @(& git -C $repo worktree list --porcelain) })
}
$finalization = $null
try {
    $finalization = Complete-LintFinalization -SaveEvidenceAction { $null = @(Invoke-CppcheckBatch -Descriptors $descriptors -CppcheckJobs 2 -Evidence $evidence) } -CleanupAction $cleanupAction -SaveResultAction {
        param($status, $exitCode, $errorMessage)
        Save-LintEvidenceResult -Evidence $evidence -Status $status -ExitCode $exitCode -Error $errorMessage
        $lifecycle.Add(@{ Event = 'result'; Status = $status; Error = $errorMessage; Time = [DateTime]::UtcNow.ToString('o') })
    }
}
finally {
    try { if (-not $cleanupState.Completed) { & $cleanupAction } }
    finally { Exit-CppcheckCacheLock -Lock $lock }
}
$reacquired = Enter-CppcheckCacheLock -CacheRoot $cacheRoot -Name 'failure-fixture'
Exit-CppcheckCacheLock -Lock $reacquired
$lifecycle.Add(@{ Event = 'lock-reacquired'; Time = [DateTime]::UtcNow.ToString('o'); Counter = $evidence.Counter })
$lifecycle | ConvertTo-Json -Depth 15 | Set-Content (Join-Path $Root 'lifecycle.json')
if ($null -eq $finalization -or (-not $finalization.Succeeded)) { exit 1 }
exit 0
'@
    Set-Content -LiteralPath $childPath -Value $child -NoNewline
    $expectedErrors = @{ exit2 = 'with code 2'; 'exit1-empty' = 'before producing diagnostics'; fatal = 'tool or configuration error'; missing = 'unsupported missing input'; start = 'not-an-analyzer.exe'; evidence = '0001-cppcheck-fixture-unit-0.cpp.raw.txt' }
    foreach ($case in @('valid', 'exit2', 'exit1-empty', 'fatal', 'missing', 'start', 'evidence')) {
        $caseRoot = Join-Path $root $case
        $outerEvidenceRoot = Join-Path $caseRoot 'outer'
        New-Item -ItemType Directory -Force -Path (Join-Path $outerEvidenceRoot 'commands') | Out-Null
        $arguments = @('-NoProfile', '-File', $childPath, '-Case', $case, '-Root', $caseRoot, '-LintScript', $LintScript, '-TestScript', $TestScript)
        $outer = Invoke-LintChild -Path (Get-Process -Id $PID).Path -Arguments $arguments -Label "outer-$case" -Evidence ([PSCustomObject]@{ Root = $outerEvidenceRoot; Counter = 0 })
        $expectedExit = if ($case -eq 'valid') { 0 } else { 1 }
        if ($outer.ExitCode -ne $expectedExit) { throw "V6 $case outer exit was $($outer.ExitCode): $($outer.Output -join ' ')" }
        $result = Get-Content (Join-Path $caseRoot 'evidence/final-result.json') -Raw | ConvertFrom-Json
        if ($result.ExitCode -ne $expectedExit -or $result.Status -ne $(if ($case -eq 'valid') { 'passed' } else { 'failed' })) { throw "V6 $case final result mismatch." }
        if ($case -ne 'valid' -and $result.Error -notmatch [regex]::Escape($expectedErrors[$case])) { throw "V6 $case lost original error: $($result.Error)" }
        if ($case -eq 'evidence' -and ($result.Error -notmatch 'Secondary failures:.*0002-cppcheck-fixture-unit-1.cpp.raw.txt')) { throw 'V6 secondary save failure was lost.' }
        $lifecycle = @(Get-Content (Join-Path $caseRoot 'lifecycle.json') -Raw | ConvertFrom-Json)
        $stopped = @($lifecycle | Where-Object { $_.Event -eq 'owned-stopped-before-cleanup' })[0]
        $cleanup = @($lifecycle | Where-Object { $_.Event -eq 'cleanup' })[0]
        $reacquired = @($lifecycle | Where-Object { $_.Event -eq 'lock-reacquired' })[0]
        if ($stopped.PIDs.Count -lt 1 -or $cleanup.StageExists -or $cleanup.BaselineExists -or [DateTime]$cleanup.Time -lt [DateTime]$stopped.Time) { throw "V6 $case cleanup occurred before owned process exit or leaked stage/worktree." }
        foreach ($ownedPid in $stopped.PIDs) { if (Get-Process -Id $ownedPid -ErrorAction SilentlyContinue) { throw "V6 PID $ownedPid remains alive." } }
        if ($case -eq 'start') {
            if ($stopped.Invocations.Count -ne 1 -or $reacquired.Counter -ne 1) { throw 'V6 infrastructure failure did not stop further submissions.' }
        }
        elseif ($reacquired.Counter -ne 3) { throw "V6 $case did not attempt all started TU evidence before interpreting." }
        if ($case -eq 'evidence' -and $stopped.Invocations.Count -ne 1) { throw 'V6 save failure skipped later TU evidence.' }
        foreach ($invocation in $stopped.Invocations) {
            $unitArgument = @($invocation.Arguments | Where-Object { $_ -like '*config-*.json' })[0]
            $unitIndex = [regex]::Match($unitArgument, 'config-(\d+)\.json').Groups[1].Value
            if ($case -ne 'start' -and $invocation.Stdout -notmatch "progress-$unitIndex|injected|cppcheck: error") { throw 'V6 saved raw output was associated with the wrong TU.' }
        }
    }
    Write-Host 'V6 real process/finalization/worktree/lock failure matrix: passed (six expected failures, one success).'
}

function Assert-CppcheckProjectSelection {
    $preparationState = [PSCustomObject]@{ Count = 0 }
    $emptySelection = Select-CppcheckProjects `
        -Projects @() `
        -ChangedFiles @('src/sdl/i_main.cpp', 'tools/timidity_pipe_tests.cpp') `
        -IsInputManifest:$false `
        -IsWindowsVisualStudioBuild:$true `
        -BuildDirectory 'build-v143' `
        -PrepareAnalysis { $preparationState.Count++ }

    if (($emptySelection.Projects.Count -ne 0) -or ($emptySelection.PlatformNotApplicable.Count -ne 2) -or
        ($preparationState.Count -ne 0) -or
        ($emptySelection.PlatformNotApplicable[0].Reason -notmatch 'src/CMakeLists\.txt') -or
        ($emptySelection.PlatformNotApplicable[1].Reason -notmatch 'tools/CMakeLists\.txt')) {
        throw 'Platform-not-applicable selection did not skip analysis preparation for the exact Windows-excluded sources.'
    }

    $registeredProject = [PSCustomObject]@{
        RelativeProject = 'src/fixture.vcxproj'
        Sources = @{ 'src/registered.cpp' = 'src/registered.cpp'; 'src/other.cpp' = 'src/other.cpp' }
        Files = @()
    }
    $mixedSelection = Select-CppcheckProjects `
        -Projects @($registeredProject) `
        -ChangedFiles @('src/registered.cpp', 'tools/timidity_pipe_tests.cpp') `
        -IsInputManifest:$false `
        -IsWindowsVisualStudioBuild:$true `
        -BuildDirectory 'build-v143' `
        -PrepareAnalysis { $preparationState.Count++ }

    if (($mixedSelection.Projects.Count -ne 1) -or ($mixedSelection.Projects[0].Files.Count -ne 2) -or
        ($mixedSelection.PlatformNotApplicable.Count -ne 1) -or ($preparationState.Count -ne 1)) {
        throw 'Mixed project selection did not retain every translation unit of the registered project.'
    }

    $registeredAllowedPathProject = [PSCustomObject]@{
        RelativeProject = 'src/registered-allowed.vcxproj'
        Sources = @{ 'src/sdl/i_main.cpp' = 'src/sdl/i_main.cpp'; 'src/other.cpp' = 'src/other.cpp' }
        Files = @()
    }
    $registeredAllowedPathSelection = Select-CppcheckProjects `
        -Projects @($registeredAllowedPathProject) `
        -ChangedFiles @('src/sdl/i_main.cpp') `
        -IsInputManifest:$false `
        -IsWindowsVisualStudioBuild:$true `
        -BuildDirectory 'build-v143'

    if (($registeredAllowedPathSelection.Projects.Count -ne 1) -or
        ($registeredAllowedPathSelection.Projects[0].Files.Count -ne 2) -or
        ($registeredAllowedPathSelection.PlatformNotApplicable.Count -ne 0)) {
        throw 'A registered platform-not-applicable path was not analyzed through its owning project.'
    }

    Assert-ExpectedException -Expected "Changed source 'src/unknown.cpp'" -Action {
        Select-CppcheckProjects -Projects @() -ChangedFiles @('src/unknown.cpp') -IsInputManifest:$false -IsWindowsVisualStudioBuild:$true -BuildDirectory 'build-v143' | Out-Null
    }
    Assert-ExpectedException -Expected "Changed source 'src/sdl/i_main.cpp'" -Action {
        Select-CppcheckProjects -Projects @() -ChangedFiles @('src/sdl/i_main.cpp') -IsInputManifest:$false -IsWindowsVisualStudioBuild:$false -BuildDirectory 'build-v143' | Out-Null
    }
    Assert-ExpectedException -Expected "InputManifest C/C++ source 'tools/timidity_pipe_tests.cpp'" -Action {
        Select-CppcheckProjects -Projects @() -ChangedFiles @('tools/timidity_pipe_tests.cpp') -IsInputManifest:$true -IsWindowsVisualStudioBuild:$true -BuildDirectory 'build-v143' | Out-Null
    }

    $secondProject = [PSCustomObject]@{
        RelativeProject = 'tools/fixture.vcxproj'
        Sources = @{ 'src/registered.cpp' = 'src/registered.cpp'; 'tools/local.cpp' = 'tools/local.cpp' }
        Files = @()
    }
    foreach ($changed in @(@('src/registered.cpp'), @('src/registered.cpp', 'src/other.cpp'))) {
        $selection = Select-CppcheckProjects -Projects @($registeredProject, $secondProject) -ChangedFiles $changed -IsInputManifest:$false -IsWindowsVisualStudioBuild:$true -BuildDirectory 'build-v143' -SelectedOnly
        if ($selection.Projects.Count -ne 2) { throw 'Selected-only multi-target selection lost an owning project.' }
        foreach ($project in $selection.Projects) {
            $expected = @($changed | Where-Object { $project.Sources.ContainsKey($_) } | Sort-Object -Unique)
            if (($project.Files -join '|') -cne ($expected -join '|')) { throw 'Selected-only selection expanded or lost translation units.' }
        }
    }
    $single = Select-CppcheckProjects -Projects @($registeredProject) -ChangedFiles @('src/other.cpp', 'src/sdl/i_main.cpp', 'tools/timidity_pipe_tests.cpp') -IsInputManifest:$false -IsWindowsVisualStudioBuild:$true -BuildDirectory 'build-v143' -SelectedOnly
    if (($single.Projects[0].Files -join '|') -cne 'src/other.cpp' -or $single.PlatformNotApplicable.Count -ne 2) { throw 'Selected-only single-TU/platform exclusion mismatch.' }
    Assert-ExpectedException -Expected "Changed source 'src/unknown.cpp'" -Action {
        Select-CppcheckProjects -Projects @($registeredProject) -ChangedFiles @('src/unknown.cpp') -IsInputManifest:$false -IsWindowsVisualStudioBuild:$true -BuildDirectory 'build-v143' -SelectedOnly | Out-Null
    }
    $full = Select-CppcheckProjects -Projects @($registeredProject, $secondProject) -ChangedFiles @('src/registered.cpp') -IsInputManifest:$false -IsWindowsVisualStudioBuild:$true -BuildDirectory 'build-v143'
    if (@($full.Projects | Where-Object { $_.Files.Count -ne 2 }).Count) { throw 'Full selection after selected-only did not restore every TU.' }
    Write-Host 'Full/selected-only single/multiple/multi-target selection: passed.'
}

function Assert-CppcheckSourceRouting {
    param([string]$LintScript = (Join-Path $PSScriptRoot 'lint.ps1'))

    Import-LintFunction -Name 'Get-CppcheckSourceRoute' -LintScript $LintScript
    $source = [PSCustomObject]@{ Status = 'M'; Path = 'src/sample.cpp'; OldPath = '' }
    $exemptChanges = @(
        'tools/testdata/audio/phase1b-validation.json',
        'tools/testdata/audio/validate_phase1b_manifest.py',
        'tools/testdata/audio/README.md'
    ) | ForEach-Object { [PSCustomObject]@{ Status = 'M'; Path = $_; OldPath = '' } }
    foreach ($change in $exemptChanges) {
        $route = Get-CppcheckSourceRoute -Changes @($change)
        if ($route.Mode -ne 'Skip' -or $route.Sources.Count) { throw "Ordinary audio contract modification did not skip: $($change.Path)." }
        foreach ($status in @('A', 'D', 'T', 'R100', 'C100', 'M')) {
            $route = Get-CppcheckSourceRoute -Changes @([PSCustomObject]@{ Status = $status; Path = $change.Path; OldPath = if ($status -in @('R100', 'C100', 'M')) { 'docs/audio.txt' } else { '' } })
            if ($route.Mode -ne 'Error' -or $route.Sources.Count) { throw "Non-ordinary audio contract change was skipped: $status $($change.Path)." }
        }
    }
    $cleanupChanges = @($exemptChanges) + @(
        'scripts/lint.ps1',
        'scripts/test-lint-vendor-dispositions.ps1',
        'scripts/phase1b_audio_gate.py',
        'AGENTS.md',
        'README.md',
        'docs/native-openal-soft-phase-2.md'
    ) | ForEach-Object {
        if ($_ -is [string]) { [PSCustomObject]@{ Status = 'M'; Path = $_; OldPath = '' } } else { $_ }
    }
    $cleanupChanges += @(
        '.github/workflows/ci-windows.yml',
        '.github/workflows/ci-linux.yml',
        '.gitlab-ci.yml',
        'appimage/AppRun',
        'appimage/zandronum.desktop',
        'appimage/zandronum.png'
    ) | ForEach-Object { [PSCustomObject]@{ Status = 'D'; Path = $_; OldPath = '' } }
    foreach ($case in @(
        @{ Changes = @($source); Dirty = $false; Mode = 'Candidate'; Sources = @('src/sample.cpp') },
        @{ Changes = @($source); Dirty = $true; Mode = 'Full'; Sources = @('src/sample.cpp') },
        @{ Changes = @($source, [PSCustomObject]@{ Status = 'M'; Path = 'src/sample.h' }); Dirty = $false; Mode = 'Full'; Sources = @('src/sample.cpp') },
        @{ Changes = @([PSCustomObject]@{ Status = 'M'; Path = 'README.md' }); Dirty = $false; Mode = 'Skip'; Sources = @() },
        @{ Changes = @([PSCustomObject]@{ Status = 'M'; Path = 'src/sample.h' }); Dirty = $false; Mode = 'Skip'; Sources = @() },
        @{ Changes = @([PSCustomObject]@{ Status = 'M'; Path = 'CMakeLists.txt' }); Dirty = $false; Mode = 'Error'; Sources = @() },
        @{ Changes = @([PSCustomObject]@{ Status = 'M'; Path = 'protocolspec/spec.txt' }); Dirty = $false; Mode = 'Error'; Sources = @() },
        @{ Changes = @([PSCustomObject]@{ Status = 'D'; Path = 'src/sample.cpp' }); Dirty = $false; Mode = 'Error'; Sources = @() },
        @{ Changes = @([PSCustomObject]@{ Status = 'M'; Path = 'tools/lemon/lemon.c' }); Dirty = $false; Mode = 'Full'; Sources = @('tools/lemon/lemon.c') },
        @{ Changes = @([PSCustomObject]@{ Status = 'M'; Path = 'src/sound/thirdparty/vendor.cpp' }); Dirty = $false; Mode = 'Full'; Sources = @('src/sound/thirdparty/vendor.cpp') },
        @{ Changes = @($exemptChanges); Dirty = $false; Mode = 'Skip'; Sources = @() },
        @{ Changes = $cleanupChanges; Dirty = $false; Mode = 'Skip'; Sources = @() },
        @{ Changes = @($exemptChanges) + @([PSCustomObject]@{ Status = 'M'; Path = 'tools/testdata/audio/generate_phase1b_music_fixtures.py'; OldPath = '' }); Dirty = $false; Mode = 'Error'; Sources = @() },
        @{ Changes = @($source) + @($exemptChanges); Dirty = $false; Mode = 'Full'; Sources = @('src/sample.cpp') }
    )) {
        $route = Get-CppcheckSourceRoute -Changes $case.Changes -TrackedDirty:$case.Dirty
        if ($route.Mode -ne $case.Mode -or $route.Sources.Count -ne $case.Sources.Count -or ($route.Sources -join '|') -cne ($case.Sources -join '|')) { throw "Source routing mismatch: $($route.Reason)." }
        if ($case.Mode -eq 'Error') {
            $route = Get-CppcheckSourceRoute -Changes (@($case.Changes) + @($exemptChanges))
            if ($route.Mode -ne 'Error' -or $route.Sources.Count) { throw 'Audio contract modification masked an unsupported mixed range.' }
        }
    }
    foreach ($status in @('R100', 'C100')) {
        $pairs = @(
            @('src/sc_man.cpp', 'src/sc_man.txt'),
            @('protocolspec/spec.txt', 'docs/spec.txt'),
            @('CMakeLists.txt', 'CMakeLists.old'),
            @('src/parser.y', 'docs/parser.txt'),
            @('tools/testdata/audio/generate_phase1b_music_fixtures.py', 'docs/fixtures.txt'),
            @('src/sc_man.cpp', $exemptChanges[0].Path),
            @('protocolspec/spec.txt', $exemptChanges[0].Path),
            @('CMakeLists.txt', $exemptChanges[0].Path)
        )
        foreach ($change in $exemptChanges) {
            $pairs += ,@($change.Path, 'docs/audio.txt')
        }
        foreach ($pair in $pairs) {
            $route = Get-CppcheckSourceRoute -Changes @([PSCustomObject]@{ Status = $status; OldPath = $pair[0]; Path = $pair[1] })
            if ($route.Mode -ne 'Error' -or $route.Sources.Count) { throw "Relevant old endpoint was skipped: $status $($pair -join ' -> ')." }
            $route = Get-CppcheckSourceRoute -Changes (@([PSCustomObject]@{ Status = $status; OldPath = $pair[0]; Path = $pair[1] }) + @($exemptChanges))
            if ($route.Mode -ne 'Error' -or $route.Sources.Count) { throw "Audio contract modification masked an old endpoint: $status $($pair -join ' -> ')." }
        }
        $route = Get-CppcheckSourceRoute -Changes @([PSCustomObject]@{ Status = $status; OldPath = 'src/sample.h'; Path = 'docs/sample.txt' })
        if ($route.Mode -ne 'Skip' -or $route.Sources.Count) { throw 'True header/document-only change no longer skips.' }
    }
    foreach ($status in @('A', 'R100', 'C100', 'T')) {
        $route = Get-CppcheckSourceRoute -Changes @([PSCustomObject]@{ Status = $status; Path = 'src/sample.cpp'; OldPath = 'src/old.cpp' })
        if ($route.Mode -ne 'Full' -or $route.Sources.Count -ne 1 -or $route.Sources[0] -cne 'src/sample.cpp') { throw "Unsafe source status $status entered Fast qualification." }
    }
    Write-Host 'Conservative all-diff source routing: passed.'
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

function Assert-ProductionPreparationFailureReleasesAcquiredLock {
    $lintScript = Join-Path $PSScriptRoot 'lint.ps1'
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($lintScript, [ref]$tokens, [ref]$errors)

    if ($errors.Count -ne 0) {
        throw "Could not parse lint.ps1: $($errors[0].Message)"
    }

    $prepareAnalysisArgument = $ast.FindAll({
            param($node)
            ($node -is [System.Management.Automation.Language.CommandAst]) -and
            ($node.GetCommandName() -eq 'Select-CppcheckProjects')
        }, $true) |
        ForEach-Object {
            $_.CommandElements | Where-Object { $_ -is [System.Management.Automation.Language.ScriptBlockExpressionAst] } | Select-Object -First 1
        } |
        Select-Object -First 1

    if ($null -eq $prepareAnalysisArgument) {
        throw 'lint.ps1 does not retain a production PrepareAnalysis callback.'
    }

    $preparationTry = $prepareAnalysisArgument.ScriptBlock.FindAll({
            param($node)
            ($node -is [System.Management.Automation.Language.TryStatementAst]) -and
            ($node.CatchClauses.Count -ne 0)
        }, $true) | Select-Object -First 1

    if (($null -eq $preparationTry) -or
        ($preparationTry.CatchClauses[0].Extent.Text -notmatch 'Exit-CppcheckCacheLock -Lock \$acquiredCacheLock') -or
        ($preparationTry.CatchClauses[0].Extent.Text -notmatch 'throw')) {
        throw 'Production PrepareAnalysis callback does not release its locally acquired cache lock before rethrowing stage creation failures.'
    }

    $childScriptPath = Join-Path ([System.IO.Path]::GetTempPath()) ('zandronum-prepare-analysis-' + [guid]::NewGuid().ToString('N') + '.ps1')
    $resultPath = Join-Path ([System.IO.Path]::GetTempPath()) ('zandronum-prepare-analysis-' + [guid]::NewGuid().ToString('N') + '.json')
    $callbackText = $prepareAnalysisArgument.ScriptBlock.Extent.Text
    $childScript = @'
param([bool]$UseExistingLock, [string]$ResultPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$state = [PSCustomObject]@{ EnterCalls = 0; ExitCalls = 0; Message = $null }
$acquiredLock = [System.IO.MemoryStream]::new()
$cacheLock = if ($UseExistingLock) { [System.IO.MemoryStream]::new() } else { $null }
function Enter-CppcheckCacheLock {
    param($CacheRoot, $Name)
    $state.EnterCalls++
    return $acquiredLock
}
function New-CppcheckDisposableStage {
    param($CacheRoot, $Name)
    throw 'fixture stage creation failure'
}
function Exit-CppcheckCacheLock {
    param($Lock)
    $state.ExitCalls++
    $Lock.Dispose()
}
$cppcheckCacheRoot = 'fixture-cache-root'
$inputManifestData = $null
$inputManifestRoots = $null
$stageName = 'regression-normal'
$prepare =
'@ + $callbackText + @'

try {
    & $prepare
    $state.Message = 'no exception'
}
catch {
    $state.Message = $_.Exception.Message
}
finally {
    @{ Message = $state.Message; EnterCalls = $state.EnterCalls; ExitCalls = $state.ExitCalls; AcquiredCanRead = $acquiredLock.CanRead; ExistingCanRead = if ($null -eq $cacheLock) { $null } else { $cacheLock.CanRead } } | ConvertTo-Json -Compress | Set-Content -LiteralPath $ResultPath -NoNewline
    $acquiredLock.Dispose()
    if ($null -ne $cacheLock) { $cacheLock.Dispose() }
}
'@

    try {
        Set-Content -LiteralPath $childScriptPath -Value $childScript -NoNewline
        foreach ($case in @(
            @{ Name = 'acquired'; UseExistingLock = $false; ExpectedEnterCalls = 1; ExpectedExitCalls = 1; ExpectedAcquiredCanRead = $false; ExpectedExistingCanRead = $null },
            @{ Name = 'existing'; UseExistingLock = $true; ExpectedEnterCalls = 0; ExpectedExitCalls = 0; ExpectedAcquiredCanRead = $true; ExpectedExistingCanRead = $true }
        )) {
            & pwsh -NoProfile -File $childScriptPath -UseExistingLock:$case.UseExistingLock -ResultPath $resultPath
            if ($LASTEXITCODE -ne 0) {
                throw "Production PrepareAnalysis $($case.Name) lock fixture exited with $LASTEXITCODE."
            }

            $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
            if (($result.Message -ne 'fixture stage creation failure') -or
                ($result.EnterCalls -ne $case.ExpectedEnterCalls) -or
                ($result.ExitCalls -ne $case.ExpectedExitCalls) -or
                ($result.AcquiredCanRead -ne $case.ExpectedAcquiredCanRead) -or
                ($result.ExistingCanRead -ne $case.ExpectedExistingCanRead)) {
                throw "Production PrepareAnalysis callback did not preserve the expected $($case.Name) cache lock ownership after stage creation failure."
            }
        }
    }
    finally {
        Remove-Item -LiteralPath $childScriptPath, $resultPath -Force -ErrorAction SilentlyContinue
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

function Assert-NativeCppcheckBatchSmoke {
    param([string]$FixtureRoot, [string]$OldDefinitionsPath = '')

    $cppcheck = (Get-Command cppcheck -ErrorAction Stop).Source
    $nativeSource = Join-Path $FixtureRoot 'native-cache/build/src/sample.cpp'
    $nativeHeader = Join-Path $FixtureRoot 'native-cache/build/src/sample.h'
    if (-not (Test-Path $nativeSource) -or -not (Test-Path $nativeHeader)) { throw 'Native cache fixture must run before smoke.' }
    $root = Join-Path $FixtureRoot 'native-smoke'
    $templateSource = Get-Content $nativeSource -Raw
    $templateHeader = Get-Content $nativeHeader -Raw
    $smokePassed = $false
    foreach ($attempt in @(1, 2)) {
        $functionCount = if ($attempt -eq 1) { 200 } else { 1000 }
        $attemptRoot = Join-Path $root "attempt-$attempt"
        $observedOverlap = $true
        foreach ($role in @('head', 'baseline')) {
            $buildRoot = Join-Path $attemptRoot "$role/build"
            $sourceRoot = Join-Path $buildRoot 'src'
            New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
            Set-Content (Join-Path $buildRoot 'CMakeCache.txt') 'CMAKE_GENERATOR_TOOLSET:INTERNAL=v143' -NoNewline
            Set-Content (Join-Path $sourceRoot 'sample.h') $templateHeader -NoNewline
            $entries = @(for ($unitIndex = 0; $unitIndex -lt 3; $unitIndex++) {
                $unit = "sample-$unitIndex.cpp"
                $helpers = @(for ($functionIndex = 0; $functionIndex -lt $functionCount; $functionIndex++) { "int helper_$functionIndex(int value) { return value * value + $functionIndex; }" }) -join "`n"
                Set-Content (Join-Path $sourceRoot $unit) ($templateSource + "`n" + $helpers) -NoNewline
                @{ directory = $sourceRoot; command = ('cl.exe /I"' + $sourceRoot + '" /c ' + $unit); file = (Join-Path $sourceRoot $unit) }
            })
            $projectPath = Join-Path $buildRoot 'compile_commands.json'
            $entries | ConvertTo-Json -AsArray | Set-Content $projectPath
            $context = Get-CppcheckRegressionAnalysisContext -BuildRoot $buildRoot -RepositoryRoot $buildRoot -ProjectPath $projectPath -InputRootIdentities @('native-cache-fixture') -AnalyzerConfigurationPaths (Get-CppcheckInstalledConfigurationPaths -AnalyzerPath $cppcheck) -AnalyzerOptions @('--project-configuration=Release|x64', '--enable=warning,performance,portability')
            $identity = Get-CppcheckCacheIdentity -AnalyzerVersion (& $cppcheck --version) -AnalyzerSHA256 (Get-FileHash $cppcheck).Hash -InputMode 'normal' -Toolset 'v143' -RootIdentity 'native-cache-fixture' -AnalysisContext $context
            $identity | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $buildRoot 'identity.json')
            $expected = $null
            $serialInvocations = @()
            foreach ($jobs in @(1, 3)) {
                $evidenceRoot = Join-Path $attemptRoot "$role/jobs-$jobs"
                New-Item -ItemType Directory -Force -Path (Join-Path $evidenceRoot 'commands') | Out-Null
                $evidence = [PSCustomObject]@{ Root = $evidenceRoot; Counter = 0 }
                $descriptors = @(for ($unitIndex = 0; $unitIndex -lt 3; $unitIndex++) {
                    $unit = "sample-$unitIndex.cpp"
                    $leaf = Get-CppcheckCacheLeaf -CacheRoot (Join-Path $FixtureRoot "smoke-cache-$attempt-$jobs") -Namespace 'regression' -Identity $identity -Role $role -TargetName 'compile_commands.json' -TranslationUnit $unit
                    New-CppcheckDescriptor -CppcheckPath $cppcheck -ProjectPath $projectPath -CachePath $leaf -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit $unit -Index $unitIndex -UseProjectConfiguration:$false
                })
                $diagnostics = @(Invoke-CppcheckBatch -Descriptors $descriptors -CppcheckJobs $jobs -Evidence $evidence)
                $invocations = @(Get-ChildItem (Join-Path $evidenceRoot 'commands') -Filter '*.invocation.json' | Sort-Object Name | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
                @{ Diagnostics = $diagnostics; Invocations = $invocations; Descriptors = $descriptors } | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $evidenceRoot 'results.json')
                $actual = $diagnostics | ConvertTo-Json -Depth 8 -Compress
                if ($jobs -eq 1) { $expected = $actual; $serialInvocations = $invocations }
                elseif ($expected -cne $actual) { throw 'Native Jobs=1/3 diagnostics differ.' }
                if ($diagnostics.Count -lt 3) { throw 'Native smoke fixture did not produce all TU diagnostics.' }
                foreach ($invocation in $invocations) {
                    $jobIndex = [Array]::IndexOf($invocation.Arguments, '-j')
                    if ($jobIndex -lt 0 -or $invocation.Arguments[$jobIndex + 1] -ne '1' -or $invocation.Path -ne $cppcheck) { throw 'Native smoke did not run installed Cppcheck with -j 1.' }
                }
                $overlap = Assert-CppcheckProcessIntervals -Invocations $invocations -Limit $jobs -Minimum 0
                if ($jobs -gt 1 -and $overlap -lt 2) { $observedOverlap = $false }
                if ($jobs -eq 3) {
                    $withoutEvidence = @(Invoke-CppcheckBatch -Descriptors $descriptors -CppcheckJobs 2)
                    if (($withoutEvidence | ConvertTo-Json -Depth 8 -Compress) -cne $expected) { throw 'Native evidence-disabled batch differs.' }
                }
            }
            if (-not [string]::IsNullOrWhiteSpace($OldDefinitionsPath)) {
                Import-LintFunction -Name 'Invoke-LintChild' -LintScript $OldDefinitionsPath -ImportedName 'Invoke-OldLintChild'
                Import-LintFunction -Name 'Invoke-CppcheckProject' -LintScript $OldDefinitionsPath -ImportedName 'Invoke-OldCppcheckProject'
                $oldDefinition = (Get-Command Invoke-OldCppcheckProject).Definition -replace 'Invoke-LintChild', 'Invoke-OldLintChild'
                Invoke-Expression ("function global:Invoke-OldCppcheckProject {`n" + $oldDefinition + "`n}")
                $oldEvidenceRoot = Join-Path $attemptRoot "$role/old"
                New-Item -ItemType Directory -Force -Path (Join-Path $oldEvidenceRoot 'commands') | Out-Null
                $oldEvidence = [PSCustomObject]@{ Root = $oldEvidenceRoot; Counter = 0 }
                $oldDiagnostics = @(for ($unitIndex = 0; $unitIndex -lt 3; $unitIndex++) {
                    Invoke-OldCppcheckProject -CppcheckPath $cppcheck -ProjectPath $projectPath -CachePath (Join-Path $oldEvidenceRoot "cache-$unitIndex") -RepositoryRoot $buildRoot -BuildRoot $buildRoot -TargetName 'compile_commands.json' -TranslationUnit "sample-$unitIndex.cpp" -CppcheckJobs 1 -Evidence $oldEvidence -UseProjectConfiguration:$false
                })
                $oldInvocations = @(Get-ChildItem (Join-Path $oldEvidenceRoot 'commands') -Filter '*.invocation.json' | Sort-Object Name | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
                @{ Diagnostics = $oldDiagnostics; Invocations = $oldInvocations } | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $oldEvidenceRoot 'results.json')
                if (($oldDiagnostics | ConvertTo-Json -Depth 8 -Compress) -cne $expected) { throw 'V1 three native TU old/new full diagnostics differ.' }
                for ($unitIndex = 0; $unitIndex -lt 3; $unitIndex++) { if ($oldInvocations[$unitIndex].ExitCode -ne $serialInvocations[$unitIndex].ExitCode) { throw 'V1 native three-TU exit mismatch.' } }
            }
        }
        @{ Attempt = $attempt; FunctionsPerTU = $functionCount; ObservedRealCppcheckOverlap = $observedOverlap } | ConvertTo-Json | Set-Content (Join-Path $attemptRoot 'observation.json')
        if ($observedOverlap) { $smokePassed = $true; break }
    }
    if (-not $smokePassed) { throw 'Native Cppcheck PID overlap was not observed after the one permitted fixture increase.' }
    Write-Host 'V1/V7 three native TU diagnostics/exit and real Cppcheck process overlap: passed.'
}

function Assert-CppcheckFastContracts {
    param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot), [string]$LintScript = (Join-Path $PSScriptRoot 'lint.ps1'))

    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($LintScript, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw $errors[0].Message }
    foreach ($definition in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
        Invoke-Expression ($definition.Extent.Text -replace '^function\s+', 'function global:')
    }
    foreach ($name in @('generatedBundlePaths', 'fastSourceGeneratedPaths', 'generatedBuildInputRules')) {
        $assignment = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] }, $true) | Where-Object { $_.Left.Extent.Text -eq ('$' + $name) } | Select-Object -First 1
        Set-Variable -Name $name -Scope Global -Value @(& ([scriptblock]::Create($assignment.Right.Extent.Text)))
    }
    $global:utf8 = [Text.UTF8Encoding]::new($false)
    $evidenceRoot = Join-Path $RepositoryRoot ('completes/lint-source-tu-fast-path/phase-1/contracts-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $evidenceRoot 'commands') -Force | Out-Null
    $evidence = [PSCustomObject]@{ Root = $evidenceRoot; Counter = 0 }
    $clone = Join-Path $evidenceRoot 'objects'
    $cloneResult = Invoke-LintChild -Path 'git' -Arguments @('clone', '--shared', '--no-hardlinks', '--no-checkout', $RepositoryRoot, $clone) -Label 'fixture-objects-clone' -Evidence $evidence
    if ($cloneResult.ExitCode) { throw 'Fast object fixture clone failed.' }
    $cache = Join-Path $evidenceRoot 'cache'
    $lock = Enter-CppcheckCacheLock -CacheRoot $cache -Name 'regression'
    $stage = New-CppcheckDisposableStage -CacheRoot $cache -Name 'regression-fast'
    try {
        Assert-ExpectedException -Expected 'already exists' -Action { New-CppcheckDisposableStage -CacheRoot $cache -Name 'regression-fast' | Out-Null }
        $source = Join-Path $stage 'source'
        New-Item -ItemType Directory -Path $source | Out-Null
        $raw = [byte[]]@(0, 128, 255, 13, 10, 65, 10)
        $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $clone -Arguments @('hash-object', '-w', '--stdin') -InputBytes $raw)).Trim()
        $treeBytes = [Text.Encoding]::UTF8.GetBytes("100644 blob $blob`traw.cpp" + [char]0)
        $tree = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $clone -Arguments @('mktree', '-z') -InputBytes $treeBytes)).Trim()
        [IO.File]::WriteAllBytes((Join-Path $source 'raw.cpp'), [byte[]]@(65))
        Set-CppcheckBaseBlobs -RepositoryRoot $clone -BaseCommit $tree -Files @('raw.cpp') -StageSourceRoot $source
        if ([Convert]::ToHexString([IO.File]::ReadAllBytes((Join-Path $source 'raw.cpp'))) -cne [Convert]::ToHexString($raw)) { throw 'Raw Base blob was transcoded.' }
        [IO.File]::WriteAllBytes((Join-Path $evidenceRoot 'raw-input.bin'), $raw)
        @{ Blob = $blob; Tree = $tree; Path = 'raw.cpp'; Bytes = $raw } | ConvertTo-Json | Set-Content (Join-Path $evidenceRoot 'raw-identity.json')
        $reader = (Get-Command Get-CppcheckGitBytes).Definition
        try {
            $global:fastNulFixture = [Text.Encoding]::UTF8.GetBytes("M" + [char]0 + "src/tab`tnewline`n.cpp" + [char]0 + 'R100' + [char]0 + 'src/old.cpp' + [char]0 + 'src/new.cpp' + [char]0 + 'D' + [char]0 + 'src/deleted.cpp' + [char]0)
            function global:Get-CppcheckGitBytes { param($RepositoryRoot, $Arguments, $InputBytes); return ,$global:fastNulFixture }
            $changes = @(Get-CppcheckTreeChanges -RepositoryRoot $clone -BaseCommit base -HeadCommit head)
            if ($changes.Count -ne 3 -or $changes[0].Path -cne "src/tab`tnewline`n.cpp" -or $changes[1].OldPath -cne 'src/old.cpp' -or $changes[2].Status -ne 'D') { throw 'NUL all-diff parser corrupted status/path data.' }
            [IO.File]::WriteAllBytes((Join-Path $evidenceRoot 'routing-input.bin'), $global:fastNulFixture)
            $changes | ConvertTo-Json | Set-Content (Join-Path $evidenceRoot 'routing-output.json')
        }
        finally { Invoke-Expression ("function global:Get-CppcheckGitBytes {`n" + $reader + "`n}"); Remove-Variable fastNulFixture -Scope Global }
        $build = Join-Path $RepositoryRoot 'build-v143'
        $header = Join-Path $build 'src/sc_man_scanner.h'
        $rule = Join-Path $RepositoryRoot 'src/sc_man_scanner.re'
        $aliases = @(Get-CppcheckGeneratedDiagnosticAliases -GeneratedPaths @($header) -InputPaths @($rule) -RepositoryRoot $RepositoryRoot -BuildRoot $build)
        if ($aliases.Count -ne 1) { throw 'Real generated scanner #line alias missing.' }
        $rawDiagnostic = $aliases[0].AbsolutePath + "`t20`t1`twarning`tfixture`tmessage retains " + $aliases[0].AbsolutePath
        $headOutput = @(ConvertTo-CppcheckAliasedOutput -Output @($rawDiagnostic) -Aliases $aliases -StageSourceRoot $source)
        $baseOutput = @(ConvertTo-CppcheckAliasedOutput -Output @($rawDiagnostic -replace "`t20`t", "`t40`t") -Aliases $aliases -StageSourceRoot $source)
        $head = @(ConvertFrom-CppcheckProjectOutput -Output $headOutput -ExitCode 1 -RepositoryRoot $source -BuildRoot $build -TargetName 'src/zdoom.vcxproj' -TranslationUnit 'src/sc_man.cpp')
        $base = @(ConvertFrom-CppcheckProjectOutput -Output $baseOutput -ExitCode 1 -RepositoryRoot $source -BuildRoot $build -TargetName 'src/zdoom.vcxproj' -TranslationUnit 'src/sc_man.cpp')
        if ($head[0].RelativePath -cne 'src/sc_man_scanner.re' -or $head[0].Fingerprint -cne $base[0].Fingerprint -or $head[0].Message -notmatch [regex]::Escape($aliases[0].AbsolutePath)) { throw 'Exact real #line file-only alias/fingerprint failed.' }
        $unknown = Join-Path $stage 'unknown.h'
        [IO.File]::WriteAllText($unknown, [IO.File]::ReadAllText($header) + "`n#line 1 `"C:/unknown/unverified.re`"")
        Assert-ExpectedException -Expected 'Unknown generated #line input' -Action { Get-CppcheckGeneratedDiagnosticAliases -GeneratedPaths @($unknown) -InputPaths @($rule) -RepositoryRoot $RepositoryRoot -BuildRoot $build | Out-Null }
        $unverified = "C:/unknown/unverified.re`t1`t1`twarning`tfixture`tunchanged"
        if (@(ConvertTo-CppcheckAliasedOutput -Output @($unverified) -Aliases $aliases -StageSourceRoot $source)[0] -cne $unverified) { throw 'Unknown diagnostic file was broadly aliased.' }
        @{ Header = $header; HeaderSHA256 = (Get-FileHash $header).Hash; Aliases = $aliases; Raw = $rawDiagnostic; HEAD = $head; Base = $base; Unknown = $unverified } | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $evidenceRoot 'real-line-alias.json')
        [IO.File]::Copy($header, (Join-Path $evidenceRoot 'sc_man_scanner.h'))
        $project = Get-ProjectSources -BuildRoot $build -RepositoryRoot $RepositoryRoot | Where-Object RelativeProject -eq 'src/zdoom.vcxproj'
        $project.Files = @('src/sc_man.cpp')
        $mapped = New-CppcheckFastProject -Project $project -RepositoryRoot $RepositoryRoot -BuildRoot $build -StageSourceRoot $source -ProjectRoot (Join-Path $stage 'project') -AnalyzerVersion 'fixture'
        if (($mapped.Files -join '|') -cne 'src/sc_man.cpp') { throw 'Real XML selected-only set changed.' }
        [IO.File]::Copy($mapped.ProjectPath, (Join-Path $evidenceRoot 'mapped-zdoom.vcxproj'))
        $lineLiveRoot = Join-Path $evidenceRoot 'line/live'
        $lineBuildRoot = Join-Path $evidenceRoot 'line/build'
        New-Item -ItemType Directory -Force -Path (Join-Path $lineLiveRoot 'src'), $lineBuildRoot, (Join-Path $source 'src') | Out-Null
        $lineRule = Join-Path $lineLiveRoot 'src/line_fixture.re'
        $lineHeader = Join-Path $lineBuildRoot 'generated.h'
        [IO.File]::WriteAllText($lineRule, 'inline int line_fixture() { int *generatedPointer = 0; return *generatedPointer; }', [Text.UTF8Encoding]::new($false))
        $generator = Invoke-LintChild -Path (Join-Path $build 'tools/re2c/Release/re2c.exe') -Arguments @('-o', $lineHeader, $lineRule.Replace('\', '/')) -Label 'real-line-generator' -Evidence $evidence
        if ($generator.ExitCode) { throw 'Real line fixture re2c generation failed.' }
        $lineAliases = @(Get-CppcheckGeneratedDiagnosticAliases -GeneratedPaths @($lineHeader) -InputPaths @($lineRule) -RepositoryRoot $lineLiveRoot -BuildRoot $lineBuildRoot)
        $lineResults = @()
        foreach ($role in @('head', 'baseline')) {
            $prefix = if ($role -eq 'baseline') { "`n`n" } else { '' }
            [IO.File]::WriteAllText((Join-Path $source 'src/sc_man.cpp'), ($prefix + '#include "' + $lineHeader.Replace('\', '/') + '"'), [Text.UTF8Encoding]::new($false))
            $descriptor = New-CppcheckDescriptor -CppcheckPath (Get-Command cppcheck -ErrorAction Stop).Source -ProjectPath $mapped.ProjectPath -CachePath (Join-Path $cache "line-$role") -RepositoryRoot $source -BuildRoot $build -TargetName 'src/zdoom.vcxproj' -TranslationUnit 'src/sc_man.cpp' -Index 0
            $descriptor.WorkingDirectory = $source
            $descriptor | Add-Member -NotePropertyName DiagnosticFileAliases -NotePropertyValue $lineAliases
            $diagnostics = @(Invoke-CppcheckBatch -Descriptors @($descriptor) -CppcheckJobs 1 -Evidence $evidence)
            $lineResults += ,$diagnostics
            if ($diagnostics.Count -ne 1 -or $diagnostics[0].RelativePath -cne 'src/line_fixture.re') { throw 'Real generated native #line logical path mismatch.' }
        }
        if ($lineResults[0][0].Fingerprint -cne $lineResults[1][0].Fingerprint) { throw 'Real generated native #line HEAD/Base fingerprint mismatch.' }
        @{ Generator = (Join-Path $build 'tools/re2c/Release/re2c.exe'); GeneratorSHA256 = (Get-FileHash (Join-Path $build 'tools/re2c/Release/re2c.exe')).Hash; Aliases = $lineAliases; Results = $lineResults } | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $evidenceRoot 'native-line-alias.json')
        $state = Get-CppcheckPreparationState -RepositoryRoot $RepositoryRoot -BuildRoot $build -Projects @($project) -AnalyzerVersion 'fixture'
        Save-CppcheckPreparationReceipt -CacheRoot $cache -BuildRoot $build -Receipt ([PSCustomObject]@{ SchemaVersion = 1; Anchor = 'fixture'; PreparationVerified = $true; State = $state })
        $receipt = Get-CppcheckPreparationReceipt -CacheRoot $cache -BuildRoot $build
        Assert-CppcheckPreparationState -Expected $state -Actual $receipt.State
        $changed = $state | ConvertTo-Json -Depth 16 | ConvertFrom-Json
        $changed.Inputs[0].SHA256 = 'changed'
        Assert-ExpectedException -Expected 'context/input/output changed' -Action { Assert-CppcheckPreparationState -Expected $receipt.State -Actual $changed }
        Assert-ExpectedException -Expected 'live HEAD/index/tracked inputs changed' -Action { Assert-CppcheckFastLiveHead -RepositoryRoot $RepositoryRoot -HeadCommit 'not-current-head' }
        Assert-CppcheckFastSourceSafety -RepositoryRoot $RepositoryRoot -BuildRoot $build -HeadCommit (& git -C $RepositoryRoot rev-parse HEAD) -ChangedFiles @('src/sc_man.cpp') -Projects @($project)
        $receipt | ConvertTo-Json -Depth 16 | Set-Content (Join-Path $evidenceRoot 'receipt-roundtrip.json')
    }
    finally { try { Remove-CppcheckDisposableStage -CacheRoot $cache -StageRoot $stage -Name 'regression-fast' } finally { Exit-CppcheckCacheLock -Lock $lock } }
    $reacquired = Enter-CppcheckCacheLock -CacheRoot $cache -Name 'regression'
    Exit-CppcheckCacheLock -Lock $reacquired
    if (Test-Path -LiteralPath $stage) { throw 'Fast contract fixture leaked its owned stage.' }
    Write-Host "Fast raw/NUL/real-line/XML/receipt/cleanup contracts: passed ($evidenceRoot)."
}

function Assert-CppcheckFastSafetyPreflight {
    param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot), [string]$LintScript = (Join-Path $PSScriptRoot 'lint.ps1'))

    $root = Join-Path $RepositoryRoot ('completes/lint-source-tu-fast-path/phase-1/safety-' + [guid]::NewGuid().ToString('N'))
    $source = Join-Path $root 'source'
    $build = Join-Path $source 'build'
    $global:utf8 = [Text.UTF8Encoding]::new($false)
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'commands'), (Join-Path $source 'src/xlat'), (Join-Path $build 'CMakeFiles'), (Join-Path $build 'src') | Out-Null
    $evidence = [PSCustomObject]@{ Root = $root; Counter = 0 }
    $initialize = Invoke-LintChild -Path 'git' -Arguments @('init', $source) -Label 'safety-init' -Evidence $evidence
    if ($initialize.ExitCode) { throw 'Safety fixture Git initialization failed.' }
    $files = @{
        '.gitignore' = "/build/`n/src/review-ignored`n"
        'src/sc_man.cpp' = "#include `"sample.h`"`nint sample;`n"
        'src/sample.h' = '#define SAMPLE 1'
        'src/xlat/parse_xlat.cpp' = '#include "xlat_parser.c"'
        'src/guard.h' = "#ifdef ROUTINE_OPTIONAL_HEADER`n#include ROUTINE_OPTIONAL_HEADER`n#endif`n"
    }
    foreach ($path in $files.Keys) {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($files[$path])
        [IO.File]::WriteAllBytes((Join-Path $source $path), $bytes)
        $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('hash-object', '-w', '--stdin') -InputBytes $bytes)).Trim()
        $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--add', '--cacheinfo', "100644,$blob,$path")
    }
    $tree = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('write-tree'))).Trim()
    [IO.File]::WriteAllText((Join-Path $build 'CMakeFiles/generate.stamp.depend'), '# fixture')
    $generated = Join-Path $build 'src/xlat_parser.c'
    [IO.File]::WriteAllText($generated, 'int generated;')
    $projectPath = Join-Path $build 'fixture.vcxproj'
    $escaped = [Security.SecurityElement]::Escape((Join-Path $source 'src'))
    [IO.File]::WriteAllText($projectPath, ('<Project><PropertyGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><PlatformToolset>v143</PlatformToolset></PropertyGroup><ItemDefinitionGroup Condition="''$(Configuration)|$(Platform)''==''Release|x64''"><ClCompile><PreprocessorDefinitions>RELEASE</PreprocessorDefinitions><AdditionalIncludeDirectories>' + $escaped + '</AdditionalIncludeDirectories></ClCompile></ItemDefinitionGroup></Project>'))
    $project = [PSCustomObject]@{ ProjectPath = $projectPath; RelativeProject = 'fixture.vcxproj'; Sources = @{ 'src/sc_man.cpp' = (Join-Path $source 'src/sc_man.cpp') }; Files = @('src/sc_man.cpp') }
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($LintScript, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw $errors[0].Message }
    $route = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.StartsWith('if ($null -eq $inputManifestData -and $sourceRoute.Mode') }, $true) | Select-Object -First 1
    if ($null -eq $route) { throw 'Actual Auto preflight route block not found.' }
    $routeScript = [scriptblock]::Create($route.Extent.Text)
    $results = @()
    foreach ($case in @('ordinary', 'macro-source', 'macro-header', 'macro-unknown', 'literal-source', 'absolute-header', 'tracked-def', 'untracked-def', 'ignored-extensionless', 'unknown-extension', 'approved-generated', 'generated-macro-source', 'operational-outside', 'missing-generated', 'shared-relative-header', 'shared-line-alias', 'base-absolute-header', 'base-bom-absolute-header', 'base-macro-source', 'base-macro-unknown', 'base-unresolved-source', 'base-included-def', 'base-guard-head-definition')) {
        $unitPath = Join-Path $source 'src/sc_man.cpp'
        $guardPath = Join-Path $source 'src/guard.h'
        $extras = @()
        $caseTree = $tree
        $baseTree = $tree
        $unit = 'src/sc_man.cpp'
        try {
            switch ($case) {
                'macro-source' { [IO.File]::WriteAllText($guardPath, "#define REVIEW_SOURCE `"sc_man.cpp`"`n#include REVIEW_SOURCE`n") }
                'macro-header' { [IO.File]::WriteAllText($guardPath, ('#define REVIEW_HEADER "' + (Join-Path $source 'src/sample.h').Replace('\', '/') + '"' + "`n#include REVIEW_HEADER`n")) }
                'macro-unknown' { [IO.File]::WriteAllText($guardPath, '#include REVIEW_UNKNOWN') }
                'literal-source' { [IO.File]::WriteAllText($guardPath, '#include "sc_man.cpp"') }
                'absolute-header' { [IO.File]::WriteAllText($guardPath, ('#include "' + (Join-Path $source 'src/sample.h').Replace('\', '/') + '"')) }
                'tracked-def' {
                    $extra = Join-Path $source 'src/review.def'; $extras += $extra
                    $bytes = [Text.UTF8Encoding]::new($false).GetBytes('#include "sc_man.cpp"')
                    [IO.File]::WriteAllBytes($extra, $bytes)
                    $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('hash-object', '-w', '--stdin') -InputBytes $bytes)).Trim()
                    $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--add', '--cacheinfo', "100644,$blob,src/review.def")
                    $caseTree = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('write-tree'))).Trim()
                    [IO.File]::WriteAllText($unitPath, '#include "review.def"')
                }
                'untracked-def' { $extra = Join-Path $source 'src/review.def'; $extras += $extra; [IO.File]::WriteAllText($extra, '#define REVIEW 1'); [IO.File]::WriteAllText($unitPath, '#include "review.def"') }
                'ignored-extensionless' { $extra = Join-Path $source 'src/review-ignored'; $extras += $extra; [IO.File]::WriteAllText($extra, '#define REVIEW 1'); [IO.File]::WriteAllText($unitPath, '#include "review-ignored"') }
                'unknown-extension' { $extra = Join-Path $source 'src/review.custom'; $extras += $extra; [IO.File]::WriteAllText($extra, '#define REVIEW 1') }
                'approved-generated' { $extra = Join-Path $source 'src/gitinfo.h'; $extras += $extra; [IO.File]::WriteAllText($extra, '#define REVIEW 1') }
                'generated-macro-source' { $extra = Join-Path $source 'src/gitinfo.h'; $extras += $extra; [IO.File]::WriteAllText($extra, '#define ROUTINE_OPTIONAL_HEADER "sc_man.cpp"') }
                'operational-outside' { New-Item -ItemType Directory -Path (Join-Path $source 'completes') | Out-Null; $extra = Join-Path $source 'completes/review.def'; $extras += $extra; [IO.File]::WriteAllText($extra, '#include REVIEW_UNKNOWN') }
                'missing-generated' { Remove-Item -LiteralPath $generated }
                'shared-relative-header' { $unit = 'src/xlat/parse_xlat.cpp'; [IO.File]::WriteAllText($generated, '#include "../../src/sample.h"') }
                'shared-line-alias' { $unit = 'src/xlat/parse_xlat.cpp'; [IO.File]::WriteAllText($generated, ('#line 1 "' + (Join-Path $source 'src/sample.h').Replace('\', '/') + '"')) }
                'base-absolute-header' { $baseText = '#include "' + (Join-Path $source 'src/sample.h').Replace('\', '/') + '"' }
                'base-bom-absolute-header' { $baseText = [string][char]0xFEFF + '#include "' + (Join-Path $source 'src/sample.h').Replace('\', '/') + '"' }
                'base-macro-source' { $baseText = "#define REVIEW_BASE_SOURCE `"sc_man.cpp`"`n#include REVIEW_BASE_SOURCE`n" }
                'base-macro-unknown' { $baseText = '#include REVIEW_BASE_UNKNOWN' }
                'base-unresolved-source' { $baseText = '#include "missing.cpp"' }
                'base-included-def' {
                    $baseText = '#include "review.def"'
                    $extra = Join-Path $source 'src/review.def'; $extras += $extra
                    $bytes = [Text.UTF8Encoding]::new($false).GetBytes('#include "' + (Join-Path $source 'src/sample.h').Replace('\', '/') + '"')
                    [IO.File]::WriteAllBytes($extra, $bytes)
                    $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('hash-object', '-w', '--stdin') -InputBytes $bytes)).Trim()
                    $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--add', '--cacheinfo', "100644,$blob,src/review.def")
                    $caseTree = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('write-tree'))).Trim()
                }
                'base-guard-head-definition' {
                    $baseText = "#ifdef REVIEW_BASE_HEADER`n#include REVIEW_BASE_HEADER`n#endif`n"
                    [IO.File]::WriteAllText($guardPath, '#define REVIEW_BASE_HEADER "sample.h"')
                }
            }
            if ($case.StartsWith('base-')) {
                $bytes = [Text.UTF8Encoding]::new($false).GetBytes($baseText)
                $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('hash-object', '-w', '--stdin') -InputBytes $bytes)).Trim()
                $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--cacheinfo', "100644,$blob,src/sc_man.cpp")
                $baseTree = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('write-tree'))).Trim()
            }
            $result = & {
                param($routeScript, $source, $build, $caseTree, $baseTree, $project, $unit)
                $repositoryRoot = $source; $buildRoot = $build; $headCommit = $caseTree; $baseCommit = $baseTree
                $inputManifestData = $null; $sourceRoute = [PSCustomObject]@{ Mode = 'Candidate' }
                $cppcheckCacheRoot = 'fixture'; $CppcheckMode = 'Auto'; $cppcheckVersion = @('fixture')
                $changedFiles = @($unit); $headProjects = @($project)
                if ($unit -eq 'src/xlat/parse_xlat.cpp') { $project.Sources[$unit] = Join-Path $source $unit }
                $isWindowsVisualStudioBuild = $true; $BuildDir = 'build'
                $useFast = $false; $routeReason = ''; $probe = [PSCustomObject]@{ StateCalls = 0; BaseBlobReads = 0 }
                $gitReader = [scriptblock]::Create((Get-Command Get-CppcheckGitBytes).Definition)
                function Get-CppcheckGitBytes {
                    param($RepositoryRoot, $Arguments, $InputBytes = $null)
                    if ($Arguments[0] -eq 'cat-file' -and $Arguments[2].StartsWith($baseCommit + ':')) { $probe.BaseBlobReads++ }
                    $bytes = & $gitReader -RepositoryRoot $RepositoryRoot -Arguments $Arguments -InputBytes $InputBytes
                    return ,$bytes
                }
                function Enter-CppcheckCacheLock { return $null }
                function Get-CppcheckPreparationReceipt { return [PSCustomObject]@{ Anchor = $caseTree; PreparationVerified = $true; State = 'valid unchanged receipt' } }
                function Get-CppcheckTreeChanges { return @() }
                function Get-CppcheckPreparationState { $probe.StateCalls++; return 'valid unchanged receipt' }
                . $routeScript
                [PSCustomObject]@{ Mode = $(if ($useFast) { 'Fast' } else { 'Full' }); Reason = $routeReason; StateCalls = $probe.StateCalls; BaseBlobReads = $probe.BaseBlobReads }
            } $routeScript $source $build $caseTree $baseTree $project $unit
            $expected = if ($case -in @('ordinary', 'approved-generated', 'operational-outside', 'shared-line-alias')) { 'Fast' } else { 'Full' }
            if ($result.Mode -ne $expected) { throw "Safety route mismatch for ${case}: $($result | ConvertTo-Json -Compress)." }
            if ($expected -eq 'Full' -and $result.StateCalls) { throw "Unsafe $case reached receipt comparison before rejection." }
            if ($expected -eq 'Fast' -and $result.StateCalls -ne 1) { throw "Safe $case did not compare the preparation receipt." }
            if (($expected -eq 'Fast' -or $case.StartsWith('base-')) -and $result.BaseBlobReads -ne 1) { throw "Selected Base TU was not inspected before routing for '$case'." }
            if ($case -eq 'missing-generated' -and $result.Reason -notmatch 'Missing approved generated source inclusion') { throw 'Missing generated input did not route Full for regeneration.' }
            $results += [PSCustomObject]@{ Case = $case; Result = $result; Expected = $expected; TU = [IO.File]::ReadAllText((Join-Path $source $unit)); Include = [IO.File]::ReadAllText($guardPath); Generated = $(if (Test-Path $generated) { [IO.File]::ReadAllText($generated) } else { $null }); Tree = $caseTree; BaseTree = $baseTree; BaseTU = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('cat-file', 'blob', "${baseTree}:$unit"))) }
        }
        finally {
            if ($case -in @('tracked-def', 'base-included-def')) { $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--force-remove', 'src/review.def') }
            if ($case.StartsWith('base-')) {
                $bytes = [Text.UTF8Encoding]::new($false).GetBytes($files['src/sc_man.cpp'])
                $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('hash-object', '-w', '--stdin') -InputBytes $bytes)).Trim()
                $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--cacheinfo', "100644,$blob,src/sc_man.cpp")
            }
            foreach ($extra in $extras) { Remove-Item -LiteralPath $extra -Force }
            [IO.File]::WriteAllText($unitPath, $files['src/sc_man.cpp'])
            [IO.File]::WriteAllText($guardPath, $files['src/guard.h'])
            [IO.File]::WriteAllText($generated, 'int generated;')
        }
    }
    $results | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $root 'route-results.json')
    Get-FileHash $LintScript, (Join-Path $RepositoryRoot 'scripts/test-lint-vendor-dispositions.ps1') | ConvertTo-Json | Set-Content (Join-Path $root 'script-hashes.json')
    Write-Host "R1/R2/R4 actual Auto preflight concrete safety routes: passed ($root)."
}

function Assert-CppcheckFastNativeRoute {
    param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot), [string]$PreparedFixtureRoot = '', [string]$PreparedCommitFile = 'input-commits.json')

    $root = if ($PreparedFixtureRoot) { $PreparedFixtureRoot } else { Join-Path $RepositoryRoot ('completes/lint-source-tu-fast-path/phase-1/n-' + [guid]::NewGuid().ToString('N').Substring(0, 8)) }
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'commands') | Out-Null
    $evidence = [PSCustomObject]@{ Root = $root; Counter = 0 }
    $source = Join-Path $root 'r'
    $baseRoot = Join-Path $root 'b'
    $build = Join-Path $source 'build-v143'
    $baseBuild = Join-Path $baseRoot 'build-v143'
    $analyzer = (Get-Command cppcheck -ErrorAction Stop).Source
    $version = & $analyzer --version
    $hash = (Get-FileHash $analyzer).Hash
    $cmakePath = (Get-Command cmake -ErrorAction Stop).Source
    $unit = 'src/sc_man.cpp'
    if (-not $PreparedFixtureRoot) {
    $clone = Invoke-LintChild -Path 'git' -Arguments @('clone', '--shared', '--no-hardlinks', $RepositoryRoot, $source) -Label 'native-fixture-clone' -Evidence $evidence
    if ($clone.ExitCode) { throw 'Native Fast fixture clone failed.' }
    $original = [IO.File]::ReadAllText((Join-Path $source $unit))
    $original += "`n#if defined(FAST_FIXTURE_GENERATED)`nint fast_fixture_output() { int *outputPointer = 0; return *outputPointer; }`n#endif`n"
    $original += "`n#if defined(FAST_FIXTURE_CONTEXT)`nint fast_fixture_context() { int *contextPointer = 0; return *contextPointer; }`n#endif`n"
    foreach ($relative in @('scripts/lint.ps1', 'scripts/cppcheck-cache.ps1', 'scripts/test-lint-vendor-dispositions.ps1')) {
        [IO.File]::Copy((Join-Path $RepositoryRoot $relative), (Join-Path $source $relative), $true)
        $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('hash-object', '-w', '--stdin') -InputBytes ([IO.File]::ReadAllBytes((Join-Path $source $relative))))).Trim()
        $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--cacheinfo', "100644,$blob,$relative")
    }
    $commits = @()
    foreach ($suffix in @(
        "`nint fast_fixture_existing() { int *basePointer = 0; return *basePointer; }`n",
        "`n`n`nint fast_fixture_existing() { int *basePointer = 0; return *basePointer; }`nint fast_fixture_new() { int *headPointer = 0; return *headPointer; }`n",
        "`n`n`nint fast_fixture_existing() { int *basePointer = 0; return *basePointer; }`nint fast_fixture_new() { int *headPointer = 0; return *headPointer; }`nint fast_fixture_advanced() { int *nextPointer = 0; return *nextPointer; }`n"
    )) {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($original + $suffix)
        $blob = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('hash-object', '-w', '--stdin') -InputBytes $bytes)).Trim()
        $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('update-index', '--cacheinfo', "100644,$blob,$unit")
        $tree = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('write-tree'))).Trim()
        $parent = if ($commits.Count) { $commits[-1] } else { [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('rev-parse', 'HEAD'))).Trim() }
        $commit = [Text.Encoding]::UTF8.GetString((Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('-c', 'user.name=Fast fixture', '-c', 'user.email=fast-fixture@example.invalid', 'commit-tree', $tree, '-p', $parent, '-m', 'isolated source-only fixture'))).Trim()
        $commits += $commit
        [IO.File]::WriteAllBytes((Join-Path $root ("input-$($commits.Count).cpp")), $bytes)
    }
    $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('checkout', '--detach', '--force', $commits[1])
    $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('worktree', 'add', '--detach', $baseRoot, $commits[0])
    $cacheFile = Join-Path $RepositoryRoot 'build-v143/CMakeCache.txt'
    foreach ($pair in @(@($source, $build), @($baseRoot, $baseBuild))) {
        $arguments = @('-S', $pair[0], '-B', $pair[1], '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143')
        foreach ($name in @('BUILD_TESTING', 'DYN_FLUIDSYNTH', 'NO_SOUND', 'FMOD_INCLUDE_DIR', 'FMOD_LIBRARY', 'OPENAL_INCLUDE_DIR', 'OPENAL_LIBRARY', 'OPUS_INCLUDE_DIR', 'OPUS_LIBRARIES', 'ZSTD_INCLUDE_DIR', 'ZSTD_LIBRARY', 'FLUIDSYNTH_INCLUDE_DIR', 'FLUIDSYNTH_LIBRARIES')) {
            $setting = Get-CMakeCacheSetting -CachePath $cacheFile -Name $name
            if ($setting -and $setting.Value -notmatch 'NOTFOUND') { $arguments += "-D$($name):$($setting.Type)=$($setting.Value)" }
        }
        $configure = Invoke-LintChild -Path $cmakePath -Arguments $arguments -Label "configure-$([IO.Path]::GetFileName($pair[0]))" -Evidence $evidence
        if ($configure.ExitCode) { throw "Native fixture configure failed: $($configure.Output -join [Environment]::NewLine)" }
        Invoke-GeneratedBuildInputPreparation -CMakePath $cmakePath -BuildRoot $pair[1] -RevisionName ([IO.Path]::GetFileName($pair[0])) -Evidence $evidence
        Invoke-ProtocolspecGeneration -CMakePath $cmakePath -BuildRoot $pair[1] -RevisionName ([IO.Path]::GetFileName($pair[0])) -Evidence $evidence
    }
    $revision = Invoke-LintChild -Path $cmakePath -Arguments @('--build', $build, '--config', 'Release', '--target', 'revision_check') -Label 'fixture-head-revision' -Evidence $evidence
    if ($revision.ExitCode) { throw 'Native fixture revision preparation failed.' }
    $commits | ConvertTo-Json | Set-Content (Join-Path $root 'input-commits.json')
    }
    else {
        $commits = @(Get-Content (Join-Path $root $PreparedCommitFile) -Raw | ConvertFrom-Json)
        $evidence.Counter = @(Get-ChildItem (Join-Path $root 'commands') -Filter '*.invocation.json').Count
    }
    Push-Location $source
    try { $bundle = Get-VerifiedGeneratedBundle -SourceRoot $source -GeneratedOutputRoot $source -BaseCommit $commits[0] -HeadCommit $commits[1] -TempRoot (Join-Path $root 'verification') -RelativePaths $generatedBundlePaths -PythonPath (Get-CMakePythonExecutable -CachePath (Join-Path $build 'CMakeCache.txt')) -Evidence $evidence }
    finally { Pop-Location }
    Copy-VerifiedGeneratedBundle -VerifiedBundle $bundle -DestinationRoot $baseRoot
    $projects = @(Get-ProjectSources -BuildRoot $build -RepositoryRoot $source | Where-Object RelativeProject -eq 'src/zdoom.vcxproj')
    $baseProjects = @(Get-ProjectSources -BuildRoot $baseBuild -RepositoryRoot $baseRoot)
    $projects[0].Files = @($unit)
    Assert-CppcheckFastSourceSafety -RepositoryRoot $source -BuildRoot $build -HeadCommit $commits[1] -ChangedFiles @($unit) -Projects $projects
    $state = Get-CppcheckPreparationState -RepositoryRoot $source -BuildRoot $build -Projects $projects -AnalyzerVersion $version
    $receipt = New-CppcheckPreparationReceipt -State $state -HeadCommit $commits[1] -RepositoryRoot $source -BuildRoot $build -Projects $projects -BaselineProjects $baseProjects -BaselineRoot $baseRoot -BaselineBuildRoot $baseBuild -AnalyzerVersion $version
    $cache = Join-Path $source '.cppcheck-cache'
    $attemptRoot = Join-Path $root ('attempt-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $lock = Enter-CppcheckCacheLock -CacheRoot $cache -Name 'regression'
    try {
        Save-CppcheckPreparationReceipt -CacheRoot $cache -BuildRoot $build -Receipt $receipt
        $runs = @()
        foreach ($run in @('cold', 'warm', 'advanced', 'base-changed', 'fresh')) {
            if ($run -eq 'advanced') { $null = Get-CppcheckGitBytes -RepositoryRoot $source -Arguments @('checkout', '--detach', $commits[2]) }
            $headCommit = if ($run -in @('advanced', 'base-changed', 'fresh')) { $commits[2] } else { $commits[1] }
            $baseCommit = if ($run -in @('base-changed', 'fresh')) { $commits[1] } else { $commits[0] }
            $runCache = if ($run -eq 'fresh') { Join-Path $cache 'fresh' } else { $cache }
            $stage = New-CppcheckDisposableStage -CacheRoot $cache -Name 'regression-fast'
            $runRoot = Join-Path $attemptRoot $run
            New-Item -ItemType Directory -Force -Path (Join-Path $runRoot 'commands') | Out-Null
            $script:lintTotalTimer = [Diagnostics.Stopwatch]::StartNew()
            try {
                $result = Invoke-CppcheckFastAnalysis -RepositoryRoot $source -BuildRoot $build -Projects $projects -State $state -HeadCommit $headCommit -BaseCommit $baseCommit -TempRoot $stage -CacheRoot $runCache -CppcheckPath $analyzer -AnalyzerVersion $version -AnalyzerSHA256 $hash -CppcheckJobs 1 -Evidence ([PSCustomObject]@{ Root = $runRoot; Counter = 0 })
                $result | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $runRoot 'result.json')
                $runs += $result
            }
            finally { Remove-CppcheckDisposableStage -CacheRoot $cache -StageRoot $stage -Name 'regression-fast' }
            $calls = @(Get-ChildItem (Join-Path $runRoot 'commands') -Filter '*.invocation.json' | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
            if (@($calls | Where-Object { $_.Path -eq $analyzer }).Count -ne 2 -or @($calls | Where-Object { $_.Path -match 'cmake|lemon|re2c|python' -or $_.Arguments -contains 'worktree' }).Count) { throw 'Fast did not execute exactly the two selected native descriptors with zero preparation calls.' }
            if ($run -in @('warm', 'advanced')) {
                $baseRaw = Get-Content (Get-ChildItem (Join-Path $runRoot 'commands') -Filter '*cppcheck*.raw.txt' | Sort-Object Name | Select-Object -Last 1).FullName -Raw
                if ($baseRaw -notmatch 'skipping analysis - loaded [0-9]+ cached finding\(s\)') { throw 'Recreated Fast stage did not reuse native Base cache.' }
                if ($result.CachePaths["baseline/src/zdoom.vcxproj/$unit"] -cne $runs[0].CachePaths["baseline/src/zdoom.vcxproj/$unit"]) { throw 'HEAD advance changed the native Base leaf.' }
            }
            if (@($result.Head | Where-Object Identifier -eq 'nullPointer').Count -lt 2 -or @($result.Baseline | Where-Object Identifier -eq 'nullPointer').Count -lt 1) { throw 'Native Fast source-only diagnostic fixture was not detected.' }
        }
        if (($runs[0].Baseline.Fingerprint -join '|') -cne ($runs[2].Baseline.Fingerprint -join '|')) { throw 'HEAD advance changed Base diagnostics.' }
        if (($runs[3].Baseline.Fingerprint -join '|') -cne ($runs[4].Baseline.Fingerprint -join '|') -or ($runs[3].Head.Fingerprint -join '|') -cne ($runs[4].Head.Fingerprint -join '|') -or @($runs[3].Baseline | Where-Object { $_.Message -match 'headPointer' }).Count -eq 0) { throw 'Base blob native re-evaluation differs from fresh diagnostics.' }
        $stage = New-CppcheckDisposableStage -CacheRoot $cache -Name 'regression-fast'
        $stageSource = Join-Path $stage 'source'
        Push-Location $source
        try { Expand-GitArchive -Commit $commits[2] -Destination $stageSource -Evidence $evidence }
        finally { Pop-Location }
        try {
            foreach ($relative in $generatedBundlePaths + $fastSourceGeneratedPaths) { [IO.File]::Copy((Join-Path $source $relative), (Join-Path $stageSource $relative), $true) }
            $mapped = New-CppcheckFastProject -Project $projects[0] -RepositoryRoot $source -BuildRoot $build -StageSourceRoot $stageSource -ProjectRoot (Join-Path $stage 'project') -AnalyzerVersion $version
            $header = Join-Path $stageSource 'src/sc_man.h'
            [IO.File]::AppendAllText($header, "`ninline int fast_fixture_header() { int *headerPointer = 0; return *headerPointer; }`n")
            $generated = Join-Path $build 'src/sc_man_scanner.h'
            $generatedBytes = [IO.File]::ReadAllBytes($generated)
            try {
                foreach ($scenario in @('header', 'generated', 'context')) {
                    if ($scenario -eq 'generated') { [IO.File]::AppendAllText($generated, "`n#define FAST_FIXTURE_GENERATED 1`n") }
                    if ($scenario -eq 'context') {
                        [xml]$document = [IO.File]::ReadAllText($mapped.ProjectPath)
                        foreach ($node in $document.SelectNodes("//*[local-name()='PreprocessorDefinitions']")) { $node.InnerText = 'FAST_FIXTURE_CONTEXT;' + $node.InnerText }
                        $document.Save($mapped.ProjectPath)
                    }
                    $scenarioRoot = Join-Path $attemptRoot $scenario
                    New-Item -ItemType Directory -Force -Path (Join-Path $scenarioRoot 'commands') | Out-Null
                    $scenarioEvidence = [PSCustomObject]@{ Root = $scenarioRoot; Counter = 0 }
                    $diagnostics = @()
                    foreach ($role in @('cached', 'fresh')) {
                        $leaf = if ($role -eq 'cached') { $runs[2].CachePaths["head/src/zdoom.vcxproj/$unit"] } else { Join-Path $cache ("fresh-$scenario") }
                        $descriptor = New-CppcheckDescriptor -CppcheckPath $analyzer -ProjectPath $mapped.ProjectPath -CachePath $leaf -RepositoryRoot $stageSource -BuildRoot $build -TargetName 'src/zdoom.vcxproj' -TranslationUnit $unit -Index 0
                        $descriptor.WorkingDirectory = $stageSource
                        $descriptor | Add-Member -NotePropertyName DiagnosticFileAliases -NotePropertyValue $state.Aliases
                        $diagnostics += ,@(Invoke-CppcheckBatch -Descriptors @($descriptor) -CppcheckJobs 1 -Evidence $scenarioEvidence)
                    }
                    $diagnostics | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $scenarioRoot 'diagnostics.json')
                    if (($diagnostics[0].Fingerprint -join '|') -cne ($diagnostics[1].Fingerprint -join '|') -or @($diagnostics[0] | Where-Object Message -match 'headerPointer').Count -eq 0) { throw "Native $scenario re-evaluation differs from fresh diagnostics." }
                    if ($scenario -eq 'generated' -and @($diagnostics[0] | Where-Object Message -match 'outputPointer').Count -eq 0) { throw 'Changed generated input did not produce its native diagnostic.' }
                    if ($scenario -eq 'context' -and @($diagnostics[0] | Where-Object Message -match 'contextPointer').Count -eq 0) { throw 'Changed project context did not produce its native diagnostic.' }
                }
            }
            finally { [IO.File]::WriteAllBytes($generated, $generatedBytes) }
        }
        finally { Remove-CppcheckDisposableStage -CacheRoot $cache -StageRoot $stage -Name 'regression-fast' }
        @{ Base = $commits[0]; HEAD = $commits[1]; AdvancedHEAD = $commits[2]; Target = 'src/zdoom.vcxproj'; TU = $unit; Analyzer = $analyzer; AnalyzerSHA256 = $hash; AnalyzerVersion = $version; Receipt = $receipt; ScriptHashes = @(Get-FileHash (Join-Path $RepositoryRoot 'scripts/lint.ps1'), (Join-Path $RepositoryRoot 'scripts/cppcheck-cache.ps1'), (Join-Path $RepositoryRoot 'scripts/test-lint-vendor-dispositions.ps1')) } | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $attemptRoot 'identities.json')
    }
    finally { Exit-CppcheckCacheLock -Lock $lock }
    $auto = Invoke-LintChild -Path (Get-Process -Id $PID).Path -Arguments @('-NoProfile', '-File', (Join-Path $source 'scripts/lint.ps1'), '-BuildDir', 'build-v143', '-BaseSha', $commits[0], '-HeadSha', 'HEAD', '-CppcheckMode', 'Auto', '-CppcheckJobs', '1') -Label 'actual-auto' -WorkingDirectory $source -Evidence $evidence
    if ($auto.ExitCode -ne 1 -or ($auto.Output -join "`n") -notmatch 'Cppcheck mode: Fast;' -or ($auto.Output -join "`n") -notmatch 'selected TUs=1') { throw "Actual Auto did not use the verified 1TU Fast route: $($auto.Output -join [Environment]::NewLine)" }
    Write-Host "Real Release|x64 Fast cold/warm/HEAD-advance/Auto: passed ($root)."
}

function Save-NativeCppcheckCacheEvidence {
    param(
        [string]$FixtureRoot,
        [string]$EvidenceRoot
    )

    $runRoot = Join-Path $evidenceRoot ('native-cache-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $FixtureRoot 'native-cache/evidence') -Destination (Join-Path $runRoot 'isolated-fixture') -Recurse
    Copy-Item -LiteralPath (Join-Path $FixtureRoot 'real-project-evidence') -Destination (Join-Path $runRoot 'generated-project') -Recurse
    Copy-Item -LiteralPath (Join-Path $FixtureRoot 'native-cache/build/compile_commands.json') -Destination (Join-Path $runRoot 'compile_commands.json')
}

function Assert-RealCppcheckProjectCacheRoute {
    param(
        [string]$FixtureRoot,
        [string]$CacheRoot,
        [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
    )

    $cppcheck = Get-Command cppcheck -ErrorAction Stop
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
        [string]$CacheRoot,
        [string]$LintScript = (Join-Path $PSScriptRoot 'lint.ps1')
    )

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
Import-LintFunction -Name 'Assert-MaterializedProtocolspecProvenance'
Import-LintFunction -Name 'Save-LintEvidenceState'
Import-LintFunction -Name 'Save-LintEvidenceResult'
Import-LintFunction -Name 'Complete-LintFinalization'
Import-LintFunction -Name 'Invoke-LintChild'
Import-LintFunction -Name 'Save-LintChildEvidence'
Import-LintFunction -Name 'New-CppcheckDescriptor'
Import-LintFunction -Name 'Invoke-CppcheckProcessBatch'
Import-LintFunction -Name 'Invoke-CppcheckBatch'
Import-LintFunction -Name 'Invoke-CppcheckProject'
Import-LintFunction -Name 'Select-CppcheckProjects'

$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('zn-vp-' + [guid]::NewGuid().ToString('N'))

try {
    Assert-CppcheckProjectSelection
    Assert-CppcheckSourceRouting
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
    Assert-ProductionPreparationFailureReleasesAcquiredLock
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
    $schedulerEvidenceRoot = Join-Path (Split-Path -Parent $PSScriptRoot) ('completes/lint-cppcheck-tu-parallel/suite-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $schedulerEvidenceRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $fixtureRoot 'native-cache') -Destination (Join-Path $schedulerEvidenceRoot 'native-cache') -Recurse
    Assert-CppcheckBatchIntegration
    Assert-CppcheckSchedulerTransport -FixtureRoot $schedulerEvidenceRoot
    Assert-CppcheckRoleClassification -FixtureRoot $schedulerEvidenceRoot
    Assert-CppcheckSchedulerFailures -FixtureRoot $schedulerEvidenceRoot
    Assert-NativeCppcheckBatchSmoke -FixtureRoot $fixtureRoot
    Copy-Item -LiteralPath (Join-Path $fixtureRoot 'native-smoke') -Destination (Join-Path $schedulerEvidenceRoot 'native-smoke') -Recurse
    Save-NativeCppcheckCacheEvidence -FixtureRoot $fixtureRoot -EvidenceRoot (Join-Path (Split-Path -Parent $PSScriptRoot) 'completes/windows-prepush-platform-scope')
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
    & git -C $fixtureRoot init -q
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize canonical text hash fixture repository.' }
    & git -C $fixtureRoot add -- $adapterPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not track canonical text hash fixture.' }
    [System.IO.File]::WriteAllBytes((Join-Path $fixtureRoot $adapterPath), [System.Text.Encoding]::ASCII.GetBytes("ma_decoder_init_memory()`r`n"))
    $canonicalAdapterHash = Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath -NormalizeTrackedTextNewlines
    $expectedCanonicalAdapterHash = Get-ByteSHA256 -Bytes ([System.Text.Encoding]::ASCII.GetBytes("ma_decoder_init_memory()`n"))
    if ($canonicalAdapterHash -ne $expectedCanonicalAdapterHash) { throw 'Tracked CRLF API root did not use canonical LF hash.' }
    [System.IO.File]::WriteAllBytes((Join-Path $fixtureRoot $adapterPath), [System.Text.Encoding]::ASCII.GetBytes("changed_adapter`r`n"))
    if ((Get-VendorPolicyFileHash -RepositoryRoot $fixtureRoot -RelativePath $adapterPath -NormalizeTrackedTextNewlines) -eq $expectedCanonicalAdapterHash) { throw 'Canonical API root hash accepted a content mutation.' }
    [System.IO.File]::WriteAllBytes((Join-Path $fixtureRoot $adapterPath), [System.Text.Encoding]::ASCII.GetBytes('ma_decoder_init_memory()'))
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
    $protocolspecFixtureRoot = Join-Path $fixtureRoot 'protocolspec'
    $protocolspecFixturePath = Join-Path $protocolspecFixtureRoot 'protocolspec/spec.txt'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $protocolspecFixturePath) | Out-Null
    $protocolspecBlob = @(& git rev-parse 'HEAD:protocolspec/spec.txt')
    if (($LASTEXITCODE -ne 0) -or ($protocolspecBlob.Count -ne 1)) { throw 'Could not resolve protocolspec fixture blob.' }
    $protocolspecSourcePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'protocolspec\spec.txt'
    $protocolspecBytes = [System.IO.File]::ReadAllBytes($protocolspecSourcePath)
    $protocolspecLfStream = New-Object System.IO.MemoryStream
    try {
        for ($index = 0; $index -lt $protocolspecBytes.Length; $index++) {
            if ($protocolspecBytes[$index] -eq [byte]13) {
                if (($index + 1 -ge $protocolspecBytes.Length) -or ($protocolspecBytes[$index + 1] -ne [byte]10)) { throw 'Protocolspec fixture contains a bare CR byte.' }
                $index++
            }
            $protocolspecLfStream.WriteByte($protocolspecBytes[$index])
        }
        [System.IO.File]::WriteAllBytes($protocolspecFixturePath, (Convert-LFBytesToCRLFBytes -Bytes $protocolspecLfStream.ToArray()))
    }
    finally {
        $protocolspecLfStream.Dispose()
    }
    Assert-MaterializedProtocolspecProvenance -SourceRoot $protocolspecFixtureRoot -ExpectedInputs @{ 'protocolspec/spec.txt' = $protocolspecBlob[0].Trim() }
    [System.IO.File]::AppendAllText($protocolspecFixturePath, 'mutated')
    Assert-ExpectedException -Expected 'protocolspec inputs differ' -Action {
        Assert-MaterializedProtocolspecProvenance -SourceRoot $protocolspecFixtureRoot -ExpectedInputs @{ 'protocolspec/spec.txt' = $protocolspecBlob[0].Trim() }
    }
    Remove-Item -LiteralPath $protocolspecFixturePath -Force
    Assert-ExpectedException -Expected 'protocolspec inputs differ' -Action {
        Assert-MaterializedProtocolspecProvenance -SourceRoot $protocolspecFixtureRoot -ExpectedInputs @{ 'protocolspec/spec.txt' = $protocolspecBlob[0].Trim() }
    }
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
    Assert-CppcheckFastContracts
    Assert-CppcheckFastSafetyPreflight
    Assert-CppcheckFastNativeRoute -PreparedFixtureRoot $PreparedNativeFixtureRoot -PreparedCommitFile $PreparedNativeCommitFile
    Write-Host 'cppcheck vendor disposition tests: PASS'
}
finally {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
}