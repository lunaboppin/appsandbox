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

    # Driver: one packed send per frame
    assert "static int VddPackFrame(" in vdd
    assert "VddSendAll(proc->hClientConn, (const char*)proc->pSendBuf, len)" in vdd
    assert "mapped.pData + row * mapped.RowPitch),\n" not in vdd, "no per-row sends"

    # Driver updates reach existing VMs
    assert "apply_staged_vdd_update();" in agent and "vdd-update.pending" in agent
    assert "refresh_guest_display_driver(drive, source_dir);" in core
    assert "guest_payload_current(vm->vhdx_path, stamp)" in core
    # Drivers are built beside the app, not in resources\ with the helpers.
    assert "static void guest_drivers_dir(" in core
    assert "guest_drivers_dir(source_dir, drivers_dir, MAX_PATH);" in core
    assert "guest_drivers_dir(dir, drivers_dir, MAX_PATH);" in core
    print("[ok] IDD game mode (relative mouse capture, packed frames) contract")


if __name__ == "__main__":
    main()
