"""Contract: the shared appliance is LAN-only and has no route to the internet.

Host side (HCN): in normal operation the appliance's only adapter is the share
endpoint, whose ACLs allow traffic to and from the share subnet / host only and
block everything else. A NAT adapter exists only for maintenance boots.
Guest side: shared_net strips any default route from the share adapter and
blocks egress off the share subnet, on both the Windows and Linux agents.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    while ";" in source[start:brace]:   # skip forward declarations
        start = source.index(signature, start + 1)
        brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:i + 1]
    raise ValueError("unterminated function: " + signature)


def main() -> None:
    appliance = (ROOT / "src/backend_win/shared_appliance.c").read_text(encoding="utf-8")
    hcn = (ROOT / "src/backend_win/hcn_network.c").read_text(encoding="utf-8")
    win_agent = (ROOT / "tools/agent/agent.c").read_text(encoding="utf-8")
    linux_agent = (ROOT / "tools/linux/agent/appsandbox-agent.c").read_text(encoding="utf-8")

    # ---- HCN: the share server endpoint only talks to the share subnet ----
    server = function_body(hcn, "HRESULT hcn_create_share_server_endpoint(")
    server = re.sub(r'"\s*L"', "", server)   # join the wide-string continuation lines
    for direction in ("In", "Out"):
        block = ('\\"Priority\\":200,\\"Action\\":\\"Block\\",'
                 f'\\"Direction\\":\\"{direction}\\"')
        if block not in server:
            raise AssertionError(f"appliance endpoint lost its default {direction}bound block ACL")
    allows = re.findall(r'\\"RemoteAddresses\\":\\"([^\\"]*)\\"', server)
    if not allows or any(addr not in ("%S.0/24", "%S.1/32") for addr in allows):
        raise AssertionError(f"appliance endpoint allows traffic beyond the share subnet: {allows}")

    # ---- Maintenance NAT only on provisioning boots ----
    start = function_body(appliance, "static HRESULT start_internal(")
    nat_at = start.index("hcn_create_nat_network(&g_appliance.maintenance_network_id)")
    guard_at = start.rindex("if (provisioning) {", 0, nat_at)
    if start[guard_at:nat_at].count("}") > 0:
        raise AssertionError("maintenance NAT endpoint is created outside the provisioning guard")
    if appliance.count("hcn_create_nat_network(") != 1:
        raise AssertionError("the appliance creates a NAT network outside the maintenance boot")
    if "start_internal(FALSE, wait_ready, timeout_ms)" not in function_body(
            appliance, "HRESULT shared_appliance_start("):
        raise AssertionError("public appliance start (VM auto-start, restart) is no longer a normal boot")
    update = function_body(appliance, "HRESULT shared_appliance_update(")
    if "if (FAILED(hr)) { shared_appliance_stop(TRUE); return hr; }" not in update:
        raise AssertionError("a failed maintenance boot can stay running on its NAT adapter")

    # ---- Guest hardening is sent to every backend ----
    if "if (!net_configured) {" not in start or \
            "!net_configured &&\n                g_appliance.status.backend" in start:
        raise AssertionError("shared_net is not sent to every appliance backend")

    win_net = function_body(win_agent, "static int configure_shared_nic(")
    for token, failure in {
        "-DestinationPrefix '0.0.0.0/0'": "Windows share adapter keeps any default route",
        "Remove-NetRoute": "Windows share adapter keeps any default route",
        "'AppSandbox Shared Network Egress' -Direction Outbound -Action Block -InterfaceAlias $a.Name":
            "Windows share adapter has no egress block",
        "-ResetServerAddresses": "Windows share adapter keeps DNS servers",
    }.items():
        if token not in win_net:
            raise AssertionError(failure)
    if "DefaultGateway" in win_net:
        raise AssertionError("Windows share adapter is configured with a gateway")

    linux_net = function_body(linux_agent, "static int configure_shared_nic(")
    for token, failure in {
        "ip route del default dev $iface": "Linux share adapter keeps any default route",
        "iptables -A OUTPUT -o $iface ! -d %s -j DROP": "Linux share adapter has no egress block",
        "isdigit((unsigned char)*p)": "Linux shared_net interpolates an unvalidated IP into the shell",
    }.items():
        if token not in linux_net:
            raise AssertionError(failure)
    if " via " in linux_net:
        raise AssertionError("Linux share adapter is configured with a gateway")

    print("[ok] shared appliance LAN-only / no internet route contract")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, FileNotFoundError, ValueError) as exc:
        print(f"[fail] {exc}")
        raise SystemExit(1)
