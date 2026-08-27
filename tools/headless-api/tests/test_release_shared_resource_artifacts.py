#!/usr/bin/env python3
"""Verify that a packaged Windows build contains the appliance transport."""

from pathlib import Path
import argparse
import sys


def contains(data: bytes, text: str, *, wide: bool = False) -> bool:
    encoding = "utf-16-le" if wide else "ascii"
    return text.encode(encoding) in data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "build_dir",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[3] / "bin" / "Release",
    )
    args = parser.parse_args()

    core_path = args.build_dir / "appsandbox_core.dll"
    agent_path = args.build_dir / "resources" / "appsandbox-agent.exe"
    failures: list[str] = []

    for path in (core_path, agent_path):
        if not path.is_file():
            failures.append(f"missing artifact: {path}")

    if not failures:
        core = core_path.read_bytes()
        agent = agent_path.read_bytes()

        if not contains(core, "Shared appliance provisioning failed", wide=True):
            failures.append("host DLL is missing the shared-appliance lifecycle marker")
        if not contains(core, "AppSandbox shared appliance credential", wide=True):
            failures.append("host DLL is missing DPAPI appliance credential support")
        if contains(core, "Virtual SMB share", wide=True):
            failures.append("host DLL still contains the obsolete VSMB startup marker")
        if contains(core, "Attached isolated SMB adapter", wide=True):
            failures.append("host DLL still contains the obsolete host-SMB transport marker")
        if not contains(agent, "shared_smb_map:"):
            failures.append("bundled guest agent is missing shared_smb_map support")
        if not contains(agent, "New-SmbGlobalMapping"):
            failures.append("bundled guest agent is missing New-SmbGlobalMapping")
        if not contains(agent, "Get-NetAdapter -IncludeHidden"):
            failures.append("bundled guest agent is missing delayed NIC discovery")
        if not contains(agent, "shared_net:"):
            failures.append("bundled guest agent is missing appliance NIC configuration")
        if not contains(agent, "Start-Sleep -Milliseconds 500"):
            failures.append("bundled guest agent is missing the NIC discovery retry delay")
        if not contains(agent, "--refresh-drive"):
            failures.append("bundled guest agent is missing interactive shell drive refresh")
        if not contains(agent, "using only non-shared adapter"):
            failures.append("bundled guest agent is missing NAT MAC-mismatch fallback")
        if contains(agent, "shared_map:"):
            failures.append("bundled guest agent still contains the obsolete VSMB mapper")

    if failures:
        print("Release shared-resource artifact verification FAILED:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print(f"Release shared-resource artifacts verified: {args.build_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
