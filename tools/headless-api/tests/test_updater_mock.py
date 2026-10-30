"""The updater, run for real against a mock release feed and real signed files.

Drives bin/Release/updater-test.exe, which compiles the app's own updater.c. Serves
the feed from 127.0.0.1 (the one place updater.c allows plain HTTP), so this needs no
network, VM or elevation. Skipped, not failed, if the build isn't there.

The good case uses Microsoft's signed WebView2Loader.dll as a stand-in installer: it
is properly signed, and pinning its signer needs no key. If a test-signed installer
has been built, it is checked too, because it takes the other path through the code:
an intact signature whose root Windows does not trust.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BIN = ROOT / "bin" / "Release"
TOOL = BIN / "updater-test.exe"
CURRENT = "0.1.7.0"
ZERO = "0" * 64
failures = []


def run(*args):
    p = subprocess.run([str(TOOL), *map(str, args)], capture_output=True)
    return p.returncode, p.stdout.decode("utf-16-le", "replace") if b"\0" in p.stdout else p.stdout.decode("mbcs", "replace")


def expect(label, ok, detail=""):
    print(("[ok] " if ok else "[FAIL] ") + label + ("" if ok else "  -> " + str(detail).strip()))
    if not ok:
        failures.append(label)


def asset(version, base, name=None, digest="sha256:" + "a" * 64, state="uploaded", size=1000, url=None):
    name = name or f"AppSandbox-{version}-win-x64-setup.exe"
    return {"name": name, "state": state, "size": size, "digest": digest,
            "browser_download_url": url or f"{base}/releases/download/v{version}/{name}",
            "uploader": {"login": "x", "nested": {"name": "not the asset name"}}}


def release(version, base, assets=None, prerelease=False, draft=False):
    return {"tag_name": f"v{version}", "html_url": f"{base}/releases/tag/v{version}",
            "name": 'notes with "quotes" and \\u00e9', "draft": draft, "prerelease": prerelease,
            "body": "x" * 5000, "author": {"login": "someone", "name": "tag_name"},
            "assets": assets if assets is not None else [asset(version, base)]}


class Feed(BaseHTTPRequestHandler):
    body = b"[]"
    status = 200
    requests = []
    blobs = {}

    def do_GET(self):
        Feed.requests.append((self.path, self.headers.get("User-Agent", "")))
        if self.path in Feed.blobs:
            data, self_status = Feed.blobs[self.path], 200
        else:
            data, self_status = Feed.body, Feed.status
        self.send_response(self_status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def check_cases(base):
    def check(feed, *extra, status=200, raw=None):
        Feed.status = status
        Feed.body = raw if raw is not None else json.dumps(feed).encode()
        return run("check", "--api", base + "/api/releases?per_page=30", "--asset-prefix", base + "/releases/download/",
                   "--release-prefix", base + "/releases/", "--current", CURRENT, *extra)

    rc, out = check([])
    expect("empty feed: up to date", rc == 0 and out.startswith("none"), out)
    expect("the query string reaches the server", Feed.requests[-1][0] == "/api/releases?per_page=30", Feed.requests[-1])
    expect("identifies itself with a User-Agent", Feed.requests[-1][1].startswith("AppSandbox/"), Feed.requests[-1])

    rc, out = check([release("0.1.8.0", base)])
    expect("newer release offered", rc == 0 and "available version=0.1.8.0 " in out and "tag=v0.1.8.0" in out, out)
    expect("carries the digest and page", "sha256=" + "a" * 64 in out and "/releases/tag/v0.1.8.0" in out, out)

    rc, out = check([release(CURRENT, base), release("0.1.6.1", base)])
    expect("same or older: not offered", rc == 0 and out.startswith("none"), out)

    rc, out = check([release("0.1.8.0", base), release("0.1.10.0", base), release("0.1.9.0", base)])
    expect("highest wins, numerically (10 > 9)", "version=0.1.10.0" in out, out)

    rc, out = check([release("0.2.0.0", base, draft=True), release("0.1.8.0", base)])
    expect("drafts ignored", "version=0.1.8.0" in out, out)

    feed = [release("0.2.0.0", base, prerelease=True), release("0.1.8.0", base)]
    rc, out = check(feed)
    expect("pre-releases ignored by default", "version=0.1.8.0" in out, out)
    rc, out = check(feed, "--prerelease")
    expect("pre-releases offered when asked for", "version=0.2.0.0" in out and "prerelease=1" in out, out)

    # What's on the real feed today: pre-releases that carry only a zip.
    rc, out = check([{**release("0.1.9.0", base, prerelease=True), "tag_name": "v0.1.9.0-immersive",
                      "assets": [asset("0.1.9.0", base, name="AppSandbox-0.1.9.0-win-x64-testsigned.zip")]}], "--prerelease")
    expect("a release without an installer is skipped", rc == 0 and out.startswith("none"), out)

    bad_assets = {
        "wrong architecture": asset("0.3.0.0", base, name="AppSandbox-0.3.0.0-win-arm64-setup.exe"),
        "version not numeric": asset("0.3.0.0", base, name="AppSandbox-0.3.0.x-win-x64-setup.exe"),
        "path in the name": asset("0.3.0.0", base, name="AppSandbox-..\\..\\evil-win-x64-setup.exe"),
        "no digest": asset("0.3.0.0", base, digest=None),
        "malformed digest": asset("0.3.0.0", base, digest="sha256:abc"),
        "non-SHA-256 digest": asset("0.3.0.0", base, digest="md5:" + "a" * 64),
        "upload unfinished": asset("0.3.0.0", base, state="starter"),
        "zero size": asset("0.3.0.0", base, size=0),
        "absurd size": asset("0.3.0.0", base, size=5 * 1024 ** 3),
        "hosted elsewhere": asset("0.3.0.0", base, url="https://evil.example/AppSandbox-0.3.0.0-win-x64-setup.exe"),
        "plain HTTP elsewhere": asset("0.3.0.0", base, url="http://evil.example/releases/download/x"),
    }
    for label, bad in bad_assets.items():
        rc, out = check([release("0.3.0.0", base, assets=[bad])])
        expect(f"asset refused: {label}", rc == 0 and out.startswith("none"), out)

    rc, out = check([{**release("0.1.8.0", base), "html_url": "https://evil.example/x"}])
    expect("foreign release page dropped, update kept", "version=0.1.8.0" in out and "evil" not in out, out)

    rc, out = check(None, raw=b'[{"tag_name": "v1", "assets": [')
    expect("truncated feed is an error", rc == 1 and out.startswith("error"), out)
    rc, out = check(None, raw=b'{"message": "Not Found"}')
    expect("non-list answer is an error", rc == 1 and out.startswith("error"), out)
    rc, out = check(None, raw=b"[" * 200000)
    expect("deeply nested feed is an error, not a crash", rc == 1, (rc, out))
    rc, out = check([], status=403)
    expect("rate limit reported as such", rc == 1 and "rate limit" in out, out)
    rc, out = check([], status=500)
    expect("server error reported", rc == 1 and "500" in out, out)

    rc, out = run("check", "--api", "http://evil.example/releases", "--asset-prefix", "x", "--release-prefix", "x")
    expect("plain HTTP refused except to loopback", rc == 1 and "non-HTTPS" in out, out)


def download_cases(base, work):
    payload = os.urandom(300000)
    Feed.blobs["/blob"] = payload
    dest = work / "dl.exe"
    rc, out = run("download", "--url", base + "/blob", "--size", len(payload), "--dest", dest)
    expect("download writes the file", rc == 0 and dest.read_bytes() == payload, out)
    expect("no .part left behind", not (work / "dl.exe.part").exists())

    dest = work / "short.exe"
    rc, out = run("download", "--url", base + "/blob", "--size", len(payload) + 1, "--dest", dest)
    expect("short download refused", rc == 1 and not dest.exists() and not (work / "short.exe.part").exists(), out)
    dest = work / "long.exe"
    rc, out = run("download", "--url", base + "/blob", "--size", len(payload) - 1, "--dest", dest)
    expect("oversized download refused", rc == 1 and not dest.exists(), out)
    rc, out = run("download", "--url", "http://evil.example/x", "--size", 1, "--dest", work / "x.exe")
    expect("download over plain HTTP refused", rc == 1 and "non-HTTPS" in out, out)


def file_version(path):
    script = f"(Get-Item -LiteralPath '{path}').VersionInfo | ForEach-Object {{ '{{0}}.{{1}}.{{2}}.{{3}}' -f $_.FileMajorPart,$_.FileMinorPart,$_.FileBuildPart,$_.FilePrivatePart }}"
    return subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True).stdout.strip()


def verify_cases(signed, label, work):
    rc, out = run("signer", "--file", signed)
    expect(f"{label}: signer read", rc == 0 and "signer=" in out, out)
    if rc != 0:
        return
    pin = out.split("signer=")[1].split()[0]
    status = out.split("status=")[1].strip()
    print(f"     {label}: signer {pin[:16]}..., WinVerifyTrust {status}")
    sha = hashlib.sha256(signed.read_bytes()).hexdigest()
    version = file_version(signed)

    def verify(path=signed, digest=sha, ver=version, pins=(pin,)):
        args = ["verify", "--file", path, "--sha256", digest, "--version", ver]
        for p in pins:
            args += ["--pin", p]
        return run(*args)

    rc, out = verify()
    expect(f"{label}: accepted with the right hash, pin and version", rc == 0 and "verified" in out, out)
    rc, out = verify(digest=sha.upper())
    expect(f"{label}: hash case doesn't matter", rc == 0, out)
    rc, out = verify(pins=("A" * 64, pin.lower()))
    expect(f"{label}: any pin in the list will do", rc == 0, out)

    rc, out = verify(digest=ZERO)
    expect(f"{label}: wrong hash refused", rc == 1 and "SHA-256" in out, out)
    rc, out = verify(digest="")
    expect(f"{label}: missing hash refused", rc == 1, out)
    rc, out = verify(pins=("A" * 64,))
    expect(f"{label}: valid signature by an unpinned publisher refused", rc == 1 and "unknown publisher" in out, out)
    rc, out = verify(pins=())
    expect(f"{label}: no pins, nothing passes", rc == 1, out)
    rc, out = verify(ver="99.0.0.0")
    expect(f"{label}: replay under another version refused", rc == 1 and "version" in out, out)

    data = bytearray(signed.read_bytes())
    # Tamper with the code and with the signature blob; each keeps the hash "right",
    # as an attacker controlling the feed would, so only the signature can catch it.
    pe = int.from_bytes(data[0x3C:0x40], "little")
    magic = int.from_bytes(data[pe + 24:pe + 26], "little")
    cert_dir = pe + 24 + (144 if magic == 0x20B else 128)
    cert_off = int.from_bytes(data[cert_dir:cert_dir + 4], "little")
    cert_len = int.from_bytes(data[cert_dir + 4:cert_dir + 8], "little")
    for what, where in (("code", cert_off // 2), ("signature", cert_off + cert_len - 40)):
        bad = bytearray(data)
        bad[where] ^= 0xFF
        path = work / f"tampered-{what}-{signed.name}"
        path.write_bytes(bad)
        rc, out = verify(path=path, digest=hashlib.sha256(bad).hexdigest())
        expect(f"{label}: tampered {what} refused even with a matching hash", rc == 1, out)

    # Pasting a trusted signature blob onto other code.
    unsigned = BIN / "iso-patch.exe"
    if unsigned.exists():
        rc, out = run("signer", "--file", unsigned)
        if "signer=none" in out:
            rc, out = verify(path=unsigned, digest=hashlib.sha256(unsigned.read_bytes()).hexdigest())
            expect("unsigned file refused", rc == 1 and "valid signature" in out, out)


def main():
    if not TOOL.exists():
        print(f"[skip] {TOOL} not built")
        return 0

    rc, out = run("compare", "0.1.10.0", "0.1.9.0")
    expect("versions compare as numbers", out.strip() == "1", out)
    rc, out = run("compare", "0.1.7", "0.1.7.0")
    expect("a missing part counts as zero", out.strip() == "0", out)
    rc, out = run("compare", "garbage", "0.0.0.1")
    expect("garbage is older than anything", out.strip() == "-1", out)

    server = HTTPServer(("127.0.0.1", 0), Feed)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    work = Path(tempfile.mkdtemp(prefix="asb-updater-"))
    try:
        check_cases(base)
        download_cases(base, work)
        verify_cases(ROOT / "vendor" / "webview2" / "x64" / "WebView2Loader.dll", "Microsoft-signed", work)
        for setup in sorted((BIN / "installer").glob("AppSandbox-*-setup.exe")) if (BIN / "installer").exists() else []:
            verify_cases(setup, "installer", work)
    finally:
        server.shutdown()
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("[ok] updater against the mock feed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
