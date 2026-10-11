param(
    [Parameter(Mandatory = $true)][string]$TestRoot,
    [Parameter(Mandatory = $true)][Guid]$Namespace,
    [Parameter(Mandatory = $true)][string]$ServerIp
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$root = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
$allowed = [IO.Path]::GetFullPath((Join-Path $repo 'bin\disposable-tests')).TrimEnd('\') + '\'
if (-not $root.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Not a disposable test root.' }
$data = Join-Path $root 'AppSandbox'
if ([IO.File]::ReadAllText((Join-Path $data '.asb-test-namespace')) -cne $Namespace.ToString('B')) { throw 'Namespace mismatch.' }
$discovery = Get-Content -LiteralPath (Join-Path $data 'host.json') -Raw | ConvertFrom-Json
$headers = @{Authorization = 'Bearer ' + $discovery.token}
$base = $discovery.endpoint.TrimEnd('/') + '/v1'
if ((Invoke-RestMethod -Uri ($base + '/version') -Headers $headers).testNamespace -cne $Namespace.ToString('B')) { throw 'Wrong daemon.' }
$appliance = Invoke-RestMethod -Uri ($base + '/shared-appliance') -Headers $headers
if ($appliance.backend -ne 'ubuntu' -or -not $appliance.ready -or
    -not $appliance.storageRoot.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Not the disposable Ubuntu appliance.' }
if ([Net.IPAddress]::Parse($ServerIp).AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { throw 'IPv4 required.' }
$key = Join-Path $data 'ssh\id_appsandbox'
$known = Join-Path $data 'ssh\known_hosts'
$ssh = "$env:WINDIR\System32\OpenSSH\ssh.exe"
$scp = "$env:WINDIR\System32\OpenSSH\scp.exe"
$options = @('-o', 'LogLevel=ERROR', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', "UserKnownHostsFile=$known", '-i', $key)
$target = 'asbtest@' + $ServerIp
$report = Join-Path $data 'agent-update-test.log'
'' | Out-File $report -Encoding utf8

# Windows PowerShell turns redirected native stderr into ErrorRecords, which
# terminate under ErrorActionPreference=Stop (gcc warnings aborted earlier
# runs). Run native tools with Continue, log every line as text and judge
# success only by the real exit code.
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments, [string]$StdIn)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($PSBoundParameters.ContainsKey('StdIn')) { $output = $StdIn | & $Exe @Arguments 2>&1 }
        else { $output = & $Exe @Arguments 2>&1 }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $saved }
    $lines = @($output | ForEach-Object { "$_" })
    if ($lines.Count) { $lines | Out-File $report -Append -Encoding utf8 }
    "[exit $code] $(Split-Path $Exe -Leaf) $($Arguments[-1])" | Out-File $report -Append -Encoding utf8
    return [pscustomobject]@{ Code = $code; Lines = $lines }
}

try {
    $source = Join-Path $repo 'tools\linux\agent\appsandbox-agent.c'
    if ((Invoke-Native $scp ($options + @($source, "${target}:/home/asbtest/appsandbox-agent.test.c"))).Code -ne 0) { throw 'Source copy failed.' }
    $build = 'rm -f /home/asbtest/appsandbox-agent.test && gcc -O2 -Wall -Wextra -pthread -D_GNU_SOURCE -o /home/asbtest/appsandbox-agent.test /home/asbtest/appsandbox-agent.test.c && test -x /home/asbtest/appsandbox-agent.test'
    if ((Invoke-Native $ssh ($options + @($target, $build))).Code -ne 0) { throw 'Guest agent build failed.' }
    $hash = Invoke-Native $ssh ($options + @($target, 'sha256sum /home/asbtest/appsandbox-agent.test'))
    if ($hash.Code -ne 0 -or -not ($hash.Lines -join ' ' -match '^([0-9a-f]{64})\s')) { throw 'Could not hash built agent.' }
    $builtHash = $Matches[1]
    Add-Type -AssemblyName System.Security
    $bytes = [IO.File]::ReadAllBytes((Join-Path $data 'shared-appliance.cred'))
    if ([BitConverter]::ToUInt32($bytes, 0) -ne 0x43534141 -or [BitConverter]::ToUInt32($bytes, 4) -ne 1) { throw 'Bad credential format.' }
    $count = [BitConverter]::ToUInt32($bytes, 8)
    [byte[]]$blob = $bytes[16..(16 + $count - 1)]
    $entropy = [Text.Encoding]::ASCII.GetBytes("AppSandbox.SharedAppliance.v1`0")
    $plain = [Security.Cryptography.ProtectedData]::Unprotect($blob, $entropy, [Security.Cryptography.DataProtectionScope]::LocalMachine)
    try {
        $password = [Text.Encoding]::Unicode.GetString($plain).TrimEnd([char]0)
        foreach ($command in @(
            "sudo -S -p '' install -m 0755 /home/asbtest/appsandbox-agent.test /usr/local/sbin/appsandbox-agent.new",
            "sudo -S -p '' mv -f /usr/local/sbin/appsandbox-agent.new /usr/local/sbin/appsandbox-agent",
            "sudo -S -p '' systemctl restart appsandbox-agent"
        )) {
            if ((Invoke-Native $ssh ($options + @($target, $command)) -StdIn $password).Code -ne 0) { throw 'Disposable guest agent update failed.' }
        }
        $password = $null
    } finally { [Array]::Clear($plain, 0, $plain.Length) }
    $installed = Invoke-Native $ssh ($options + @($target, 'sha256sum /usr/local/sbin/appsandbox-agent; sleep 2; systemctl is-active appsandbox-agent'))
    $text = $installed.Lines -join "`n"
    if ($text -notmatch "(?m)^$builtHash\s") { throw 'Installed agent hash does not match the built binary.' }
    if ($text -notmatch '(?m)^active$') { throw 'Agent service is not active after restart.' }
    "Disposable appliance agent rebuilt, installed (sha256 $builtHash) and active." | Out-File $report -Append -Encoding utf8
} catch { $_.Exception.Message | Out-File $report -Append -Encoding utf8; exit 1 }
