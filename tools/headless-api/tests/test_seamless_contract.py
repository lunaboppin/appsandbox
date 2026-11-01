"""Contract: seamless mode (each guest window is a host window of its own).

The agent starts appsandbox-seamless.exe in the signed-in user's session, because
Windows.Graphics.Capture does not work as SYSTEM, with the user's full token so
input reaches elevated windows. It captures top-level windows one by one and sends
only what changed on ASB_CH_SEAMLESS (9), which the HCS service table has to list.
The host (vm_seamless.c) draws each in a window of its own, marked with the VM's
colour and name, and falls back to the normal display while the sign-in screen, a
lock or a UAC prompt has the guest's input. Guest data is untrusted: sizes are
checked before use and host windows are never topmost. Windows guests only.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    helper = read("tools/agent/appsandbox-seamless.cpp")
    proto = read("tools/agent/seamless_proto.h")
    proj = read("tools/agent/appsandbox-seamless.vcxproj")
    agent = read("tools/agent/agent.c")
    transport = read("tools/transport/asb_transport.h")
    hcs = read("src/backend_win/hcs_vm.c")
    host = read("src/backend_win/vm_seamless.c")
    core = read("src/backend_win/asb_core.c")
    disk = read("src/backend_win/disk_util.c")
    ui = read("src/app_win/ui.c")
    headless = read("src/app_win/headless.c")
    sdk = read("tools/headless-api/asb.py")
    web = read("web/app.js")
    index = read("web/index.html")
    css = read("web/style.css")
    sln = read("AppSandbox.sln")
    app_proj = read("AppSandbox.vcxproj")

    # The channel exists and the VM's service table carries it.
    assert "#define ASB_CH_SEAMLESS         9" in transport
    assert "port <= 9" in hcs
    assert "asb_listen(ASB_CH_SEAMLESS)" in helper

    # Windows are captured one by one, and only what changed goes over the wire.
    assert "GraphicsCaptureItem" in helper
    assert "PW_RENDERFULLCONTENT" in helper
    assert "SL_G_FRAME" in proto and "send_changes" in helper

    # The sign-in screen, a lock or UAC falls back to the normal display.
    assert "OpenInputDesktop" in helper and "SL_G_SECURE" in helper
    assert "on_secure" in host and "VmDisplayAuto" in host
    assert '"error:no_user"' in agent or "error:no_user" in agent

    # The agent starts it in the user's session; every Windows guest gets it.
    assert '"seamless"' in agent and "seamless_start" in agent
    assert 'L"+appsandbox-seamless.exe"' in core
    assert disk.count("appsandbox-seamless") >= 2
    assert "appsandbox-seamless" in read("tools/sign/stage-package.ps1")
    assert "appsandbox-seamless" in read("tools/installer/test-install.ps1")
    assert "appsandbox-seamless.vcxproj" in sln
    assert "windowsapp.lib" in proj
    assert "vm_seamless.c" in app_proj

    # Marked as the VM's, with a taskbar button each, and never above everything.
    assert "DWMWA_BORDER_COLOR" in host
    assert "give_own_taskbar_entry" in host
    assert "WS_EX_TOPMOST" not in host and "HWND_TOPMOST" not in host

    # Guest-supplied sizes are checked on the host.
    assert "SL_MAX_SIDE" in host
    assert "static void net_frame(" in host
    assert "clean_title" in host

    # Reachable from the app and the API; hidden with the reason where it can't work.
    assert 'L"seamlessVm"' in ui and "seamlessUnavailable" in ui
    assert 'v->lab_mode' in ui and "needs a Windows guest" in ui
    assert "makeIconCell('seamless'" in web and "vm.seamlessUnavailable" in web
    assert 'title="Seamless mode"' in index
    assert ".icon-btn.seamless.hidden" in css
    assert 'L"seamless"' in headless and 'L"seamless/input"' in headless
    assert "seamless_unavailable" in headless and '\\"seamless\\":true' in headless
    for name in ("seamless_open", "seamless_close", "seamless_status", "seamless_input"):
        assert "def %s(" % name in sdk, name

    print("OK")


if __name__ == "__main__":
    main()
