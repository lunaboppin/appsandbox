"""Regression contract for retrying an interrupted appliance setup."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> None:
    appliance = (ROOT / "src/backend_win/shared_appliance.c").read_text(encoding="utf-8")
    ui = (ROOT / "src/app_win/ui.c").read_text(encoding="utf-8")

    required = {
        "owned_stale_root": "setup does not distinguish its stale managed root",
        "_wcsicmp(g_appliance.status.storage_root, root) == 0": "stale-root ownership is not path-bound",
        "!owned_stale_root": "unrelated collision protection was removed",
        "format_hresult_message": "WebView setup failures still discard the HRESULT message",
        'jb_string(&sj,L"message"': "WebView setup result has no actionable error text",
    }
    for token, failure in required.items():
        if token not in appliance + ui:
            raise AssertionError(failure)

    print("[ok] interrupted appliance setup retry and error-reporting contract")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, FileNotFoundError) as exc:
        print(f"[fail] {exc}")
        raise SystemExit(1)
