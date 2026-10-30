"""Contract: the IDD display plays games (Parsec-style input, 60 fps frames).

Game mode captures the mouse when the guest hides its cursor and the user
clicks: the host cursor is hidden and clipped to the window, and raw mouse
deltas go to the guest as relative moves, which games reading raw input need.
Ctrl+Alt or losing activation releases it, as does the guest showing its
cursor again. The guest driver sends each frame in one packed send rather
than a send per row, and a changed driver reaches existing VMs: the host
stages it beside the agent, which installs it at its next start.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    idd = read("src/backend_win/vm_display_idd.c")
    guest = read("tools/agent/appsandbox-input.c")
    vdd = read("tools/vdd/vdd.cpp")
    agent = read("tools/agent/agent.c")
    core = read("src/backend_win/asb_core.c")

    # Host: capture, relative moves from raw input, release paths
    begin = idd[idd.index("static void idd_begin_capture("):]
    begin = begin[:begin.index("\n}\n")]
    assert "RegisterRawInputDevices(&rid, 1, sizeof(rid))" in begin and "ClipCursor(&rc)" in idd
    assert "send_input(d, INPUT_MOUSE_REL, (UINT32)ri.data.mouse.lLastX," in idd
    assert "if (d && d->captured) return 0;   /* relative moves come from WM_INPUT */" in idd
    assert "d->game_mode && !d->cursor_visible" in idd, "a click with the cursor hidden captures"
    assert "if (d && wp && d->captured && d->capture_auto)" in idd, "a shown cursor releases"
    assert "idd_end_capture(d);\n                idd_flush_held_keys(d);" in idd, "deactivation releases"
    assert "(GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000)" in idd
    assert "INPUT_BTN_X2" in idd and "WM_MOUSEHWHEEL" in idd
    assert '"gameMode\\":%d' in idd

    # Guest: relative moves, X buttons, horizontal wheel
    assert "inp.mi.dwFlags = MOUSEEVENTF_MOVE;" in guest
    assert "MOUSEEVENTF_XDOWN" in guest and "MOUSEEVENTF_HWHEEL" in guest
    # Absolute moves land on the AppSandbox monitor even when another display
    # (e.g. Hyper-V basic video) is primary or sits to its left.
    assert 'wcsstr(dd.DeviceString, L"App Sandbox Virtual Display")' in guest
    assert "MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK" in guest
    assert "x += g_vdd_rect.left;" in guest
    # ...and other monitors (e.g. a GPU-PV adapter's own output) are switched
    # off so the AppSandbox display is the only, main one.
    disp = read("tools/agent/appsandbox-displays.c")
    assert 'strcmp(argv[1], "--only-appsandbox") == 0' in disp
    assert "paths[i].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;" in disp
    assert "SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG" in disp
    assert agent.count("keep_only_appsandbox_display();") == 2

    # Driver: one packed send per frame
    assert "static int VddPackFrame(" in vdd
    assert "VddSendAll(proc->sendConn, (const char*)proc->pSendBufOut, proc->sendLen)" in vdd
    assert "mapped.pData + row * mapped.RowPitch),\n" not in vdd, "no per-row sends"

    # Driver updates reach existing VMs
    assert "apply_staged_vdd_update();" in agent and "vdd-update.pending" in agent
    # ...on this boot: the running device is disabled around the update, so
    # its binary isn't in use and isn't left for the next reboot.
    upd = agent[agent.index("static void apply_staged_driver_update("):]
    upd = upd[:upd.index("\n}\n")]
    assert upd.index('L"disable %s"') < upd.index('run_devcon_inf(L"update"') < upd.index('L"enable %s"')
    # ...also when no marker is left but the staged build isn't the installed one.
    assert "if (!pending && strcmp(stamp, installed) == 0) return;" in upd
    assert 'L"AppSandboxVDD.dll", L"vdd-installed.stamp"' in agent
    # Runs from the driver store, so an update never overwrites a loaded DLL.
    inf = read("tools/vdd/AppSandboxVDD.inf")
    assert "ServiceBinary = %13%\\AppSandboxVDD.dll" in inf
    assert "UMDriverCopy = 13" in inf and "%12%\\UMDF\\AppSandboxVDD.dll" not in inf.split("ServiceBinary")[1]
    assert "refresh_guest_display_driver(drive, source_dir);" in core
    assert "guest_payload_current(vm->vhdx_path, stamp)" in core
    # Drivers are built beside the app, not in resources\ with the helpers.
    assert "static void guest_drivers_dir(" in core
    # Any changed file of a package (an INF-only change too) restages it.
    refresh = core[core.index("static void refresh_guest_driver("):]
    refresh = refresh[:refresh.index("\n}\n")]
    assert "memcmp(a, b, 32) != 0) break;" in refresh and "if (!files[i]) return;" in refresh
    assert 'L"*AppSandboxVDD.inf", L"*AppSandboxVAD.inf"' in core
    assert "guest_drivers_dir(source_dir, drivers_dir, MAX_PATH);" in core
    assert "guest_drivers_dir(dir, drivers_dir, MAX_PATH);" in core
    print("[ok] IDD game mode (relative mouse capture, packed frames) contract")


if __name__ == "__main__":
    main()
