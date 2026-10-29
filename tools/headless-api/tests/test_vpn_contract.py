"""Contract: WireGuard VPN configs and VPN-routed VMs.

A VPN VM sits on a Private HCN network (no host adapter, no NAT) whose only
way out is the VPN gateway appliance at <base>.2. The gateway runs one
WireGuard interface per config and routes each VM's source IP into its
tunnel; anything unclaimed falls to an unreachable table, and the forward
chain only allows LAN traffic into asbvpn interfaces, so a dropped tunnel
leaves its VMs offline instead of leaking. Config keys are DPAPI-protected
and never leave the host except to the gateway, redacted from logs.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    hcn = read("src/backend_win/hcn_network.c")
    configs = read("src/backend_win/vpn_configs.c")
    gateway = read("src/backend_win/vpn_gateway.c")
    core = read("src/backend_win/asb_core.c")
    vm_agent = read("src/backend_win/vm_agent.c")
    linux_agent = read("tools/linux/agent/appsandbox-agent.c")
    win_agent = read("tools/agent/agent.c")
    disk = read("src/backend_win/disk_util.c")
    ui = read("src/app_win/ui.c")
    app = read("web/app.js")
    html = read("web/index.html")
    project = read("AppSandboxCore.vcxproj")

    # ---- Network: Private, gateway-only ----
    vpn_net = hcn[hcn.index("HRESULT hcn_create_vpn_network("):]
    vpn_net = vpn_net[:vpn_net.index("\n}\n")]
    assert '\\"Type\\":\\"Private\\"' in vpn_net, "VPN network must be Private (no host adapter)"
    assert "Routes" not in vpn_net, "VPN network must not route via the host"
    assert "APPSANDBOX_VPN_GUID" in hcn[hcn.index("void hcn_cleanup_stale_networks"):]

    # ---- Configs: keys protected, IPv4 only, validated ----
    assert "CryptProtectData(" in configs and "CRYPTPROTECT_LOCAL_MACHINE" in configs
    assert "D:P(A;;FA;;;SY)(A;;FA;;;BA)" in configs
    assert "Table = off" in configs and "AllowedIPs = 0.0.0.0/0" in configs
    assert "DNS = " not in configs, "the gateway's config must not carry DNS"
    assert "static BOOL valid_key(" in configs and "static BOOL valid_endpoint(" in configs

    # ---- Gateway: kill switch and per-VM routing ----
    assert "unreachable default table 99" in linux_agent
    assert "iptables -A ASBVPN -s %s -o asbvpn+ -j ACCEPT" in linux_agent
    assert "iptables -A ASBVPN -s %s -j DROP" in linux_agent
    assert "iptables -I INPUT 1 -s %s -j DROP" in linux_agent
    assert "ip rule add from %s/32 lookup %d priority 10000" in linux_agent
    assert 'agent_log("cmd: vpn_set:<redacted>")' in linux_agent
    assert 'strncmp(command, "vpn_set:", 8) == 0' in vm_agent, "host log must redact vpn_set"
    assert "net.ipv6.conf.all.disable_ipv6 = 1" in disk, "the gateway must not route IPv6"
    assert "restore_bindings_locked(" in gateway, "a restarted gateway must rebind running VMs"

    # ---- VMs: NET_VPN endpoint from the gateway, guest gets gateway + VPN DNS ----
    assert core.count("vpn_gateway_prepare_client(") == 4, "every network setup path handles NET_VPN"
    assert "vpn_gateway_release_client(vm);" in core
    assert "vm->network_mode != NET_VPN &&" in core, "VM cleanup must not delete the gateway's network"
    assert re.search(r'"set_ip:%s/24:%s:%s:%s",\s*vm->vpn_ip, vpn_gateway_lan_ip\(\), vm->vpn_mac, dns\)', vm_agent)
    assert "ASB_NAT_DNS" in win_agent and "dns_field" in linux_agent

    # ---- UI ----
    assert 'L"importVpnConfig"' in ui and 'L"deleteVpnConfig"' in ui
    assert "SecureZeroMemory(text, 32768 * sizeof(wchar_t))" in ui
    assert '<option value="4">VPN</option>' in html and 'id="vpn-config"' in html
    assert "function importVpnConfig()" in app and "function networkOptionsHtml()" in app
    # Existing VMs switch to a VPN from the Manage dialog or the Network cell.
    assert 'id="manage-network"' in html and "function applyManageNetwork()" in app
    assert "args->network_mode == NET_VPN &&" in core, "VPN VMs get the current guest agent"

    assert "vpn_configs.c" in project and "vpn_gateway.c" in project
    print("[ok] WireGuard VPN configs and gateway-routed VMs contract")


if __name__ == "__main__":
    main()
