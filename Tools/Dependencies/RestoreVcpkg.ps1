[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,

    [Parameter(Mandatory = $true)]
    [string]$InstallRoot,

    [Parameter(Mandatory = $true)]
    [string]$GitExecutable,

    [string]$InstalledVersionRoot,

    [string]$DependencyRootId,

    [string]$DependencyDefinitionId,

    [string]$DependencyBuildIdentityJson
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$thirdPartyRoot = Join-Path $repositoryRoot "ThirdParty"
$configurationPath = Join-Path $thirdPartyRoot "vcpkg-tool.json"
$configuration = Get-Content -Raw -LiteralPath $configurationPath | ConvertFrom-Json

function Test-PathInside
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Candidate,

        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $relative = [IO.Path]::GetRelativePath($Root, $Candidate)
    return -not [IO.Path]::IsPathFullyQualified($relative) -and
        $relative -ne ".." -and
        -not $relative.StartsWith("..$([IO.Path]::DirectorySeparatorChar)", [StringComparison]::Ordinal)
}

function Test-SamePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Left,

        [Parameter(Mandatory = $true)]
        [string]$Right
    )

    return $Left.Equals($Right, [StringComparison]::OrdinalIgnoreCase)
}

function Get-GitForWindowsVersion
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $repositoryRoot
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    [void]$startInfo.ArgumentList.Add("--version")

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try
    {
        if (-not $process.Start())
        {
            throw "Git for Windows prerequisite could not be started."
        }
        $standardOutput = $process.StandardOutput.ReadToEndAsync()
        $standardError = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000))
        {
            $process.Kill($true)
            $process.WaitForExit()
            throw "Git for Windows prerequisite diagnosis timed out."
        }
        $outputText = $standardOutput.GetAwaiter().GetResult().Trim()
        [void]$standardError.GetAwaiter().GetResult()
        $versionMatch = [Text.RegularExpressions.Regex]::Match(
            $outputText,
            '^git version (\d+)\.(\d+)\.(\d+)\.windows\.\d+$',
            [Text.RegularExpressions.RegexOptions]::CultureInvariant)
        if ($process.ExitCode -ne 0 -or -not $versionMatch.Success)
        {
            throw "Git for Windows prerequisite identity could not be diagnosed."
        }
        return [Version]::new(
            [int]$versionMatch.Groups[1].Value,
            [int]$versionMatch.Groups[2].Value,
            [int]$versionMatch.Groups[3].Value)
    }
    finally
    {
        $process.Dispose()
    }
}

if (-not [IO.Path]::IsPathFullyQualified($ToolRoot) -or -not [IO.Path]::IsPathFullyQualified($InstallRoot))
{
    throw "ToolRoot and InstallRoot must be absolute paths."
}
if (-not [IO.Path]::IsPathFullyQualified($GitExecutable) -or
    -not [IO.File]::Exists([IO.Path]::GetFullPath($GitExecutable)))
{
    throw "GitExecutable must be an absolute path to an existing file."
}
$gitExecutable = [IO.Path]::GetFullPath($GitExecutable)
$toolRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($ToolRoot))
$installedRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($InstallRoot))
$gitVersion = Get-GitForWindowsVersion -Executable $gitExecutable
if ($gitVersion -lt [Version]::new(2, 44, 0))
{
    throw "Git for Windows 2.44.0 or newer is required before dependency restore."
}

if ((Test-SamePath -Left $toolRoot -Right $installedRoot) -or
    (Test-PathInside -Candidate $toolRoot -Root $installedRoot) -or
    (Test-PathInside -Candidate $installedRoot -Root $toolRoot))
{
    throw "ToolRoot and InstallRoot must be separate directory trees."
}

$versionRoot = $null
if (-not [string]::IsNullOrWhiteSpace($InstalledVersionRoot))
{
    if (-not [IO.Path]::IsPathFullyQualified($InstalledVersionRoot))
    {
        throw "InstalledVersionRoot must be an absolute path."
    }
    $versionRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($InstalledVersionRoot))
    if ((Test-PathInside -Candidate $toolRoot -Root $versionRoot) -or
        (Test-PathInside -Candidate $installedRoot -Root $versionRoot))
    {
        throw "vcpkg ToolRoot and InstallRoot must remain outside the immutable installed version."
    }
}

