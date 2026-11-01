"""Contract: a Linux first boot that can't finish says so, on the host.

The agent is built on the VM, and it's the agent connecting that ends "installing",
so a failed build used to mean "installing" forever. firstboot.sh now reports over
COM1, and a watchdog on the host turns a failure report, or silence, into a "failed"
state with a reason. A healthy install must not be affected.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


def main() -> None:
    guest = read("tools/iso-patch/linux_firstboot.c")
    core = read("src/backend_win/asb_core.c")
    agent = read("src/backend_win/vm_agent.c")
    headless = read("src/app_win/headless.c")
    ui = read("src/app_win/ui.c")
    js = read("web/app.js")
    css = read("web/style.css")
    client = read("tools/headless-api/asb.py")

    # Guest: reports go straight to the serial port, one per line, untraced.
    assert '> /dev/" IP_SERIAL_A "' in guest, "reports must reach COM1 without the journal's prefix"
    assert 'ASB-FIRSTBOOT:$(' in guest
    assert re.search(r'printf \\"\\\\n%s\\\\n\\"', guest), "a report has to start its own line"
    assert "{ local _x=$-; set +x; } 2>/dev/null" in guest, "a trace of the report must not look like one"
    assert "timeout 5 sh -c" in guest, "a stuck port must not hang setup"
    for report in ("asb_report START", "asb_report HEARTBEAT", "asb_report DONE",
                   'asb_report \\"STEP Building the guest agent\\"', 'asb_report \\"FAIL $1\\"'):
        assert report in guest, report

    # Guest: the causes that stop the agent being built name themselves.
    for cause in ("Could not install the build tools:", "Could not build the guest agent:",
                  "Could not install the guest agent:", "The guest agent source was not copied"):
        assert f"asb_note_failure \\\"{cause}" in guest, cause

    # Guest: only a missing agent is fatal, or a healthy install could be failed
    # by something that didn't stop it, like a kernel module.
    check = guest[guest.index("STEP 98: report"):guest.index("STEP 99: Mark done")]
    assert "[ ! -x /usr/local/bin/appsandbox-agent ]" in check
    assert "is-enabled appsandbox-agent.service" in check
    assert 'ASB_FAILURE=\\"\\"\\n"' in check, "with an agent, earlier failures don't count"
    assert "asb_report_failure" in check and "exit 1" in check
    assert "systemctl reboot" not in check and "firstboot.done" not in check

    # Guest: a failed setup reports again at each boot rather than re-running, since
    # the markers it would need are gone.
    rerun = guest.index("[ -s /var/lib/appsandbox-firstboot.failed ]")
    assert rerun < guest.index("asb_report START")
    assert "> /var/lib/appsandbox-firstboot.failed" in check

    # Guest: the console is now kept on the host, so the hash stays out of the trace.
    step5 = guest[guest.index('STEP 5: user account ====') : guest.index("STEP 6: Skip GNOME")]
    assert step5.index("{ set +x; }") < step5.index("ASB_HASH=$(") < step5.rindex("set -x")
    assert 'usermod -p' in step5 and step5.index('usermod -p') < step5.rindex("set -x")

    # Host: deadlines leave room for slow hosts. A normal install is about 5 minutes,
    # and the guest's own limit has to fit between them.
    limits = {m[0]: int(m[1]) for m in re.findall(r"#define (INSTALL_WATCH_\w+_MS)\s+\((\d+) \* 60", core)}
    assert limits == {"INSTALL_WATCH_NO_REPORT_MS": 15, "INSTALL_WATCH_SILENCE_MS": 10,
                      "INSTALL_WATCH_AGENT_MS": 15}, limits
    assert "TimeoutStartSec=1800" in guest
    assert "while sleep 30" in guest and limits["INSTALL_WATCH_SILENCE_MS"] * 60 > 30 * 4

    # Host: every way a Linux VM gets powered on starts the watchdog.
    assert core.count("install_watch_start(vm, TRUE);") == 1
    assert core.count("install_watch_start(inst, TRUE);") == 2
    assert "install_watch_start(&g_vms[i], FALSE);" in core, "reattach after an app restart"
    create = function(core, "static DWORD WINAPI linux_create_thread(")
    assert "install_watch_start(inst, TRUE);" in create

    # Host: limited to Linux setups; Windows and the appliance are unaffected.
    start = function(core, "static void install_watch_start(VmInstance *vm, BOOL fresh_boot)\n{")
    for guard in ('L"Linux"', "vm->is_appliance", "vm->is_template", "vm->install_complete"):
        assert guard in start, guard
    assert "firstboot-console.log" in start
    assert "FILE_FLAG_OVERLAPPED" in core, "must be able to stop without waiting on the guest"

    # Host: only a report at the start of a line counts, and it's cleaned before use.
    line = function(core, "static void install_watch_line(")
    assert "strncmp(clean, INSTALL_REPORT_PREFIX" in line
    assert "strstr" not in line
    text = function(core, "static void install_report_text(")
    assert "0x20" in text and "0x7F" in text

    # Host: a timeout is withdrawn when the guest reports again, and the agent
    # connecting always wins, so a slow host recovers by itself.
    assert 'install_watch_publish(w->vm_id, L"", L"Starting the guest agent");' in line
    assert 'install_watch_publish(w->vm_id, L"", NULL);' in line
    assert "vm->install_error[0] = L'\\0';" in agent
    assert agent.index("vm->install_error[0] = L'\\0';") < agent.index('L"Install complete for')
    assert "!vm->shutdown_requested" in core, "stopping a VM isn't a failure"

    # Host: survives an app restart.
    assert 'L"InstallError=%s\\n"' in core and 'L"InstallError="' in core

    # Reaches the API.
    assert '"failed" : "installing"' in headless
    assert '\\"installError\\":' in headless and '\\"installStatus\\":' in headless
    assert 'if s["state"] == "failed":' in client, "wait() shouldn't sit out its timeout"

    # Reaches the UI, as text rather than markup, since it comes from the guest.
    assert 'L"installError"' in ui and 'L"installStatus"' in ui
    assert "vm.installError && !vm.installComplete" in js
    assert "detailEl.textContent = detail" in js and "innerHTML = detail" not in js
    assert ".status-failed" in css and "--danger" in css.split(":root")[1]

    print("PASS: test_linux_firstboot_failure_contract")


if __name__ == "__main__":
    main()
