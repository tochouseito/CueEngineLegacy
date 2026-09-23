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

$installerSource = Join-Path ([IO.Path]::GetDirectoryName($bootstrapPath)) "CueEngineInstallerTool.exe"
if (-not [IO.File]::Exists($installerSource))
{
    throw "CueEngineBootstrap sibling Installer is missing."
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("CueEngineBootstrapContract-" + [Guid]::NewGuid().ToString("N"))
$binRoot = Join-Path $testRoot "Bin"
try
{
    [IO.Directory]::CreateDirectory($binRoot) | Out-Null
    $testBootstrap = Join-Path $binRoot "CueEngineBootstrap.exe"
    $testInstaller = Join-Path $binRoot "CueEngineInstallerTool.exe"
    [IO.File]::Copy($bootstrapPath, $testBootstrap, $false)
    [IO.File]::Copy($installerSource, $testInstaller, $false)
    $installerInfo = [IO.FileInfo]::new($testInstaller)
    $installerHash = (Get-FileHash -LiteralPath $testInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = '{"schemaVersion":1,"distributionKind":"DeveloperSourceSdk","files":[' +
        '{"role":"installer","path":"Bin/CueEngineInstallerTool.exe","sizeBytes":' +
        $installerInfo.Length.ToString([Globalization.CultureInfo]::InvariantCulture) +
        ',"sha256":"' + $installerHash + '"}]}' + "`n"
    [IO.File]::WriteAllText(
        (Join-Path $testRoot "CueEngineDistribution.json"),
        $manifest,
        [Text.UTF8Encoding]::new($false))

    $forwardingOutput = (& $testBootstrap invalid 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 2 -or
        -not $forwardingOutput.Contains("Usage: CueEngineInstallerTool", [StringComparison]::Ordinal))
    {
        throw "CueEngineBootstrap did not forward execution to the inventory-matched Installer: $forwardingOutput"
    }

    $manifestPath = Join-Path $testRoot "CueEngineDistribution.json"
    [IO.File]::WriteAllText(
        $manifestPath,
        $manifest.Replace('"role":"installer"', '"role":"editor"'),
        [Text.UTF8Encoding]::new($false))
    $roleOutput = (& $testBootstrap invalid 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 23 -or
        -not $roleOutput.Contains("inventory entry", [StringComparison]::OrdinalIgnoreCase))
    {
        throw "CueEngineBootstrap did not reject the mismatched Installer role: $roleOutput"
    }
    [IO.File]::WriteAllText($manifestPath, $manifest, [Text.UTF8Encoding]::new($false))

    $stream = [IO.File]::Open($testInstaller, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try
    {
        $stream.WriteByte(0)
    }
    finally
    {
        $stream.Dispose()
    }
    $tamperOutput = (& $testBootstrap invalid 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 23 -or
        -not $tamperOutput.Contains("inventory entry", [StringComparison]::OrdinalIgnoreCase))
    {
        throw "CueEngineBootstrap did not reject the tampered Installer: $tamperOutput"
    }
}
finally
{
    if ([IO.Directory]::Exists($testRoot))
    {
        [IO.Directory]::Delete($testRoot, $true)
    }
}
