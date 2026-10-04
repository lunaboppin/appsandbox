# ViGEm (virtual game controllers)

Used by `appsandbox-input.exe` to present the host's game controllers to the
guest as Xbox 360 pads.

| Folder | What | Version | License |
|---|---|---|---|
| `ViGEmClient/` | Client library source (`include/`, `src/ViGEmClient.cpp`, `Internal.h`, `UniUtil.h`), compiled into appsandbox-input.exe | github.com/nefarius/ViGEmClient @ b66d02d57e32cc8595369c53418b843e958649b4 | MIT (`ViGEmClient/LICENSE`) |
| `ViGEmBus/ViGEmBus_Setup.exe` | Nefarius' signed bus-driver installer, renamed from `ViGEmBus_1.22.0_x64_x86_arm64.exe` | github.com/nefarius/ViGEmBus v1.22.0 | BSD-3-Clause (`ViGEmBus/LICENSE`) |

Both upstream projects are archived (no longer maintained) but remain usable.
The installer keeps its original Authenticode signature (Nefarius Software
Solutions e.U.); `tools/sign/make-release.ps1` excludes it from re-signing.
The guest agent installs it silently (`/exenoui /qn /norestart`) when the
`ViGEmBus` service is missing.
