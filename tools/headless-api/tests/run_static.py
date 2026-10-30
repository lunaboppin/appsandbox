"""Run every check that needs no daemon, VM or elevation. This is what CI runs.

run_all.py is the full suite and needs a running daemon and ISOs. This covers its
static half: every test_*_contract.py, the updater against its mock feed and, once a
build exists, the packaged-binary checks.
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

# Reads bin/Release, so it only means something after a build.
AFTER_BUILD = ["test_release_shared_resource_artifacts.py"]


def main():
    tests = sorted(p.name for p in HERE.glob("test_*_contract.py")) + ["test_updater_mock.py"]
    if (ROOT / "bin" / "Release" / "AppSandbox.exe").exists():
        tests += AFTER_BUILD

    failed = []
    for name in tests:
        print(f"--- {name}", flush=True)
        if subprocess.run([sys.executable, str(HERE / name)], cwd=HERE).returncode != 0:
            failed.append(name)

    print(f"\n{len(tests) - len(failed)} of {len(tests)} passed")
    for name in failed:
        print(f"FAILED: {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
