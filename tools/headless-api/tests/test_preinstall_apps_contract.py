"""Contract: apps chosen at creation are installed in a new Windows VM.

The create dialog offers a checklist of winget packages plus free-form IDs.
The VM keeps the list (PreinstallApps= in vms.cfg) until the guest agent has
queued it; the agent installs them with winget in the signed-in user's
session through the user's elevated linked token, waits for winget to
register on a new account, and retries failures for a few service starts.
IDs are restricted to winget's character set at every hop.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    js = read("web/app.js")
    html = read("web/index.html")
    ui = read("src/app_win/ui.c")
    core = read("src/backend_win/asb_core.c")
    link = read("src/backend_win/vm_agent.c")
    agent = read("tools/agent/agent.c")

    # UI: checklist and validated extra IDs (test_linux_parity_contract covers Ubuntu).
    assert 'id="create-app-list"' in html and 'id="create-app-extra"' in html
    for app in ("Anthropic.Claude", "Valve.Steam", "Notepad++.Notepad++",
                "Microsoft.VisualStudioCode", "voidtools.Everything"):
        assert "id: '%s'" % app in js, app
    assert "var WINGET_ID_RE = /^[A-Za-z0-9.+_-]{1,100}$/;" in js
    assert "preinstallApps: gatherPreinstallApps()," in js
    assert "if (cfg.preinstallApps === null) return;" in js
    assert 'json_get_string(json, L"preinstallApps", apps_buf, _countof(apps_buf));' in ui

    # Host: kept per VM (not templates) until the agent queued it.
    assert 'fwprintf(f, L"PreinstallApps=%s\\n", g_vms[i].preinstall_apps);' in core
    assert 'wcsncmp(line, L"PreinstallApps=", 15) == 0' in core
    assert "config->preinstall_apps && !is_template_create" in core
    assert "static void preinstall_apps_normalize(" in core
    send = link[link.index("if (vm->preinstall_apps[0] && !vm->is_template) {"):]
    send = send[:send.index("\n        }\n")]
    assert '"install_apps:%S"' in send
    assert send.index('strcmp(buf, "ok") == 0') < send.index("vm->preinstall_apps[0] = L'\\0';")

    # Agent: validated queue, user-session elevated winget, retries.
    assert 'strncmp(cmd, "install_apps:", 13) == 0' in agent
    assert "static BOOL valid_winget_id(" in agent
    worker = agent[agent.index("static DWORD WINAPI apps_install_thread("):]
    worker = worker[:worker.index("\nstatic void start_app_install")]
    assert "GetTokenInformation(user, TokenLinkedToken" in agent
    assert "CreateProcessAsUserW(token" in worker
    assert "Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe" in worker
    assert "--accept-package-agreements --accept-source-agreements --disable-interactivity" in worker
    assert "runs >= APPS_MAX_RUNS" in worker
    start = agent[agent.index("    ensure_fallback_audio();\n"):]
    assert "start_app_install();" in start[:400], "pending apps resume at service start"
    print("[ok] preinstall apps on new Windows VMs contract")


if __name__ == "__main__":
    main()
