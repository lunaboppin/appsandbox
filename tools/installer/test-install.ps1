<#
  test-install.ps1 - Install, check, upgrade and uninstall App Sandbox for real, in a throwaway folder.

  Run by CI on a disposable machine. Safe on a PC too: it installs no drivers, starts
  no VMs and never touches %ProgramData%\AppSandbox beyond checking it survives. It
  stops if App Sandbox is already installed here, so it can't disturb a real install.

  The per-machine installer needs an elevated prompt. -PerUser takes the installer
  from build-installer.ps1 -TestPerUser and needs none, but can't cover the HKLM
  uninstall entry or the update run that reopens the app.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Setup,
  [switch]$PerUser
)
$ErrorActionPreference = 'Stop'

$failures = New-Object System.Collections.ArrayList
function Check([string]$label, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Host "[ok] $label" } else { Write-Host "[FAIL] $label $detail"; [void]$failures.Add($label) }
}

function Run-Setup([string]$exe, [string[]]$arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -Wait
    return $p.ExitCode
}

function Get-PeVersion([string]$path) {
    # The numeric parts: Inno pads its version strings with spaces.
    $v = (Get-Item -LiteralPath $path).VersionInfo
    return "$($v.FileMajorPart).$($v.FileMinorPart).$($v.FileBuildPart).$($v.FilePrivatePart)"
}

$appId = '{7D3F2A64-9B1E-4C58-A0D7-3E6C1F82B954}_is1'
$root = if ($PerUser) { 'HKCU:' } else { 'HKLM:' }
$uninstallKey = "$root\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appId"
if (Test-Path $uninstallKey) { throw 'App Sandbox is already installed here; refusing to test over it.' }

