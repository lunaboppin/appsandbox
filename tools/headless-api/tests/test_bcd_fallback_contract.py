"""Contract: a Windows VM still gets a BCD store when bcdboot can't make one.

bcdboot loads a new store under the fixed key HKLM\\BCD00000000, which collides
with the host's own system store on some hosts (seen on 26300.9711, BFSVC
c0000035), and the image's own bcdboot fails the same way. It has copied the boot
files by then, so iso-patch writes the store with bcdedit, which loads it under a
key of its own. hd_partition keeps host VHDX paths out of the store. The store has
to be marked as a system store or Setup's specialize pass stops on it, and that
marking uses the offline registry library because loading a hive fails on these
hosts too.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> None:
    src = (ROOT / "tools/iso-patch/iso-patch.c").read_text(encoding="utf-8")

    # Only after both bcdboots have failed, and it can still fail the build.
    step = src[src.index("Step 10: Set up UEFI boot files"):]
    host = step.index('L"%s\\\\bcdboot.exe')
    image = step.index('L"%sWindows\\\\System32\\\\bcdboot.exe')
    fallback = step.index("write_bcd_store(efi_clean, efi_vol, win_vol, image_locale)")
    failure = step.index("Failed to install boot files")
    assert host < image < fallback < failure

    store = src[src.index("static BOOL write_bcd_store("):]
    store = store[:store.index("\n}\n")]
    # Needs the boot files on the ESP, and starts from a clean store.
    assert "bootmgfw.efi" in store and "cannot write the BCD store" in store
    assert "DeleteFileW(store);" in store
    assert "/createstore" in store
    # hd_partition, never partition=: that records the host's VHDX path.
    assert store.count("hd_partition=%s") == 3 and "device partition=" not in store
    assert "winload.efi" in store and "{bootmgr} default" in store
    assert "return mark_bcd_system_store(store);" in store

    mark = src[src.index("static BOOL mark_bcd_system_store("):]
    mark = mark[:mark.index("\n}\n")]
    assert 'LoadLibraryExW(L"offreg.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)' in mark
    for value in ('L"System"', 'L"TreatAsSystem"', 'L"KeyName"'):
        assert value in mark, value
    assert "RegLoadKey" not in mark.replace("RegLoadKey/reg.exe", "")

    # A child that can't load a DLL fails instead of waiting on an OK click.
    assert "SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);" in src

    print("OK")


if __name__ == "__main__":
    main()
