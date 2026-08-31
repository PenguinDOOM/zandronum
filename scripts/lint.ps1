param(
    [string]$BuildDir = "build-v143",
    [string]$BaseSha = "",
    [string]$HeadSha = "HEAD",
    [int]$CppcheckJobs = [Math]::Min(12, [Environment]::ProcessorCount)
)

$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$ErrorActionPreference = "Stop"

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

function Assert-WorkspaceProtocolspecProvenance {
    param(
        [string]$RepositoryRoot,
        [string]$HeadCommit,
        [hashtable]$ExpectedInputs
    )

    $differentPaths = @()

    foreach ($relativePath in @($ExpectedInputs.Keys | Sort-Object)) {
        $workspacePath = Join-Path $RepositoryRoot $relativePath

        if (-not (Test-Path -LiteralPath $workspacePath -PathType Leaf)) {
            $differentPaths += $relativePath
            continue
        }

        $workspaceItem = Get-Item -LiteralPath $workspacePath -Force

        if (($workspaceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            $differentPaths += $relativePath
            continue
        }

        & git -C $RepositoryRoot diff --quiet $HeadCommit -- $relativePath

        if ($LASTEXITCODE -ne 0) {
            $differentPaths += $relativePath
        }
    }

    if ($differentPaths.Count -ne 0) {
        throw "Generated bundle cannot be materialized because protocolspec workspace inputs differ from checked-out HEAD: $($differentPaths -join ', ')."
    }
}

function Get-VerifiedGeneratedBundle {
    param(
        [string]$RepositoryRoot,
        [string]$BaseCommit,
        [string]$HeadCommit,
        [string]$TempRoot,
        [string[]]$RelativePaths,
        [string]$PythonPath
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

        $workspacePath = Join-Path $RepositoryRoot $relativePath

        if (-not [System.IO.File]::Exists($workspacePath)) {
            throw "Generated bundle path '$relativePath' is missing or is not a normal workspace file."
        }

        $workspaceItem = Get-Item -LiteralPath $workspacePath -Force

        if (($workspaceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Generated bundle path '$relativePath' must be a normal workspace file, not a reparse point."
        }

        & git check-ignore --quiet -- $relativePath

        if ($LASTEXITCODE -ne 0) {
            throw "Generated bundle path '$relativePath' must be ignored by Git."
        }

        & git ls-files --error-unmatch -- $relativePath 2>$null

        if ($LASTEXITCODE -eq 0) {
            throw "Generated bundle path '$relativePath' must remain untracked by Git."
        }

        if ($LASTEXITCODE -ne 1) {
            throw "Could not verify the Git tracking contract for generated bundle path '$relativePath'."
        }
    }

    $headInputs = Assert-EqualProtocolspecProvenance -BaseCommit $BaseCommit -HeadCommit $HeadCommit
    Assert-WorkspaceProtocolspecProvenance -RepositoryRoot $RepositoryRoot -HeadCommit $HeadCommit -ExpectedInputs $headInputs
    $generatedRoot = Join-Path $TempRoot 'generated'
    $sourceOutput = Join-Path $generatedRoot $RelativePaths[0]
    $headerOutput = Join-Path $generatedRoot $RelativePaths[1]
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sourceOutput) | Out-Null

    $generatorPath = Join-Path $RepositoryRoot 'protocolspec/generator/codegenerator.py'
    $specPath = Join-Path $RepositoryRoot 'protocolspec/spec.txt'
    $generatorDirectory = Split-Path -Parent $generatorPath
    Push-Location $generatorDirectory

    try {
        $generatorOutput = @(& $PythonPath $generatorPath --spec $specPath --source $sourceOutput --header $headerOutput 2>&1 | ForEach-Object { $_.ToString() })

        if ($LASTEXITCODE -ne 0) {
            $details = ($generatorOutput -join [Environment]::NewLine).Replace($RepositoryRoot, '<repo>').Replace($TempRoot, '<temp>')
            throw "Protocolspec generator failed: $details"
        }
    }
    finally {
        Pop-Location
    }

    $verifiedBundle = @()

    foreach ($relativePath in $RelativePaths) {
        $generatedPath = Join-Path $generatedRoot $relativePath
        $workspacePath = Join-Path $RepositoryRoot $relativePath

        if (-not [System.IO.File]::Exists($generatedPath)) {
            throw "Protocolspec generator did not produce expected bundle path '$relativePath'."
        }

        $generatedHash = (Get-FileHash -LiteralPath $generatedPath -Algorithm SHA256).Hash
        $workspaceHash = (Get-FileHash -LiteralPath $workspacePath -Algorithm SHA256).Hash

        if ($generatedHash -ne $workspaceHash) {
            throw "Generated bundle verification failed for '$relativePath': workspace SHA-256 does not match generator output."
        }

        $verifiedBundle += [PSCustomObject]@{
            RelativePath = $relativePath
            GeneratedPath = $generatedPath
            Hash = $generatedHash
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
        [string]$RevisionName
    )

    $toolOutput = @(& $CMakePath --build $BuildRoot --config Release --target lemon re2c 2>&1 | ForEach-Object { $_.ToString() })

    if ($LASTEXITCODE -ne 0) {
        $details = $toolOutput -join [Environment]::NewLine
        throw "Failed to build generated-input tools for ${RevisionName}: $details"
    }

    foreach ($command in Get-GeneratedBuildInputCommands -BuildRoot $BuildRoot) {
        $batchPath = Join-Path ([System.IO.Path]::GetTempPath()) ("zandronum-cppcheck-generator-" + [guid]::NewGuid().ToString('N') + '.cmd')
        $projectDirectory = Join-Path $BuildRoot 'src'

        try {
            [System.IO.File]::WriteAllText($batchPath, "@echo off`r`n" + $command.Command, $utf8)
            Push-Location -LiteralPath $projectDirectory

            try {
                $commandOutput = @(& $env:ComSpec /d /c $batchPath 2>&1 | ForEach-Object { $_.ToString() })
            }
            finally {
                Pop-Location
            }

            if ($LASTEXITCODE -ne 0) {
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

function Invoke-CppcheckProject {
    param(
        [string]$CppcheckPath,
        [string]$ProjectPath,
        [string]$CachePath,
        [string]$RepositoryRoot,
        [string]$BuildRoot,
        [string]$TargetName,
        [int]$CppcheckJobs
    )

    New-Item -ItemType Directory -Force -Path $CachePath | Out-Null

    $template = '{file}' + "`t" + '{severity}' + "`t" + '{id}' + "`t" + '{message}'
    $arguments = @(
        "--project=$ProjectPath"
        "--project-configuration=Release|x64"
        "--enable=warning,performance,portability"
        "--error-exitcode=1"
        "--cppcheck-build-dir=$CachePath"
        "--template=$template"
        "--quiet"
        "-j"
        "$CppcheckJobs"
    )

    $output = @(& $CppcheckPath @arguments 2>&1 | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    $diagnostics = @()
    $unexpectedOutput = @()

    foreach ($line in $output) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $match = [regex]::Match($line, '^(?<file>.+?)\t(?<severity>[^\t]+)\t(?<id>[^\t]+)\t(?<message>.*)$')

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
            Display = "${TargetName}: $relativePath [$severity/$identifier] $message"
            RelativePath = $relativePath
            Identifier = $identifier
        }
    }

    $missingFileDiagnostics = @($diagnostics | Where-Object { $_.Identifier -eq 'missingFile' })

    if ($missingFileDiagnostics.Count -ne 0) {
        $missingPaths = @($missingFileDiagnostics | ForEach-Object { $_.RelativePath } | Sort-Object -Unique)
        $unsupportedPaths = @($missingPaths | Where-Object { $_ -notin $generatedBundlePaths })

        if ($unsupportedPaths.Count -ne 0) {
            throw "Cppcheck reported unsupported missing input(s): $($unsupportedPaths -join ', '). Only src/network/servercommands.cpp and src/network/servercommands.h may be materialized."
        }

        throw "Cppcheck reported missing verified generated input(s): $($missingPaths -join ', ')."
    }

    if (($exitCode -ne 0) -and ($diagnostics.Count -eq 0)) {
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

Write-Host "Cppcheck parallel jobs: $CppcheckJobs"

$repositoryRoot = Get-GitSingleLine -Arguments @('rev-parse', '--show-toplevel') -ErrorMessage 'This script must run inside a Git worktree.'
$headCommit = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', "$HeadSha^{commit}") -ErrorMessage "HeadSha '$HeadSha' does not resolve to a commit."
$workspaceHead = Get-GitSingleLine -Arguments @('rev-parse', '--verify', '--quiet', 'HEAD^{commit}') -ErrorMessage 'Could not resolve the current worktree HEAD.'

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

$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDir))
$cacheFile = Join-Path $buildRoot 'CMakeCache.txt'

if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) {
    Write-Error "No CMake cache found in '$BuildDir'. Configure the project first."
    exit 1
}

$changedFiles = @(
    & git diff --name-only --diff-filter=ACMR $baseCommit $headCommit -- '*.c' '*.cc' '*.cpp' '*.cxx'

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to determine changed C/C++ source files."
        exit $LASTEXITCODE
    }
)

$changedFiles = @($changedFiles | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)

if ($changedFiles.Count -eq 0) {
    Write-Host "No changed C/C++ translation units require Cppcheck."
    exit 0
}

$headProjects = Get-ProjectSources -BuildRoot $buildRoot -RepositoryRoot $repositoryRoot

foreach ($file in $changedFiles) {
    $projectMatches = @($headProjects | Where-Object { $_.Sources.ContainsKey($file) })

    if ($projectMatches.Count -eq 0) {
        Write-Error "Changed source '$file' is not present in any generated Visual Studio project. Reconfigure '$BuildDir' before linting."
        exit 1
    }

    foreach ($project in $projectMatches) {
        $project.Files += $file
    }
}

$analysisProjects = @($headProjects | Where-Object { $_.Files.Count -ne 0 })
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("zandronum-cppcheck-" + [guid]::NewGuid().ToString('N'))
$baselineRoot = Join-Path $tempRoot 'baseline'
$baselineBuildRoot = Join-Path $baselineRoot 'build'
$baselineWorktreeCreated = $false

try {
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

    $verifiedGeneratedBundle = Get-VerifiedGeneratedBundle `
        -RepositoryRoot $repositoryRoot `
        -BaseCommit $baseCommit `
        -HeadCommit $headCommit `
        -TempRoot $tempRoot `
        -RelativePaths $generatedBundlePaths `
        -PythonPath $python

    foreach ($generatedFile in $verifiedGeneratedBundle) {
        $baselinePath = Join-Path $baselineRoot $generatedFile.RelativePath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $baselinePath) | Out-Null
        Copy-Item -LiteralPath $generatedFile.GeneratedPath -Destination $baselinePath -Force
        $baselineHash = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash

        if ($baselineHash -ne $generatedFile.Hash) {
            throw "Generated bundle materialization failed for '$($generatedFile.RelativePath)': destination SHA-256 does not match verified generator output."
        }
    }

    Write-Host 'Protocolspec provenance equality: confirmed.'

    foreach ($generatedFile in $verifiedGeneratedBundle) {
        Write-Host "Generated baseline input: $($generatedFile.RelativePath) SHA-256 $($generatedFile.Hash)"
    }

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

    foreach ($name in @('BUILD_TESTING', 'NO_SOUND', 'FMOD_INCLUDE_DIR', 'FMOD_LIBRARY', 'OPENAL_INCLUDE_DIR', 'OPENAL_LIBRARY', 'OPUS_INCLUDE_DIR', 'OPUS_LIBRARIES', 'ZSTD_INCLUDE_DIR', 'ZSTD_LIBRARY', 'FLUIDSYNTH_INCLUDE_DIR', 'FLUIDSYNTH_LIBRARIES')) {
        $setting = Get-CMakeCacheSetting -CachePath $cacheFile -Name $name

        if ($setting -and ($setting.Value -notmatch 'NOTFOUND')) {
            $configureArguments += "-D$($name):$($setting.Type)=$($setting.Value)"
        }
    }

    $configureOutput = @(& $cmake.Source @configureArguments 2>&1 | ForEach-Object { $_.ToString() })

    if ($LASTEXITCODE -ne 0) {
        $details = ($configureOutput -join [Environment]::NewLine).Replace($baselineRoot, '<baseline>').Replace($repositoryRoot, '<repo>')
        throw "Failed to configure the isolated baseline worktree: $details"
    }

    Invoke-GeneratedBuildInputPreparation -CMakePath $cmake.Source -BuildRoot $buildRoot -RevisionName 'HEAD'
    Invoke-GeneratedBuildInputPreparation -CMakePath $cmake.Source -BuildRoot $baselineBuildRoot -RevisionName 'baseline'

    $baselineProjects = Get-ProjectSources -BuildRoot $baselineBuildRoot -RepositoryRoot $baselineRoot
    $baselineDiagnostics = @()
    $headDiagnostics = @()

    Write-Host "Running isolated Cppcheck analysis for $($analysisProjects.Count) target(s):"

    foreach ($project in $analysisProjects) {
        $targetName = $project.RelativeProject
        Write-Host "  $targetName"

        $headCache = Join-Path $tempRoot (Join-Path 'head-cache' $targetName.Replace('/', '_'))
        $headDiagnostics += Invoke-CppcheckProject `
            -CppcheckPath $cppcheck.Source `
            -ProjectPath $project.ProjectPath `
            -CachePath $headCache `
            -RepositoryRoot $repositoryRoot `
            -BuildRoot $buildRoot `
            -TargetName $targetName `
            -CppcheckJobs $CppcheckJobs

        $baselineProject = $baselineProjects | Where-Object { $_.RelativeProject -eq $project.RelativeProject } | Select-Object -First 1

        if ($baselineProject) {
            $baselineCache = Join-Path $tempRoot (Join-Path 'baseline-cache' $targetName.Replace('/', '_'))
            $baselineDiagnostics += Invoke-CppcheckProject `
                -CppcheckPath $cppcheck.Source `
                -ProjectPath $baselineProject.ProjectPath `
                -CachePath $baselineCache `
                -RepositoryRoot $baselineRoot `
                -BuildRoot $baselineBuildRoot `
                -TargetName $targetName `
                -CppcheckJobs $CppcheckJobs
        }
    }

    $baselineByFingerprint = @{}

    foreach ($diagnostic in $baselineDiagnostics) {
        $baselineByFingerprint[$diagnostic.Fingerprint] = $diagnostic
    }

    $headByFingerprint = @{}

    foreach ($diagnostic in $headDiagnostics) {
        $headByFingerprint[$diagnostic.Fingerprint] = $diagnostic
    }

    $unchangedDiagnostics = @($headByFingerprint.Keys | Where-Object { $baselineByFingerprint.ContainsKey($_) } | Sort-Object | ForEach-Object { $headByFingerprint[$_] })
    $newDiagnostics = @($headByFingerprint.Keys | Where-Object { -not $baselineByFingerprint.ContainsKey($_) } | Sort-Object | ForEach-Object { $headByFingerprint[$_] })
    $baselineOnlyDiagnostics = @($baselineByFingerprint.Keys | Where-Object { -not $headByFingerprint.ContainsKey($_) } | Sort-Object | ForEach-Object { $baselineByFingerprint[$_] })

    Write-Host "Baseline diagnostics ($($baselineDiagnostics.Count)):"
    $baselineByFingerprint.Values | Sort-Object Display | ForEach-Object { Write-Host "  $($_.Display)" }
    Write-Host "Unchanged baseline diagnostics ($($unchangedDiagnostics.Count)):"
    $unchangedDiagnostics | ForEach-Object { Write-Host "  $($_.Display)" }
    Write-Host "Baseline-only diagnostics ($($baselineOnlyDiagnostics.Count)):"
    $baselineOnlyDiagnostics | ForEach-Object { Write-Host "  $($_.Display)" }
    Write-Host "New diagnostics ($($newDiagnostics.Count)):"
    $newDiagnostics | ForEach-Object { Write-Host "  $($_.Display)" }

    if ($newDiagnostics.Count -ne 0) {
        Write-Error "Cppcheck reported $($newDiagnostics.Count) new diagnostic fingerprint(s)."
        exit 1
    }

    Write-Host "Cppcheck passed: no new diagnostic fingerprints."
}
finally {
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
