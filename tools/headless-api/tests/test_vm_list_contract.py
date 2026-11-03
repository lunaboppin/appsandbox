"""Contract: the VM list keeps working after a VM is deleted.

A row's buttons act on the VM's index, so a row that moves up when one above it is
deleted has to be rebuilt, or its buttons point past the end of the list and do
nothing. Deleting a VM that hasn't run since the app started has no network to remove,
so it shouldn't try (and log an HCN error).
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    js = read("web/app.js")
    hcn = read("src/backend_win/hcn_network.c")

    render = js[js.index("function renderVmTable"):js.index("function makeIconCell")]
    sig = render[render.index("var sig = ["):render.index("].join('|')")]
    assert "\n            i,\n" in sig.replace("\r\n", "\n"), "the row signature must include its index"

    for fn in ("HRESULT hcn_delete_network(", "HRESULT hcn_delete_endpoint("):
        body = hcn[hcn.index(fn):]
        body = body[:body.index("\n}")]
        assert "IsEqualGUID(" in body and "&GUID_NULL" in body, fn

    print("VM list contract verified.")


if __name__ == "__main__":
    main()
