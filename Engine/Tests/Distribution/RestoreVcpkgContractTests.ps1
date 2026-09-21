[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
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
    $failingExecutable = Join-Path ([Environment]::GetFolderPath('System')) "where.exe"
    $output = (& $powerShell -NoProfile -File $restoreScript `
        -ToolRoot $quarantineTool `
        -InstallRoot $quarantineInstall `
        -GitExecutable $failingExecutable `
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
    $failingExecutable = Join-Path ([Environment]::GetFolderPath('System')) "where.exe"
    $output = (& $powerShell -NoProfile -File $restoreScript `
        -ToolRoot $fileCollisionTool `
        -InstallRoot $fileCollisionInstall `
        -GitExecutable $failingExecutable `
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
