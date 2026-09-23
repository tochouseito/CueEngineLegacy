[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$GitFailureProbe
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$restoreScript = Join-Path $RepositoryRoot "Tools\Dependencies\RestoreVcpkg.ps1"
$powerShell = Join-Path $PSHOME "pwsh.exe"
$gitExecutable = (Get-Command git).Source

function Assert-RestoreRejected
{
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedMessage
    )

    $output = (& $powerShell -NoProfile -File $restoreScript @Arguments -GitExecutable $gitExecutable 2>&1 |
        Out-String)
    if ($LASTEXITCODE -eq 0)
    {
        throw "Restore unexpectedly succeeded: $($Arguments -join ' ')"
    }
    if (-not $output.Contains($ExpectedMessage, [StringComparison]::Ordinal))
    {
        throw "Restore rejection did not contain '$ExpectedMessage'. Output: $output"
    }
}

$missingGitRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "CueEngine-RestoreContract-MissingGit-$([Guid]::NewGuid().ToString('N'))"
$missingGitTool = Join-Path $missingGitRoot "Tool"
$missingGitInstall = Join-Path $missingGitRoot "Installed"
$missingGitOutput = (& $powerShell -NoProfile -File $restoreScript `
    -ToolRoot $missingGitTool `
    -InstallRoot $missingGitInstall `
    -GitExecutable (Join-Path $missingGitRoot "missing-git.exe") 2>&1 | Out-String)
if ($LASTEXITCODE -eq 0 -or
    -not $missingGitOutput.Contains("GitExecutable must be an absolute path to an existing file.",
        [StringComparison]::Ordinal))
{
    throw "Missing Git prerequisite was not diagnosed before restore: $missingGitOutput"
}
if ((Test-Path -LiteralPath $missingGitTool) -or (Test-Path -LiteralPath $missingGitInstall))
{
    throw "Missing Git prerequisite mutated dependency roots."
}

$invalidGitRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "CueEngine-RestoreContract-InvalidGit-$([Guid]::NewGuid().ToString('N'))"
$invalidGitTool = Join-Path $invalidGitRoot "Tool"
$invalidGitInstall = Join-Path $invalidGitRoot "Installed"
$invalidGitExecutable = Join-Path ([Environment]::GetFolderPath('System')) "where.exe"
$invalidGitOutput = (& $powerShell -NoProfile -File $restoreScript `
    -ToolRoot $invalidGitTool `
    -InstallRoot $invalidGitInstall `
    -GitExecutable $invalidGitExecutable 2>&1 | Out-String)
if ($LASTEXITCODE -eq 0 -or
    -not $invalidGitOutput.Contains("Git for Windows prerequisite identity could not be diagnosed.",
        [StringComparison]::Ordinal))
{
    throw "Invalid Git prerequisite was not diagnosed before restore: $invalidGitOutput"
}
if ((Test-Path -LiteralPath $invalidGitTool) -or (Test-Path -LiteralPath $invalidGitInstall))
{
    throw "Invalid Git prerequisite mutated dependency roots."
}

$outdatedGitRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "CueEngine-RestoreContract-OutdatedGit-$([Guid]::NewGuid().ToString('N'))"
$outdatedGitTool = Join-Path $outdatedGitRoot "Tool"
$outdatedGitInstall = Join-Path $outdatedGitRoot "Installed"
$env:CUE_TEST_OUTDATED_GIT = "1"
try
{
    $outdatedGitOutput = (& $powerShell -NoProfile -File $restoreScript `
        -ToolRoot $outdatedGitTool `
        -InstallRoot $outdatedGitInstall `
        -GitExecutable $GitFailureProbe 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0 -or
        -not $outdatedGitOutput.Contains("Git for Windows 2.44.0 or newer is required before dependency restore.",
            [StringComparison]::Ordinal))
    {
        throw "Outdated Git prerequisite was not rejected before restore: $outdatedGitOutput"
    }
    if ((Test-Path -LiteralPath $outdatedGitTool) -or (Test-Path -LiteralPath $outdatedGitInstall))
    {
        throw "Outdated Git prerequisite mutated dependency roots."
    }
}
finally
{
    Remove-Item Env:CUE_TEST_OUTDATED_GIT -ErrorAction SilentlyContinue
}

Assert-RestoreRejected -Arguments @(
    "-ToolRoot", "relative-tool",
    "-InstallRoot", "relative-install"
) -ExpectedMessage "must be absolute paths"

$versionRoot = Join-Path ([IO.Path]::GetTempPath()) "CueEngine-RestoreContract-Version"
Assert-RestoreRejected -Arguments @(
    "-ToolRoot", (Join-Path $versionRoot "Dependencies\Tool"),
    "-InstallRoot", (Join-Path $versionRoot "Dependencies\Installed"),
    "-InstalledVersionRoot", $versionRoot
) -ExpectedMessage "outside the immutable installed version"

$toolRoot = Join-Path ([IO.Path]::GetTempPath()) "CueEngine-RestoreContract-Tool"
Assert-RestoreRejected -Arguments @(
    "-ToolRoot", $toolRoot,
    "-InstallRoot", (Join-Path $toolRoot "Installed")
) -ExpectedMessage "separate directory trees"

$externalRoot = Join-Path ([IO.Path]::GetTempPath()) "CueEngine-RestoreContract-Dependencies"
$dependencyId = "a" * 64
$definitionId = "b" * 64
$dependencyRoot = Join-Path $externalRoot $dependencyId
$dependencyTool = Join-Path $dependencyRoot "Tool\vcpkg"
$dependencyInstall = Join-Path $dependencyRoot "Installed"
$buildIdentity = '{"targetTriplet":"x64-windows","hostArchitecture":"x64","targetArchitecture":"x64","compilerVendor":"msvc","compilerVersion":"19.44","toolsetVersion":"14.44","crtLinkage":"dynamic","crtVersion":"14.44","windowsSdkTargetVersion":"10.0.26100.0"}'

Assert-RestoreRejected -Arguments @(
    "-ToolRoot", $dependencyTool,
    "-InstallRoot", $dependencyInstall,
    "-InstalledVersionRoot", $versionRoot,
    "-DependencyRootId", $dependencyId
) -ExpectedMessage "must be provided together"

Assert-RestoreRejected -Arguments @(
    "-ToolRoot", $dependencyTool,
    "-InstallRoot", $dependencyInstall,
    "-InstalledVersionRoot", $versionRoot,
    "-DependencyRootId", ("A" * 64),
    "-DependencyDefinitionId", $definitionId,
    "-DependencyBuildIdentityJson", $buildIdentity
) -ExpectedMessage "64 lowercase hexadecimal"

Assert-RestoreRejected -Arguments @(
    "-ToolRoot", $dependencyTool,
    "-InstallRoot", $dependencyInstall,
    "-InstalledVersionRoot", $versionRoot,
    "-DependencyRootId", $dependencyId,
    "-DependencyDefinitionId", $definitionId,
    "-DependencyBuildIdentityJson", ($buildIdentity.Replace(":", ": "))
) -ExpectedMessage "must be canonical"

Assert-RestoreRejected -Arguments @(
    "-ToolRoot", $dependencyTool,
    "-InstallRoot", $dependencyInstall,
    "-InstalledVersionRoot", $versionRoot,
    "-DependencyRootId", $dependencyId,
    "-DependencyDefinitionId", $definitionId,
    "-DependencyBuildIdentityJson", ($buildIdentity.Replace("x64-windows", "x86-windows"))
) -ExpectedMessage "supported x64-windows MSVC ABI"

$quarantineParent = Join-Path ([IO.Path]::GetTempPath()) `
    "CueEngine-RestoreContract-Quarantine-$([Guid]::NewGuid().ToString('N'))"
$quarantineDependencyRoot = Join-Path $quarantineParent $dependencyId
$quarantineTool = Join-Path $quarantineDependencyRoot "Tool\vcpkg"
$quarantineInstall = Join-Path $quarantineDependencyRoot "Installed"
try
{
    New-Item -ItemType Directory -Path $quarantineDependencyRoot | Out-Null
    $output = (& $powerShell -NoProfile -File $restoreScript `
        -ToolRoot $quarantineTool `
        -InstallRoot $quarantineInstall `
        -GitExecutable $GitFailureProbe `
        -InstalledVersionRoot $versionRoot `
        -DependencyRootId $dependencyId `
        -DependencyDefinitionId $definitionId `
        -DependencyBuildIdentityJson $buildIdentity 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0)
    {
        throw "Restore with the failing Git probe unexpectedly succeeded."
    }
    if (Test-Path -LiteralPath $quarantineDependencyRoot)
    {
        throw "Invalid Dependency Root remained at its final path."
    }
    $quarantined = @(Get-ChildItem -LiteralPath $quarantineParent -Directory `
        -Filter ".invalid-$dependencyId-*")
    if ($quarantined.Count -ne 1)
    {
        throw "Invalid Dependency Root was not preserved in exactly one quarantine path. Output: $output"
    }
    $staging = @(Get-ChildItem -LiteralPath $quarantineParent -Directory -Filter ".staging-*")
    if ($staging.Count -ne 0)
    {
        throw "Failed Dependency staging was not cleaned up."
    }
}
finally
{
    if (Test-Path -LiteralPath $quarantineParent -PathType Container)
    {
        Remove-Item -LiteralPath $quarantineParent -Recurse -Force
    }
}

$fileCollisionParent = Join-Path ([IO.Path]::GetTempPath()) `
    "CueEngine-RestoreContract-FileCollision-$([Guid]::NewGuid().ToString('N'))"
$fileCollisionDependencyRoot = Join-Path $fileCollisionParent $dependencyId
$fileCollisionTool = Join-Path $fileCollisionDependencyRoot "Tool\vcpkg"
$fileCollisionInstall = Join-Path $fileCollisionDependencyRoot "Installed"
try
{
    New-Item -ItemType Directory -Path $fileCollisionParent | Out-Null
    [IO.File]::WriteAllText($fileCollisionDependencyRoot, "invalid dependency root")
    $output = (& $powerShell -NoProfile -File $restoreScript `
        -ToolRoot $fileCollisionTool `
        -InstallRoot $fileCollisionInstall `
        -GitExecutable $GitFailureProbe `
        -InstalledVersionRoot $versionRoot `
        -DependencyRootId $dependencyId `
        -DependencyDefinitionId $definitionId `
        -DependencyBuildIdentityJson $buildIdentity 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0)
    {
        throw "Restore with a file collision unexpectedly succeeded."
    }
    if (Test-Path -LiteralPath $fileCollisionDependencyRoot)
    {
        throw "File collision remained at the final Dependency Root path."
    }
    $quarantinedFiles = @(Get-ChildItem -LiteralPath $fileCollisionParent -File `
        -Filter ".invalid-$dependencyId-*")
    if ($quarantinedFiles.Count -ne 1)
    {
        throw "File collision was not preserved in exactly one quarantine path. Output: $output"
    }
    $staging = @(Get-ChildItem -LiteralPath $fileCollisionParent -Directory -Filter ".staging-*")
    if ($staging.Count -ne 0)
    {
        throw "Failed Dependency staging after a file collision was not cleaned up."
    }
}
finally
{
    if (Test-Path -LiteralPath $fileCollisionParent -PathType Container)
    {
        Remove-Item -LiteralPath $fileCollisionParent -Recurse -Force
    }
}
