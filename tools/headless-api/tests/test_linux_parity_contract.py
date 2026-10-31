"""Contract: Ubuntu guests get what Windows guests get, where it can work.

Guest source ships with the app rather than coming from GitHub, and the package
index covers the update pockets, without which a point-release ISO can't install
its build tools at all. Shared drives, apps and the helper refresh use the same
agent commands as Windows; the display follows the window through ASRZ.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    core = read("src/backend_win/asb_core.c")
    link = read("src/backend_win/vm_agent.c")
    shared = read("src/backend_win/shared_resources.c")
    appliance = read("src/backend_win/shared_appliance.c")
    agent = read("tools/linux/agent/appsandbox-agent.c")
    display = read("tools/linux/agent/appsandbox-display.c")
    drm = read("tools/linux/asb_drm/asb_drm_drv.c")
    deps = read("tools/iso-patch/prefetch_build_deps.c")
    repo = read("tools/iso-patch/prefetch_repo.c")
    firstboot = read("tools/iso-patch/ubuntu_vhdx.c")
    js = read("web/app.js")
    html = read("web/index.html")
    proj = read("AppSandbox.vcxproj")

    # Build: guests compile the source this app shipped, from packages that match.
    assert '--local-root' in core and "guest-src" in proj
    assert proj.count("normalize-eol.ps1") == 4, "CRLF breaks dkms.conf and scripts in the guest"
    assert "lunaboppin/appsandbox" in repo and "jamesstringer90/appsandbox/tar.gz" not in repo
    assert 'L"appsandbox-guest-update.sh"' in repo
    assert 'L"security", L"updates"' in deps
    assert '"cifs-utils"' in deps and "pkg-config cifs-utils" in firstboot
    assert "TimeoutStartSec=1800" in firstboot

    # Shared drives: attached for every OS, from the first boot, mounted with a
    # root-only credentials file.
    assert 'if (!os_type) return 0;' in shared
    create = core[core.index("static DWORD WINAPI linux_create_thread("):]
    create = create[:create.index("HANDLE worker=CreateThread(NULL,0,linux_create_thread")]
    assert "prepare_shared_transport(&args->config, temp_inst," in create
    assert "inst->share_endpoint_id = heap_inst->share_endpoint_id;" in create
    assert 'config->os_type, L"Windows"' not in appliance
    assert 'strncmp(cmd, "shared_smb_map:", 15) == 0' in agent
    assert 'strncmp(cmd, "shared_smb_unmap:", 17) == 0' in agent
    assert "credentials=%s" in agent and "fchmod(fd, 0600)" in agent
    assert 'agent_log("cmd: shared_smb_map:<redacted>")' in agent
    assert "unmanaged-devices=mac:" in agent
    assert 'macaddress: ' in agent, "the static address must not land on the share adapter"

    # Apps: same host path as Windows, apt then snap in the guest, retried.
    assert 'config->preinstall_apps && !is_template_create)' in core
    assert 'strncmp(cmd, "install_apps:", 13) == 0' in agent
    assert "apt-get install -y %s" in agent and "snap install %s" in agent
    assert "APPS_MAX_ATTEMPTS" in agent and "static int apps_valid_name(" in agent
    assert "var PREINSTALL_APPS_LINUX" in js

    # Refresh: a stamp from the shipped source, staged at create, compared at connect.
    assert 'L"/etc/appsandbox-guest-stamp"' in core
    assert "static BOOL linux_guest_refresh(" in link
    assert '"guest_stamp"' in link and "guest_update:%s:%lld" in link
    assert '_wcsicmp(vm->os_type, L"Linux") == 0 && !vm->is_appliance' in link
    assert 'strcmp(cmd, "guest_stamp") == 0' in agent
    assert "appsandbox-guest-update.sh" in agent
    update = read("tools/linux/agent/appsandbox-guest-update.sh")
    assert update.index('echo "$STAMP"') < update.index("systemctl restart appsandbox-agent")

    # First sign-in: no Ubuntu welcome wizard, on new VMs and refreshed ones.
    assert "gnome-initial-setup-upgrade-login.service" in firstboot
    assert "gnome-initial-setup-upgrade-login.service" in update

    # Display: the host's ASRZ reaches asb_drm's single published mode.
    assert "RESIZE_MAGIC" in display and "resize_poll(client_fd, &resize);" in display
    assert "DEVICE_ATTR_RW(preferred_mode)" in drm

    # UI: drives and apps offered on Ubuntu, and a reason where a feature isn't.
    assert 'id="manage-linux-note-section"' in html
    assert '<label>Shared Drives:</label>' in html
    assert "manage-linux-note-section').style.display = vm.osType === 'Linux'" in js
    print("[ok] Linux guest parity contract")


if __name__ == "__main__":
    main()
