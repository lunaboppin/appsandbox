"""Contract: high-motion IDD frames travel as NV12.

A 1080p BGRA frame is 8.3 MB and the guest link moves ~340 MB/s, which held
games to ~20 fps. Frames where most of the screen changed go as NV12 (3.1 MB);
the first frame after connecting and the idle resend stay lossless BGRA, so a
still screen is sharp again within ~100 ms. The host decodes NV12 with the
exact inverse of the guest's full-range BT.601 conversion.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    vdd = read("tools/vdd/vdd.cpp")
    vddh = read("tools/vdd/vdd.h")
    idd = read("src/backend_win/vm_display_idd.c")

    assert "#define VDD_FRAME_NV12_MAGIC 0x3231564E" in vddh
    assert "#define FRAME_NV12_MAGIC    0x3231564E" in idd
    assert "#define VDD_NV12_AREA_PCT   40" in vddh

    # Guest: NV12 only once a lossless frame went, for full or mostly-dirty frames.
    assert "static void VddPackNv12(" in vdd
    assert "if (bSentFullFrame) {" in vdd
    assert "area * 100 >= (UINT64)VDD_WIDTH * VDD_HEIGHT * VDD_NV12_AREA_PCT" in vdd
    assert "int len = VddPackFrame(proc, &mapped, NULL, 0, FALSE);" in vdd, "idle resend is lossless"

    # Same coefficients on both ends.
    for c in ("77 * (px)[2] + 150 * (px)[1] + 29 * (px)[0]", "-43 * sr - 85 * sg + 128 * sb", "128 * sr - 107 * sg - 21 * sb"):
        assert c in vdd, c
    for c in ("359 * v", "-88 * u - 183 * v", "454 * u"):
        assert c in idd, c

    # Host: validated before decoding into the BGRA frame buffer.
    assert "if (magic != FRAME_MAGIC && magic != FRAME_NV12_MAGIC) {" in idd
    assert "(nv12 && data_size != hdr.width * hdr.height / 2 * 3)" in idd
    assert "idd_nv12_to_bgra(recv_buf, hdr.width, hdr.height," in idd

    # Sending overlaps the next frame's readback and packing.
    assert "static DWORD WINAPI VddSenderThread(" in vdd
    assert "send_ok = len > 0 && VddQueueSend(proc, len);" in vdd
    assert "VddSendAll(proc->hClientConn" not in vdd, "frames go through the sender"
    q = vdd[vdd.index("static BOOL VddQueueSend("):]
    q = q[:q.index("\n}\n")]
    assert q.index("VddWaitSendIdle(proc)") < q.index("SetEvent(proc->hSendWork)")
    cur = vdd[vdd.index("static void VddSendCursorUpdate("):]
    cur = cur[:cur.index("\n}\n")]
    assert "EnterCriticalSection(&proc->sendLock);" in cur, "cursor never splits a frame"
    assert "VddDropClient(proc);" in cur
    print("[ok] IDD NV12 frames for high-motion content contract")


if __name__ == "__main__":
    main()
