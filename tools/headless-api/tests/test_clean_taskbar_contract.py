"""Contract: new Windows VMs get a clean taskbar.

SetupComplete.cmd (every provisioning path) writes an OEM taskbar layout that
replaces the default pins with Edge and File Explorer only, and hides search,
Task View, widgets, chat and Copilot through policies and the Default user
profile, so the first sign-in of any account starts with that taskbar.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> None:
    for rel, count in (("src/backend_win/disk_util.c", 2), ("tools/provision/win_provision.c", 1)):
        src = (ROOT / rel).read_text(encoding="utf-8")
        for needle in ('PinListPlacement=\\"Replace\\"',
                       'DesktopApplicationID=\\"MSEdge\\"',
                       'DesktopApplicationID=\\"Microsoft.Windows.Explorer\\"',
                       "/v LayoutXMLPath ",
                       "/v AllowNewsAndInterests ",
                       "/v SearchOnTaskbarMode ",
                       "/v TurnOffWindowsCopilot ",
                       "Users\\\\Default\\\\NTUSER.DAT",
                       "/v SearchboxTaskbarMode ",
                       "/v ShowTaskViewButton ",
                       "/v ShowCopilotButton ",
                       "reg unload HKU\\\\AsbDefault"):
            assert src.count(needle) >= count, (rel, needle)
    print("[ok] clean taskbar on new Windows VMs contract")


if __name__ == "__main__":
    main()
