<#
  build-installer.ps1 - Build the App Sandbox installer from an already built bin\ folder.

  Output: bin\Release\installer\AppSandbox-<version>-win-<arch>-setup.exe, plus a .sha256 beside it.
  Build the solution first. make-release.ps1 -Installer calls this for EV-signed releases.

  USAGE:
    # A test-signed installer (keeps the drivers' .cer files, which guests need):
    .\build-installer.ps1 -SignThumbprint 6546C03C0B5C55CD8688FCC203AD5598844C89C2 -TestCerts

    # What make-release.ps1 does, with the EV token present:
    .\build-installer.ps1 -SignThumbprint <EV thumbprint>

    # Unsigned, for looking at the wizard. The app's updater will refuse this one.
    .\build-installer.ps1

    # Per-user variant for testing install/uninstall without a UAC prompt. Never published.
    .\build-installer.ps1 -TestPerUser

  The version is read from Directory.Build.props, the single source of truth, and
  must match what the staged AppSandbox.exe was built with.
#>
[CmdletBinding()]
param(
  [string]$Configuration = 'Release',
  [ValidateSet('x64','ARM64')][string]$Platform = 'x64',
  [string]$SignThumbprint = '',
  [string]$TimestampUrl = '',
  [switch]$TestCerts,
  [switch]$TestPerUser
)
$ErrorActionPreference = 'Stop'

if ($TestPerUser -and $SignThumbprint) { throw '-TestPerUser builds are never signed: they are not for release.' }

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$binLeaf = if ($Platform -eq 'ARM64') { "$Configuration-ARM64" } else { $Configuration }
$bin = Join-Path $repo ("bin\{0}" -f $binLeaf)
$arch = $Platform.ToLower()

function Find-Iscc {
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($dir in @("$env:LOCALAPPDATA\Programs\Inno Setup 6", "${env:ProgramFiles(x86)}\Inno Setup 6", "$env:ProgramFiles\Inno Setup 6")) {
        $candidate = Join-Path $dir 'ISCC.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    throw 'Inno Setup 6 was not found. Install it with: winget install --id JRSoftware.InnoSetup -e'
}

function Find-SignTool {
    $c = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $c) { throw 'signtool.exe not found under Windows Kits\10\bin\*\x64.' }
    return $c.FullName
}

function Get-PeVersion([string]$path) {
    # The numeric parts: Inno pads its version strings with spaces.
    $v = (Get-Item -LiteralPath $path).VersionInfo
    return "$($v.FileMajorPart).$($v.FileMinorPart).$($v.FileBuildPart).$($v.FilePrivatePart)"
}

# ---------------------------------------------------------------- version ---

[xml]$props = Get-Content (Join-Path $repo 'Directory.Build.props') -Raw
$pg = @($props.Project.PropertyGroup) | Where-Object { $_.AsbVersionMajor } | Select-Object -First 1
if (-not $pg) { throw 'Could not read the version from Directory.Build.props.' }
$version = (@("$($pg.AsbVersionMajor)", "$($pg.AsbVersionMinor)", "$($pg.AsbVersionPatch)", "$($pg.AsbVersionRevision)") | ForEach-Object { $_.Trim() }) -join '.'
Write-Host "Installer version: $version (source: Directory.Build.props)"

$exe = Join-Path $bin 'AppSandbox.exe'
if (-not (Test-Path $exe)) { throw "$exe was not found. Build the solution first." }
$built = Get-PeVersion $exe
if ($built -ne $version) { throw "AppSandbox.exe is version '$built' but Directory.Build.props says '$version'. Rebuild first." }

# ------------------------------------------------------------------ stage ---

$work = Join-Path $bin '_installer'
$stage = Join-Path $work 'stage'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
$stageArgs = @{ Bin = $bin; Stage = $stage }
if ($TestCerts) { $stageArgs.IncludeTestCerts = $true }
& (Join-Path $repo 'tools\sign\stage-package.ps1') @stageArgs

# ---------------------------------------------------------------- compile ---

$outDir = if ($TestPerUser) { Join-Path $work 'output' } else { Join-Path $bin 'installer' }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$isccArgs = @("/DAppVersion=$version", "/DStageDir=$stage", "/DOutputDir=$outDir", "/DArch=$arch")
if ($TestPerUser) { $isccArgs += '/DTestPerUser' }
if ($SignThumbprint) {
    $signtool = Find-SignTool
    if (-not $TimestampUrl) {
        $cfg = Get-Content (Join-Path $repo 'tools\sign\submission-config.json') -Raw | ConvertFrom-Json
        $TimestampUrl = $cfg.timestampUrl
    }
    # ISCC signs Setup and the uninstaller inside it through this named tool; $f is the
    # file and $q a quote. The uninstaller can only be signed during the build.
    $isccArgs += '/DSignTool'
    $isccArgs += "/Sasb=`$q$signtool`$q sign /sha1 $SignThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 `$f"
}
$iss = Join-Path $PSScriptRoot 'AppSandbox.iss'
& (Find-Iscc) @isccArgs $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)." }

# ----------------------------------------------------------------- checks ---

$setup = Join-Path $outDir ("AppSandbox-{0}-win-{1}-setup.exe" -f $version, $arch)
if (-not (Test-Path $setup)) { throw "ISCC did not produce $setup." }
if ((Get-PeVersion $setup) -ne $version) { throw "$setup does not carry version $version." }

if ($SignThumbprint) {
    $sig = Get-AuthenticodeSignature -LiteralPath $setup
    if (-not $sig.SignerCertificate -or $sig.SignerCertificate.Thumbprint -ne $SignThumbprint) {
        throw "$setup is not signed by $SignThumbprint."
    }
    # The app's updater only accepts certificates pinned in update_config.h.
    $sha256 = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($sig.SignerCertificate.RawData)).Replace('-', '')
    $config = Get-Content (Join-Path $repo 'src\app_win\update_config.h') -Raw
    if ($config -notmatch $sha256) {
        Write-Warning "Signer SHA-256 $sha256 is not pinned in src\app_win\update_config.h. Installed copies will refuse this update."
    }
}

if (-not $TestPerUser) {
    $hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
    Set-Content -LiteralPath "$setup.sha256" -Value "$hash *$(Split-Path $setup -Leaf)" -Encoding ascii
    Write-Host "SHA-256: $hash"
}
Remove-Item $stage -Recurse -Force
Write-Host "Installer ready: $setup"