$rootModeValues = @($DependencyRootId, $DependencyDefinitionId, $DependencyBuildIdentityJson)
$rootMode = @($rootModeValues | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -ne 0
if ($rootMode -and
    @($rootModeValues | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -ne 0)
{
    throw "DependencyRootId, DependencyDefinitionId, and DependencyBuildIdentityJson must be provided together."
}
if ($rootMode -and $null -eq $versionRoot)
{
    throw "Immutable Dependency Root publication requires InstalledVersionRoot."
}

$buildIdentity = $null
$expectedMarker = $null
$finalDependencyRoot = $null
$stagingDependencyRoot = $null
$dependencyLease = $null
$expectedTriplet = $null
$overlayTripletRoot = $null

if ($rootMode)
{
    if ($DependencyRootId -cnotmatch '^[0-9a-f]{64}$' -or $DependencyDefinitionId -cnotmatch '^[0-9a-f]{64}$')
    {
        throw "Dependency Root and Definition IDs must be 64 lowercase hexadecimal characters."
    }

    $buildIdentity = $DependencyBuildIdentityJson | ConvertFrom-Json
    $expectedBuildMembers = @(
        "targetTriplet",
        "hostArchitecture",
        "targetArchitecture",
        "compilerVendor",
        "compilerVersion",
        "toolsetVersion",
        "crtLinkage",
        "crtVersion",
        "windowsSdkTargetVersion"
    )
    $actualBuildMembers = @($buildIdentity.psobject.Properties.Name)
    if (($actualBuildMembers -join "|") -cne ($expectedBuildMembers -join "|") -or
        ($buildIdentity | ConvertTo-Json -Compress) -cne $DependencyBuildIdentityJson)
    {
        throw "DependencyBuildIdentityJson must be canonical and contain the fixed v1 members."
    }
    if ($buildIdentity.targetTriplet -cne "x64-windows" -or
        $buildIdentity.hostArchitecture -cne "x64" -or
        $buildIdentity.targetArchitecture -cne "x64" -or
        $buildIdentity.compilerVendor -cne "msvc" -or
        $buildIdentity.crtLinkage -cne "dynamic" -or
        $buildIdentity.toolsetVersion -cnotmatch '^\d+\.\d+(\.\d+)?$' -or
        $buildIdentity.windowsSdkTargetVersion -cnotmatch '^\d+\.\d+\.\d+\.\d+$')
    {
        throw "DependencyBuildIdentityJson must describe the supported x64-windows MSVC ABI."
    }

    $expectedTriplet = @(
        "set(VCPKG_TARGET_ARCHITECTURE x64)",
        "set(VCPKG_CRT_LINKAGE dynamic)",
        "set(VCPKG_LIBRARY_LINKAGE dynamic)",
        "set(VCPKG_PLATFORM_TOOLSET_VERSION `"$($buildIdentity.toolsetVersion)`")",
        "set(VCPKG_CMAKE_SYSTEM_VERSION `"$($buildIdentity.windowsSdkTargetVersion)`")"
    ) -join "`n"
    $expectedTriplet += "`n"

    $finalDependencyRoot = Split-Path -Parent $installedRoot
    if ((Split-Path -Leaf $finalDependencyRoot) -cne $DependencyRootId -or
        -not (Test-SamePath -Left $toolRoot -Right (Join-Path $finalDependencyRoot "Tool\vcpkg")) -or
        -not (Test-SamePath -Left $installedRoot -Right (Join-Path $finalDependencyRoot "Installed")))
    {
        throw "Immutable Dependency Root paths must use <root-id>/Tool/vcpkg and <root-id>/Installed."
    }
    if (Test-PathInside -Candidate $finalDependencyRoot -Root $versionRoot)
    {
        throw "Dependency Root must remain outside the immutable installed version."
    }

    $marker = [ordered]@{
        schemaVersion = 1
        definitionId = $DependencyDefinitionId
        buildIdentity = $buildIdentity
        rootId = $DependencyRootId
        pin = [ordered]@{
            repository = [string]$configuration.repository
            commit = [string]$configuration.commit
            release = [string]$configuration.release
            windowsX64Version = [string]$configuration.windowsX64Version
            windowsX64Sha256 = [string]$configuration.windowsX64Sha256
            sourceSha512 = [string]$configuration.sourceSha512
        }
    }
    $expectedMarker = ($marker | ConvertTo-Json -Compress -Depth 4) + "`n"

    $dependencyParent = Split-Path -Parent $finalDependencyRoot
    $lockRoot = Join-Path $dependencyParent ".locks"
    New-Item -ItemType Directory -Path $lockRoot -Force | Out-Null
    $lockPath = Join-Path $lockRoot "$DependencyRootId.lock"
    try
    {
        $dependencyLease = [IO.File]::Open(
            $lockPath,
            [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None)
    }
    catch
    {
        throw "Dependency Root lease is already held: $DependencyRootId"
    }
}

function Invoke-CheckedProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    foreach ($argument in $ArgumentList)
    {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try
    {
        if (-not $process.Start())
        {
            throw "Process could not be started: $FilePath"
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0)
        {
            throw "Process failed with exit code $($process.ExitCode): $FilePath $($ArgumentList -join ' ')"
        }
    }
    finally
    {
        $process.Dispose()
    }
}

function Invoke-CapturedProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $ArgumentList)
    {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try
    {
        if (-not $process.Start())
        {
            throw "Process could not be started: $FilePath"
        }
        $standardOutput = $process.StandardOutput.ReadToEndAsync()
        $standardError = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $outputText = $standardOutput.GetAwaiter().GetResult()
        $errorText = $standardError.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0)
        {
            throw "Process failed with exit code $($process.ExitCode): $FilePath $($ArgumentList -join ' '); $errorText"
        }
        return $outputText
    }
    finally
    {
        $process.Dispose()
    }
}

