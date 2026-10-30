"""Contract: the installer, the auto-updater and the CI that builds them.

No build needed: this reads the sources. test_updater_mock.py runs the updater for
real, and tools/installer/test-install.ps1 runs the installer for real.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def section(text: str, start: str, end: str) -> str:
    body = text[text.index(start):]
    return body[:body.index(end)]


def main() -> None:
    iss = read("tools/installer/AppSandbox.iss")
    builder = read("tools/installer/build-installer.ps1")
    stage = read("tools/sign/stage-package.ps1")
    release = read("tools/sign/make-release.ps1")
    config = read("src/app_win/update_config.h")
    updater = read("src/app_win/updater.c")
    update_ui = read("src/app_win/update_ui.c")
    ui = read("src/app_win/ui.c")
    js = read("web/app.js")
    html = read("web/index.html")
    project = read("AppSandbox.vcxproj")
    solution = read("AppSandbox.sln")
    targets = read("Directory.Build.targets")
    ci = read(".github/workflows/ci.yml")
    signed = read(".github/workflows/build-windows.yml")
    auto = read(".github/workflows/release.yml")
    main_c = read("src/app_win/main.c")

    # One version: Directory.Build.props. The installer script holds none of its own,
    # and the build refuses a binary built at a different version.
    assert "ASB_VERSION_FULL=$(AsbVersion)" in targets
    assert "AppVersion={#AppVersion}" in iss
    assert "VersionInfoVersion={#AppVersion}" in iss
    assert not re.search(r"AppVersion=\d", iss)
    assert "Directory.Build.props" in builder
    assert "Get-PeVersion $exe" in builder and "Rebuild first" in builder

    # The upgrade identity is shared between the installer and the updater.
    app_id = re.search(r'ASB_INSTALLER_APP_ID L"(\{[0-9A-F-]+\})"', config).group(1)
    assert "AppId={" + app_id in iss
    assert app_id + "_is1" in iss

    # Elevation: the app needs admin, so the installer does too. The per-user variant
    # exists for tests and can't be signed, so it can't become an update.
    assert "PrivilegesRequired=admin" in iss
    assert "-TestPerUser builds are never signed" in builder

    # The guest helpers ship, each named so a missing one stops the build. Both the
    # zip and the installer use the one list.
    for helper in ("agent", "audio", "backstage", "clipboard", "clipboard-reader", "displays", "input"):
        assert f"resources\\appsandbox-{helper}.exe" in stage, helper
    assert "stage-package.ps1" in release and "stage-package.ps1" in builder
    assert "$rootBins" not in release
    assert "updater-test" not in stage, "the test driver must not ship"

    # No host drivers or services: the drivers folder is only payload for guests.
    assert "pnputil" not in iss.lower() and "devcon" not in iss.lower()
    assert "[Services]" not in iss

    # The installer never closes a running app: closing it stops every VM. It uses the
    # app's own single-instance mutex, which the headless daemon holds as well.
    mutex = re.search(r'CreateMutexW\(NULL, FALSE, L"Global\\\\([A-Za-z]+)"\)', main_c).group(1)
    assert f'#define AppMutexName "Global\\{mutex}"' in iss
    assert "CloseApplications=no" in iss
    assert "RestartApplications=no" in iss
    setup = section(iss, "function InitializeSetup", "// Runs however Setup ends")
    assert "WizardSilent" in setup and "Result := False" in setup
    assert "function InitializeUninstall" in iss
    uninstall = section(iss, "function InitializeUninstall", "// The VMMS certificate")
    assert "AppIsRunning" in uninstall and "UninstallSilent" in uninstall

    # An update gives the app back however Setup ends.
    deinit = section(iss, "procedure DeinitializeSetup", "function InitializeUninstall")
    assert "IsUpdateRun" in deinit and "ShellExec" in deinit
    assert "/ASBUPDATE=1" in updater

    # Uninstalling keeps VMs unless the user says otherwise, default No, silent keeps.
    purge = iss[iss.index("procedure CurUninstallStepChanged"):]
    assert "UninstallSilent" in purge
    assert "MB_DEFBUTTON2" in purge
    assert purge.index("MB_DEFBUTTON2") < purge.index("DelTree")
    # It removes runtime files by name, never the whole folder (it may hold other files).
    uninstall_delete = section(iss, "[UninstallDelete]", "[Code]")
    assert 'Name: "{app}"' in uninstall_delete and "dirifempty" in uninstall_delete
    assert 'filesandordirs; Name: "{app}"' not in uninstall_delete

    # Shortcuts run from the install folder: the log file is CWD-relative.
    assert iss.count('WorkingDir: "{app}"') >= 3

    # Update source: HTTPS, this repo, and only ever a listed release.
    assert 'L"https://api.github.com/repos/lunaboppin/appsandbox/releases' in config
    assert "/releases/latest" not in config, "/latest hides pre-releases, which is every release so far"
    assert "Refusing a non-HTTPS" in updater
    assert "WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP" in updater
    assert re.search(r'wcscmp\(host, L"127\.0\.0\.1"\)', updater), "plain HTTP only to loopback"
    select = section(updater, "static BOOL release_to_info", "UpdateCheckResult updater_select_release")
    for check in ("r->draft", '"uploaded"', "digest_to_hex", "endpoint->asset_prefix", "UPDATER_MAX_INSTALLER_BYTES"):
        assert check in select, check

    # Verification, in order: hash, then signature by a pinned certificate, then the
    # file's own version. A status list that accepted a bad signature would undo it all.
    verify = section(updater, "BOOL updater_verify_installer", "/* ---- Install state")
    assert verify.index("sha256_of_handle") < verify.index("updater_read_signer") < verify.index("file_version_equals")
    assert "pinned" in verify
    intact = section(updater, "static BOOL signature_intact", "static BOOL file_version_equals")
    assert "CERT_E_UNTRUSTEDROOT" in intact
    for bad in ("TRUST_E_BAD_DIGEST", "TRUST_E_NOSIGNATURE", "TRUST_E_EXPLICIT_DISTRUST", "CERT_E_EXPIRED", "CERT_E_REVOKED"):
        assert bad not in intact, bad
    assert re.search(r'L"[0-9A-F]{64}"', config), "at least one signer pin"
    # The verified bytes are the ones launched: writers stay locked out until then.
    assert "GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING" in verify
    # An elevated app runs what lands in this folder, so ordinary users can't touch it.
    assert 'L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"' in updater
    assert "OWNER_SECURITY_INFORMATION" in updater

    # Running VMs: the same set the headless shutdown guard refuses on, and it's
    # checked at the moment the app would close, not only when the user clicked.
    blocks = section(update_ui, "static BOOL vm_blocks_update", "static int count_blockers")
    assert "v->running" in blocks and "v->building_vhdx" in blocks
    install = section(update_ui, "static void install_if_clear", "static void start_install")
    assert install.index("count_blockers() > 0") < install.index("updater_launch_installer")
    assert install.index("PHASE_WAITING_FOR_VMS") < install.index("DestroyWindow")
    assert "hcs_terminate_vm" not in update_ui and "asb_vm_stop" not in update_ui
    # A waiting update resumes when VMs change, and the app wires that in.
    assert "update_ui_on_vm_state();" in section(ui, "case WM_VM_STATE_CHANGED:", "case WM_VM_REMOVED:")
    assert "update_ui_on_vm_state();" in section(ui, "case WM_VM_REMOVED:", "case WM_TRAYICON")
    assert "update_ui_handle_action(action, json)" in ui
    assert "update_ui_on_ready(g_hwnd_main);" in ui
    assert "case WM_UPDATE_EVENT:" in ui
    # A copy not set up by the installer never installs over somewhere else.
    assert "updater_is_installed()" in update_ui
    assert "!g_installed" in section(update_ui, "static void start_install", "static void cancel_install")

    # UI: told which VMs are in the way before anything starts; untrusted text as text.
    assert "case 'updateState':" in js
    prompt = section(js, "function installUpdate()", "function showModal(")
    assert "blockers.join" in prompt and "showModal('Install update'" in prompt
    assert prompt.index("showModal") < prompt.index("sendCmd('updateInstall')")
    render = section(js, "function renderUpdate()", "function dismissUpdateBanner")
    assert "innerHTML" not in render and ".textContent = text" in render
    assert 'id="update-banner"' in html and 'id="updates-overlay"' in html

    # Build wiring.
    for name in ("updater.c", "update_ui.c"):
        assert f'src\\app_win\\{name}"' in project, name
    assert 'tools\\updater-test\\updater-test.vcxproj' in solution

    # CI: every push and PR is built and tested on GitHub's own machines, without
    # secrets, and the installer is really run. Self-hosted signing runs only when asked.
    assert "pull_request:" in ci
    assert "runs-on: [self-hosted" not in ci
    assert "secrets." not in ci
    assert "windows-" in ci
    assert "test-install.ps1" in ci and "run_static.py" in ci
    assert "SignMode=TestSign" in ci and "SignMode=Off" not in ci
    trigger = section(signed, "\non:", "permissions:")
    assert "workflow_dispatch:" in trigger and "push:" not in trigger
    assert "runs-on: [self-hosted" in signed
    # A release is only ever a draft; publishing is a person's decision.
    assert "--draft" in signed
    assert not re.search(r"gh release (create|edit)(?!.*--draft)", signed)
    assert "already published" in signed

    # Automatic release: only from main, only for a version with no release, and only
    # after the signed installer has been installed and removed.
    trigger = section(auto, "\non:", "permissions:")
    assert "branches: [main]" in trigger and "pull_request" not in trigger
    assert "paths:" not in trigger  # merging a fix must retry a release that failed
    assert "needed == 'true'" in section(auto, "  build:", "runs-on:")
    assert "already exists" in auto
    # The key is only reachable through the main-only environment, and write access to
    # the repo only exists in the job that never sees it.
    build_job = section(auto, "  build:", "  install:")
    assert "environment: release" in build_job
    assert "secrets.RELEASE_SIGNING_PFX_BASE64" in build_job
    assert auto.count("secrets.") == 2
    publish = auto[auto.index("  publish:"):]
    assert "contents: write" in publish and "contents: write" not in auto[:auto.index("  publish:")]
    assert "[plan, install]" in publish
    assert "test-install.ps1" in section(auto, "  install:", "  publish:")
    assert "if: always()" in build_job and "-DeleteKey" in build_job
    # It signs with a certificate the app accepts: the pin is that certificate's SHA-256.
    assert "SIGNING_THUMBPRINT: 6546C03C0B5C55CD8688FCC203AD5598844C89C2" in auto
    assert "18A05C560707077AF685CE0DA874F2820E27724B2302F4250EA5A80BBB903032" in config
    # Full releases, or installed copies wouldn't be offered them by default.
    assert "--prerelease" not in publish and "--draft" not in publish

    print("[ok] installer, updater and CI contract")


if __name__ == "__main__":
    main()
