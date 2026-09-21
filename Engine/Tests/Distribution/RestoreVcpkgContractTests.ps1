[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$restoreScript = Join-Path $RepositoryRoot "Tools\Dependencies\RestoreVcpkg.ps1"
$powerShell = Join-Path $PSHOME "pwsh.exe"

function Assert-RestoreRejected
{
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedMessage
    )

    $output = (& $powerShell -NoProfile -File $restoreScript @Arguments 2>&1 | Out-String)
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