function Test-VcpkgExecutable
{
    $executablePath = Join-Path $toolRoot "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf))
    {
        return $false
    }

    $actualHash = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -cne $configuration.windowsX64Sha256)
    {
        return $false
    }

    try
    {
        $versionText = Invoke-CapturedProcess -FilePath $executablePath -ArgumentList @("version") `
            -WorkingDirectory $toolRoot
        return $versionText.Contains($configuration.windowsX64Version)
    }
    catch
    {
        return $false
    }
}

function Test-VcpkgCheckout
{
    if (-not (Test-Path -LiteralPath $toolRoot -PathType Container))
    {
        return $false
    }
    try
    {
        $commonArguments = @(
            "-c", "core.longpaths=true", "-c", "safe.directory=$toolRoot", "-C", $toolRoot
        )
        $trackedChanges = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList (
            $commonArguments + @("status", "--porcelain", "--untracked-files=no")
        ) -WorkingDirectory $repositoryRoot).Trim()
        $actualRepository = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList (
            $commonArguments + @("remote", "get-url", "origin")
        ) -WorkingDirectory $repositoryRoot).Trim()
        $actualCommit = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList (
            $commonArguments + @("rev-parse", "HEAD")
        ) -WorkingDirectory $repositoryRoot).Trim()
        $longPaths = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList (
            $commonArguments + @("config", "--local", "--get", "core.longpaths")
        ) -WorkingDirectory $repositoryRoot).Trim()
        $metadataPath = Join-Path $toolRoot "scripts\vcpkg-tool-metadata.txt"
        $metadata = Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-StringData
        return $trackedChanges.Length -eq 0 -and
            $actualRepository -ceq $configuration.repository -and
            $actualCommit -ceq $configuration.commit -and
            $longPaths -ceq "true" -and
            $metadata.VCPKG_TOOL_RELEASE_TAG -ceq $configuration.release -and
            $metadata.VCPKG_TOOL_SOURCE_SHA -ceq $configuration.sourceSha512
    }
    catch
    {
        return $false
    }
}

function Test-CompletedDependencyRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    if (-not $rootMode -or -not (Test-Path -LiteralPath $Root -PathType Container))
    {
        return $false
    }
    $markerPath = Join-Path $Root "CueDependencyRoot.complete.json"
    $tripletPath = Join-Path $Root "Triplets\x64-windows.cmake"
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf))
    {
        return $false
    }
    $actualMarker = [IO.File]::ReadAllText($markerPath)
    if (-not (Test-Path -LiteralPath $tripletPath -PathType Leaf) -or
        [IO.File]::ReadAllText($tripletPath) -cne $expectedTriplet -or
        $actualMarker -cne $expectedMarker -or -not (Test-VcpkgCheckout) -or -not (Test-VcpkgExecutable))
    {
        return $false
    }
    return Test-Path -LiteralPath (Join-Path $installedRoot "vcpkg\status") -PathType Leaf
}

function Move-InvalidDependencyRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $quarantineRoot = Join-Path (Split-Path -Parent $Root) `
        ".invalid-$DependencyRootId-$([Guid]::NewGuid().ToString('N'))"
    try
    {
        Move-Item -LiteralPath $Root -Destination $quarantineRoot -ErrorAction Stop
    }
    catch
    {
        throw "Invalid Dependency Root could not be quarantined: $($_.Exception.Message)"
    }
    return $quarantineRoot
}

