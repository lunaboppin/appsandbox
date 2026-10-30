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
    print("[ok] backstage session contract")


if __name__ == "__main__":
    main()
