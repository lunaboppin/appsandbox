"""Low disk space warning: both guest agents report their system disk, and the host
warns in the log and the VM list before it fills (the VPN gateway once filled its own
disk unnoticed)."""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    agent = read("tools/agent/agent.c")
    linux = read("tools/linux/agent/appsandbox-agent.c")
    link = read("src/backend_win/vm_agent.c")
    hdr = read("src/backend_win/hcs_vm.h")
    ui = read("src/app_win/ui.c")
    headless = read("src/app_win/headless.c")
    js = read("web/app.js")

    # Windows agent: at connect and every 5 minutes, untagged.
    assert "static void report_disk_space(AsbConn *client)" in agent
    assert '"disk:%llu:%llu"' in agent and "GetDiskFreeSpaceExW(root, &free_bytes, &total_bytes, NULL)" in agent
    client = agent[agent.index("static void handle_client(AsbConn *client)"):]
    assert "DWORD disk_check_interval = 5 * 60 * 1000;" in client
    assert client.index("report_disk_space(client);") < client.index("while (WaitForSingleObject(g_stop_event")

    # Linux agent (desktops, the domain appliance and the VPN gateway): from the heartbeat thread.
    beat = linux[linux.index("static void *heartbeat_thread"):linux.index("/* ---- Command handlers ---- */")]
    assert 'statvfs("/", &vfs)' in beat and '"disk:%llu:%llu"' in beat and "disk_beats++ % 60 == 0" in beat

    # Host: parsed, a threshold, logged on change only, reset each boot.
    assert "DWORD       disk_free_mb;" in hdr and "BOOL        disk_low;" in hdr
    report = link[link.index("static void disk_report"):link.index("static int process_async_message")]
    assert "free_mb < 1024 || (free_mb * 10 < total_mb && free_mb < 10240)" in report
    assert "if (low == vm->disk_low) return;" in report and "notify_agent_status(vm);" in report
    assert 'strncmp(buf, "disk:", 5) == 0' in link and "disk_report(vm, buf + 5);" in link
    assert "vm->disk_low = FALSE;" in link

    # Shown for running VMs only.
    assert 'jb_bool(jb, L"diskLow", v->running && v->disk_low);' in ui
    assert '\\"diskLow\\":%s' in headless
    assert "if (vm.diskLow)" in js and "'Low disk space: '" in js
    print("[ok] low disk space warning contract")


if __name__ == "__main__":
    main()