function Invoke-VcpkgRestore
{
    $env:VCPKG_ROOT = $toolRoot
    $env:GIT_CONFIG_COUNT = "1"
    $env:GIT_CONFIG_KEY_0 = "core.longpaths"
    $env:GIT_CONFIG_VALUE_0 = "true"
    if (-not (Test-Path -LiteralPath $toolRoot -PathType Container))
    {
        $toolParent = Split-Path -Parent $toolRoot
        New-Item -ItemType Directory -Path $toolParent -Force | Out-Null
        Invoke-CheckedProcess -FilePath $gitExecutable -ArgumentList @(
            "-c",
            "core.longpaths=true",
            "clone",
            "--filter=blob:none",
            "--no-checkout",
            $configuration.repository,
            $toolRoot
        ) -WorkingDirectory $toolParent
        Invoke-CheckedProcess -FilePath $gitExecutable -ArgumentList @(
            "-c",
            "core.longpaths=true",
            "checkout",
            "--detach",
            $configuration.commit
        ) -WorkingDirectory $toolRoot
    }

    Invoke-CheckedProcess -FilePath $gitExecutable -ArgumentList @(
        "-c", "safe.directory=$toolRoot", "-C", $toolRoot, "config", "core.longpaths", "true"
    ) -WorkingDirectory $repositoryRoot

    $trackedChanges = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList @(
        "-c", "core.longpaths=true", "-c", "safe.directory=$toolRoot", "-C", $toolRoot,
        "status", "--porcelain", "--untracked-files=no"
    ) -WorkingDirectory $repositoryRoot).Trim()
    if ($trackedChanges.Length -ne 0)
    {
        throw "Managed vcpkg checkout contains tracked changes."
    }

    $actualRepository = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList @(
        "-c", "core.longpaths=true", "-c", "safe.directory=$toolRoot", "-C", $toolRoot,
        "remote", "get-url", "origin"
    ) -WorkingDirectory $repositoryRoot).Trim()
    if ($actualRepository -cne $configuration.repository)
    {
        throw "Managed vcpkg checkout origin does not match the pinned repository."
    }

    $actualCommit = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList @(
        "-c", "core.longpaths=true", "-c", "safe.directory=$toolRoot", "-C", $toolRoot,
        "rev-parse", "HEAD"
    ) -WorkingDirectory $repositoryRoot).Trim()
    if ($actualCommit -cne $configuration.commit)
    {
        Invoke-CheckedProcess -FilePath $gitExecutable -ArgumentList @(
            "-c",
            "core.longpaths=true",
            "-c",
            "safe.directory=$toolRoot",
            "-C",
            $toolRoot,
            "fetch",
            "--filter=blob:none",
            "origin",
            $configuration.commit
        ) -WorkingDirectory $repositoryRoot
        Invoke-CheckedProcess -FilePath $gitExecutable -ArgumentList @(
            "-c",
            "core.longpaths=true",
            "-c",
            "safe.directory=$toolRoot",
            "-C",
            $toolRoot,
            "checkout",
            "--detach",
            $configuration.commit
        ) -WorkingDirectory $repositoryRoot
    }

    $actualCommit = (Invoke-CapturedProcess -FilePath $gitExecutable -ArgumentList @(
        "-c", "core.longpaths=true", "-c", "safe.directory=$toolRoot", "-C", $toolRoot,
        "rev-parse", "HEAD"
    ) -WorkingDirectory $repositoryRoot).Trim()
    if ($actualCommit -cne $configuration.commit)
    {
        throw "Managed vcpkg checkout does not match the pinned commit."
    }

    $metadataPath = Join-Path $toolRoot "scripts\vcpkg-tool-metadata.txt"
    $metadata = Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-StringData
    if ($metadata.VCPKG_TOOL_RELEASE_TAG -cne $configuration.release)
    {
        throw "Managed vcpkg tool release does not match the pinned release."
    }
    if ($metadata.VCPKG_TOOL_SOURCE_SHA -cne $configuration.sourceSha512)
    {
        throw "Managed vcpkg tool source hash does not match the pinned hash."
    }

    if (-not (Test-VcpkgExecutable))
    {
        $bootstrapPath = Join-Path $toolRoot "bootstrap-vcpkg.bat"
        Invoke-CheckedProcess -FilePath $bootstrapPath -ArgumentList @("-disableMetrics") -WorkingDirectory $toolRoot
    }
    if (-not (Test-VcpkgExecutable))
    {
        throw "Bootstrapped vcpkg executable does not match the pinned version and hash."
    }

    $vcpkgPath = Join-Path $toolRoot "vcpkg.exe"
    $installArguments = @(
        "install",
        "--x-manifest-root=$thirdPartyRoot",
        "--x-install-root=$installedRoot",
        "--triplet=x64-windows"
    )
    if ($rootMode)
    {
        $overlayTripletRoot = Join-Path $stagingDependencyRoot "Triplets"
        New-Item -ItemType Directory -Path $overlayTripletRoot -Force | Out-Null
        $tripletPath = Join-Path $overlayTripletRoot "x64-windows.cmake"
        [IO.File]::WriteAllText($tripletPath, $expectedTriplet, [Text.UTF8Encoding]::new($false))
        $installArguments += "--overlay-triplets=$overlayTripletRoot"
    }
    $env:VCPKG_DISABLE_METRICS = "1"
    Invoke-CheckedProcess -FilePath $vcpkgPath -ArgumentList $installArguments -WorkingDirectory $repositoryRoot
}

