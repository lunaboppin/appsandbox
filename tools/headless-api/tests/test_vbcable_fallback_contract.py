"""Contract: a VM under Secure Boot still gets sound, through VB-CABLE.

Secure Boot blocks the test-signed AppSandbox audio driver. When it is on and
our driver doesn't run, the agent downloads VB-CABLE (VB-Audio donationware,
Microsoft-signed) from vb-audio.com, checks the setup's signature, installs it
silently in the background with retries, and tells the host where it came
from, as VB-Audio's licence asks. The audio helper captures the default
speaker, so it records VB-CABLE as well as our driver.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    agent = read("tools/agent/agent.c")
    audio = read("tools/agent/appsandbox-audio.c")

    thread = agent[agent.index("static DWORD WINAPI vbcable_thread("):]
    thread = thread[:thread.index("\n}\n")]
    # Only when needed: Secure Boot on and our driver not running.
    assert "Confirm-SecureBootUEFI" in thread and "exit 3" in thread
    assert "'*App Sandbox Virtual Audio*' -Status OK" in thread
    assert "'*VB-Audio Virtual Cable*'" in thread
    # From the vendor, signature checked, silent.
    assert "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip" in thread
    assert "Get-AuthenticodeSignature" in thread
    assert thread.index("Get-AuthenticodeSignature") < thread.index("Start-Process")
    assert "-ArgumentList '-i','-h'" in thread
    # Background, retried, and the origin is shown to the user.
    assert "WaitForSingleObject(g_stop_event, 60000)" in thread
    assert "vb-cable.com" in thread and "donationware" in thread
    assert "CreateThread(NULL, 0, vbcable_thread, NULL, 0, NULL)" in agent
    start = agent[agent.index("    ensure_vad_installed();\n\n"):]
    assert start.index("ensure_fallback_audio();") < start.index("ensure_hyperv_video_disabled(NULL);")

    # Helper: the default speaker first, the VAD by name as the fallback.
    find = audio[audio.index("static IMMDevice *find_vad_device(void)"):]
    find = find[:find.index("\n}\n")]
    assert "GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pFound)" in find
    assert find.index("GetDefaultAudioEndpoint") < find.index("EnumAudioEndpoints")
    print("[ok] VB-CABLE fallback audio under Secure Boot contract")


if __name__ == "__main__":
    main()
