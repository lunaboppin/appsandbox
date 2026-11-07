"""Other people on this PC can't reach App Sandbox's data or channels: the data
folder belongs to Administrators, the API token and settings aren't readable by
other users, the Hyper-V sockets refuse unelevated processes, names are checked
before they reach paths, XML or command lines, and system DLLs load from System32."""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    core = read("src/backend_win/asb_core.c")
    header = read("src/backend_win/asb_core.h")
    headless = read("src/app_win/headless.c")
    ui = read("src/app_win/ui.c")
    hcs = read("src/backend_win/hcs_vm.c")
    disk = read("src/backend_win/disk_util.c")
    main_c = read("src/app_win/main.c")
    app = read("web/app.js")

    # Data folder: owned by Administrators, Users read only, checked at start-up.
    assert 'L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"' in core
    assert 'L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"' in core
    assert 'L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)"' in core
    assert "ASB_API BOOL asb_secure_data_dir(void);" in header
    init = core[core.index("ASB_API HRESULT asb_init(void)"):]
    assert init.index("asb_secure_data_dir();") < init.index("hcs_init()")
    secure = core[core.index("ASB_API BOOL asb_secure_data_dir(void)"):core.index("static BOOL owner_trusted")]
    assert "FILE_ATTRIBUTE_REPARSE_POINT" in secure
    assert 'L"%s\\\\ssh", root' in secure and "PRIVATE_DIR_SDDL" in secure
    assert "get_config_path(path, MAX_PATH);" in secure and "PRIVATE_FILE_SDDL" in secure
    apply = core[core.index("static BOOL apply_security"):core.index("ASB_API BOOL asb_secure_data_dir(void)")]
    assert "security_matches(path, sd)" in apply
    assert 'enable_privilege(L"SeTakeOwnershipPrivilege");' in apply
    assert "PROTECTED_DACL_SECURITY_INFORMATION" in apply
    assert "asb_secure_data_dir();" in headless[headless.index("static void open_log"):]

    # The UI's WebView2 browser runs as the signed-in user, unelevated: its profile
    # keeps that user's write access, set before the browser starts.
    wv = core[core.index("static void secure_webview_dir"):core.index("ASB_API BOOL asb_secure_data_dir(void)")]
    assert "WebView2Data" in wv and "GetShellWindow()" in wv and "append_token_user_ace" in wv
    assert "secure_webview_dir(root);" in secure
    bridge = read("src/app_win/webview2_bridge.c")
    assert bridge.index("asb_secure_data_dir();") < bridge.index("pfnCreateEnv(NULL, udf")

    # Settings: the temp file gets the admin-only ACL before it replaces vms.cfg.
    save = core[core.index('L"%s.tmp", path'):]
    assert save.index("apply_security(temp_path, PRIVATE_FILE_SDDL);") < save.index("MoveFileExW(temp_path, path,")

    # SSH key: a key pair someone else left is replaced.
    key = core[core.index("ASB_API BOOL asb_ensure_ssh_key"):]
    assert key.index("!owner_trusted(pub) || !owner_trusted(priv)") < key.index("ssh-keygen.exe")
    assert "apply_security(ssh_dir, PRIVATE_DIR_SDDL);" in key
    owner = core[core.index("static BOOL owner_trusted"):core.index("static BOOL name_ok")]
    assert "WinLocalSystemSid" in owner and "WinBuiltinAdministratorsSid" in owner and "TokenUser" in owner

    # API token: a new host.json readable by SYSTEM, Administrators and the daemon's user.
    disc = headless[headless.index("static PSECURITY_DESCRIPTOR discovery_sd"):headless.index("static void delete_discovery")]
    assert 'L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;%s)"' in disc
    assert "CREATE_NEW" in disc and "DeleteFileW(path);" in disc
    assert disc.index("DeleteFileW(path);") < disc.index("CreateFileW(path")
    assert '_wfopen_s(&f, path, L"w")' not in disc

    # Lab reports: only a report.html under LabReports is ever opened.
    assert "ASB_API BOOL asb_lab_report_path_ok(const wchar_t *path);" in header
    lab = core[core.index("ASB_API BOOL asb_lab_report_path_ok"):core.index("ASB_API void asb_lab_reports_dir")]
    assert 'L"\\\\report.html"' in lab and 'L"\\\\.."' in lab
    assert "if (asb_lab_report_path_ok(line + 10))" in core
    assert ui.count("asb_lab_report_path_ok(") == 2
    for call in ("ShellExecuteW(g_hwnd_main, L\"open\", w->report", "ShellExecuteW(g_hwnd_main, L\"open\", v->lab_report"):
        before = ui[:ui.index(call)]
        assert before.rfind("asb_lab_report_path_ok(") > before.rfind("\n    }")

    # Names: checked in the core, escaped in unattend XML, quoted for ssh.
    name = core[core.index("static BOOL name_ok"):core.index("ASB_API BOOL asb_vm_name_ok")]
    assert "iswalnum(ch)" in name and 'L"CON"' in name and "s[0] == L'.'" in name
    create = core[core.index("Error: VM name is required."):]
    assert "if (!asb_vm_name_ok(config->name))" in create[:1500]
    assert "!asb_user_name_ok(config->username)" in create[:1500]
    rename = core[core.index("ASB_API HRESULT asb_vm_set_name"):]
    assert "if (!asb_vm_name_ok(name))" in rename[:1200]
    assert disk.count("xml_escape_w(admin_user, user_xml, _countof(user_xml));") == 2
    assert disk.count("sanitize_computer_name_w(comp_name, 16);") == 3
    assert "xml_escape_user_u(admin_user, user_u, sizeof user_u)" in disk
    assert 'L"cmd.exe /k ssh %s-p %lu \\"%s@localhost\\""' in ui
    assert "asb_user_name_ok(inst->admin_user)" in ui
    assert "/^[\\p{L}\\p{N} ._-]+$/u.test(name)" in app

    # Run in Sandbox requests: local files the signed-in user could read anyway.
    cd = ui[ui.index("static BOOL copydata_path_ok"):ui.index("static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)\n{")]
    assert "DRIVE_REMOTE" in cd and 'L"\\\\\\\\?\\\\UNC\\\\"' in cd
    assert "TokenLinkedToken" in cd and "AccessCheck(" in cd
    handler = ui[ui.index("    case WM_COPYDATA:"):]
    assert "if (!copydata_path_ok(path))" in handler[:2000]
    assert "(path[0] == L'\\\\' && path[1] == L'\\\\')" not in handler[:2000]

    # Hyper-V sockets: SYSTEM and Administrators only, never Everyone.
    assert '#define HV_SERVICE_SDDL L"D:P(A;;FA;;;SY)(A;;FA;;;BA)"' in hcs
    assert "(A;;FA;;;WD)" not in hcs
    assert hcs.count('L"\\"BindSecurityDescriptor\\":\\"" HV_SERVICE_SDDL') == 2
    assert hcs.count('L"\\"ConnectSecurityDescriptor\\":\\"" HV_SERVICE_SDDL') == 2

    # DLLs and the log: System32 by flag, no working directory.
    assert "SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);" in main_c
    for rel, dll in (("src/backend_win/hcs_vm.c", "computecore.dll"),
                     ("src/backend_win/hcn_network.c", "computenetwork.dll"),
                     ("src/backend_win/vm_display.c", "RdpBase.dll"),
                     ("src/backend_win/vm_display.c", "atl.dll"),
                     ("src/backend_win/vm_web_encoder.c", "mfplat.dll"),
                     ("src/backend_win/vm_backstage.c", "dwmapi.dll")):
        src = read(rel)
        assert 'LoadLibraryExW(L"%s", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)' % dll in src, dll
        assert 'LoadLibraryW(L"%s")' % dll not in src, dll
    for rel in ("src/backend_win/asb_core.c", "src/backend_win/hcs_vm.c"):
        assert 'L"appsandbox.log", L"a"' not in read(rel), rel
    assert 'L"%s\\\\AppSandbox\\\\appsandbox.log"' in core

    print("local access contract: OK")


if __name__ == "__main__":
    main()
