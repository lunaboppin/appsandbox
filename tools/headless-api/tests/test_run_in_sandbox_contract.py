"""Contract: Run in Sandbox never loses track of a throwaway disk and never rolls back
unasked, and the two guest agents and the host agree on the channel.

No build needed: this reads the sources.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def section(text: str, start: str, end: str) -> str:
    body = text[text.index(start):]
    return body[:body.index(end)]


def main() -> None:
    core = read("src/backend_win/asb_core.c")
    run = read("src/backend_win/sandbox_run.c")
    headless = read("src/app_win/headless.c")
    ui = read("src/app_win/ui.c")
    iss = read("tools/installer/AppSandbox.iss")
    launcher = read("tools/shell-launcher/appsandbox-run.c")
    project = read("tools/shell-launcher/appsandbox-run.vcxproj")

    # The run is on disk: all of it is saved and loaded.
    for key in ("SandboxRunId", "SandboxRunParent", "SandboxRunOverlay", "SandboxRunFile"):
        assert f'{key}=%s' in core, f"{key} isn't saved"
        assert f'L"{key}="' in core, f"{key} isn't loaded"

    # The disk swap and the record are one atomic write, and a failed write undoes it.
    create = section(run, "static HRESULT overlay_create(", "static HRESULT overlay_drop(")
    assert create.index("sandbox_run_id, _countof") < create.index("asb_save_checked()")
    assert "DeleteFileW(overlay)" in create
    # Going back: recorded first, deleted second.
    drop = section(run, "static HRESULT overlay_drop(", "void asb_run_sweep_orphans(")
    assert drop.index("asb_save_checked()") < drop.index("delete_overlay_file(overlay)")

    # Nothing rolls back by itself: the only callers of the drop are a VM that never
    # booted and an explicit finish, and loading a config only sweeps what nothing names.
    assert run.count("overlay_drop(") == 3
    sweep = section(run, "void asb_run_sweep_orphans(", "/* ---- Sending the file ---- */")
    assert "sandbox_run_overlay" in sweep and "vhdx_path" in sweep
    assert "overlay_drop" not in core

    # A running VM isn't touched unless the caller said so.
    one = section(run, "static void run_one(", "static void finish_run(")
    assert one.index("ASB_RUN_MODE_SHUTDOWN_FIRST") < one.index("asb_vm_shutdown(vm)")
    file_fn = section(run, "HRESULT asb_run_file(", "HRESULT asb_run_finish(")
    assert "ERROR_BUSY" in file_fn

    # Other disk operations can't strand the overlay.
    for guarded in ("asb_snap_take", "asb_snap_delete", "asb_snap_new_branch", "asb_snap_delete_branch"):
        body = section(core, f"ASB_API HRESULT {guarded}(", "\n}\n" if "\r" not in core else "\r\n}\r\n")
        assert "sandbox_run_blocks" in body, f"{guarded} isn't guarded"
    assert "sandbox_run_blocks" in section(core, "static HRESULT management_begin(", "return S_OK")

    # The file goes by copy, and the host's handle only reads.
    source = section(run, "static HANDLE open_source(", "/* ---- Suitability ---- */")
    assert "GENERIC_READ" in source and "GENERIC_WRITE" not in source

    # All three ends agree on the channel, and the HCS service table carries it.
    assert re.search(r"#define\s+ASB_CH_FILEDROP\s+16", read("tools/transport/asb_transport.h"))
    assert "#define ASB_FILE_DROP_CHANNEL 16" in read("src/backend_win/hcs_vm.h")
    assert "ASB_FILE_DROP_CHANNEL" in read("src/backend_win/hcs_vm.c")
    assert "ASB_CH_FILEDROP" in read("tools/agent/file_drop.c")
    assert "#define DROP_PORT           16" in read("tools/linux/agent/appsandbox-agent.c")

    # The API won't guess at throwing work away.
    finish = section(headless, 'L"run-in-sandbox/finish"', 'L"shutdown"')
    assert "bad_action" in finish

    # Explorer's entry: unelevated launcher, removed on uninstall, shipped.
    assert r"*\shell\AppSandbox.RunInSandbox" in iss.replace("\\\\", "\\")
    assert "uninsdeletekey" in iss
    assert "<UACExecutionLevel>AsInvoker</UACExecutionLevel>" in project
    assert "appsandbox-run.exe" in read("tools/sign/stage-package.ps1")
    assert "appsandbox-run.vcxproj" in read("AppSandbox.sln")

    # The launcher and the window it talks to agree, and the window can hear it.
    assert "0x41534252" in launcher and "0x41534252" in ui
    assert "ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA" in ui
    # A path from another process only ever opens the picker.
    copydata = section(ui, "case WM_COPYDATA:", "case WM_VM_STATE_CHANGED:")
    assert "asb_run_file" not in copydata

    print("ok")


if __name__ == "__main__":
    main()
