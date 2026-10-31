"""Contract: roaming files, per-group drives and disposable VMs on the domain.

Roaming files: the DC shares C:\\AppSandboxData\\Users as Users$, gives every
domain user a home folder (H:) and links an "AppSandbox Roaming Files" GPO
that points Desktop, Documents, Downloads and Pictures at it, applies user
policy before the desktop and stops machine password rotation (disposable
VMs roll back to an old one). Prepared once per session from the Domain
dialog and right after promotion; saving a user creates their home.

Per-group drives: a shared resource may name domain groups. The appliance
then admits only them (plus the host's service account); managed VMs map it
per user at sign-in with the user's own credentials, local VMs skip it.

Disposable VMs: turning it on checkpoints the stopped VM; every normal start
boots a new branch of that checkpoint and deletes the last session's branch.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    agent = read("tools/agent/agent.c")
    app = read("src/backend_win/shared_appliance.c")
    res = read("src/backend_win/shared_resources.c")
    link = read("src/backend_win/vm_agent.c")
    core = read("src/backend_win/asb_core.c")
    ui = read("src/app_win/ui.c")
    js = read("web/app.js")
    html = read("web/index.html")

    # Roaming files
    ensure = agent[agent.index("static const char DOMAIN_ENSURE_PS[]"):]
    ensure = ensure[:ensure.index("static const char *const DOMAIN_ENV[]")]
    assert "New-SmbShare -Name 'Users$' -Path $root" in ensure
    assert "$name='AppSandbox Roaming Files'" in ensure and "New-GPLink -Name $name" in ensure
    for v in ("'Desktop'='Desktop'", "'Personal'='Documents'",
              "'{374DE290-123F-4565-9164-39C4925E467B}'='Downloads'", "'My Pictures'='Pictures'"):
        assert v in ensure, v
    assert "$want='%HOMESHARE%\\\\'+$map[$v]" in ensure
    assert "-ValueName 'SyncForegroundPolicy'" in ensure
    assert "-ValueName 'DisablePasswordChange'" in ensure
    assert "Set-ADUser -Identity $sam -HomeDirectory $unc -HomeDrive 'H:'" in agent
    assert agent.count("function AsbHome($sam){") == 2      # ensure + user_set
    assert " try{ AsbHome $n }" in agent
    assert 'strcmp(cmd, "domain_ensure") == 0' in agent
    assert app.count('"domain_ensure"') == 2                  # after promotion + once per session
    assert "if (!g_domain_ensured) {" in app

    # Per-group drives
    assert 'L"Groups=", 7' in res and "out[n].groups" in res
    assert '"appliance_reconcile:%s:%s:%s"' in app
    assert "$who+=$q" in agent and "-%sAccess $who" in agent
    assert "if (r->groups[0]) {" in link and '"user_drives:%s"' in link
    assert 'L"Restricted to domain groups; only managed VMs map it."' in link
    assert "static BOOL set_user_drives(" in agent
    assert "& net.exe use $l ($s+'\\\\'+$p[1]) /persistent:no" in agent
    assert "if (event_type == WTS_SESSION_LOGON)\n                map_user_drives(sid);" in agent.replace("\r\n", "\n")
    assert 'json_get_string(json,L"groups",r.groups,_countof(r.groups));' in ui
    assert 'id="resource-groups"' in html

    # Disposable VMs
    start = core[core.index("ASB_API HRESULT asb_vm_start_ex("):]
    start = start[:start.index("/* Switch to snapshot/base branch before booting */")]
    assert "if (inst->disposable_snap[0] && snap_idx == -1" in start and "asb_disposable_new_session(vm)" in start
    session = core[core.index("HRESULT asb_disposable_new_session("):]
    assert session.index("snapshot_new_branch(tree, inst, node)") < session.index("snapshot_delete_branch(tree, inst, node, i)")
    assert 'asb_snap_take(vm, L"Disposable clean state")' in core
    assert 'fwprintf(f, L"DisposableSnap=%s\\nDisposableBranch=%s\\n",' in core
    assert 'wcscmp(action, L"setVmDisposable") == 0' in ui
    assert 'id="manage-disposable"' in html and "sendCmd('setVmDisposable'" in js
    print("[ok] roaming files, per-group drives and disposable VMs contract")


if __name__ == "__main__":
    main()
