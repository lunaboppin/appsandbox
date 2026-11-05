"""What App Sandbox downloads for its guests is checked before it's used: Ubuntu
packages against the archive's signed InRelease, the D3D mapping layers against
Microsoft's digest, and guest source only from this release's tag."""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    av = read("tools/iso-patch/apt_verify.c")
    pf = read("tools/iso-patch/prefetch_build_deps.c")
    proj = read("tools/iso-patch/iso-patch.vcxproj")
    d3d = read("src/backend_win/d3dlayers.c")
    repo = read("tools/iso-patch/prefetch_repo.c")
    isop = read("tools/iso-patch/iso-patch.c")
    core = read("src/backend_win/asb_core.c")

    # Ubuntu: the 2018 archive key, a v4 RSA text signature, and the hash chain.
    assert "0xf6, 0xec, 0xb3, 0x76, 0x24, 0x74, 0xed, 0xa9, 0xd2, 0x1b" in av
    assert "static const unsigned char UBUNTU_2018_N[512]" in av
    assert "b[0] != 4 || b[1] != 0x01 || b[2] != 1" in av and "BCRYPT_PAD_PKCS1" in av
    assert "int apt_filename_ok" in av and '"pool/", 5' in av
    assert '<ClCompile Include="apt_verify.c" />' in proj
    assert 'L"https://archive.ubuntu.com/ubuntu"' in pf and 'L"http://archive.ubuntu.com' not in pf
    fetch = pf[pf.index("static int fetch_verified_packages"):pf.index("int do_prefetch_build_deps")]
    assert "apt_inrelease_verify(data, len, &text, &text_len) != 0" in fetch
    assert 'apt_release_sha256(text, text_len, "main/binary-" IP_DEB_ARCH_A "/Packages.xz"' in fetch
    assert "_stricmp(actual, want) != 0" in fetch
    main_fn = pf[pf.index("int do_prefetch_build_deps"):]
    assert "if (fetch_verified_packages(mirror, codename, staging, pkgs_xz) != 0)" in main_fn
    assert "if (found < 0) return -1;" in main_fn
    assert "if (!apt_filename_ok(fn_utf8, r->filename_len))" in main_fn
    assert "r->sha256_len == 64" in main_fn and "if (sha_utf8[0])" not in main_fn

    # D3D mapping layers: the FE3 digest is required and checked; no unchecked fallback.
    assert "static BOOL file_digest_matches" in d3d
    assert "if (!file_digest_matches(appx, algo, digest))" in d3d
    assert "fallback: first delivery.mp CDN url" not in d3d
    assert '!attr_in(tag, "DigestAlgorithm", algo, algo_cch)' in d3d
    assert "DL_VERIFIED_STAMP" in d3d and "if (!file_exists(stamp)) present = 0;" in d3d

    # Guest source: this version's tag, never a branch.
    assert "refs/tags/%s" in repo and "refs/heads/" not in repo
    assert "not a release tag" in repo
    assert '--tag' in isop and '"--branch"' not in isop
    assert 'L"--prefetch-repo --tag \\"v%s\\" "' in core and "ASB_CORE_VERSION_W, exe_dir, extras);" in core
    assert '--branch \\"main\\"' not in core
    print("[ok] download integrity contract")


if __name__ == "__main__":
    main()
