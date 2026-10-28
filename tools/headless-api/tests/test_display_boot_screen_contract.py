"""Offline contract checks for the boot-screen display (Hyper-V console while a
VM boots, then a switch of the same window to the IDD stream)."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    auto = read("src/backend_win/vm_display_auto.c")
    con = read("src/backend_win/vm_display.c")
    idd = read("src/backend_win/vm_display_idd.c")
    ui = read("src/app_win/ui.c")
    headless = read("src/app_win/headless.c")
    proj = read("AppSandbox.vcxproj")
    readme = read("tools/headless-api/README.md")
    client = read("tools/headless-api/asb.py")

    check("vm_display_auto.c" in proj, "vm_display_auto.c is not compiled into AppSandbox.exe")

    # The console is vmwp.exe's BasicSession pipe; readiness is the passive latched
    # flag, never a frame-channel probe.
    check(".BasicSession" in auto, "boot screen does not use the Hyper-V BasicSession pipe")
    check("asb_vm_idd_ready" in auto, "auto display does not gate the IDD switch on asb_vm_idd_ready")
    check("vm_display_idd_create_switchable" in auto and "vm_display_create_switchable" in auto,
          "auto display does not create switchable windows")
    check("vm_display_get_pos" in auto and "vm_display_idd_get_pos" in auto,
          "a swap does not keep the window position")
    check("d->follow && idd_ready(d)" in auto, "console does not follow the boot to IDD")
    # A window whose thread hasn't created its HWND yet must not count as closed.
    check("VDA_OPEN_GRACE_MS" in auto and "window_gone(" in auto,
          "a still-starting window can be mistaken for a user close")

    check("IDC_BTN_SWITCH_IDD" in con and "switch_requested" in con,
          "console window has no manual switch to the VM display")
    check("IDM_SHOW_CONSOLE" in idd and "switch_requested" in idd,
          "IDD window has no manual switch to the Hyper-V screen")

    # GUI: one controller per VM, auto view on the monitor button / tray.
    check("static VmDisplayAuto *g_vm_displays[ASB_MAX_VMS];" in ui, "GUI does not track auto displays")
    check("g_idd_displays" not in ui and "do_connect_idd" not in ui,
          "GUI still opens bare IDD windows for VMs")
    check("do_connect_vm(cmd - TRAY_CMD_CONNECT_BASE, VDA_VIEW_AUTO)" in ui,
          "tray Connect does not open the auto display")
    check('json_get_string(json, L"view", view, 16)' in ui, "connectIddVm ignores the view option")

    # Headless: a booting VM opens on the console; only an explicit idd view is gated.
    post = headless[headless.index('if (wcscmp(sub, L"display") == 0)'):]
    post = post[:post.index("if (verb == HttpVerbDELETE)")]
    check("vm_display_auto_create(v, g_hinst, NULL, view)" in post, "headless does not open the auto display")
    gate = post.index('"display_not_ready"')
    check("view == VDA_VIEW_IDD && !ready" in post[:gate],
          "display_not_ready is not limited to an explicit view=idd request")
    check('\\"displayView\\":%s' in headless, "VM status lacks displayView")
    check('\\"view\\":%s' in headless and '\\"following\\":%s' in headless,
          "GET display does not report view/following")

    check("def open_display(self, name, view=None)" in client, "asb.py open_display has no view option")
    check("Hyper-V console" in readme and "display_not_ready" in readme and '"view"' in readme,
          "README does not document the boot-screen display")

    print("display boot-screen contract: OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
