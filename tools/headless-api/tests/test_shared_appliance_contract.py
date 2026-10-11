"""Offline contract for the invisible shared-storage appliance."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def check(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> None:
    header = read("src/backend_win/shared_appliance.h")
    impl = read("src/backend_win/shared_appliance.c")
    resources = read("src/backend_win/shared_resources.c")
    resource_header = read("src/backend_win/shared_resources.h")
    hcn = read("src/backend_win/hcn_network.c")
    hcs = read("src/backend_win/hcs_vm.c")
    hcs_header = read("src/backend_win/hcs_vm.h")
    core = read("src/backend_win/asb_core.c")
    win_agent = read("tools/agent/agent.c")
    linux_agent = read("tools/linux/agent/appsandbox-agent.c")
    ui = read("src/app_win/ui.c")
    html = read("web/index.html")
    js = read("web/app.js")
    headless = read("src/app_win/headless.c")
    sdk = read("tools/headless-api/asb.py")
    project = read("AppSandboxCore.vcxproj")
    disk = read("src/backend_win/disk_util.c")

    for token in (
        "SharedApplianceConfig", "SharedApplianceStatus", "shared_appliance_setup",
        "shared_appliance_start", "shared_appliance_stop", "shared_appliance_grow",
        "shared_appliance_rebuild", "shared_appliance_prepare_client",
        "shared_appliance_release_client", "shared_appliance_reconcile",
    ):
        check(token in header, f"shared appliance interface missing {token}")

    check("shared_appliance.c" in project and "shared_appliance.h" in project,
          "shared appliance module is not built by AppSandboxCore")
    check("smb_transport.c" not in project and "smb_transport.h" not in project,
          "rejected host SMB transport is still built")
    check("[DEBUG-smb42]" not in impl + core,
          "temporary host SMB diagnostics remain")

    for token in ("CryptProtectData", "CryptUnprotectData", "shared-appliance.cfg",
                  "shared-appliance.cred", "ReplaceFileW", "SecureZeroMemory"):
        check(token in impl, f"appliance persistence/credential contract missing {token}")
    for token in ("ManagementCert=", "AuthenticateAsClient", "GetCertHashString",
                  "certificate mismatch"):
        check(token in impl, f"Server Core certificate pinning missing {token}")
    for token in ("cloud-images.ubuntu.com", "noble-server-cloudimg-amd64.img",
                  "UBUNTU_IMAGE_BUILD", "SHA256SUMS", "--qcow2-to-vhdx", "CIDATA"):
        check(token in impl, f"managed Ubuntu appliance provisioning missing {token}")
    check("/current/" not in impl, "Ubuntu appliance image still follows a moving alias")
    seed = disk[disk.index("HRESULT iso_create_appliance_cloud_init"):disk.index("HRESULT iso_create_instance_resources", disk.index("HRESULT iso_create_appliance_cloud_init"))]
    check('L"w,ccs=UTF-8"' not in seed and "cloud_init_printf(file" in seed and
          "network-config" in seed and "macaddress:" in seed and
          "build_ubuntu_seed(nat_mac, server_mac, server_ip)" in impl,
          "NoCloud seed has a BOM or lacks a MAC-bound static bootstrap network")
    for token in ("server_core", "windows_image_name", "product_key"):
        check(token in impl + header, f"Server Core provisioning missing {token}")

    check("data_vhdx_path" in hcs_header and "data_vhdx" in hcs,
          "HCS document cannot attach the appliance data disk")
    check("default_endpoint_clsid" in hcs and 'L"{%s}"' in hcs and
          "hcn_get_endpoint_mac(&default_endpoint_id" in hcs,
          "HCS bootstrap MAC lookup must accept the bare endpoint UUID from HCN")
    check("hcn_create_share_server_endpoint" in hcn and ".2" in hcn,
          "isolated HCN network has no fixed appliance endpoint")
    host_reconcile = impl[impl.index("HRESULT shared_appliance_reconcile_host_resources"):
                          impl.index("HRESULT shared_appliance_purge_resource")]
    check(host_reconcile.index("shared_appliance_reconcile();") <
          host_reconcile.index("host_mapping_command(resource, TRUE)"),
          "host mounts are attempted before publishing a newly created share")
    purge = impl[impl.index("HRESULT shared_appliance_purge_resource"):
                 impl.index("HRESULT shared_appliance_unpublish_resource")]
    check("host_mounts" not in purge and
          "shared_appliance_unmount_host_resource(snapshot.id)" in purge,
          "purge must only unmount its own resource; other host drives are always mounted")
    check("asb_request_shared_resource_sync();" in
          core[core.index("ASB_API HRESULT asb_shared_resource_purge"):],
          "purged resources must be republished and remapped")
    check("include = /etc/samba/appsandbox-shares.conf" in linux_agent and
          "appliance_reload_shares" in linux_agent and "sed -i" in linux_agent,
          "Samba does not expand include globs; the agent must aggregate share files")
    check("smbcontrol smbd close-share" in linux_agent,
          "changed share settings (read only) must drop existing tree connections")
    check("inet_pton(AF_INET, separator" in linux_agent and "copy[i] = ':'" in linux_agent,
          "Linux private NIC configuration must normalize the HCN MAC format")
    check("RemoteAddresses\\\":\\\"%S.2/32" in hcn,
          "client SMB ACL does not target the appliance")
    check("shared_appliance_prepare_client" in core and
          "shared_appliance_release_client" in core,
          "normal VM lifecycle does not depend on the appliance")

    for command in ("appliance_ready", "appliance_reconcile:",
                    "appliance_remove:", "appliance_purge:", "appliance_grow:"):
        check(command in win_agent, f"Windows appliance agent missing {command}")
        check(command in linux_agent, f"Linux appliance agent missing {command}")
    for token in ("New-SelfSignedCertificate", "WSMan:\\\\localhost\\\\Listener",
                  "ASB_MANAGEMENT_HOST", "management-cert.thumbprint"):
        check(token in win_agent, f"Server Core management provisioning missing {token}")

    for token in ("host_drive_letter", "legacy_host_path", "storage_kind"):
        check(token in resource_header + resources,
              f"appliance resource persistence missing {token}")
    check("normalize_folder" not in resources and "set_vm_group_acl" not in resources,
          "resource definitions still mutate arbitrary host folders")

    for token in ("shared-appliance-card", "appliance-backend", "appliance-storage-parent",
                  "appliance-data-size", "appliance-admin-user", "appliance-admin-pass"):
        check(token in html, f"Settings appliance UI missing {token}")
    for token in ("setupSharedAppliance", "startSharedAppliance", "stopSharedAppliance",
                  "growSharedAppliance", "rebuildSharedAppliance", "mountHostResource"):
        check(token in js + ui, f"WebView appliance action missing {token}")
    for token in ("sharedDependencyUnavailable", "allowMissingSharedResources",
                  "Start Without Shared Drives"):
        check(token in js + ui, f"GUI dependency bypass missing {token}")

    for route in ("/shared-appliance", "/host-mount", "/host-unmount", "/purge"):
        check(route in headless, f"headless appliance route missing {route}")
    for method in ("shared_appliance", "setup_shared_appliance", "start_shared_appliance",
                   "stop_shared_appliance", "grow_shared_appliance",
                   "rebuild_shared_appliance", "mount_host_resource",
                   "unmount_host_resource", "purge_shared_resource"):
        check(f"def {method}" in sdk, f"Python SDK missing {method}")
    check("host_path_unsupported" in headless,
          "legacy hostPath requests are not rejected clearly")

    check(not re.search(r"(adminPassword|productKey).*json", impl, re.I),
          "appliance secrets appear to be serialized into status JSON")
    print("[ok] invisible shared-storage appliance contracts")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, FileNotFoundError) as exc:
        print(f"[fail] {exc}")
        raise SystemExit(1)
