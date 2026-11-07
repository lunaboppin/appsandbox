"""Game Streaming: an administrator can mark a user's VM to keep, so nothing automatic
deletes it, and give it its own network (VPN or NAT) from its next start."""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    gs = read("src/backend_win/game_stream.c")
    header = read("src/backend_win/game_stream.h")
    ui = read("src/app_win/ui.c")
    headless = read("src/app_win/headless.c")
    app = read("web/app.js")
    html = read("web/index.html")

    # The mark is a per-user file naming the VM; deleting the VM drops it.
    assert 'identity_path(user, L"vm-pin", path, MAX_PATH)' in gs
    remove = gs[gs.index("static void kept_remove"):gs.index("static const wchar_t *kept_unusable")]
    assert "kept_unpin(user);" in remove

    # Unused, image refreshed, keeping turned off: none of these end a marked VM.
    unusable = gs[gs.index("static const wchar_t *kept_unusable"):gs.index("static BOOL session_active")]
    assert "BOOL pinned" in unusable
    assert unusable.index("if (pinned) {") < unusable.index('if (days <= 0) return')
    pinned_part = unusable[unusable.index("if (pinned) {"):unusable.index('if (days <= 0) return')]
    assert "too long" not in pinned_part and "refreshed" not in pinned_part
    assert "kept_unusable(name, image, last, kept_pinned(d.cFileName, name))" in gs

    # Its image isn't pruned while it uses it, and it starts on that image.
    prune = gs[gs.index("static void prune_images(void)"):gs.index("/* ---- Settings ---- */")]
    assert "if (!used) used = kept_uses_image(dir);" in prune
    thread = gs[gs.index("static DWORD WINAPI session_thread"):]
    assert "wcscpy_s(image_dir, MAX_PATH, kept_image);" in thread
    assert "wcscpy_s(s->image, MAX_PATH, kept_image);" in thread
    # In use: the session fails rather than replacing it.
    assert '"Your VM is still in use. Try again in a minute."' in thread
    # A failed start doesn't delete it, and a session end always keeps it.
    assert "if (reuse && !pinned) kept_remove(s->user, s->vm_name, L\"it couldn't start\");" in thread
    assert "(pinned || (keep_days > 0 && (reuse || ready_once)))" in thread
    assert "if (keep_days > 0 || pinned) kept_write(" in thread
    assert "!kept_pinned(s->user, held)" in thread

    # Per-VM network: the user's choice wins over the Game Streaming setting.
    assert 'identity_path(user, L"vm-vpn", path, MAX_PATH)' in gs
    own = thread[thread.index("user_vpn_read(s->user, own"):thread.index("/* A VPN deleted since it was chosen")]
    assert '_wcsicmp(own, L"nat") == 0' in own

    # Admin API, checked: the user must have a folder, the VPN must exist.
    assert "ASB_API HRESULT game_stream_set_vm_options(" in header
    opts = gs[gs.index("HRESULT game_stream_set_vm_options"):gs.index("static int append_user_vms")]
    assert "admin_user_ok(user)" in opts and "vpn_configs_find(vpn, NULL)" in opts
    ok = gs[gs.index("static BOOL admin_user_ok"):gs.index("HRESULT game_stream_set_vm_options")]
    assert 'wcspbrk(user, L"\\\\/:*?\\"<>|")' in ok and 'L".."' in ok
    assert '",\\"userVms\\":["' in gs and "append_user_vms(out, cap, pos, copy);" in gs
    assert 'L"gameStreamingVmOptions"' in ui
    assert 'L"/v1/game-streaming/vm"' in headless

    # Dialog.
    assert 'id="gs-user-vms"' in html
    assert "sendCmd('gameStreamingVmOptions', { user: v.user, pinned: check.checked });" in app
    assert "sendCmd('gameStreamingVmOptions', { user: v.user, vpn: net.value });" in app

    # Idle timeout: Never (0) everywhere, up to a week, and per user's VM; re-read as
    # the session runs so a change applies at once.
    assert "#define GS_IDLE_MAX_MIN      (7 * 24 * 60)" in gs
    assert "idle_minutes > GS_IDLE_MAX_MIN" in gs
    assert 'identity_path(user, L"vm-idle", path, MAX_PATH)' in gs
    idle_fn = gs[gs.index("static int user_idle_minutes"):gs.index("static void kept_unpin")]
    assert '_wcsicmp(line, L"never") == 0) return 0;' in idle_fn
    watch = thread[thread.index("\nwatch:"):thread.index("\nstopped:")]
    assert watch.index("idle_minutes = user_idle_minutes(s->user, idle_minutes);") < watch.index("idle_minutes > 0 && now - idle_since")
    assert "idle_minutes = g_idle_minutes;" in watch
    assert "#define GS_IDLE_LEAVE   (-2)" in header and "#define GS_IDLE_DEFAULT (-1)" in header
    assert "if (idle != GS_IDLE_LEAVE)" in opts
    assert '\\"idleMinutes\\":%d}' in gs
    for src in (ui, headless):
        assert 'L"idleMinutes", &idle' in src and "has_vpn ? vpn : NULL, idle," in src
    assert '<select id="gs-idle"></select>' in html
    assert "if (m === 0) return 'Never';" in app
    assert "sendCmd('gameStreamingVmOptions', { user: v.user, idleMinutes: parseInt(idle.value, 10) });" in app

    print("game streaming VM options contract: OK")


if __name__ == "__main__":
    main()
