"""Contract: closing the main window hides it to the tray, and nothing that has to
really quit the app goes through that.

No build needed: this reads the sources.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def section(text: str, start: str, end: str) -> str:
    body = text[text.index(start):]
    return body[:body.index(end)]


def main() -> None:
    ui = read("src/app_win/ui.c")
    update_ui = read("src/app_win/update_ui.c")
    main_c = read("src/app_win/main.c")
    iss = read("tools/installer/AppSandbox.iss")

    # The X hides the window, whatever is running, and asks nothing.
    close = section(ui, "case WM_CLOSE:", "case WM_QUERYENDSESSION:")
    assert "main_window_to_tray(hwnd)" in close
    assert "TaskDialog" not in ui
    # Hiding needs a way back: with no icon the window stays on the taskbar.
    to_tray = section(ui, "static void main_window_to_tray", "static void main_window_show")
    assert to_tray.index("SW_MINIMIZE") < to_tray.index("SW_HIDE")
    # Showing it again keeps a maximized window maximized.
    assert "IsIconic(hwnd) ? SW_RESTORE : SW_SHOW" in section(ui, "static void main_window_show", "#ifndef DWMWA_BORDER_COLOR")

    # The updater, and the tray's Exit, quit without going through WM_CLOSE.
    assert "DestroyWindow(g_hwnd);" in section(update_ui, "static void install_if_clear", "static void start_install")
    assert "WM_CLOSE" not in update_ui
    menu = section(ui, "static void tray_show_menu", "/* ---- Prerequisite feature enable")
    assert "cmd == TRAY_CMD_EXIT" in menu and "DestroyWindow(hwnd);" in menu
    assert 'L"Open"' in menu and 'L"Exit"' in menu
    # Exit removes the icon.
    assert "tray_remove();" in section(ui, "case WM_DESTROY:", "PostQuitMessage")

    # A shutdown or logoff is never refused, and a close during one really closes.
    ending = section(ui, "case WM_QUERYENDSESSION:", "case WM_DESTROY:")
    assert "return TRUE;" in ending and "return FALSE;" not in ending
    assert "if (!wp) g_session_ending = FALSE;" in ending
    assert "if (g_session_ending) DestroyWindow(hwnd);" in close

    # The installer and uninstaller never close the app; they send the user to the tray.
    assert iss.count("exit App Sandbox from its tray icon") == 2

    # The icon survives Explorer restarting. The app is elevated and Explorer isn't, so
    # the message has to be let through.
    assert 'RegisterWindowMessageW(L"TaskbarCreated")' in ui
    assert "ChangeWindowMessageFilterEx(hwnd, g_wm_taskbar_created, MSGFLT_ALLOW, NULL)" in ui

    # Settings aren't saved over before they've been loaded.
    explain = section(ui, "static void tray_explain_once", "static void main_window_to_tray")
    assert explain.index("g_prereq_ok") < explain.index("asb_save()")

    # A second launch brings the window back; the message is for the headless daemon,
    # which has no window.
    instance = section(main_c, "CreateMutexW", "/* Create main window */")
    assert instance.index("ui_show_running_instance()") < instance.index("MessageBoxW")
    assert "AllowSetForegroundWindow" in section(ui, "BOOL ui_show_running_instance", "HWND ui_create_main_window")
    assert "ui_show_running_instance" not in read("src/app_win/headless.c")
    assert "MAIN_WINDOW_CLASS" not in read("src/app_win/headless.c")

    print("OK: close-to-tray contract")


if __name__ == "__main__":
    main()