try
{
    if ($rootMode)
    {
        if (Test-Path -LiteralPath $finalDependencyRoot)
        {
            if ((Test-Path -LiteralPath $finalDependencyRoot -PathType Container) -and
                (Test-CompletedDependencyRoot -Root $finalDependencyRoot))
            {
                return
            }
            [void](Move-InvalidDependencyRoot -Root $finalDependencyRoot)
        }

        $stagingDependencyRoot = Join-Path (Split-Path -Parent $finalDependencyRoot) `
            ".staging-$([Guid]::NewGuid().ToString('N'))"
        New-Item -ItemType Directory -Path $stagingDependencyRoot | Out-Null
        $toolRoot = Join-Path $stagingDependencyRoot "Tool\vcpkg"
        $installedRoot = Join-Path $stagingDependencyRoot "Installed"
    }

    Invoke-VcpkgRestore

    if ($rootMode)
    {
        $markerPath = Join-Path $stagingDependencyRoot "CueDependencyRoot.complete.json"
        $markerBytes = [Text.UTF8Encoding]::new($false).GetBytes($expectedMarker)
        $markerStream = [IO.File]::Open(
            $markerPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try
        {
            $markerStream.Write($markerBytes, 0, $markerBytes.Length)
            $markerStream.Flush($true)
        }
        finally
        {
            $markerStream.Dispose()
        }

        if (-not (Test-CompletedDependencyRoot -Root $stagingDependencyRoot))
        {
            throw "Staged Dependency Root failed pre-publish validation."
        }

        Move-Item -LiteralPath $stagingDependencyRoot -Destination $finalDependencyRoot
        $stagingDependencyRoot = $null
        $toolRoot = Join-Path $finalDependencyRoot "Tool\vcpkg"
        $installedRoot = Join-Path $finalDependencyRoot "Installed"
        if (-not (Test-CompletedDependencyRoot -Root $finalDependencyRoot))
        {
            $quarantineRoot = Move-InvalidDependencyRoot -Root $finalDependencyRoot
            throw "Published Dependency Root failed post-publish validation and was quarantined at '$quarantineRoot'."
        }
    }
}
finally
{
    if ($null -ne $stagingDependencyRoot -and (Test-Path -LiteralPath $stagingDependencyRoot -PathType Container))
    {
        $cleanupFailure = $null
        for ($attempt = 0; $attempt -lt 8; ++$attempt)
        {
            try
            {
                Remove-Item -LiteralPath $stagingDependencyRoot -Recurse -Force -ErrorAction Stop
                $cleanupFailure = $null
                break
            }
            catch
            {
                $cleanupFailure = $_
                Start-Sleep -Milliseconds 125
            }
        }
        if ($null -ne $cleanupFailure)
        {
            Write-Warning "Dependency staging cleanup failed: $($cleanupFailure.Exception.Message)"
        }
    }
    if ($null -ne $dependencyLease)
    {
        $dependencyLease.Dispose()
    }
}
