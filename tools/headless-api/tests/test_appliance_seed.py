"""Read a generated NoCloud ISO without mounting it or printing its secrets."""
import argparse
import re
import struct
from pathlib import Path


def files(path):
    with path.open("rb") as f:
        descriptor = None
        joliet = False
        for sector in range(16, 64):
            f.seek(sector * 2048)
            block = f.read(2048)
            if block[1:6] != b"CD001":
                continue
            if block[0] == 1 and descriptor is None:
                descriptor = block
            if block[0] == 2 and block[88:91] in (b"%/@", b"%/C", b"%/E"):
                descriptor, joliet = block, True
            if block[0] == 255:
                break
        assert descriptor is not None, "ISO volume descriptor missing"
        root = descriptor[156:190]
        f.seek(struct.unpack_from("<I", root, 2)[0] * 2048)
        directory = f.read(struct.unpack_from("<I", root, 10)[0])
        pos, result = 0, {}
        while pos < len(directory):
            length = directory[pos]
            if not length:
                pos = (pos // 2048 + 1) * 2048
                continue
            entry = directory[pos:pos + length]
            pos += length
            raw_name = entry[33:33 + entry[32]]
            if raw_name in (b"\0", b"\1") or entry[25] & 2:
                continue
            name = raw_name.decode("utf-16-be" if joliet else "ascii").split(";")[0].lower()
            f.seek(struct.unpack_from("<I", entry, 2)[0] * 2048)
            result[name] = f.read(struct.unpack_from("<I", entry, 10)[0])
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    args = parser.parse_args()
    data = files(args.iso)
    assert data["user-data"].startswith(b"#cloud-config\n"), "cloud-config header missing or BOM-prefixed"
    for name in ("user-data", "meta-data", "network-config"):
        assert not data[name].startswith(b"\xef\xbb\xbf"), name + " contains a BOM"
        data[name].decode("utf-8")
    network = data["network-config"].decode("utf-8")
    assert network.count("macaddress:") == 2, "seed must match both actual HCN endpoint MACs"
    assert len(re.findall(r"macaddress: '[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}'", network)) == 2, "seed contains a missing or invalid MAC"
    assert ".254/24" in network and "to: default" in network, "static NAT bootstrap route missing"
    assert network.count("dhcp4: false") == 2, "seed unexpectedly depends on DHCP"
    print("[ok] generated NoCloud seed has BOM-free UTF-8 and static MAC-bound bootstrap networking")


if __name__ == "__main__":
    main()
