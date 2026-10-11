param(
    [Parameter(Mandatory = $true)][string]$Namespace,
    [Parameter(Mandatory = $true)][string]$TestRoot,
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$id = [Guid]::Parse($Namespace)
$canonical = $id.ToString('B')
$root = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
$allowed = [IO.Path]::GetFullPath((Join-Path $repo 'bin\disposable-tests')).TrimEnd('\') + '\'
if (-not $root.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test root must be a fresh child of bin\disposable-tests.' }
if (Test-Path -LiteralPath $root) {
    $marker = Join-Path $root 'AppSandbox\.asb-test-namespace'
    if (-not $Resume -or -not (Test-Path -LiteralPath $marker) -or
        [IO.File]::ReadAllText($marker) -cne $canonical) {
        throw 'Existing root requires -Resume and an exact matching namespace marker.'
    }
} elseif ($Resume) { throw 'Cannot resume a missing test root.' }
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Launch this script with Windows administrator privileges.' }
$data = Join-Path $root 'AppSandbox'
New-Item -ItemType Directory -Path $data -Force | Out-Null
if (-not $Resume) {
    [IO.File]::WriteAllText((Join-Path $data '.asb-test-namespace'), $canonical, [Text.Encoding]::ASCII)
}
$env:ProgramData = $root
$env:ASB_TEST_NAMESPACE = $canonical
# Windows PowerShell child helpers must not load PowerShell 7-only modules.
Remove-Item Env:PSModulePath -ErrorAction SilentlyContinue
& (Join-Path $repo 'bin\Release\AppSandbox.exe') --headless
exit $LASTEXITCODE
