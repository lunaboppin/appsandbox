# Test certificates are file-only and imported into guests only: disposable test
# guests, or with -Release the guests of a test-signed release build.
# This script never writes to a host certificate store or changes host signing policy.
param([ValidateSet('x64')][string]$Platform = 'x64', [switch]$Release)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $repo 'bin\Release'
$signDir = Join-Path $repo 'bin\test-signing'
New-Item -ItemType Directory -Path $signDir -Force | Out-Null
[xml]$props = Get-Content -LiteralPath (Join-Path $repo 'Directory.Build.props')
$wdkVersion = ($props.Project.PropertyGroup.AsbWdkPackageVersion | Where-Object { $_ })
$sdkVersion = ($props.Project.PropertyGroup.AsbSdkPackageVersion | Where-Object { $_ })
$wdk = Join-Path $repo "packages\Microsoft.Windows.WDK.$Platform.$wdkVersion\c"
$sdk = Join-Path $repo "packages\Microsoft.Windows.SDK.CPP.$sdkVersion\c"
$signTool = Join-Path $sdk 'bin\10.0.28000.0\x64\signtool.exe'
$inf2cat = Join-Path $wdk 'bin\10.0.28000.0\x86\Inf2Cat.exe'
$infverif = Join-Path $wdk 'tools\10.0.28000.0\x64\infverif.exe'
$pfx = Join-Path $signDir 'disposable-only.pfx'
$cer = Join-Path $signDir 'disposable-only.cer'
$subject = 'CN=AppSandbox Disposable Guest Tests ONLY'
$lifetimeMonths = 2
if ($Release) {
    # Kept out of git (tools\vdd\certs\ is ignored) and reused for every test-signed
    # release, so guests keep trusting the same certificate.
    $signDir = Join-Path $repo 'tools\vdd\certs'
    New-Item -ItemType Directory -Path $signDir -Force | Out-Null
    $pfx = Join-Path $signDir 'appsandbox-test-signing.pfx'
    $cer = Join-Path $signDir 'appsandbox-test-signing.cer'
    $subject = 'CN=AppSandbox Test Signing'
    $lifetimeMonths = 60
}
if (-not (Test-Path -LiteralPath $pfx)) {
    $rsa = [Security.Cryptography.RSA]::Create(3072)
    $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
        $subject, $rsa,
        [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
        [Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
    $eku = [Security.Cryptography.OidCollection]::new()
    $eku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3')) | Out-Null
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $true))
    $certificate = $request.CreateSelfSigned([DateTimeOffset]::Now.AddDays(-1), [DateTimeOffset]::Now.AddMonths($lifetimeMonths))
    [IO.File]::WriteAllBytes($pfx, $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx))
    [IO.File]::WriteAllBytes($cer, $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    $certificate.Dispose()
    $rsa.Dispose()
}
New-Item -ItemType Directory -Path (Join-Path $out 'drivers'), (Join-Path $out 'resources') -Force | Out-Null
foreach ($driver in @(
    @{ Stem = 'AppSandboxVDD'; Folder = 'vdd'; Ext = 'dll'; Verify = '/u' },
    @{ Stem = 'AppSandboxVHID'; Folder = 'vhid'; Ext = 'sys'; Verify = '/w' },
    @{ Stem = 'AppSandboxVAD'; Folder = 'vad'; Ext = 'sys'; Verify = '/w' }
)) {
    $package = Join-Path $repo "tools\$($driver.Folder)\$Platform\Release\$($driver.Stem)"
    $binary = Join-Path $package "$($driver.Stem).$($driver.Ext)"
    & $signTool sign /fd SHA256 /f $pfx $binary
    if ($LASTEXITCODE -ne 0) { throw "Binary signing failed: $binary" }
    & $infverif $driver.Verify (Join-Path $package "$($driver.Stem).inf")
    if ($LASTEXITCODE -ne 0) { throw "INF validation failed: $($driver.Stem)" }
    & $inf2cat "/driver:$package" /os:10_25H2_X64 /uselocaltime
    if ($LASTEXITCODE -ne 0) { throw "Catalog generation failed: $($driver.Stem)" }
    $catalog = Join-Path $package "$($driver.Stem).cat"
    & $signTool sign /fd SHA256 /f $pfx $catalog
    if ($LASTEXITCODE -ne 0) { throw "Catalog signing failed: $catalog" }
    foreach ($ext in @($driver.Ext, 'inf', 'cat')) {
        Copy-Item -LiteralPath (Join-Path $package "$($driver.Stem).$ext") -Destination (Join-Path $out 'drivers') -Force
    }
    Copy-Item -LiteralPath $cer -Destination (Join-Path $out "drivers\$($driver.Stem).cer") -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'vendor\devcon\devcon.exe') -Destination (Join-Path $out 'drivers\devcon.exe') -Force
foreach ($name in @('appsandbox-agent', 'appsandbox-input', 'appsandbox-displays', 'appsandbox-clipboard', 'appsandbox-clipboard-reader', 'appsandbox-audio')) {
    Copy-Item -LiteralPath (Join-Path $repo "tools\agent\bin\Release\$name.exe") -Destination (Join-Path $out 'resources') -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'tools\iso-patch\bin\Release\iso-patch.exe') -Destination $out -Force
Write-Host 'Disposable guest payload staged. No host certificate store was changed.'
