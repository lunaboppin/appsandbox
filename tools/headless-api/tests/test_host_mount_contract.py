"""Contract: Mount Host maps a drive Explorer sees at once, and Unmount drops it.

The app runs elevated, so a drive it maps lands in its own logon session and
Explorer (unelevated) never sees it; a global SMB mapping shows only after
Explorer restarts. Letter mappings are therefore made with WNet while the
thread impersonates the shell's token, and Explorer is told the drive came or
went. Mount Host gives a resource without a host letter a free one first, and
the UI shows Unmount while the resource is mounted.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    appliance = read("src/backend_win/shared_appliance.c")
    ui = read("src/app_win/ui.c")
    app = read("web/app.js")

    token = appliance[appliance.index("static HANDLE shell_user_token(void)"):]
    token = token[:token.index("\n}\n")]
    assert "GetShellWindow()" in token and "TokenImpersonation" in token

    letter = appliance[appliance.index("static HRESULT host_letter_mapping("):]
    letter = letter[:letter.index("\n}\n")]
    assert "impersonate_shell_user(&token)" in letter
    assert "WNetAddConnection2W(&resource, password, user, CONNECT_TEMPORARY)" in letter
    assert "WNetCancelConnection2W(local, 0, TRUE)" in letter
    assert "notify_explorer_drive(letter, TRUE)" in letter
    assert "notify_explorer_drive(letter, FALSE)" in letter
    assert "remove_legacy_global_mapping(letter)" in letter
    assert "New-SmbGlobalMapping -LocalPath" not in appliance, "letters are no longer global mappings"
    assert "SHCNE_DRIVEADD" in appliance and "DBT_DEVICEARRIVAL" in appliance

    assert "asb_shared_resource_assign_host_letter(work->id)" in ui
    assert 'L"hostMounted", asb_shared_resource_host_mounted(r.id)' in ui
    assert "if(r.hostMounted)mount.style.display='none';else unmount.style.display='none';" in app
    # Mounting can wait minutes on the appliance: it runs off the UI thread.
    assert "CreateThread(NULL, 0, host_resource_work_thread, work, 0, NULL)" in ui
    assert "case WM_HOST_RESOURCE_DONE:" in ui
    assert "Open Share" not in app, "Mount Host already opens the drive in Explorer"
    print("[ok] host mount maps an Explorer-visible drive contract")


if __name__ == "__main__":
    main()
