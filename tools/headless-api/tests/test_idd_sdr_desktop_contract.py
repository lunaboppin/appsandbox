"""Contract: the IDD display gets an ordinary 8-bit SDR desktop.

With the IddCx 1.10 (HDR-aware) callbacks registered, Windows composed the
display's desktop in FP16 even though it reported SDR and the driver never set
CAN_PROCESS_FP16. The driver's own frames were right (browser view), but Desktop
Duplication handed Sunshine and Parsec FP16 with sRGB-encoded values, which they
gamma-corrected again: washed-out streams. The v1 callbacks are used instead.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    vdd = read("tools/vdd/vdd.cpp")
    assert "#define VDD_IDDCX_V2 0" in vdd
    assert "if (VDD_IDDCX_V2 && IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))" in vdd
    assert "if (VDD_IDDCX_V2 && IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2))" in vdd, \
        "the v2 acquire goes with the v2 callbacks"
    v1 = vdd[vdd.index('VddLog("DeviceAdd: registering v1 callbacks");'):]
    v1 = v1[:v1.index("}")]
    for cb in ("EvtIddCxParseMonitorDescription ", "EvtIddCxMonitorQueryTargetModes ", "EvtIddCxAdapterCommitModes "):
        assert "iddConfig." + cb in v1, cb
    assert "adapterCaps.Flags = IDDCX_ADAPTER_FLAGS_NONE;" in vdd, "FP16 is never advertised"
    print("[ok] IDD SDR desktop contract")


if __name__ == "__main__":
    main()
