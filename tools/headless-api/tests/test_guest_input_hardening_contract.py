"""Data a VM sends the host is bounded and can't name host paths: game list files,
browser-view messages, guest text in fixed buffers, clipboard formats and files, and
the backstage viewer's frames."""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    gs = read("src/backend_win/game_stream.c")
    web = read("src/backend_win/vm_web.c")
    clip = read("src/backend_win/vm_clipboard.c")
    bs = read("src/backend_win/vm_backstage.c")
    core = read("src/backend_win/asb_core.c")
    link = read("src/backend_win/vm_agent.c")
    stream = read("src/backend_win/vm_stream.c")

    # Game list: one name check for reads and writes, and limits on what's copied.
    leaf = gs[gs.index("static BOOL catalog_leaf_ok"):gs.index("BOOL game_stream_catalog_file")]
    assert "*p != '_' && *p != '.'" in leaf and 'strstr(leaf, "..")' in leaf and "leaf[0] != '.'" in leaf
    assert "if (!catalog_leaf_ok(leaf)) return FALSE;" in gs
    fetch = gs[gs.index("static HRESULT fetch_catalog_file"):gs.index("static HRESULT collect_catalog")]
    assert fetch.index("if (!catalog_leaf_ok(leaf))") < fetch.index("CreateFileW(path, GENERIC_WRITE")
    assert "if (total > max || total > *budget)" in fetch and "bytes > total - offset" in fetch
    assert "if (FAILED(hr)) DeleteFileW(path);" in fetch
    collect = gs[gs.index("static HRESULT collect_catalog"):gs.index("/* ---- Building the image ---- */")]
    assert "if (++files > GS_CATALOG_FILES_MAX) break;" in collect
    assert "fetch_catalog_file(vm, leaf, dir, GS_ART_MAX, &budget);" in collect
    assert "total > GS_IDENTITY_FILE_MAX" in gs

    # Browser view: no addition before the size check, so a 64-bit length can't wrap.
    assert "payload > WEB_MAX_MESSAGE || payload > (UINT64)(WEB_MAX_MESSAGE - msg_len)" in web
    assert "msg_len + payload > WEB_MAX_MESSAGE" not in web

    # Guest text in fixed buffers is cut short, and a CRT overflow no longer ends the app.
    assert "_set_invalid_parameter_handler(core_invalid_parameter);" in core
    assert '%.200S",' in core
    assert link.count("_snwprintf_s(") >= 6 and "_TRUNCATE, L\"%S\", buf" in link
    assert '_TRUNCATE, L"The VM\'s agent said: %S.' in stream

    # Clipboard: an allowlist, files only through the temp folder, and limits.
    assert "static BOOL clip_standard_format_allowed" in clip and "static BOOL clip_named_format_allowed" in clip
    assert '"HTML Format", "PNG"' in clip
    assert "if (clip->reader_fmts[idx].local_id == CF_HDROP) {" in clip
    assert "if (hdr->format == CF_HDROP && hdr->data_size == 0)" not in clip
    assert "++clip->reader_files > CLIP_MAX_FILES" in clip and "CLIP_DISK_SPARE" in clip

    # Backstage: a band is only copied into a buffer of the frame's own size.
    assert "(int)hdr.width == v->width && (int)hdr.height == v->height" in bs
    print("[ok] guest input hardening contract")


if __name__ == "__main__":
    main()
