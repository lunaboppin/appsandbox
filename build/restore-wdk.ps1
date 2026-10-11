param(
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [Parameter(Mandatory = $true)][string]$NuGetPath
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
[xml]$props = Get-Content -LiteralPath (Join-Path $repo 'Directory.Build.props')
$wdkVersion = ($props.Project.PropertyGroup.AsbWdkPackageVersion | Where-Object { $_ })
$sdkVersion = ($props.Project.PropertyGroup.AsbSdkPackageVersion | Where-Object { $_ })
$env:NUGET_PACKAGES = Join-Path $repo 'packages'
$env:NUGET_HTTP_CACHE_PATH = Join-Path $repo 'bin\build-tools\http-cache'
$env:NUGET_PLUGINS_CACHE_PATH = Join-Path $repo 'bin\build-tools\plugin-cache'
foreach ($package in @(
    @{ Id = 'Microsoft.Windows.SDK.CPP'; Version = $sdkVersion },
    @{ Id = "Microsoft.Windows.SDK.CPP.$Platform"; Version = $sdkVersion },
    @{ Id = "Microsoft.Windows.WDK.$Platform"; Version = $wdkVersion }
)) {
    & $NuGetPath install $package.Id -Version $package.Version -OutputDirectory $env:NUGET_PACKAGES `
        -Source 'https://api.nuget.org/v3/index.json' -NonInteractive -DirectDownload
    if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed: $($package.Id)" }
}
Write-Host 'Restored local kits. Build with /p:AsbUseLocalWdk=true (do not override driver PlatformToolset).'
