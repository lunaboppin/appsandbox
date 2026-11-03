"""Contract: Windows ISOs that bring their own setup keep it.

App Sandbox applies the image straight to the VHDX, so Windows Setup never reads
an ISO's autounattend.xml or copies its sources\\$OEM$ folder. The ISO mount that
picks the Pro edition copies both out; the $OEM$ files are staged onto the disk
and the ISO's answer file is merged into ours, with App Sandbox keeping the
computer name, its account, autologon and the OOBE skip.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> None:
    core = (ROOT / "src/backend_win/asb_core.c").read_text(encoding="utf-8")
    merge = (ROOT / "src/backend_win/iso_custom.c").read_text(encoding="utf-8")
    prov = (ROOT / "tools/provision/win_provision.c").read_text(encoding="utf-8")
    proj = (ROOT / "AppSandboxCore.vcxproj").read_text(encoding="utf-8")

    # One ISO mount: edition index plus the ISO's own setup files.
    assert "$u=$l+':\\\\autounattend.xml'" in core
    assert "$o=$l+':\\\\sources\\\\$OEM$'" in core
    assert "args->config.is_template ? NULL : custom_dir" in core
    # Merged both times the answer file is written (initial and after LANG:).
    assert core.count("has_custom_unattend ? custom_unattend : NULL") == 2
    assert "iso_custom_append_manifest(manifest, custom_dir, args->config.image_path)" in core

    # App Sandbox owns these; the ISO's are dropped.
    for el in ("ComputerName", "UserAccounts", "AutoLogon", "OOBE", "FirstLogonCommands"):
        assert 'xml_is(el, "%s")' % el in merge, el
    # Their commands run after ours, renumbered; setup-flow passes are not taken.
    assert "add_renumbered(&edits[1].text, &cmd, spec_order++)" in merge
    assert "add_renumbered(&edits[6].text, &cmd, flc_order++)" in merge
    assert "Enable-NetAdapter -IncludeHidden" in merge
    # $OEM$ mapping.
    assert 'L"\\\\Windows\\\\Setup\\\\Scripts\\\\SetupComplete.iso.cmd"' in merge
    assert 'L"\\\\Windows\\\\Panther"' in merge
    assert 'L"%s\\t\\\\Windows\\\\AppSandbox\\\\custom-iso.txt\\n"' in merge

    # SetupComplete.cmd keeps the ISO's taskbar pins and runs its SetupComplete last.
    assert "custom-iso.txt\\\" goto :pins_done" in prov and ":pins_done\\r\\n" in prov
    assert prov.index(":done\\r\\n") < prov.index("SetupComplete.iso.cmd")

    assert 'Include="src\\backend_win\\iso_custom.c"' in proj
    print("[ok] custom ISO setup (autounattend.xml + $OEM$) contract")


if __name__ == "__main__":
    main()
