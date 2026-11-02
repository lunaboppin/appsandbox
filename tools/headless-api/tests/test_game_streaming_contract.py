"""Contract: Game Streaming stays optional and keeps a domain password contained.

It's off until an admin sets it up, and only for Windows guests. Duplicates are never
saved, and they and their AD accounts go when the session ends. The password stays out
of logs and out of the registry, and the guest deletes it once the user is signed in.
The agent only opens launcher ids its own scan can produce. The dashboard sits behind
the gateway's existing checks.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    gs = read("src/backend_win/game_stream.c")
    core = read("src/backend_win/asb_core.c")
    web = read("src/backend_win/vm_web.c")
    link = read("src/backend_win/vm_agent.c")
    agent = read("tools/agent/agent.c")
    appliance = read("src/backend_win/shared_appliance.c")
    disk = read("src/backend_win/disk_util.c")
    ui = read("src/app_win/ui.c")
    headless = read("src/app_win/headless.c")
    js = read("web/app.js")
    html = read("web/index.html")
    dash = read("web/dash.js")
    proj = read("AppSandboxCore.vcxproj")

    assert "src\\backend_win\\game_stream.c" in proj and "src\\backend_win\\game_stream.h" in proj

    # Optional: its own button, off until set up, needs the domain, and can be turned off.
    assert 'onclick="openGameStreamingModal()"' in html and 'id="gs-overlay"' in html
    assert "static BOOL      g_enabled;" in gs
    assert "BOOL game_stream_has_settings(void) { return g_enabled; }" in gs
    assert "if (!appliance.domain_enabled)" in gs
    assert "game_stream_turn_off" in ui and "/v1/game-streaming/off" in headless
    assert "gameStreaming\\\":true" in headless

    # Windows only.
    assert 'L"Game Streaming needs a Windows VM."' in gs
    assert "v.osType === 'Windows'" in js

    # A duplicate is never saved; its disk is a child of the image; it's deleted when done.
    assert "g_vms[i].building_vhdx || g_vms[i].session_clone" in core
    assert "config.session_clone = TRUE" in gs and "config.base_vhdx = base" in gs
    assert "asb_session_clone_delete(id)" in gs
    assert "shared_appliance_domain_computer_remove(s->vm_name)" in gs
    assert 'strncmp(name, "GS-", 3) != 0' in agent   # nothing else in the domain goes that way
    assert 'wcsncmp(name, L"GS-", 3) != 0' in appliance
    assert "sweep_thread" in gs and "game_stream_shutdown();" in core

    # The image: the source's account stays, and no pairing or password is inherited.
    assert "NetUnjoinDomain($null,$null,$null,0)" in agent
    for leftover in ("sunshine_state.json", "credentials", "webui.txt"):
        assert leftover in agent
    assert "/generalize" in agent
    # A failed refresh leaves the working image in place.
    assert "wcscpy_s(g_image, MAX_PATH, base)" in gs

    # The password: checked on the appliance, encrypted while kept, and gone after use.
    assert "ValidateCredentials" in agent
    assert "CryptProtectMemory" in gs and "GS_PASSWORD_KEEP_MS" in gs
    assert "SecureZeroMemory(start->password" in gs and "SecureZeroMemory(logon" in gs
    # In the guest: an LSA secret, never the registry value, and deleted after sign-in.
    assert "LsaStorePrivateData" in agent
    assert 'RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, L"DefaultPassword")' in agent
    assert 'L"DefaultPassword", REG_SZ' not in agent
    assert 'strcmp(cmd, "session_signed_in") == 0' in agent and "session_autologon_clear();" in agent
    assert 'L"AutoLogonCount"' in agent
    assert "SecureZeroMemory(vm->session_logon" in link
    # The command starts domain_, which both ends keep out of their logs.
    assert '"domain_autologon:%s:%s:%s"' in appliance
    assert '"domain_validate:%s:%s:%s"' in appliance
    assert 'strncmp(command, "domain_", 7) == 0' in link
    assert 'agent_log("Command: %.20s<redacted>", cmd)' in agent
    # The sign-in runs early in boot, so its link to the domain controller is saved for that
    # boot, and the host's network setup waits for it to finish.
    assert 'PrefixLength 24 | Out-Null' in agent[agent.index("SESSION_LINK_PS[]"):agent.index("session_wait_signin(char")]
    assert "IMAGE_STATE_COMPLETE" in agent
    # Setup's own last session has to end before the join restarts the guest.
    assert "InterlockedIncrement(&g_session_logoffs)" in agent
    assert "!InterlockedCompareExchange(&g_session_logoffs, 0, 0)" in agent
    assert "if (signed_in && session_has_shell(session))" in agent
    # The network setup doesn't remove a link that's already right.
    assert "Where-Object {$_.IPAddress -eq $env:ASB_NET_IP})){" in agent
    assert link.index('"session_wait_signin"') < link.index("Configure the independent host-only NIC")
    assert link.index('"session_link:%s"') < link.index("shared_appliance_domain_join_command(vm->share_host_ip")
    # After the join, which resets the automatic sign-in settings.
    assert link.index('strcmp(buf, "ok:joined") == 0') < link.index("vm->session_logon, buf")
    # A duplicate's unattend signs nobody in first.
    assert "if (session) {" in disk and "iso_create_session_resources" in core

    # Launching takes ids, not paths or links, and only ones in the image's list.
    launch = re.search(r"static void session_launch.*?\n}\n", agent, re.S).group(0)
    assert "session_digits(id + 6)" in launch and 'domain_field_ok(id + 5, ":-_", 400)' in launch
    assert '"error:invalid arguments"' in launch
    assert "if (!item_allowed(item))" in gs

    # The dashboard: after the Host and Origin checks, signed in, limited, HTTPS on the network.
    route = web.index('strcmp(path, "/") == 0')
    assert web.index("host_allowed(host, c->tls)") < route
    assert web.index("origin_allowed(headers, host, c->tls)") < route
    assert web.count("g_dashboard && (!c->tls || g_dashboard_lan)") >= 3
    assert "login_allowed(ip)" in web[web.index("static void dashboard_login"):web.index("static void dashboard_serve_catalog_file")]
    assert 'asb_app=%s; Path=/; HttpOnly; SameSite=Strict%s' in web
    assert "game_stream_signed_in(token, c->tls" in web
    assert "return g_dashboard && g_dashboard_lan;" in web
    assert "SameSite=None" not in web
    # A cookie only opens this user's VM, and art and the list only come from the catalog folder.
    assert "game_stream_session_vm(user, name, sizeof(name))" in web
    assert 'strstr(leaf, "..")' in gs
    # The page keeps nothing secret.
    assert not re.search(r"(localStorage|sessionStorage)\.\w+\([^)]*(password|pass|token)", dash, re.I)

    # One Moonlight identity per user: kept for admins only, restored before streaming is
    # set up, saved after a pairing, per-user, and gone with the user or the feature.
    assert 'L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"' in gs
    assert "identity_commands(s->user)" in gs and "config.stream_port = user_stream_port(s->user)" in gs
    assert 'identity_path(user, name, temp, MAX_PATH)' in gs and "identity_save(vm, user)" in gs
    assert link.index("vm->session_identity) {") < link.index("vm_stream_setup_command(vm, stream_cmd")
    assert "if (SUCCEEDED(hr)) game_stream_forget_user(name);" in core
    assert "asb_session_clone_remove_leftover(NULL, root);" in gs     # Turn Off takes identities too
    # A key never reaches a log: the commands are redacted, quiet, and only name three files.
    assert 'strncmp(command, "domain_identity_", 16) == 0' in link
    assert 'strncmp(cmd, "domain_identity_", 16) == 0' in agent
    assert re.search(r'identity_file\(const char \*leaf.*?"state".*?"cert".*?"key"', agent, re.S)
    assert "SecureZeroMemory(vm->session_identity" in link

    # Callers for one VM take turns on its command slot.
    assert "g_conn_send_locks" in link
    print("Game Streaming contract verified.")


if __name__ == "__main__":
    main()
