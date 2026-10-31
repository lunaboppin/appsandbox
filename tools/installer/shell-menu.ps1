<#
  shell-menu.ps1 - Add or remove "Run in Sandbox" on Windows 11's short right-click menu.

  Setup and the uninstaller run this from the install folder. Windows only takes that menu
  entry from a package, and only installs a package signed by a certificate it trusts:
    trust     Elevated. Trusts a self-signed package certificate (Trusted People, not a root).
              A publicly trusted certificate ships no .cer, so this does nothing.
    register  As the signed-in user. A package belongs to one user, and it points at this folder.
    remove    Elevated. Removes the package for every user and the trust. Removing it also
              ends the process holding appsandbox-shell.dll, so the DLL can be replaced.

  Failing leaves the classic entry under "Show more options", so Setup carries on. The
  reason goes in %TEMP%\AppSandbox-shell-menu.log.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('trust', 'register', 'remove')][string]$Action)
$ErrorActionPreference = 'Stop'

$package = 'AppSandbox.ShellMenu'
$msix = Join-Path $PSScriptRoot 'AppSandbox-shell.msix'
$cer = Join-Path $PSScriptRoot 'AppSandbox-shell.cer'
$store = 'Cert:\LocalMachine\TrustedPeople'

try {
    switch ($Action) {
        'trust' {
            if (Test-Path -LiteralPath $cer) { Import-Certificate -FilePath $cer -CertStoreLocation $store | Out-Null }
        }
        'register' {
            Add-AppxPackage -Path $msix -ExternalLocation $PSScriptRoot
        }
        'remove' {
            Get-AppxPackage -AllUsers -Name $package | Remove-AppxPackage -AllUsers
            if (Test-Path -LiteralPath $cer) {
                $thumbprint = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
                Remove-Item -LiteralPath (Join-Path $store $thumbprint) -ErrorAction SilentlyContinue
            }
        }
    }
} catch {
    Add-Content -LiteralPath (Join-Path $env:TEMP 'AppSandbox-shell-menu.log') -Value "$(Get-Date -Format s) $Action failed: $_"
    exit 1
}
