Set-StrictMode -Version Latest

function Get-CppcheckCacheHash {
    param([string]$Value)

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($hash.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $hash.Dispose()
    }
}

function Get-CppcheckCacheIdentity {
    param(
        [string]$AnalyzerVersion,
        [string]$AnalyzerSHA256,
        [string]$InputMode,
        [string]$Configuration = 'Release|x64',
        [string]$Compiler = 'MSVC',
        [string]$Toolset = 'v143',
        [string]$Abi = 'x64',
        [string]$RootIdentity,
        [object]$AnalysisContext = $null
    )

    $context = [ordered]@{
        SchemaVersion = 2
        AnalyzerVersion = $AnalyzerVersion
        AnalyzerSHA256 = $AnalyzerSHA256
        InputMode = $InputMode
        Configuration = $Configuration
        Compiler = $Compiler
        Toolset = $Toolset
        Abi = $Abi
        RootIdentity = $RootIdentity
        AnalysisContext = $AnalysisContext
    }
    $json = $context | ConvertTo-Json -Compress -Depth 8
    return [PSCustomObject]@{
        Key = Get-CppcheckCacheHash -Value $json
        Context = [PSCustomObject]$context
    }
}

function Get-CppcheckAnalysisContext {
    param(
        [string]$BuildRoot,
        [string[]]$ProjectPaths,
        [string[]]$InputRootIdentities,
        [string[]]$AnalyzerConfigurationPaths = @(),
        [string[]]$AnalyzerOptions = @()
    )

    $fullBuildRoot = [System.IO.Path]::GetFullPath($BuildRoot).TrimEnd('\', '/')
    $cachePath = Join-Path $fullBuildRoot 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "Cppcheck analysis context requires '$cachePath'."
    }

    $normalizedCache = @(
        Get-Content -LiteralPath $cachePath | Where-Object {
            ($_ -notmatch '^(CMAKE_CACHEFILE_DIR|CMAKE_HOME_DIRECTORY|CMAKE_FILES_DIRECTORY|CMAKE_SUPPRESS_REGENERATION):') -and
            ($_ -notmatch '^//') -and ($_ -notmatch '^#')
        } | ForEach-Object { $_.Replace($fullBuildRoot, '<build-root>') } | Sort-Object
    ) -join "`n"
    $projectHashes = @(
        $ProjectPaths | Sort-Object -Unique | ForEach-Object {
            $projectPath = [System.IO.Path]::GetFullPath($_)
            $normalizedProject = (Get-Content -LiteralPath $projectPath -Raw).
                Replace($fullBuildRoot, '<build-root>') -replace '<ProjectGuid>[^<]+</ProjectGuid>', '<ProjectGuid><normalized></ProjectGuid>' -replace 'Project="\{[^}]+\}"', 'Project="{normalized}"'
            [PSCustomObject]@{
                Project = $projectPath.Substring($fullBuildRoot.Length).Replace('\', '/')
                SHA256 = Get-CppcheckCacheHash -Value $normalizedProject
            }
        }
    )
    $configurationHashes = @(
        $AnalyzerConfigurationPaths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique | ForEach-Object {
            $configurationPath = [System.IO.Path]::GetFullPath($_)
            if (-not (Test-Path -LiteralPath $configurationPath -PathType Leaf)) {
                throw "Cppcheck analyzer configuration '$configurationPath' does not exist."
            }
            [PSCustomObject]@{
                Path = $configurationPath
                SHA256 = (Get-FileHash -LiteralPath $configurationPath -Algorithm SHA256).Hash
            }
        }
    )

    return [PSCustomObject]@{
        BuildCacheSHA256 = Get-CppcheckCacheHash -Value $normalizedCache
        Projects = $projectHashes
        InputRoots = @($InputRootIdentities | Sort-Object -Unique)
        AnalyzerConfigurations = $configurationHashes
        AnalyzerOptions = @($AnalyzerOptions | Sort-Object -Unique)
    }
}

function Get-CppcheckInstalledConfigurationPaths {
    param([string]$AnalyzerPath)

    $analyzerDirectory = Split-Path -Parent ([System.IO.Path]::GetFullPath($AnalyzerPath))
    $standardLibrary = Join-Path $analyzerDirectory 'cfg\std.cfg'
    if (-not (Test-Path -LiteralPath $standardLibrary -PathType Leaf)) {
        throw "Cppcheck automatic standard library configuration '$standardLibrary' does not exist."
    }

    return @($standardLibrary)
}

function Get-CppcheckCacheLeaf {
    param(
        [string]$CacheRoot,
        [string]$Namespace,
        [object]$Identity,
        [string]$Role,
        [string]$TargetName = '',
        [string]$TranslationUnit = ''
    )

    $parts = @($CacheRoot, $Namespace, $Identity.Key, $Role)
    if (-not [string]::IsNullOrWhiteSpace($TargetName)) {
        $parts += ('t-' + (Get-CppcheckCacheHash -Value $TargetName).Substring(0, 20))
    }
    if (-not [string]::IsNullOrWhiteSpace($TranslationUnit)) {
        $parts += ('u-' + (Get-CppcheckCacheHash -Value $TranslationUnit).Substring(0, 20))
    }
    $leaf = [System.IO.Path]::Combine([string[]]$parts)
    Assert-CppcheckPathHasNoReparseAncestor -Path $leaf
    return $leaf
}

function Save-CppcheckCacheIdentity {
    param(
        [string]$CacheRoot,
        [string]$Namespace,
        [object]$Identity
    )

    $identityRoot = Join-Path $CacheRoot (Join-Path $Namespace $Identity.Key)
    Assert-CppcheckPathHasNoReparseAncestor -Path $identityRoot
    New-Item -ItemType Directory -Force -Path $identityRoot | Out-Null
    $Identity.Context | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $identityRoot 'identity.json') -NoNewline
}

