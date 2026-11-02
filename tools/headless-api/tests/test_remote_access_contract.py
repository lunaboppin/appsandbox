"""Contract: Moonlight streaming and browser access stay opt-in and locked down.

Both are off by default. Sunshine is downloaded and hash-checked in the guest, never
shipped. The relay and browser listeners bind loopback unless the user chose the
network, and the browser gateway checks its key, cookie, Host and Origin. The display
window lets go of the guest's one-client channels during a browser session.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    web = read("src/backend_win/vm_web.c")
    stream = read("src/backend_win/vm_stream.c")
    agent = read("tools/agent/agent.c")
    core = read("src/backend_win/asb_core.c")
    link = read("src/backend_win/vm_agent.c")
    display = read("src/backend_win/vm_display_idd.c")
    headless = read("src/app_win/headless.c")
    ui = read("src/app_win/ui.c")
    js = read("web/app.js")
    html = read("web/index.html")
    page = read("web/remote.js")
    proj = read("AppSandboxCore.vcxproj")

    for name in ("vm_stream", "vm_web", "vm_web_encoder"):
        assert f"src\\backend_win\\{name}.c" in proj and f"src\\backend_win\\{name}.h" in proj

    # Sunshine and ViGEmBus: official URL, pinned hash, checked before use, never bundled.
    assert re.search(r'STREAM_SUNSHINE_SHA\s+"[0-9A-Fa-f]{64}"', agent)
    assert re.search(r'STREAM_VIGEM_SHA\s+"[0-9A-Fa-f]{64}"', agent)
    assert "github.com/LizardByte/Sunshine/releases/download/" in agent
    assert "failed its hash check" in agent
    assert "Remove-Item $f -Force" in agent

    # Setup needs the network: it starts after the guest's network is configured,
    # and a download waits out network errors rather than failing the setup.
    assert link.index("!nat_configured && !configure_guest_nat") < link.index("vm_stream_setup_command(vm, stream_cmd")
    assert "catch [Net.WebException]" in agent
    assert "Status 'network'" in agent
    # A failed setup's message reaches the host as one line; an empty one reads as a disconnect.
    assert 'strcspn(msg, "\\r\\n")' in agent
    assert "Sunshine" in read("THIRD-PARTY-NOTICES.md") and "Not bundled" in read("THIRD-PARTY-NOTICES.md")
    assert not list((ROOT / "tools").rglob("sunshine*.msi"))

    # Guest side: no UPnP, web UI kept to the guest, firewall only for the relay or subnet.
    assert "'upnp = disabled'" in agent and "'origin_web_ui_allowed = pc'" in agent
    assert "-RemoteAddress $allowed" in agent
    assert "if($encoder -eq 'software')" in agent

    # Streaming persists only when on, and the relay stops wherever SSH's proxy does.
    assert 'L"StreamEnabled=1\\n"' in core and "if (g_vms[i].stream_enabled)" in core
    assert core.count("vm_stream_relay_stop(") == core.count("vm_ssh_proxy_stop(")
    assert "vm->stream_enabled && !vm_stream_unsupported_reason(vm)" in link
    assert '"stream_pin:"' in link   # the PIN stays out of the log

    # Relay: a pairing reply and its close arrive together, and it closes without a reset.
    assert "recv(guest, &one, 1, MSG_PEEK) == 0" in stream and "shutdown(c->client, SD_SEND)" in stream
    # Relay: loopback unless network; the firewall rule stays off public networks.
    assert "ASB_STREAM_NETWORK" in stream and "INADDR_LOOPBACK" in stream
    assert "profile=private,domain" in stream and "profile=private,domain" in web
    assert re.search(r"lab_mode\) return L\"Malware lab", stream)
    assert re.search(r"lab_mode\) return L\"Malware lab", web)

    # Gateway: not persisted, loopback first, a TLS-only network listener.
    assert "BCryptGenRandom" in web and "random_hex(key, 64)" in web
    assert "htonl(INADDR_LOOPBACK), WEB_PORT_FIRST" in web
    assert "SP_PROT_TLS1_2_SERVER" in web and "SCH_USE_STRONG_CRYPTO" in web
    assert "HttpOnly; SameSite=Strict" in web and '"; Secure"' in web
    assert "SameSite=None" not in web
    for check in ("host_allowed(host, c->tls)", "origin_allowed(headers, host, c->tls)",
                  "session_cookie_ok(headers, vm_id, c->tls)", "login_allowed(ip)"):
        assert check in web, check
    assert "(!c->tls || entry->lan)" in web   # a loopback-only VM refuses the network port
    assert "a browser must mask" in web
    assert "Set-Cookie" in web and "key" not in re.search(r'sprintf_s\(cookie.*?\);', web, re.S).group(0).replace("Cookie", "")
    # The gateway only serves its own files; never the host clipboard.
    assert "OpenClipboard" not in web and "SetClipboardData" not in web
    assert 'strcmp(path, "/remote.js")' in web and 'L"remote.html"' in web
    assert "frame-ancestors 'none'" in web and "connect-src 'self'" in web
    # Off and gone at exit, firewall rule included.
    assert core.count("vm_web_shutdown();") == 2
    assert "close_lan_listener();" in web

    # The page takes the key from the fragment and wipes it.
    assert "location.hash.match" in page and "history.replaceState" in page
    assert "sessionStorage" not in page
    assert set(re.findall(r"localStorage\.\w+\(([^,)]+)", page)) == {"'asb-muted'"}   # never the key

    # A session's memory outlives every thread that reads guest channels.
    assert web.count("t_stop = &ws->stop;") == 4
    assert "if (stuck) {" in web
    # This Sunshine release wants the waiting device's id with the PIN.
    assert "pairing_id=$waiting[0].id" in agent
    # The driver hears every stream's size, even a repeat of the last.
    assert 'RegDeleteValueW(key, L"StreamMode")' in read("tools/agent/appsandbox-displays.c")

    # The display window yields the one-client channels.
    assert display.count("web_session_active") >= 3
    assert "WM_IDD_DROP_CLIPBOARD" in display
    assert 'web_session_active' in read("src/backend_win/hcs_vm.h")

    # API and UI: nobody exposes a VM without saying so.
    for route in ('L"stream"', 'L"stream/pair"', 'L"stream/unpair"', 'L"web"'):
        assert route in headless, route
    assert '\\"streaming\\":true,\\"browserAccess\\":true' in headless
    assert 'id="manage-remote-section"' in html and 'id="manage-web-reach"' in html and 'id="manage-stream-reach"' in html
    assert "Allow other devices" in js and "remoteSetWeb" in js and "remoteSetStream" in js
    assert "NVIDIA encoding" in html   # the GPU-PV host crash reports are stated, opt-in
    assert 'wcsncmp(url, L"http://127.0.0.1:", 17)' in ui   # opens only our own page
    print("Remote access contract verified.")


if __name__ == "__main__":
    main()
