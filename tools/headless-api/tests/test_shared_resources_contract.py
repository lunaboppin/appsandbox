"""Offline contract checks for custom VM storage and shared resources."""
from __future__ import annotations
import re, shutil, subprocess, sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
def read(p): return (ROOT/p).read_text(encoding="utf-8")
def check(v,m):
    if not v: raise AssertionError(m)

def main():
    core=read("src/backend_win/asb_core.c")
    core_h=read("src/backend_win/asb_core.h")
    shared=read("src/backend_win/shared_resources.c")
    hcs=read("src/backend_win/hcs_vm.c")
    agent=read("tools/agent/agent.c")
    headless=read("src/app_win/headless.c")
    sdk=read("tools/headless-api/asb.py")
    html=read("web/index.html")
    js=read("web/app.js")

    for token in ("storageParent","storage-parent","StorageRoot=","LastStorageParent="):
        check(token in core+core_h+headless+sdk+html+js,f"missing custom-storage contract: {token}")
    check("DRIVE_FIXED" in core and "NTFS" in core and "ReFS" in core,
          "storage validation lacks fixed NTFS/ReFS guards")
    check("remove_dir_recursive(vhdx_dir)" in core,"failed creation does not clean its new tree")

    for key in ("Id=","Name=","Path=","DriveLetter=","ReadOnly=","AclCreated="):
        check(key in shared,f"shared-resource persistence missing {key}")
    for guard in ("PathIsUNCW","DRIVE_FIXED","FILE_ATTRIBUTE_REPARSE_POINT","path_contains"):
        check(guard in shared,f"shared path validation missing {guard}")
    check("ReplaceFileW" in shared and "FlushFileBuffers" in shared,
          "shared resource persistence is not atomic")
    check("S-1-5-83-0" in shared and "REVOKE_ACCESS" in shared,
          "tracked Virtual Machines ACL lifecycle is missing")

    check('L"VirtualSmb"' in hcs or "\\\"VirtualSmb\\\"" in hcs,"HCS document lacks VirtualSmb")
    for token in ("ReadOnly","ShareRead","CacheIo","PseudoOplocks","NoDirectmap"):
        check(token in hcs,f"VSMB options missing {token}")
    check("VSMB-{dcc079ae-60ba-4d07-847c-3493609c0870}" in agent,
          "guest does not map the deterministic VSMB device")
    check("WTSQueryUserToken" in agent and "CreateProcessAsUserW" in agent,
          "drive mapper is not launched in the interactive user session")
    check("drive_collision" in agent,"preferred drive collisions are not reported")
    check("upgrade_windows_agent_offline" in core and "AttachVirtualDisk" in core,
          "existing stopped guests are not upgraded on the active VHDX")
    check("retrying VM without shared resources" in hcs,
          "VSMB rejection can still block VM startup")

    for route in ('/v1/shared-resources','shared-resources/'):
        check(route in headless,f"headless shared-resource route missing {route}")
    for method in ("shared_resources","create_shared_resource","update_shared_resource",
                   "remove_shared_resource","set_vm_shared_resource"):
        check(f"def {method}" in sdk,f"SDK missing {method}")
    ids=re.findall(r'\bid="([^"]+)"',html)
    check(len(ids)==len(set(ids)),"web/index.html contains duplicate element ids")
    node=shutil.which("node")
    if node: check(subprocess.run([node,"--check",str(ROOT/"web/app.js")]).returncode==0,
                   "web/app.js failed node --check")
    print("[ok] custom storage, VSMB resources, ACLs, guest mapping, API, and UI contracts")
    return 0

if __name__=="__main__":
    try: raise SystemExit(main())
    except AssertionError as exc:
        print(f"[fail] {exc}",file=sys.stderr);raise SystemExit(1)
