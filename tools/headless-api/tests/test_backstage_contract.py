"""Contract: Backstage session (hidden SYSTEM session) and the floating menu.

Like ScreenConnect's Backstage: the agent starts appsandbox-backstage.exe as
SYSTEM in session 0 on WinSta0\\Default, which the VM's own screen never
shows, so the signed-in user and the sign-in screen never see it and
everything it opens runs there too. The shell (taskbar, tools, Explorer
file view) streams its screen to the host over ASB_CH_BACKSTAGE (8) as
changed bands of BGRA rows, and injects the host's mouse and keyboard
(posted as window messages; session 0 has no input devices). The host's
Backstage window (vm_backstage.c) starts the session through the agent,
connects to :0008, draws it and sends input; it opens from the display
window's floating menu or title-bar menu.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    shell = read("tools/agent/appsandbox-backstage.c")
    proj = read("tools/agent/appsandbox-backstage.vcxproj")
    sln = read("AppSandbox.sln")
    agent = read("tools/agent/agent.c")
    transport = read("tools/transport/asb_transport.h")
    core = read("src/backend_win/asb_core.c")
    disk = read("src/backend_win/disk_util.c")
    hcs = read("src/backend_win/hcs_vm.c")
    idd = read("src/backend_win/vm_display_idd.c")
    view = read("src/backend_win/vm_backstage.c")
    app_proj = read("AppSandbox.vcxproj")

    # Guest shell: session 0, no desktop switching, streams and injects.
    # The only desktop switch is session 0 choosing its own input desktop
    # (so the pointer can be placed and windows can take the foreground).
    assert shell.count("SwitchDesktop(") == 1 and "if (g_session != 0) return;" in shell
    assert "#define ASB_CH_BACKSTAGE        8" in transport
    assert "asb_listen(ASB_CH_BACKSTAGE)" in shell
    assert "BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY | CAPTUREBLT);" in shell
    assert "memcmp(cur + off, prev + off, bytes) == 0" in shell
    # Session 0's screen copies as black, so windows paint themselves.
    assert "g_capture_mode = frame_is_black(cur, w, h) ? 2 : 1;" in shell
    assert "PrintWindow(vw.list[i], dc, PW_RENDERFULLCONTENT)" in shell
    assert "EnumDesktopWindows(g_desk, collect_visible" in shell
    assert 'view_status(v, L"Connected; waiting for the picture...");' in view
    # Session 0 has no input devices: everything is posted as window messages,
    # with the target thread's key state shared so Shift/Ctrl/Alt count.
    assert "SendInput(" not in shell
    assert "AttachThreadInput(GetCurrentThreadId(), tid, TRUE)" in shell and "SetKeyboardState(g_keys);" in shell
    assert "WM_NCHITTEST" in shell and "WM_LBUTTONDBLCLK" in shell and "SC_CLOSE" in shell
    assert "PostMessageW(w, up ? (sys ? WM_SYSKEYUP : WM_KEYUP) : (sys ? WM_SYSKEYDOWN : WM_KEYDOWN)" in shell
    # Consoles ignore key messages: typing is written as console input.
    assert "WriteConsoleInputW(g_conin, &r, 1, &n);" in shell and "AttachConsole(pid)" in shell
    assert "GenerateConsoleCtrlEvent(" in shell and "DelegationConsole" in shell
    # Exit Backstage closes the host window instead of starting a new session.
    assert "end.magic = BS_END_MAGIC;" in shell and "if (hdr.magic == BS_END_MAGIC)" in view
    assert "if (v->ended && !v->closing)" in view
    assert "SHGetKnownFolderPath(ids[i], KF_FLAG_CREATE" in shell   # SYSTEM Desktop exists
    assert "CLSID_ExplorerBrowser" in shell and "EnumDesktopWindows(g_desk" in shell
    for tool in ('L"cmd.exe"', 'L"powershell.exe"', 'L"taskmgr.exe"', 'L"regedit.exe"', 'L"services.msc"'):
        assert tool in shell, tool
    assert "<SubSystem>Windows</SubSystem>" in proj and "asb_transport.c" in proj
    assert sln.count("{F6A7B8C9-D0E1-2345-F012-6789012345AB}") >= 4

    # Agent: starts it in session 0 on the interactive window station.
    start = agent[agent.index("static void backstage_start("):]
    start = start[:start.index("\n}\n")]
    assert 'si.lpDesktop = L"WinSta0\\\\Default";' in start
    assert "SetTokenInformation" not in start and "CreateProcessW(exe_path" in start
    assert 'strcmp(cmd, "backstage") == 0 || strcmp(cmd, "backstage_stop") == 0' in agent

    # Host: channel 8 allowed, helper staged, viewer window, menus.
    assert "for (port = 1; port <= 9; port++)" in hcs
    assert 'L"+appsandbox-backstage.exe"' in core and disk.count("appsandbox-backstage.exe") >= 6
    assert "hcs_service_guid(v->os_type, BS_CHANNEL, &addr.ServiceId);" in view
    assert 'vm_agent_send(v->vm, "backstage", reply' in view
    assert "StretchDIBits(" in view and "bs_send_input(v, BS_IN_KEY" in view
    assert '<ClCompile Include="src\\backend_win\\vm_backstage.c" />' in app_proj
    assert "vm_backstage_open(d->vm, d->hInstance);" in idd

    # Floating menu: a tab on the top edge opens a panel with every option.
    assert "static void idd_fab_create(VmDisplayIdd *d)" in idd and "idd_fab_create(d);" in idd
    assert "WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED" in idd
    assert "PostMessageW(d->hwnd, WM_SYSCOMMAND, cmd, 0);" in idd
    for cmd in ("IDM_CAPTURE_MOUSE", "IDM_GAME_MODE", "IDM_XMIT_HOTKEYS", "IDM_AUDIO_MUTE",
                "IDM_BACKSTAGE", "IDM_SHOW_CONSOLE", "IDM_SHOW_LOG"):
        assert "rows[n].cmd = %s;" % cmd in idd, cmd
    print("[ok] backstage session and floating menu contract")


if __name__ == "__main__":
    main()
