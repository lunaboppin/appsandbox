"""Contract: the Ubuntu appliance gets an address on its maintenance NAT adapter.

The AppSandbox NAT network uses static IPAM and runs no DHCP server, so the
Ubuntu cloud image's default DHCP config leaves the guest offline and setup
stalls at the package install. The maintenance endpoint therefore requests a
fixed MAC, and the NoCloud seed carries a netplan network-config that matches
that MAC and sets the endpoint's NAT address, gateway and DNS.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def define(source: str, name: str) -> str:
    m = re.search(r'#define\s+%s\s+"([^"]+)"' % name, source)
    assert m, "missing #define " + name
    return m.group(1)


def main() -> None:
    appliance = (ROOT / "src/backend_win/shared_appliance.c").read_text(encoding="utf-8")
    disk = (ROOT / "src/backend_win/disk_util.c").read_text(encoding="utf-8")
    hcn = (ROOT / "src/backend_win/hcn_network.c").read_text(encoding="utf-8")

    # The two spellings of the maintenance MAC must name the same address.
    dashed = define(appliance, "APPLIANCE_MAINTENANCE_MAC")
    colon = define(appliance, "APPLIANCE_MAINTENANCE_MAC_COLON")
    assert dashed.replace("-", ":").lower() == colon, (dashed, colon)

    # The maintenance endpoint requests that MAC and the .254 NAT address.
    assert re.search(r"hcn_create_endpoint_with_mac\(&g_appliance\.maintenance_network_id,"
                     r"[^;]*nat_ip, APPLIANCE_MAINTENANCE_MAC\)", appliance), \
        "maintenance endpoint must request APPLIANCE_MAINTENANCE_MAC"
    assert '"%s.254", hcn_nat_subnet_base()' in appliance
    assert '"\\"MacAddress\\":\\"%S\\","' in hcn, "endpoint document must carry the requested MAC"

    # The seed is built with the same address, gateway and MAC.
    assert re.search(r"iso_create_appliance_cloud_init\([^;]*nat_ip, nat_gateway, "
                     r"APPLIANCE_MAINTENANCE_MAC_COLON\)", appliance), \
        "seed must be told the maintenance address and MAC"
    assert '"%s.1", hcn_nat_subnet_base()' in appliance

    # The seed's network-config matches by MAC, is static and optional.
    assert 'L"%s\\\\network-config"' in disk, "seed must include network-config"
    for needle in ('"version: 2\\n"', 'macaddress: \\"%s\\"', '"    dhcp4: false\\n"',
                   '"    optional: true\\n"', '"    addresses: [%s/24]\\n"',
                   "- to: default", "nameservers:"):
        assert needle in disk, "network-config missing " + needle
    # The Ubuntu appliance's COM1 is captured: the host owns the pipe server.
    assert "CreateNamedPipeW(" in appliance and "APPLIANCE_VM_NAME L\".com1\"" in appliance
    assert "start_serial_capture();" in appliance and "console.log" in appliance
    print("[ok] shared appliance setup network (maintenance NAT address) contract")


if __name__ == "__main__":
    main()
