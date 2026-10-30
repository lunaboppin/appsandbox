"""
Master test harness.

  1. Runs the no-VM static checks once (host vs Win32 + create-validation).
  2. Spawns ONE full per-VM suite thread per spec, CONCURRENTLY -- so multi-VM
     concurrency is tested by construction (the daemon juggles every spec's VM at
     once) and the entire suite is proven on each OS simultaneously. There is no
     separate multi-VM test. Edit SPECS to change thread count / create params.
  3. Runs the Windows template lifecycle (separate -- sysprep is a different
     lifecycle from a normal VM).

Then prints a per-thread summary + a coverage table (function -> where tested).
Launch as a background task. Self-cleaning: every VM is a brk-* throwaway.
"""
import sys, os, subprocess, threading
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # parent: for asb.py
sys.path.insert(0, HERE)                     # this dir: for vm_lifecycle + sibling test modules
import asb
from vm_lifecycle import run_vm_lifecycle

# The harness chooses how many threads and each thread's create params.
# A Windows host (HCS) builds Linux + Windows guests concurrently, so
# multi-VM concurrency is tested by construction. Feature differences are
# capability-gated inside vm_lifecycle.
# One uniform guest size everywhere: 4096 MB / 2 cores / 64 GB.
SPECS = [
    {"name": "brk-lin", "os_type": "Linux",   "ram": 4096, "hdd": 64, "cores": 2},
    {"name": "brk-win", "os_type": "Windows", "ram": 4096, "hdd": 64, "cores": 2},
]
RUN_STATIC = True
RUN_TEMPLATE = True   # template create -> sysprep -> from-template (Windows-only)

# function / endpoint -> where it's covered
COVERAGE = [
    ("GET /host (cores/ram vs Win32)",          "static"),
    ("GET /version, /templates",                "static, per-VM"),
    ("POST /vms create -- input validation",    "static (reject) + per-VM (duplicate)"),
    ("POST /vms create -- Linux & Windows",     "per-VM (one thread per OS, concurrently)"),
    ("POST /vms create -- template / from-tpl", "template"),
    ("GET /vms (list), /vms/{n} status",        "per-VM (all threads)"),
    ("PUT /vms/{n} edit + validation",          "per-VM (config-at-start + reject)"),
    ("config applied at start (ram/cpu/gpu/net)","per-VM"),
    ("POST .../start (+ from snapshot/branch)", "per-VM"),
    ("POST .../shutdown (graceful)",            "per-VM"),
    ("POST .../stop (force) + idempotency",     "per-VM"),
    ("POST .../delete (+ no orphan)",           "per-VM"),
    ("delete-during-build guard (409)",         "per-VM (each thread, mid-build)"),
    ("GET .../sshInfo",                         "per-VM"),
    ("SSH key auto-deploy + key-only login",    "per-VM (sshEnabled+sshDeployKey, each OS)"),
    ("GET/POST/DELETE .../display open+close",   "per-VM when online (Win+Linux); skip on no-desktop / Linux-pre-push"),
    ("display boot screen -> auto IDD switch",   "per-VM at the config-applied boot; skip on no-desktop"),
    ("snapshots take/rename/delete + GET tree", "per-VM"),
    ("branches: start-from-snapshot + delete",  "per-VM"),
    ("GET /events (SSE) per VM",                "per-VM (each thread, concurrent SSE)"),
    ("POST /shutdown (+ active-VM guard 409)",  "per-VM (while its VM is running)"),
    ("DELETE /templates/{name}, /templates",    "template"),
    ("multi-VM concurrency (daemon juggles N)", "INHERENT: %d threads run at once" % len(SPECS)),
]

try:
    _c = asb.connect()
    v = _c.version()
    print("daemon reachable: %s %s (%s)" % (v.get("product"), v.get("version"), v.get("hostOs")))
except Exception as e:
    print("PREFLIGHT FAIL: daemon not reachable (%s).\nStart it with: %s"
          % (e, asb.daemon_launch_hint()))
    sys.exit(2)

# Sweep leftover brk-* throwaways from a previous aborted run so a rerun is
# self-healing (every spec creates its VM from scratch).
import helpers as _helpers
for _vm in list(_c.list()):
    if _vm["name"].startswith("brk-"):
        print("sweeping leftover %s..." % _vm["name"], flush=True)
        _helpers.destroy_vm(_c, _vm["name"])

results = {}   # label -> list of failures ([] == pass)

