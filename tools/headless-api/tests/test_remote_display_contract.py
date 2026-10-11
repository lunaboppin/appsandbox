"""Offline contracts for dual-display, immersive HID input, and frame stability."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]

def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")

def check(value, message: str) -> None:
    if not value:
        raise AssertionError(message)

def main() -> int:
    vdd = read("tools/vdd/vdd.cpp")
    vdd_h = read("tools/vdd/vdd.h")
    transport = read("tools/transport/asb_transport.h")
    host = read("src/backend_win/vm_display_idd.c")
    helper = read("tools/agent/appsandbox-input.c")
    agent = read("tools/agent/agent.c")
    core = read("src/backend_win/asb_core.c")
    hcs = read("src/backend_win/hcs_vm.c")
    disk = read("src/backend_win/disk_util.c")
    provision = read("tools/provision/win_provision.c")
    vhid = read("tools/vhid/AppSandboxVHID.c")
    vhid_project = read("tools/vhid/AppSandboxVHID.vcxproj")
    signing = read("tools/sign/submission-config.json")
    release = read("tools/sign/make-release.ps1")

    check("#define VDD_MONITOR_COUNT     2" in vdd_h and
          "MaxMonitorsSupported = VDD_MONITOR_COUNT" in vdd and
          "VddCreateMonitor(pWrapper->pContext, i)" in vdd,
          "VDD does not expose two independent IddCx monitors")
    check("ASB_CH_DISPLAY2         8" in transport and
          "ASB_CH_DISPLAY2" in vdd and
          "d->display_index == 0 ? 2 : 8" in host and
          "for (port = 1; port <= 8; port++)" in hcs,
          "display 2 is not carried over its own transport")
    check("d->auxiliary && d->has_frame && !IsWindowVisible(hwnd)" in host and
          "Display 2 is created hidden" in host,
          "legacy single-display guests would expose a dead second window")
    check("INPUT_MOUSE_RELATIVE" in host and "INPUT_MOUSE_RELATIVE" in helper and
          "RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE" in host and
          "INPUT_READY_VHID" in host and "INPUT_READY_VHID" in helper,
          "immersive relative input is not capability-gated end to end")
    for token in ("VhfCreate", "VhfStart", "VhfReadReportSubmit",
                  "IOCTL_ASB_VHID_MOUSE"):
        check(token in vhid, f"virtual HID driver missing {token}")
    check("EvtVhfReadyForNextReadReport =" not in vhid and
          "WdfIoQueueDispatchSequential" in vhid and
          '"LowerFilters",0x00010000,"vhf"' in read("tools/vhid/AppSandboxVHID.inf"),
          "virtual HID does not load VHF or preserve button edges in order")
    check("total / WHEEL_DELTA" in helper and "total % WHEEL_DELTA" in helper,
          "HID wheel reports do not convert Windows deltas into detents")
    check("CreateFileW(L\"\\\\\\\\.\\\\AppSandboxVHID\"" in helper and
          "DeviceIoControl(g_vhid, IOCTL_ASB_VHID_MOUSE" in helper,
          "guest input helper does not submit reports to the virtual HID")
    check("VhfKm.lib" in vhid_project and
          "WdfExecutionLevelPassive" in vhid,
          "virtual HID project is missing VHF linkage or passive cleanup")
    check("install_staged_root_driver(L\"AppSandboxVHID.inf\"" in agent and
          '{ L"AppSandboxVHID", L"sys" }' in core and "AppSandboxVHID.inf" in disk and
          "AppSandboxVHID.inf" in provision and "AppSandboxVHID.cer" in provision,
          "existing and new Windows VMs do not receive the virtual HID driver")
    check("appsandbox-input.exe" in core and
          "if (vm->install_complete && _wcsicmp(vm->os_type, L\"Windows\") == 0)" in core,
          "existing Windows VMs do not get offline integration-tool upgrades")
    check("IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS" in core and
          "GetVirtualDiskPhysicalPath" in core and "GetLogicalDrives()&~before" not in core,
          "offline upgrades can select a concurrently mounted unrelated disk")
    check(".pending" in core and ".pending" in agent and
          "exit_code == 0 || exit_code == 1" in agent and
          'present ? L"update" : L"install"' in agent and "staged_driver_digest" in agent,
          "driver updates can install incomplete packages, duplicate roots, or restart unchanged packages")

    idle = vdd[vdd.index("if (hr == E_PENDING)"):vdd.index("} else if (SUCCEEDED(hr))")]
    check("!bSentFullFrame" in idle,
          "static desktops still resend full frames continuously")
    check("SetEvent(proc->hTerminateEvent)" in vdd and
          "preserving state to avoid use-after-free" in vdd,
          "VDD teardown does not signal cancellation or guard timed-out workers")
    destroy = vdd[vdd.index("static BOOL VddDestroySwapProc"):vdd.index("/* =========================================================================", vdd.index("static BOOL VddDestroySwapProc"))]
    check("asb_conn_socket_u64" not in destroy,
          "VDD teardown races workers freeing their connection wrappers")
    check("DXGI_SWAP_EFFECT_FLIP_DISCARD" in host and
          "Present(d->swap_chain, 1, 0)" in host and
          "frame_ready_posted" in host,
          "host presentation is not compositor-paced and message-coalesced")
    check("SetMultithreadProtected(TRUE)" in vdd,
          "two capture threads share an unprotected D3D immediate context")
    check("(*ppDevice)->AddRef()" in vdd and
          "proc->pDeviceContext->Release()" in vdd,
          "display workers do not own independent D3D references")

    check("AppSandboxVHID" in signing and "AppSandboxVHID" in release,
          "virtual HID is absent from release signing/package tripwires")
    print("[ok] dual-display, immersive input, and frame-stability contracts")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[fail] {exc}", file=sys.stderr)
        raise SystemExit(1)