function Enter-CppcheckCacheLock {
    param(
        [string]$CacheRoot,
        [string]$Name
    )

    $lockDirectory = Join-Path $CacheRoot 'locks'
    Assert-CppcheckPathHasNoReparseAncestor -Path (Join-Path $lockDirectory ($Name + '.lock'))
    New-Item -ItemType Directory -Force -Path $lockDirectory | Out-Null
    $lockPath = Join-Path $lockDirectory ($Name + '.lock')
    try {
        $stream = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    }
    catch [System.IO.IOException] {
        throw "Cppcheck cache operation '$Name' is already running; lock '$lockPath' is held."
    }
    return [PSCustomObject]@{ Path = $lockPath; Stream = $stream }
}

function Assert-CppcheckPathHasNoReparseAncestor {
    param([string]$Path)

    $candidate = [System.IO.Path]::GetFullPath($Path)
    while ($true) {
        if (Test-Path -LiteralPath $candidate) {
            $item = Get-Item -LiteralPath $candidate -Force
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Cppcheck path '$Path' has reparse-point ancestor '$candidate'."
            }
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or ($parent -eq $candidate)) {
            return
        }
        $candidate = $parent
    }
}

function Get-CppcheckOwnedStagePath {
    param(
        [string]$CacheRoot,
        [string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Name) -or ($Name.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) -or ($Name -match '[\\/]')) {
        throw "Cppcheck staging name '$Name' is invalid."
    }
    $fullCacheRoot = [System.IO.Path]::GetFullPath($CacheRoot).TrimEnd('\', '/')
    Assert-CppcheckPathHasNoReparseAncestor -Path $fullCacheRoot
    $stageRoot = Join-Path (Join-Path $fullCacheRoot 'staging') $Name
    Assert-CppcheckPathHasNoReparseAncestor -Path $stageRoot
    return $stageRoot
}

function Exit-CppcheckCacheLock {
    param([object]$Lock)

    if ($null -ne $Lock -and $null -ne $Lock.Stream) {
        $Lock.Stream.Dispose()
    }
}

function New-CppcheckDisposableStage {
    param(
        [string]$CacheRoot,
        [string]$Name
    )

    $stageRoot = Get-CppcheckOwnedStagePath -CacheRoot $CacheRoot -Name $Name
    Assert-CppcheckPathHasNoReparseAncestor -Path $stageRoot
    if (Test-Path -LiteralPath $stageRoot) {
        throw "Cppcheck staging root '$stageRoot' already exists; refusing to delete unexplained residual staging."
    }
    New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
    return $stageRoot
}

function Remove-CppcheckDisposableStage {
    param(
        [string]$CacheRoot,
        [string]$StageRoot,
        [string]$Name
    )

    $expectedStageRoot = Get-CppcheckOwnedStagePath -CacheRoot $CacheRoot -Name $Name
    $fullStageRoot = [System.IO.Path]::GetFullPath($StageRoot).TrimEnd('\', '/')
    if (-not [string]::Equals($fullStageRoot, $expectedStageRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Cppcheck staging root '$StageRoot' is not the owned '$Name' staging root."
    }
    Assert-CppcheckPathHasNoReparseAncestor -Path $fullStageRoot
    $item = Get-Item -LiteralPath $fullStageRoot -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Cppcheck staging root '$StageRoot' is a reparse point."
    }
    Remove-Item -LiteralPath $fullStageRoot -Recurse -Force
}