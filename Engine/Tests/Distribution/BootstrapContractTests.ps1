[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Bootstrap,

    [Parameter(Mandatory = $true)]
    [string]$Dumpbin
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$bootstrapPath = [IO.Path]::GetFullPath($Bootstrap)
$dumpbinPath = [IO.Path]::GetFullPath($Dumpbin)
if (-not [IO.File]::Exists($bootstrapPath) -or -not [IO.File]::Exists($dumpbinPath))
{
    throw "Bootstrap contract inputs must be existing absolute files."
}

$headers = (& $dumpbinPath /headers $bootstrapPath 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or -not $headers.Contains("machine (x64)", [StringComparison]::OrdinalIgnoreCase))
{
    throw "CueEngineBootstrap must be a Windows x64 PE image: $headers"
}

$dependents = (& $dumpbinPath /dependents $bootstrapPath 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0)
{
    throw "CueEngineBootstrap imports could not be inspected: $dependents"
}
$imports = @([Text.RegularExpressions.Regex]::Matches(
    $dependents,
    '(?m)^\s+([^\s]+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() })
if ($imports.Count -ne 1 -or $imports[0] -cne "kernel32.dll")
{
    throw "CueEngineBootstrap must import only KERNEL32.dll, but found: $($imports -join ', ')"
}

$forwardingOutput = (& $bootstrapPath invalid 2>&1 | Out-String)
if ($LASTEXITCODE -ne 2 -or
    -not $forwardingOutput.Contains("Usage: CueEngineInstallerTool", [StringComparison]::Ordinal))
{
    throw "CueEngineBootstrap did not forward execution to the sibling Installer: $forwardingOutput"
}
