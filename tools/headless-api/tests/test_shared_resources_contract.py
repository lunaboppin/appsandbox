"""Offline contract checks for custom VM storage and shared resources."""
from __future__ import annotations
import ast, re, shutil, subprocess, sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
def read(p): return (ROOT/p).read_text(encoding="utf-8")
def check(v,m):
    if not v: raise AssertionError(m)

def main():
    core=read("src/backend_win/asb_core.c")
    core_h=read("src/backend_win/asb_core.h")
    shared=read("src/backend_win/shared_resources.c")
    smb=read("src/backend_win/smb_transport.c")
    hcn=read("src/backend_win/hcn_network.c")
    vm_agent=read("src/backend_win/vm_agent.c")
    hcs=read("src/backend_win/hcs_vm.c")
    agent=read("tools/agent/agent.c")
    headless=read("src/app_win/headless.c")
    sdk=read("tools/headless-api/asb.py")
    html=read("web/index.html")
    js=read("web/app.js")

    for token in ("storageParent","storage-parent","StorageRoot=","LastStorageParent="):
        check(token in core+core_h+headless+sdk+html+js,f"missing custom-storage contract: {token}")
    check("DRIVE_FIXED" in core and "NTFS" in core and "ReFS" in core,
          "storage validation lacks fixed NTFS/ReFS guards")
    check("remove_dir_recursive(vhdx_dir)" in core,"failed creation does not clean its new tree")

    for key in ("Id=","Name=","Path=","DriveLetter=","ReadOnly=","AclCreated=","SmbAclCreated="):
        check(key in shared,f"shared-resource persistence missing {key}")
    for guard in ("PathIsUNCW","DRIVE_FIXED","FILE_ATTRIBUTE_REPARSE_POINT","path_contains"):
        check(guard in shared,f"shared path validation missing {guard}")
    check("ReplaceFileW" in shared and "FlushFileBuffers" in shared,
          "shared resource persistence is not atomic")
    check("S-1-5-83-0" in shared and "REVOKE_ACCESS" in shared,
          "tracked Virtual Machines ACL lifecycle is missing")

    check('L"VirtualSmb"' not in hcs and "\\\"VirtualSmb\\\"" not in hcs,
          "full Windows HCS document still contains VirtualSmb shared resources")
    for token in ("hcn_create_share_network", "hcn_create_share_endpoint",
                  "RemotePorts", "445", '\\"ACL\\"'):
        check(token in hcn,f"isolated SMB HCN contract missing {token}")
    check("share_endpoint_guid" in hcs,
          "HCS document cannot attach the isolated SMB endpoint")
    for token in ("CryptProtectData", "CryptUnprotectData", "NetUserAdd",
                  "NetShareAdd", "STYPE_SPECIAL", "SeDenyInteractiveLogonRight",
                  "New-NetFirewallRule"):
        check(token in smb,f"host SMB provisioning missing {token}")
    check("smb_transport_prepare" in core and "smb_transport_get_credentials" in vm_agent,
          "VM lifecycle is not connected to the SMB transport")
    check(core.count("prepare_shared_transport(") >= 4 and
          core.count("hcs_create_vm_with_endpoints(") >= 3,
          "existing, new, and cloned Windows VM lifecycle paths do not attach SMB")
    check('L"smb"' in core+hcs+vm_agent,
          "shared-resource status does not report the smb transport")
    check("shared_net:" in agent and "shared_net:" in vm_agent,
          "private guest NIC configuration protocol is missing")
    net_mapper=agent[agent.index("static int configure_shared_nic"):
                     agent.index("static int map_smb_drive_global")]
    check("New-NetIPAddress" in net_mapper and "-DefaultGateway" not in net_mapper,
          "private guest NIC can alter the guest default route")
    check("Get-NetAdapter -IncludeHidden" in net_mapper and
          "$i -lt 60" in net_mapper and
          "Start-Sleep -Milliseconds 500" in net_mapper,
          "private guest NIC discovery does not wait for Windows device enumeration")
    check(net_mapper.index("Start-Sleep -Milliseconds 500") <
          net_mapper.index("if(-not $a){exit 1168}"),
          "private guest NIC discovery returns ERROR_NOT_FOUND before retrying")
    check("New-SmbGlobalMapping" in agent and "-RequireIntegrity $true" in agent,
          "guest does not create a signed global SMB mapping")
    check("ASB_SMB_PASSWORD" in agent and "SetEnvironmentVariableW" in agent,
          "guest SMB password is not passed through an inherited environment variable")
    check("NetShareDel" in smb and "NetUserDel" in smb and "REVOKE_ACCESS" in smb,
          "AppSandbox-owned SMB infrastructure cleanup is incomplete")
    for token in ("-RemoteAddress", "-InterfaceAlias", "-LocalPort 445",
                  "GENERIC_READ", "GENERIC_ALL"):
        check(token in smb,f"SMB firewall/access-mode contract missing {token}")
    shared_cmd=agent[agent.index('else if (strncmp(cmd, "shared_smb_map:", 15) == 0)'):
                     agent.index('else if (strncmp(cmd, "ssh_deploy_key ", 15) == 0)')]
    check("map_smb_drive_global" in shared_cmd,
          "shared SMB command does not map directly from the LocalSystem service")
    for token in ("WTSQueryUserToken", "CreateProcessAsUserW", "pending_logon",
                  "map_vsmb_drive_global", "DefineDosDeviceW"):
        check(token not in shared_cmd,
              f"shared SMB command retains obsolete mapping machinery: {token}")
    mapper=agent[agent.index("static int map_smb_drive_global"):
                 agent.index("/* ---- Persistent client handler ---- */")]
    check("New-SmbGlobalMapping" in mapper and "Test-Path" in mapper,
          "global SMB mapper does not create and verify the requested drive")
    log_line=agent[agent.index('agent_log("Command:'):agent.index('agent_log("Command:')+500]
    check("shared_smb_map" in log_line and "redacted" in log_line,
          "guest command logging can expose SMB credentials")
    check("drive_collision" in agent,"preferred drive collisions are not reported")
    check('"map_failed:%lu"' in agent or '"map_failed:%d"' in agent,
          "guest mapper discards the Windows mapping failure code")
    check("[DEBUG-wts91]" not in agent,
          "temporary WTS diagnostics were not removed")
    check("upgrade_windows_agent_offline" in core and "AttachVirtualDisk" in core,
          "existing stopped guests are not upgraded on the active VHDX")
    check("retrying VM without shared resources" not in hcs,
          "obsolete VSMB retry path remains")

    if sys.platform == "win32":
        powershell=shutil.which("powershell.exe")
        scripts=re.findall(r'static const char script\[\] =\s*((?:"(?:\\.|[^"\\])*"\s*)+);',agent)
        check(len(scripts)>=2,"guest SMB PowerShell helpers were not found")
        for index,encoded in enumerate(scripts):
            source="".join(ast.literal_eval(part) for part in re.findall(r'"(?:\\.|[^"\\])*"',encoded))
            parser=("$s=[Console]::In.ReadToEnd();$t=$null;$e=$null;"
                    "[void][System.Management.Automation.Language.Parser]::ParseInput($s,[ref]$t,[ref]$e);"
                    "if($e.Count){$e|ForEach-Object{$_.Message};exit 1}")
            result=subprocess.run([powershell,"-NoLogo","-NoProfile","-NonInteractive","-Command",parser],
                                  input=source,text=True,capture_output=True)
            check(result.returncode==0,f"guest SMB PowerShell helper {index} has invalid syntax: {result.stdout}{result.stderr}")

    for route in ('/v1/shared-resources','shared-resources/'):
        check(route in headless,f"headless shared-resource route missing {route}")
    for method in ("shared_resources","create_shared_resource","update_shared_resource",
                   "remove_shared_resource","set_vm_shared_resource"):
        check(f"def {method}" in sdk,f"SDK missing {method}")
    ids=re.findall(r'\bid="([^"]+)"',html)
    check(len(ids)==len(set(ids)),"web/index.html contains duplicate element ids")
    node=shutil.which("node")
    if node: check(subprocess.run([node,"--check",str(ROOT/"web/app.js")]).returncode==0,
                   "web/app.js failed node --check")
    print("[ok] custom storage, isolated SMB resources, ACLs, guest mapping, API, and UI contracts")
    return 0

if __name__=="__main__":
    try: raise SystemExit(main())
    except AssertionError as exc:
        print(f"[fail] {exc}",file=sys.stderr);raise SystemExit(1)