if RUN_STATIC:
    print("\n" + "=" * 70 + "\nSTATIC (no-VM) CHECKS\n" + "=" * 70, flush=True)
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_manage_contract.py")], cwd=HERE)
    results["manage-contract"] = [] if p.returncode == 0 else ["test_manage_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_shared_resources_contract.py")], cwd=HERE)
    results["shared-contract"] = [] if p.returncode == 0 else ["test_shared_resources_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_shared_appliance_contract.py")], cwd=HERE)
    results["appliance-contract"] = [] if p.returncode == 0 else ["test_shared_appliance_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_shared_appliance_retry_contract.py")], cwd=HERE)
    results["appliance-retry"] = [] if p.returncode == 0 else ["test_shared_appliance_retry_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_shared_appliance_autostart_contract.py")], cwd=HERE)
    results["appliance-autostart"] = [] if p.returncode == 0 else ["test_shared_appliance_autostart_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_shared_appliance_lan_only_contract.py")], cwd=HERE)
    results["appliance-lan-only"] = [] if p.returncode == 0 else ["test_shared_appliance_lan_only_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_shared_appliance_setup_network_contract.py")], cwd=HERE)
    results["appliance-setup-network"] = [] if p.returncode == 0 else ["test_shared_appliance_setup_network_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_vpn_contract.py")], cwd=HERE)
    results["vpn"] = [] if p.returncode == 0 else ["test_vpn_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_host_mount_contract.py")], cwd=HERE)
    results["host_mount"] = [] if p.returncode == 0 else ["test_host_mount_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_idd_game_mode_contract.py")], cwd=HERE)
    results["idd_game_mode"] = [] if p.returncode == 0 else ["test_idd_game_mode_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_idd_nv12_contract.py")], cwd=HERE)
    results["idd_nv12"] = [] if p.returncode == 0 else ["test_idd_nv12_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_clean_taskbar_contract.py")], cwd=HERE)
    results["clean_taskbar"] = [] if p.returncode == 0 else ["test_clean_taskbar_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_secure_boot_setting_contract.py")], cwd=HERE)
    results["secure_boot"] = [] if p.returncode == 0 else ["test_secure_boot_setting_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_appliance_domain_contract.py")], cwd=HERE)
    results["appliance_domain"] = [] if p.returncode == 0 else ["test_appliance_domain_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_preinstall_apps_contract.py")], cwd=HERE)
    results["preinstall_apps"] = [] if p.returncode == 0 else ["test_preinstall_apps_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_vbcable_fallback_contract.py")], cwd=HERE)
    results["vbcable"] = [] if p.returncode == 0 else ["test_vbcable_fallback_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_release_shared_resource_artifacts.py")], cwd=HERE)
    results["appliance-artifacts"] = [] if p.returncode == 0 else ["test_release_shared_resource_artifacts.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_display_boot_screen_contract.py")], cwd=HERE)
    results["display-boot-screen"] = [] if p.returncode == 0 else ["test_display_boot_screen_contract.py exit %d" % p.returncode]
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_host_and_validation.py")], cwd=HERE)
    results["static"] = [] if p.returncode == 0 else ["test_host_and_validation.py exit %d" % p.returncode]

print("\n" + "=" * 70 + "\n%d CONCURRENT PER-VM SUITES: %s\n" % (len(SPECS), ", ".join(s["name"] + "/" + s["os_type"] for s in SPECS)) + "=" * 70, flush=True)
threads = []
def worker(spec):
    name, fails = run_vm_lifecycle(spec)
    results[name] = fails
for spec in SPECS:
    t = threading.Thread(target=worker, args=(spec,)); t.start(); threads.append(t)
for t in threads:
    t.join()

if RUN_TEMPLATE:
    print("\n" + "=" * 70 + "\nWINDOWS TEMPLATE LIFECYCLE\n" + "=" * 70, flush=True)
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_windows_template.py")], cwd=HERE)
    results["template"] = [] if p.returncode == 0 else ["test_windows_template.py exit %d" % p.returncode]

print("\n" + "=" * 70 + "\nSUMMARY\n" + "=" * 70)
for label in sorted(results):
    f = results[label]
    print("  %-12s %s" % (label, "PASS" if not f else "FAIL"))
    for x in f:
        print("        - %s" % x)

ran = set(results)
failed = {k for k, f in results.items() if f}
def status_for(where):
    toks = []
    if "static" in where: toks.append("static")
    if "per-VM" in where: toks.append("per-VM")
    if "template" in where: toks.append("template")
    if "INHERENT" in where: toks.append("per-VM")
    keys = set()
    for t in toks:
        if t == "static": keys.add("static")
        elif t == "template": keys.add("template")
        elif t == "per-VM": keys |= {s["name"] for s in SPECS}
    if failed & keys: return "FAIL"
    if keys & ran:    return "PASS"
    return "n/a"

print("\n" + "=" * 70 + "\nCOVERAGE  (function -> where tested)\n" + "=" * 70)
print("  %-44s %-6s %s" % ("function / endpoint", "status", "covered by"))
print("  " + "-" * 68)
for feature, where in COVERAGE:
    print("  %-44s %-6s %s" % (feature, status_for(where), where))

ok = not any(results.values())
print("\n%s" % ("ALL GREEN" if ok else "SOME FAILED"))
sys.exit(0 if ok else 1)
