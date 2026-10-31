<#
  stage-package.ps1 - Copy what App Sandbox ships out of a bin\ folder into a stage folder.

  The one list of shipped files, used by make-release.ps1 (the zip) and
  tools\installer\build-installer.ps1 (the installer), so the two can't drift.

  -IncludeTestCerts keeps the drivers' .cer files. A test-signed build needs them:
  guests trust the driver certificate from them. A release whose drivers Microsoft
  signed leaves them out.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Bin,
  [Parameter(Mandatory)][string]$Stage,
  [switch]$IncludeTestCerts
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

# Explicit allowlist (not "copy everything minus excludes"): the root holds ONLY the host app +
# its direct deps; the guest binaries ship under resources\, the signed drivers under drivers\,
# the web UI under web\.
$rootBins = @('AppSandbox.exe', 'appsandbox_core.dll', 'iso-patch.exe', 'WebView2Loader.dll')
foreach ($b in $rootBins) {
    $src = Join-Path $Bin $b
    if (-not (Test-Path $src)) { throw "Missing required root binary $b under $Bin." }
    Copy-Item $src (Join-Path $Stage $b) -Force
}

# The drivers\, resources\ and web\ subfolders are shipped whole, minus symbols/intermediates
# and (unless asked for) the test .cer (the drivers are MS-signed; devcon.exe is Microsoft's and stays).
$excludes = @('*.pdb', '*.lib', '*.exp', '*.ilk', '*.iobj', '*.ipdb', '*.log')
if (-not $IncludeTestCerts) { $excludes += '*.cer' }
foreach ($sub in @('resources', 'drivers', 'web')) {
    $srcSub = Join-Path $Bin $sub
    if (-not (Test-Path $srcSub)) { throw "Missing required folder $sub under $Bin." }
    robocopy $srcSub (Join-Path $Stage $sub) /E /XF @excludes | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $sub failed (exit $LASTEXITCODE)." }
    $global:LASTEXITCODE = 0   # robocopy 0..7 = success
}

# Ship the headless-api SDK as a sibling folder at the zip root: the Python SDK +
# examples + tests that script the daemon over the HTTP API. The folder is named
# "headless-api" - the tools\ parent is intentionally NOT shipped. __pycache__ and
# *.pyc are local build cruft and are excluded.
$sdkSrc = Join-Path $repo 'tools\headless-api'
$sdkDst = Join-Path $Stage 'headless-api'
New-Item -ItemType Directory -Force -Path $sdkDst | Out-Null
foreach ($f in @('asb.py', 'README.md')) {
    $fsrc = Join-Path $sdkSrc $f
    if (-not (Test-Path $fsrc)) { throw "Missing headless-api file $f under $sdkSrc." }
    Copy-Item $fsrc (Join-Path $sdkDst $f) -Force
}
foreach ($sub in @('examples', 'tests')) {
    robocopy (Join-Path $sdkSrc $sub) (Join-Path $sdkDst $sub) /E /XD __pycache__ /XF *.pyc | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy headless-api\$sub failed (exit $LASTEXITCODE)." }
    $global:LASTEXITCODE = 0
}

# Tripwire: the signed drivers come from the deposit (not the build) - fail loudly if any
# expected artifact is missing rather than shipping a partial package. The guest helpers are
# named one by one: a VM started from a package missing one fails in ways that are slow to trace.
$required = @(
    'AppSandbox.exe', 'appsandbox_core.dll', 'iso-patch.exe', 'WebView2Loader.dll',
    'drivers\AppSandboxVDD.dll', 'drivers\AppSandboxVDD.inf', 'drivers\AppSandboxVDD.cat',
    'drivers\AppSandboxVAD.sys', 'drivers\AppSandboxVAD.inf', 'drivers\AppSandboxVAD.cat',
    'drivers\AppSandboxSHM.sys', 'drivers\AppSandboxSHM.inf', 'drivers\AppSandboxSHM.cat',
    'drivers\devcon.exe', 'web',
    'resources\appsandbox-agent.exe', 'resources\appsandbox-audio.exe', 'resources\appsandbox-backstage.exe',
    'resources\appsandbox-clipboard.exe', 'resources\appsandbox-clipboard-reader.exe',
    'resources\appsandbox-displays.exe', 'resources\appsandbox-input.exe',
    'resources\linux\appsandbox-agent.c', 'resources\linux\guest-src\tools\linux\agent\appsandbox-agent.c', 'resources\win-mesa-gl\opengl32.dll',
    'headless-api\asb.py', 'headless-api\README.md', 'headless-api\examples', 'headless-api\tests'
)
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $Stage $_)) })
if ($missing.Count) { throw "Refusing to package - missing expected artifacts: $($missing -join ', ')" }

# Manifest: show exactly what is going into the package.
Write-Host "`n--- package contents ---"
$files = Get-ChildItem $Stage -Recurse -File
$files | ForEach-Object { Write-Host ("  " + $_.FullName.Substring($Stage.Length + 1)) }
$mb = [math]::Round((($files | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host ("--- {0} files, {1} MB ---`n" -f $files.Count, $mb)
