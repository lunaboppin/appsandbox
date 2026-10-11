"""Execute the production PowerShell mapping commands against in-memory mocks.

No SMB cmdlets, appliance, drive mappings, or VMs are used by this test.
"""
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def command(mount):
    source = (ROOT / 'src/backend_win/shared_appliance.c').read_text(encoding='utf-8')
    start = source.index('static HRESULT host_mapping_command(', source.index('static DWORD host_mount_bit'))
    body = source[start:source.index('HRESULT shared_appliance_mount_host_resource', start)]
    expressions = re.findall(r'swprintf_s\(command, _countof\(command\),\s*((?:L"(?:\\.|[^"\\])*"\s*)+)', body)
    assert len(expressions) == 2, 'expected mount and unmount commands'
    text = ''.join(json.loads(token[1:]) for token in re.findall(r'L"(?:\\.|[^"\\])*"', expressions[0 if mount else 1]))
    return text.split('-Command "', 1)[1][:-1].replace('%c', 'T').replace('%s', r'\\192.0.2.2\asb-test')


MOCKS = r'''
$env:ASB_SMB_PASSWORD='test-only';$env:ASB_SMB_USER='test-only'
function Get-SmbGlobalMapping { [CmdletBinding()]param($LocalPath) $script:m }
function Get-Item { [CmdletBinding()]param($LiteralPath,[switch]$Force)
  if(-not $script:readable){throw 'unreachable'} }
function New-SmbGlobalMapping { [CmdletBinding()]param($LocalPath,$RemotePath,$Credential,$Persistent,$RequireIntegrity)
  $script:m=[pscustomobject]@{RemotePath=$RemotePath};$script:readable=$true;Write-Host 'CREATED' }
function Remove-SmbGlobalMapping { [CmdletBinding()]param($LocalPath,[switch]$Force)
  $script:m=$null;Write-Output 'REMOVED' }
function Start-Sleep { param($Milliseconds) }
'''


def run(mount, remote, readable):
    ps = shutil.which('powershell.exe')
    if not ps:
        raise RuntimeError('Windows PowerShell is required for this behavior test')
    setup = '$script:m=$null;' if remote is None else '$script:m=[pscustomobject]@{RemotePath=' + "'" + remote + "'};"
    setup += '$script:readable=$' + ('true;' if readable else 'false;')
    # Don't pass PowerShell 7's built-in module directories to Windows PS 5.1.
    env = {k: v for k, v in os.environ.items() if k.upper() != 'PSMODULEPATH'}
    return subprocess.run([ps, '-NoLogo', '-NoProfile', '-NonInteractive', '-Command',
                           MOCKS + setup + command(mount)], capture_output=True, text=True, env=env)


def main():
    target = r'\\192.0.2.2\asb-test'
    foreign = r'\\unrelated-server\personal-share'
    for mount, remote, readable, code, created, removed in (
        (True, None, False, 0, True, False),
        (True, target, True, 100, False, False),
        (True, target, False, 0, True, True),
        (True, foreign, True, 85, False, False),
        (False, target, True, 0, False, True),
        (False, None, False, 101, False, False),
        (False, foreign, True, 101, False, False),
    ):
        result = run(mount, remote, readable)
        assert result.returncode == code, (mount, remote, result.returncode, result.stdout, result.stderr)
        assert ('CREATED' in result.stdout) == created, result.stdout
        assert ('REMOVED' in result.stdout) == removed, result.stdout
    print('[ok] host mapping create/reuse/recover/remove and foreign mapping protection')


if __name__ == '__main__':
    main()
