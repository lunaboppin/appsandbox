"""Offline contracts for custom VM storage and appliance-backed resources."""
from __future__ import annotations
import re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
def read(path): return (ROOT / path).read_text(encoding="utf-8")
def check(value, message):
    if not value: raise AssertionError(message)

def main():
    core = read("src/backend_win/asb_core.c")
    core_h = read("src/backend_win/asb_core.h")
    resources = read("src/backend_win/shared_resources.c")
    hcn = read("src/backend_win/hcn_network.c")
    hcs = read("src/backend_win/hcs_vm.c")
    agent = read("tools/agent/agent.c")
    host_agent = read("src/backend_win/vm_agent.c")
    idd = read("src/backend_win/vm_display_idd.c")
    input_agent = read("tools/agent/appsandbox-input.c")
    headless = read("src/app_win/headless.c")
    sdk = read("tools/headless-api/asb.py")
    html = read("web/index.html")
    js = read("web/app.js")

    for token in ("storageParent", "storage-parent", "StorageRoot=", "LastStorageParent="):
        check(token in core + core_h + headless + sdk + html + js,
              f"missing custom-storage contract: {token}")
    check("DRIVE_FIXED" in core and "NTFS" in core and "ReFS" in core,
          "VM storage validation lacks fixed NTFS/ReFS guards")
    check("remove_dir_recursive(vhdx_dir)" in core,
          "failed VM creation does not clean only its newly-created tree")

    for key in ("Version=2", "StorageKind=appliance", "LegacyHostPath=",
                "DriveLetter=", "HostDriveLetter=", "ReadOnly="):
        check(key in resources, f"appliance resource persistence missing {key}")
    check("ReplaceFileW" in resources and "FlushFileBuffers" in resources,
          "resource persistence is not atomic")
    for obsolete in ("set_vm_group_acl", "normalize_folder", "smb_transport_prepare"):
        check(obsolete not in resources + core,
              f"obsolete host-folder transport remains: {obsolete}")

    check('L"VirtualSmb"' not in hcs and '\\"VirtualSmb\\"' not in hcs,
          "HCS still publishes VSMB resources")
    check("data_vhdx_path" in hcs,
          "appliance data disk is not isolated on its own SCSI attachment")
    for token in ("hcn_create_share_server_endpoint", 'RemoteAddresses\\\":\\\"%S.2/32', "445"):
        check(token in hcn, f"appliance HCN isolation missing {token}")
    check("shared_appliance_prepare_client" in core and
          "shared_appliance_release_client" in core,
          "VM lifecycle is not coupled to appliance dependency lifecycle")

    check("New-SmbGlobalMapping" in agent and "-RequireIntegrity $true" in agent,
          "guest does not create a signed global SMB mapping")
    appliance = read("src/backend_win/shared_appliance.c")
    host_mapper = appliance[appliance.index("static HRESULT host_mapping_command"):
                            appliance.index("HRESULT shared_appliance_mount_host_resource")]
    check("Get-SmbGlobalMapping" in host_mapper and
          "RemotePath" in host_mapper and
          "Get-Item -LiteralPath" in host_mapper,
          "host SMB mapping does not reconcile an existing readable mapping")
    check("HOST_MAPPING_ALREADY_PRESENT" in host_mapper and
          "HOST_MAPPING_NOT_PRESENT" in host_mapper and
          "host_mapping_command(resource, TRUE)" in appliance and
          "if (hr == S_FALSE) hr = S_OK;" in appliance and
          "if (SUCCEEDED(hr) && bit)" in appliance and
          "InterlockedOr(&g_appliance.host_mount_mask" in appliance,
          "host mount accounting is not idempotent")
    check("host_mount_mask" in appliance and
          "InterlockedOr" in appliance and
          "InterlockedAnd" in appliance,
          "host mount accounting does not track unique mapped drives")
    check("Get-Item -LiteralPath $root -Force -ErrorAction Stop" in agent and
          "for($attempt=0;$attempt -lt 8;$attempt++)" in agent and
          "Remove-SmbGlobalMapping -LocalPath $local -Force" in agent,
          "guest can report an SMB drive mapped before its root is readable")
    check("#include <shlobj.h>" in agent and
          '#pragma comment(lib, "shell32.lib")' in agent and
          "SHChangeNotify" in agent and
          "SHCNE_DRIVEADDGUI" in agent and
          "--refresh-drive" in agent and
          "notify_shell_drive_added(letter_a)" in agent,
          "late global SMB mappings do not refresh the interactive Explorer shell")
    check("Set-Location -LiteralPath ([Environment]::SystemDirectory)" in agent and
          "GetSystemDirectoryW" in agent and
          "system_directory" in agent,
          "PowerShell helpers escape mapped-drive working directories")
    check("Enable-NetAdapter -Name $a.Name" in agent and
          "Status -eq 'Up'" in agent and
          "Get-NetIPAddress -InterfaceIndex $a.ifIndex" in agent,
          "shared NIC readiness is verified before SMB mapping")
    check('L"netsh interface ip set address \\\"Ethernet\\\"' not in agent and
          'L"netsh interface ip set dns \\\"Ethernet\\\"' not in agent,
          "NAT configuration does not hard-code the guest adapter name")
    check("configure_nat_nic" in agent and
          "ASB_SHARED_NIC_MAC" in agent and
          "Status -ne 'Disabled'" in agent and
          "Get-NetRoute" in agent and
          "only non-shared adapter" in agent and
          "NAT configuration failed adapter=" in agent,
           "NAT configuration selects the non-share adapter and verifies its route")
    nat = agent[agent.index("static int configure_nat_nic"):]
    check("Remove-NetRoute -Confirm:$false" in nat and
          nat.index("Remove-NetRoute -Confirm:$false") < nat.index("New-NetIPAddress"),
          "NAT configuration clears an existing default route before assigning the gateway")
    check('agent_log_to_host("nat_net: %s", line)' in agent,
          "NAT configuration diagnostics are forwarded to the host log")
    check("hcn_get_endpoint_mac" in host_agent and
          "ASB_NAT_NIC_MAC" in agent and
          "NAT adapter with MAC" in agent,
          "NAT configuration targets the HCN endpoint MAC rather than guessing")
    check("agent_initializing" in host_agent and
          "process_async_message(vm, s, rsp) != 0" in host_agent and
          "asb_request_shared_resource_sync();" in host_agent,
          "guest startup does not race shared mapping and retries after readiness")
    check(host_agent.index("if (!configure_guest_nat") <
          host_agent.index("sprintf_s(map_cmd"),
          "NAT configuration is sent before slow appliance SMB mapping")
    check("!d->vm->agent_initializing" in idd and
          idd.index("!d->vm->agent_initializing") < idd.index('vm_agent_send(d->vm, "idd_connect"'),
          "IDD helper startup can collide with the agent initialization command slot")
    check("return inst->agent_online && !inst->agent_initializing && inst->idd_ready;" in core,
          "display readiness is exposed before guest initialization is complete")
    check("MOUSE_MOVE_MIN_INTERVAL_MS" in idd and
          "pending_mouse" in idd and
          "SetTimer(hwnd, IDT_INPUT" in idd,
          "IDD mouse forwarding lacks coalescing and a UI-safe rate limit")
    input_loop = input_agent[input_agent.index("static void handle_conn"):]
    check("switch_to_input_desktop();" in input_loop,
          "guest input helper switches desktops once per mouse packet")
    style = read("web/style.css")
    check("input:not([type])" in style and
          "settings-resource-actions" in style and
          "settings-resource-details" in js,
          "shared appliance fields and resource rows use the shared control theme")
    for tooltip in ("Start the shared appliance VM.",
                    "Choose the parent folder where the shared appliance files are stored.",
                    "Open this resource in File Explorer on the host.",
                    "Permanently delete all data stored in this resource."):
        check(tooltip in html + js,
              f"shared settings button tooltip missing: {tooltip}")
    for edition in ("Windows Server 2019 Standard", "Windows Server 2019 Datacenter"):
        check(edition in html,
              f"Windows Server WIM image dropdown missing: {edition}")
    check("hcn_get_endpoint_mac" in hcs and
          "default_mac_field" in hcs and
          r'L"\"Default\":{\"EndpointId\":\"%s\"%s}' in hcs,
          "HCS binds the guest NAT adapter to the HCN endpoint MAC")
    check("shared_appliance_get_smb_credentials" in host_agent,
          "client mappings do not use appliance DPAPI credentials")
    check("strrchr(arg, ':')" in agent,
          "shared NIC parser can still split inside the deterministic MAC address")
    nic = agent[agent.index("static int configure_shared_nic"):
                agent.index("static int configure_nat_nic")]
    check("-DefaultGateway" not in nic and "New-NetIPAddress" in nic,
          "private NIC configuration can alter the guest default route")
    check("drive_collision" in agent and "map_failed:%lu" in agent,
          "mapping collisions or exact Windows failures are not surfaced")

    for route in ("/v1/shared-resources", "/shared-appliance", "/host-mount",
                  "/host-unmount", "/purge"):
        check(route in headless, f"headless route missing {route}")
    check("host_path_unsupported" in headless,
          "legacy hostPath input is not rejected with migration guidance")
    for method in ("shared_resources", "create_shared_resource", "update_shared_resource",
                   "remove_shared_resource", "set_vm_shared_resource",
                   "setup_shared_appliance", "mount_host_resource"):
        check(f"def {method}" in sdk, f"SDK missing {method}")

    ids = re.findall(r'\bid="([^"]+)"', html)
    check(len(ids) == len(set(ids)), "web/index.html contains duplicate element ids")
    node = shutil.which("node")
    if node:
        check(subprocess.run([node, "--check", str(ROOT / "web/app.js")]).returncode == 0,
              "web/app.js failed node --check")
    print("[ok] custom storage and appliance-backed resource contracts")
    return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except AssertionError as exc:
        print(f"[fail] {exc}", file=sys.stderr)
        raise SystemExit(1)
