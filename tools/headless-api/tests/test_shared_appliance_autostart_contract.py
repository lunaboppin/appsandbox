"""Regression contract for starting a shared-resource VM while the appliance is stopped.

Every start path (GUI startVm, headless POST /v1/vms/{id}/start) reaches the
background start thread, which must bring the appliance up on demand and wait
for it instead of failing the VM.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> None:
    appliance = (ROOT / "src/backend_win/shared_appliance.c").read_text(encoding="utf-8")
    core = (ROOT / "src/backend_win/asb_core.c").read_text(encoding="utf-8")

    required = {
        # The client wait covers a cold boot plus the readiness handshake.
        "shared_appliance_start(TRUE, APPLIANCE_CLIENT_START_TIMEOUT_MS)":
            "client VM start does not wait long enough for a cold appliance boot",
        # An idle stop must not take down an appliance a VM start is waiting for.
        "g_appliance.pending_clients == 0":
            "idle stop ignores VM starts that are still waiting for the appliance",
        # A start arriving during a stop waits for it and boots fresh.
        "if (!g_appliance.stopping) break;":
            "start joins a stopping appliance instead of restarting it",
        # Stale endpoints from a failed attempt would block the deterministic IP/MAC.
        "refused. Nothing is running here, so they are safe to drop. */\n    cleanup_runtime_network();":
            "fresh appliance boot does not drop endpoints left by a failed attempt",
        # Concurrent starts: watchers follow the driver to its result.
        "hr = (HRESULT)g_appliance.last_start_hr;":
            "watching callers do not report the driver's start result",
        "if (driving) InterlockedExchange(&g_appliance.readiness_wait_active, 0);":
            "a non-driving failure can release another thread's handshake claim",
        # The VM start surfaces the appliance's own error.
        "The shared-storage appliance could not be started for":
            "VM start failure does not name the appliance failure",
    }
    for token, failure in required.items():
        if token not in appliance + core:
            raise AssertionError(failure)

    forbidden = {
        "shared_appliance_start(TRUE, 120000);\n    if (FAILED(hr)) return allow_missing":
            "client VM start still uses the two-minute appliance budget",
        "if (g_appliance.status.state == ASB_APPLIANCE_STATE_FAILED)\n                return E_FAIL;":
            "watchers still bail on a stale FAILED state while a retry is driving",
    }
    for token, failure in forbidden.items():
        if token in appliance:
            raise AssertionError(failure)

    # The start thread, not the UI/HTTP thread, is what waits for the appliance.
    start_thread = core[core.index("static DWORD WINAPI start_vm_thread"):]
    start_thread = start_thread[:start_thread.index("ASB_API")]
    if "prepare_shared_transport(&args->config, vm" not in start_thread:
        raise AssertionError("appliance auto-start moved off the background start thread")

    print("[ok] shared appliance auto-start on VM start contract")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, FileNotFoundError, ValueError) as exc:
        print(f"[fail] {exc}")
        raise SystemExit(1)
