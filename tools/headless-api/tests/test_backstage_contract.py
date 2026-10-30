"""Contract: Backstage session, a SYSTEM shell on its own desktop in a VM.

Like ScreenConnect's Backstage. "Backstage Session (SYSTEM)" in the display
window's title-bar menu sends the agent "backstage", which toggles it. The
agent starts appsandbox-backstage.exe as SYSTEM in the console session; that
creates the AppSandboxBackstage desktop, starts itself on it (--shell) and
switches the screen there, then holds the running mutex. The shell has a
taskbar (tools, open windows, Exit) and an Explorer file view, and on leaving
only switches back while its own desktop is still showing, so it never skips
the lock screen. Existing VMs get the helper added at their next start.
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
    core = read("src/backend_win/asb_core.c")
    disk = read("src/backend_win/disk_util.c")
    idd = read("src/backend_win/vm_display_idd.c")

    # Shell: own desktop, SYSTEM tools, Explorer view, safe return.
    for name in ('L"AppSandboxBackstage"', 'L"Global\\\\AppSandboxBackstageExit"',
                 'L"Global\\\\AppSandboxBackstageRunning"'):
        assert name in shell, name
    assert "CreateDesktopW(BACKSTAGE_DESKTOP" in shell and 'L"\\"%s\\" --shell"' in shell
    assert "si.lpDesktop = desktop;" in shell
    assert "SwitchDesktop(g_desk)" in shell
    assert "_wcsicmp(now, BACKSTAGE_DESKTOP) != 0" in shell
    assert "CLSID_ExplorerBrowser" in shell and "EBO_SHOWFRAMES" in shell
    assert "IShellView_TranslateAccelerator" in shell
    assert "EnumDesktopWindows(g_desk" in shell
    for tool in ('L"cmd.exe"', 'L"powershell.exe"', 'L"taskmgr.exe"', 'L"regedit.exe"', 'L"services.msc"'):
        assert tool in shell, tool
    assert "<SubSystem>Windows</SubSystem>" in proj and '<ClCompile Include="appsandbox-backstage.c" />' in proj
    assert sln.count("{F6A7B8C9-D0E1-2345-F012-6789012345AB}") >= 4

    # Agent: toggle, SYSTEM in the console session, waits for the mutex.
    assert 'strcmp(cmd, "backstage") == 0' in agent
    assert "static void backstage_toggle(" in agent
    assert 'strcpy_s(reply, cap, "ok:left");' in agent and 'strcpy_s(reply, cap, "ok:entered");' in agent
    assert agent.count("BACKSTAGE_RUNNING_MUTEX") >= 3

    # Host: staged into new VMs, added to existing ones, menu item.
    assert 'L"+appsandbox-backstage.exe"' in core
    assert "BOOL add = helpers[i][0] == L'+';" in core
    assert 'L"appsandbox-backstage.exe", L"*AppSandboxVDD.dll"' in core
    assert disk.count("appsandbox-backstage.exe") >= 6
    assert "#define IDM_BACKSTAGE" in idd and 'vm_agent_send(ask->vm, "backstage"' in idd
    # The switch uses a handle opened with switch rights; failures say why.
    assert "g_desk = OpenDesktopW(BACKSTAGE_DESKTOP, 0, FALSE, GENERIC_ALL);" in shell
    assert "GetExitCodeProcess(pi.hProcess, &code)" in agent and "backstage.log" in agent
    assert "WM_IDD_BACKSTAGE_DONE" in idd

    # Floating menu: a tab on the top edge opens a panel with every option,
    # sent as the title-bar menu's own commands; it hides while captured.
    assert "static void idd_fab_create(VmDisplayIdd *d)" in idd and "idd_fab_create(d);" in idd
    assert "WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED" in idd
    assert "PostMessageW(d->hwnd, WM_SYSCOMMAND, cmd, 0);" in idd
    for cmd in ("IDM_CAPTURE_MOUSE", "IDM_GAME_MODE", "IDM_XMIT_HOTKEYS", "IDM_AUDIO_MUTE",
                "IDM_BACKSTAGE", "IDM_SHOW_CONSOLE", "IDM_SHOW_LOG"):
        assert "rows[n].cmd = %s;" % cmd in idd, cmd
    assert "want = !d->captured && IsWindowVisible(d->hwnd) && !IsIconic(d->hwnd);" in idd
    print("[ok] backstage session and floating menu contract")


if __name__ == "__main__":
    main()
