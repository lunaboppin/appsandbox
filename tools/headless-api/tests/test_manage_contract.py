"""Offline contract checks for the Windows-only per-VM Manage feature."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    html = read("web/index.html")
    js = read("web/app.js")
    ui = read("src/app_win/ui.c")
    core = read("src/backend_win/asb_core.c")
    headless = read("src/app_win/headless.c")
    snapshot = read("src/backend_win/snapshot.c")

    ids = re.findall(r'\bid="([^"]+)"', html)
    check(len(ids) == len(set(ids)), "web/index.html contains duplicate element ids")
    for element_id in (
        "manage-vm-overlay", "manage-iso-path", "manage-storage-parent",
        "manage-disk-size", "manage-auto-display", "manage-checkpoint-select",
    ):
        check(element_id in ids, f"missing Manage element #{element_id}")

    actions = (
        "browseManageIso", "browseManageStorage", "setVmInstallerIso",
        "setVmAutoOpenDisplay", "moveVmStorage", "resizeVmDisk",
    )
    for action in actions:
        check(action in js or action in html, f"frontend does not send {action}")
        check(f'L"{action}"' in ui, f"Windows bridge does not handle {action}")
        check(f'L"{action}"' not in headless, f"GUI-only action leaked into headless API: {action}")

    for key in ("AutoOpenDisplay=", "GuestGrowTargetGB="):
        check(core.count(key) >= 2, f"persistence key is not both saved and loaded: {key}")
    check("preserve_config_line" in core and "config_passthrough" in core,
          "atomic config rewrites do not preserve unrecognized VM keys")
    check("g_settings_passthrough" in core,
          "atomic config rewrites do not preserve unrecognized Settings keys")
    check("vm->auto_open_display" in core, "IDD probe is not gated by per-VM preference")
    check("vm.running, vm.buildingVhdx, vm.managementBusy" in js,
          "row cache does not refresh when lifecycle/management busy changes")
    start_body = core[core.index("ASB_API HRESULT asb_vm_start"):core.index("/* ---- VM Shutdown")]
    check("InterlockedCompareExchange(&inst->management_busy" in start_body and
          "InterlockedExchange(&inst->management_busy, 0)" in start_body,
          "VM start does not serialize against management operations")
    start_ex = core[core.index("ASB_API HRESULT asb_vm_start_ex"):core.index("/* ---- VM Shutdown")]
    pre_worker = start_ex[:start_ex.index("/* Switch to snapshot/base branch")]
    check("shared_appliance_start(" not in pre_worker and
          "shared_appliance_get_status" in pre_worker,
          "shared-resource VM start still starts or waits for the appliance on the UI thread")
    check("waiting for the shared-storage appliance" in core,
          "shared-resource VM start does not report that it is waiting for the appliance")
    prepare_failure = core[core.index("hr = prepare_shared_transport"):core.index("/* ---- Background VHDX creation thread ----")]
    check("InterlockedExchange(&vm->management_busy, 0)" in prepare_failure and
          "g_state_cb" in prepare_failure,
          "shared-resource preparation failure leaves the VM lifecycle busy")
    check("ERROR_DIR_NOT_EMPTY" in core and "branch_count > 0" in core,
          "core checkpoint deletion is not branch-first")
    check("instance->running" in snapshot and "ERROR_DIR_NOT_EMPTY" in snapshot,
          "snapshot layer lacks stopped/branch safety guards")

    node = shutil.which("node")
    if node:
        result = subprocess.run([node, "--check", str(ROOT / "web/app.js")])
        check(result.returncode == 0, "web/app.js failed node --check")

    print("[ok] Manage UI, bridge, persistence, safety, and GUI-only boundaries")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[fail] {exc}", file=sys.stderr)
        raise SystemExit(1)