$Setup = (Resolve-Path $Setup).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("asb-install-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$dir = Join-Path $work 'App Sandbox'
$log = Join-Path $work 'setup.log'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$version = Get-PeVersion $Setup
$dataDir = Join-Path $env:ProgramData 'AppSandbox'
$dataBefore = Test-Path $dataDir

$held = $null
try {
    # ------------------------------------------------------------ install ---
    $code = Run-Setup $Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/MERGETASKS=desktopicon', "/DIR=`"$dir`"", "/LOG=`"$log`"")
    Check 'installer exits 0' ($code -eq 0) "exit $code; log: $log"

    foreach ($f in @('AppSandbox.exe', 'appsandbox_core.dll', 'iso-patch.exe', 'WebView2Loader.dll', 'LICENSE', 'THIRD-PARTY-NOTICES.md',
                     'resources\appsandbox-agent.exe', 'resources\appsandbox-audio.exe', 'resources\appsandbox-backstage.exe',
                     'resources\appsandbox-clipboard.exe', 'resources\appsandbox-clipboard-reader.exe',
                     'resources\appsandbox-displays.exe', 'resources\appsandbox-input.exe',
                     'resources\linux\appsandbox-agent.c', 'resources\win-mesa-gl\opengl32.dll',
                     'drivers\AppSandboxVDD.inf', 'drivers\AppSandboxVAD.sys', 'drivers\AppSandboxSHM.sys', 'drivers\devcon.exe',
                     'web\index.html', 'web\app.js', 'headless-api\asb.py')) {
        Check "installed: $f" (Test-Path (Join-Path $dir $f))
    }
    Check 'symbols and test tools are not installed' (-not (Get-ChildItem $dir -Recurse -Include '*.pdb', 'updater-test.exe'))
    Check 'installed app is the installer''s version' ((Get-PeVersion (Join-Path $dir 'AppSandbox.exe')) -eq $version)
    Check 'uninstaller present' (Test-Path (Join-Path $dir 'unins000.exe'))

    $entry = Get-ItemProperty $uninstallKey -ErrorAction SilentlyContinue
    Check 'Apps & features entry written' ($null -ne $entry)
    if ($entry) {
        Check 'entry names the version' ($entry.DisplayVersion -eq $version) $entry.DisplayVersion
        Check 'entry records the folder (the updater relies on it)' ($entry.InstallLocation.TrimEnd('\') -eq $dir) $entry.InstallLocation
    }

    $shell = New-Object -ComObject WScript.Shell
    $programs = if ($PerUser) { [Environment]::GetFolderPath('Programs') } else { [Environment]::GetFolderPath('CommonPrograms') }
    $desktop = if ($PerUser) { [Environment]::GetFolderPath('Desktop') } else { [Environment]::GetFolderPath('CommonDesktopDirectory') }
    foreach ($path in @((Join-Path $programs 'App Sandbox.lnk'), (Join-Path $desktop 'App Sandbox.lnk'))) {
        Check "shortcut: $path" (Test-Path $path)
        if (Test-Path $path) {
            $lnk = $shell.CreateShortcut($path)
            Check 'shortcut targets the app, in its own folder' (($lnk.TargetPath -eq (Join-Path $dir 'AppSandbox.exe')) -and ($lnk.WorkingDirectory -eq $dir)) "$($lnk.TargetPath) in $($lnk.WorkingDirectory)"
        }
    }

    # ---------------------------------------------- reinstall over the top ---
    # What the app leaves beside itself must survive an upgrade; what the old version
    # shipped in the replaced folders must not.
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'd3dlayers') | Out-Null
    Set-Content (Join-Path $dir 'd3dlayers\cached.dll') 'x'
    Set-Content (Join-Path $dir 'appsandbox.log') 'x'
    Set-Content (Join-Path $dir 'OpenSSH-Win64-v10.0.0.0.msi') 'x'
    Set-Content (Join-Path $dir 'resources\dropped-by-new-version.exe') 'x'
    Set-Content (Join-Path $dir 'user-file.txt') 'keep me'

    $code = Run-Setup $Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$log`"")
    Check 'upgrade in place exits 0' ($code -eq 0) "exit $code"
    Check 'upgrade keeps the folder it was installed to' (Test-Path (Join-Path $dir 'AppSandbox.exe'))
    Check 'upgrade keeps the runtime cache' (Test-Path (Join-Path $dir 'd3dlayers\cached.dll'))
    Check 'upgrade clears files a new version dropped' (-not (Test-Path (Join-Path $dir 'resources\dropped-by-new-version.exe')))
    Check 'upgrade still has the guest helpers' (Test-Path (Join-Path $dir 'resources\appsandbox-backstage.exe'))

    # ------------------------------------------------- app running = hands off ---
    # Holding the app's own mutex looks exactly like a running app to Setup. Only an
    # elevated process may create a Global\ mutex, so this is per-machine only.
    if (-not $PerUser) {
        $held = New-Object System.Threading.Mutex($true, 'Global\AppSandboxCoreHost')
        $before = (Get-Item (Join-Path $dir 'AppSandbox.exe')).LastWriteTimeUtc
        Set-Content (Join-Path $dir 'marker-setup.txt') 'x'
        $code = Run-Setup $Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
        Check 'silent install refuses while the app runs' ($code -ne 0) "exit $code"
        $code = Run-Setup (Join-Path $dir 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
        Start-Sleep -Seconds 3
        Check 'silent uninstall refuses while the app runs' (Test-Path (Join-Path $dir 'AppSandbox.exe'))
        Check 'nothing was replaced' ($before -eq (Get-Item (Join-Path $dir 'AppSandbox.exe')).LastWriteTimeUtc)
        $held.ReleaseMutex(); $held.Dispose(); $held = $null
        Remove-Item (Join-Path $dir 'marker-setup.txt') -Force
    }

    # --------------------------------------------------------- update run ---
    # What the app's updater starts: Setup must wait for the app to let go of the
    # mutex, upgrade, then reopen the app.
    if (-not $PerUser) {
        $held = New-Object System.Threading.Mutex($true, 'Global\AppSandboxCoreHost')
        $update = Start-Process -FilePath $Setup -ArgumentList '/SILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/ASBUPDATE=1', "/LOG=`"$log`"" -PassThru
        Start-Sleep -Seconds 6
        Check 'update run waits while the app is still closing' (-not $update.HasExited)
        Check 'update run has not reopened the app yet' (-not (Get-Process AppSandbox -ErrorAction SilentlyContinue))
        $held.ReleaseMutex(); $held.Dispose(); $held = $null
        $update.WaitForExit(120000) | Out-Null
        Check 'update run exits 0' ($update.HasExited -and $update.ExitCode -eq 0) "log: $log"
        $reopened = $null
        for ($i = 0; $i -lt 30 -and -not $reopened; $i++) {
            $reopened = Get-Process AppSandbox -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $dir 'AppSandbox.exe') }
            if (-not $reopened) { Start-Sleep -Seconds 1 }
        }
        Check 'update run reopens the installed app' ($null -ne $reopened)
        if ($reopened) {
            $reopened | Stop-Process -Force
            $reopened | ForEach-Object { $_.WaitForExit(15000) | Out-Null }
        }
    }

    # ----------------------------------------------------------- uninstall ---
    $code = Run-Setup (Join-Path $dir 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    Check 'uninstaller exits 0' ($code -eq 0) "exit $code"
    # The uninstaller hands off to a temporary copy of itself, so wait for it to finish.
    for ($i = 0; $i -lt 60 -and (Test-Path (Join-Path $dir 'AppSandbox.exe')); $i++) { Start-Sleep -Seconds 1 }
    for ($i = 0; $i -lt 30 -and (Test-Path (Join-Path $dir 'unins000.exe')); $i++) { Start-Sleep -Seconds 1 }

    foreach ($f in @('AppSandbox.exe', 'appsandbox_core.dll', 'resources', 'drivers', 'web', 'headless-api', 'unins000.exe',
                     'd3dlayers', 'appsandbox.log', 'OpenSSH-Win64-v10.0.0.0.msi')) {
        Check "removed: $f" (-not (Test-Path (Join-Path $dir $f)))
    }
    Check 'a file of the user''s in the folder is left alone' (Test-Path (Join-Path $dir 'user-file.txt'))
    Check 'Apps & features entry removed' (-not (Test-Path $uninstallKey))
    Check 'shortcuts removed' (-not (Test-Path (Join-Path $programs 'App Sandbox.lnk')) -and -not (Test-Path (Join-Path $desktop 'App Sandbox.lnk')))
    Check 'a silent uninstall leaves VM data alone' ((Test-Path $dataDir) -eq $dataBefore)
} finally {
    if ($held) { $held.Dispose() }
    if (Test-Path (Join-Path $dir 'unins000.exe')) {
        Start-Process (Join-Path $dir 'unins000.exe') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 5
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count) { throw "$($failures.Count) installer check(s) failed: $($failures -join '; ')" }
Write-Host '[ok] install, upgrade and uninstall'
