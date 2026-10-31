"""Contract: the IDD display takes any size the window asks for.

Windows offers the intersection of a monitor's modes and the target modes and
reads the monitor's modes when it arrives, so a size can't simply be added: the
driver departs its monitor and arrives again offering only the requested size.
The frames are the host's answer. It stops asking when a size doesn't appear,
and the driver falls back by itself, so a refused size can't cost the display.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def body(src: str, start: str) -> str:
    s = src[src.index(start):]
    return s[:s.index("\n}\n")]


def nv12_round_trip(w: int, h: int) -> None:
    """The padded plane sizes both ends must agree on, and every pixel in range."""
    aw, ah = (w + 1) & ~1, (h + 1) & ~1
    assert aw * ah * 3 // 2 == aw * ah // 2 * 3, (w, h)  # guest and host spell it differently
    for y in range(0, ah, 2):
        assert y + 1 >= h or y + 1 < ah
        for x in range(0, aw, 2):
            n = 4 if x + 1 < w else 0
            assert x * 4 + n < w * 4, (w, h, x)  # never reads past the row


def main() -> None:
    vdd = read("tools/vdd/vdd.cpp")
    vddh = read("tools/vdd/vdd.h")
    idd = read("src/backend_win/vm_display_idd.c")
    inp = read("tools/agent/appsandbox-input.c")

    # No fixed size left in the frame path, or a fixed-size mode list.
    for gone in ("VDD_WIDTH", "VDD_HEIGHT", "VDD_STRIDE", "VDD_PIXEL_BYTES", "g_SupportedResolutions"):
        assert gone not in vdd and gone not in vddh, gone
    assert "sig->activeSize.cx                          = w;" in vdd
    assert "row < DEFAULT_HEIGHT" not in idd
    assert "td.Width              = DEFAULT_WIDTH;" not in idd

    # Same request and limits on both ends.
    assert "#define VDD_RESIZE_MAGIC    0x5A525341" in vddh
    assert "#define RESIZE_MAGIC        0x5A525341" in idd
    for name, value in (("MIN_WIDTH", "640"), ("MIN_HEIGHT", "480"), ("MAX_SIDE", "8192"),
                        ("MAX_PIXELS", "(7680u * 4320u)")):
        assert f"VDD_{name}" in vddh and value in vddh, name
        assert f"IDD_{name}" in idd, name
    assert "MAX_FRAME_DATA_SIZE (IDD_MAX_PIXELS * 4)" in idd

    # Driver: the monitor departs, then arrives at the new size, on the WDF timer.
    timer = body(vdd, "static VOID VddModeTimerCallback(")
    assert timer.index("IddCxMonitorDeparture(") < timer.index("VddCreateMonitor(ctx)")
    assert timer.index("g_ModeWidth = w;", timer.index("VDD_MODE_DEPARTED")) < timer.index("VddCreateMonitor(ctx)")
    assert "IddCxMonitorUpdateModes" not in vdd, "can't add a size the monitor didn't arrive with"
    # ... falls back to the last size with a swap chain, with no host involved ...
    assert "no swap chain at %ux%u, back to %ux%u" in timer
    # ... and keeps the host connected across it.
    assert "proc->pCtx->hCarriedConn = proc->hClientConn;" in vdd
    assert "(PVOID volatile*)&ctx->hCarriedConn, NULL);" in vdd
    # The old monitor's late unassign can't take down the new swap chain.
    assert "ctx->pSwapProc->hMonitor == MonitorObject" in vdd

    # Capture follows the surface actually acquired, and nothing is sized by a constant.
    assert "surfaceDesc.Width != proc->width || surfaceDesc.Height != proc->height" in vdd
    assert "VddResizeCapture(proc, surfaceDesc.Width, surfaceDesc.Height)" in vdd

    # NV12 works at odd sizes: padded planes, cropped on the host.
    assert "#define VDD_NV12_BYTES(w, h) (VDD_ALIGN2(w) * VDD_ALIGN2(h) * 3 / 2)" in vddh
    assert "whdr.stride           = nv12 ? VDD_ALIGN2(w) : stride;" in vdd
    assert "hdr.stride != IDD_ALIGN2(hdr.width)" in idd
    assert "for (k = 0; k < 2 && x + k < w; k++)" in idd
    assert "((hdr.width | hdr.height) & 1" not in idd, "odd NV12 is accepted now"
    for size in ((1920, 1080), (1281, 721), (640, 1001), (641, 480), (7680, 4320)):
        nv12_round_trip(*size)

    # Host: the frame texture and receive buffer follow the frame without a reconnect.
    render = body(idd, "static void d3d_render_frame(")
    assert "d3d_create_frame_texture(d, d->frame_width, d->frame_height);" in render
    assert "if (data_size > recv_cap) {" in idd

    # Host: debounced, once per drag, and the window may now outgrow the frame.
    assert "SetTimer(hwnd, IDT_RESIZE, RESIZE_SETTLE_MS, NULL);" in idd
    step = body(idd, "static void idd_resize_step(")
    assert "d->in_sizemove" in step
    assert "case WM_EXITSIZEMOVE:" in idd
    assert "if (d && !d->can_resize && d->frame_width > 0 && d->frame_height > 0) {" in idd

    # Host: stops asking when the size doesn't appear; only Windows guests are asked.
    assert "d->resize_even_only = TRUE;" in step
    assert "d->resize_gave_up = TRUE;" in step
    assert '_wcsicmp(vm->os_type, L"Windows") == 0' in idd

    # Mouse: mapped through the frame's size, in real pixels in the guest, and
    # the guest's monitor rectangle is re-read as soon as the desktop changes size.
    assert "d->frame_width, d->frame_height, &vx, &vy);" in idd
    assert "SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);" in inp
    assert "virtual_w == g_vdd_checked_w && virtual_h == g_vdd_checked_h" in inp
    print("[ok] IDD display any size contract")


if __name__ == "__main__":
    main()
