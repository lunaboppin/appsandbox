"""Contract: a Windows VM can be started without Secure Boot.

Secure Boot makes Windows ignore test signing, so a test-signed kernel driver
(the audio driver) is blocked as unsigned. New Windows VMs start with it off
from their first boot; Manage > Secure Boot changes it per VM: vms.cfg keeps SecureBootOff=1, the next start leaves the Secure Boot
template out of the firmware config, and the UI asks before turning it off
because an install set up with Secure Boot on may not boot without it.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    hcs = read("src/backend_win/hcs_vm.c")
    core = read("src/backend_win/asb_core.c")
    ui = read("src/app_win/ui.c")
    js = read("web/app.js")
    html = read("web/index.html")

    # Firmware: no template when the VM opted out; otherwise Windows keeps it.
    sb = hcs[hcs.index("secureboot_section[0] = L'\\0';"):]
    sb = sb[:sb.index("/* VMGS + VMRS")]
    assert "} else if (config->secure_boot_off) {" in sb
    assert sb.index("config->secure_boot_off") < sb.index("ApplySecureBootTemplate")

    # Persisted per VM and carried into the start config.
    assert 'fwprintf(f, L"SecureBootOff=1\\n");' in core
    assert 'wcsncmp(line, L"SecureBootOff=", 14) == 0' in core
    assert "args->config.secure_boot_off = inst->secure_boot_off;" in core
    # New Windows VMs (every create path) start with it off from the first boot.
    assert 'cfg.secure_boot_off = (_wcsicmp(cfg.os_type, L"Windows") == 0);' in core
    assert "inst->secure_boot_off = cfg.secure_boot_off;" in core
    assert hcs.count("instance->secure_boot_off = config->secure_boot_off;") == 2
    setter = core[core.index("ASB_API HRESULT asb_vm_set_secure_boot("):]
    setter = setter[:setter.index("\n}\n")]
    assert '_wcsicmp(inst->os_type, L"Windows") != 0' in setter, "Windows VMs only"

    # UI: reported, settable, Windows-only, and confirmed before turning off.
    assert 'jb_bool(jb, L"secureBoot", !v->secure_boot_off);' in ui
    assert 'wcscmp(action, L"setVmSecureBoot") == 0' in ui
    assert 'id="manage-secure-boot"' in html
    assert "vm.osType === 'Windows' ? '' : 'none'" in js
    save = js[js.index("function saveManageSecureBoot()"):]
    save = save[:save.index("\n}\n")]
    assert "showModal('Turn off Secure Boot'" in save
    assert save.index("showModal") < save.index("send(false)")
    print("[ok] per-VM Secure Boot setting contract")


if __name__ == "__main__":
    main()
