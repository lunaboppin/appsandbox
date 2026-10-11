param(
    [Parameter(Mandatory = $true)][string]$TestRoot,
    [Parameter(Mandatory = $true)][Guid]$Namespace,
    [Parameter(Mandatory = $true)][Guid]$ResourceId,
    [Parameter(Mandatory = $true)][string]$ServerIp
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$root = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
$allowed = [IO.Path]::GetFullPath((Join-Path $repo 'bin\disposable-tests')).TrimEnd('\') + '\'
if (-not $root.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Not a disposable test root.' }
$data = Join-Path $root 'AppSandbox'
if ([IO.File]::ReadAllText((Join-Path $data '.asb-test-namespace')) -cne $Namespace.ToString('B')) { throw 'Namespace mismatch.' }
$hostConfig = Get-Content -LiteralPath (Join-Path $data 'host.json') -Raw | ConvertFrom-Json
$headers = @{ Authorization = 'Bearer ' + $hostConfig.token }
$base = $hostConfig.endpoint.TrimEnd('/') + '/v1'
$version = Invoke-RestMethod -Uri ($base + '/version') -Headers $headers
if ($version.testNamespace -cne $Namespace.ToString('B')) { throw 'Daemon namespace mismatch.' }
$resource = (Invoke-RestMethod -Uri ($base + '/shared-resources') -Headers $headers).resources |
    Where-Object { $_.id -eq $ResourceId.ToString() } | Select-Object -First 1
if (-not $resource -or $resource.name -notlike 'disposable-*') { throw 'Not a disposable resource.' }
$ip = [Net.IPAddress]::Parse($ServerIp)
if ($ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { throw 'IPv4 required.' }
$local = $resource.hostDriveLetter + ':'
$remote = '\\' + $ServerIp + '\asb_' + $ResourceId.ToString('N').Substring(0, 20).ToLowerInvariant() + '$'
$report = Join-Path $data 'mapping-diagnostic.log'
$notes = [Collections.Generic.List[string]]::new()
$notes.Add('Admin=' + ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
try {
    $key = Join-Path $data 'ssh\id_appsandbox'
    $known = Join-Path $data 'ssh\known_hosts'
    $guestReport = Join-Path $data 'samba-diagnostic.log'
    & "$env:WINDIR\System32\OpenSSH\ssh.exe" -o LogLevel=ERROR -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o "UserKnownHostsFile=$known" -i $key "asbtest@$ServerIp" 'ip -br addr; testparm -s 2>/dev/null; ls -l /etc/samba/appsandbox-shares; sudo -n true >/dev/null 2>&1; echo sudo_rc=$?' 2>&1 |
        Out-File -LiteralPath $guestReport -Encoding utf8
    $mapping = Get-SmbGlobalMapping -LocalPath $local -ErrorAction SilentlyContinue
    if ($mapping -and $mapping.RemotePath -ine $remote) { throw 'Foreign mapping; refusing mutation.' }
    if ($mapping) { $notes.Add('Existing mapping: ' + $mapping.Status) }
    else {
        Add-Type -AssemblyName System.Security
        $bytes = [IO.File]::ReadAllBytes((Join-Path $data 'shared-appliance.cred'))
        if ([BitConverter]::ToUInt32($bytes, 0) -ne 0x43534141 -or [BitConverter]::ToUInt32($bytes, 4) -ne 1) { throw 'Bad credential format.' }
        $offset = 16 + [BitConverter]::ToUInt32($bytes, 8)
        $count = [BitConverter]::ToUInt32($bytes, 12)
        [byte[]]$blob = $bytes[$offset..($offset + $count - 1)]
        $entropy = [Text.Encoding]::ASCII.GetBytes("AppSandbox.SharedAppliance.v1`0")
        $plain = [Security.Cryptography.ProtectedData]::Unprotect($blob, $entropy, [Security.Cryptography.DataProtectionScope]::LocalMachine)
        try {
            $password = [Text.Encoding]::Unicode.GetString($plain).TrimEnd([char]0)
            $secure = ConvertTo-SecureString $password -AsPlainText -Force
            $credential = [pscredential]::new(($ServerIp + '\AppSandboxShare'), $secure)
            $password = $null
            New-SmbGlobalMapping -LocalPath $local -RemotePath $remote -Credential $credential -Persistent $false -RequireIntegrity $true -ErrorAction Stop | Out-Null
            $notes.Add('Diagnostic mount succeeded')
        } finally { [Array]::Clear($plain, 0, $plain.Length) }
    }
    $notes.Add('Readable=' + (Test-Path -LiteralPath ($local + '\')))
} catch {
    $notes.Add('Error type=' + $_.Exception.GetType().FullName)
    $notes.Add('Error id=' + $_.FullyQualifiedErrorId)
    $notes.Add('Error message=' + $_.Exception.Message)
} finally { [IO.File]::WriteAllLines($report, $notes) }
