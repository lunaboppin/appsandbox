# Third-Party Notices

AppSandbox is licensed under the MIT License (see [LICENSE](LICENSE)). That MIT
grant covers **only AppSandbox's own source code**. The repository also bundles
the third-party components listed below, which are **not** licensed under the
MIT License and remain under their own licenses and copyrights. Each vendored
source file additionally carries its own `SPDX-License-Identifier` header, which
governs that file.

---

## tools/linux/dxgkrnl/ — Microsoft WSL2 `dxgkrnl` GPU driver

- **License:** GNU General Public License, version 2 (`SPDX: GPL-2.0`).
  A verbatim copy of the license text is provided at
  [`tools/linux/dxgkrnl/COPYING`](tools/linux/dxgkrnl/COPYING).
- **Copyright:** Copyright (c) Microsoft Corporation.
- A vendored copy of the WSL2 paravirtualized GPU kernel driver. Every file
  carries its own `SPDX-License-Identifier: GPL-2.0` header. This component is
  GPL-2.0 and is *not* relicensed under MIT; it is included as a separate work
  (mere aggregation in the same repository).

## tools/iso-patch/engine/xz/ — XZ Embedded decoder

- **License:** BSD Zero Clause License (`SPDX: 0BSD`).
- **Copyright:** Lasse Collin and the XZ Embedded contributors.
- An embedded XZ/LZMA decompressor. Every file carries its own
  `SPDX-License-Identifier: 0BSD` header. 0BSD is a public-domain-equivalent,
  MIT-compatible permissive license.

## tools/linux/wsl-mesa/prebuilt/ — Mesa 3D Graphics Library (prebuilt binary)

- **License:** MIT License (the Mesa project's overall license; some bundled
  components are under other compatible permissive licenses, e.g. the SGI Free
  Software License B). The headline MIT license text is shipped at
  [`tools/linux/wsl-mesa/prebuilt/MESA-LICENSE.txt`](tools/linux/wsl-mesa/prebuilt/MESA-LICENSE.txt);
  the full per-component listing is at <https://docs.mesa3d.org/license.html>.
- **Copyright:** Copyright (c) the Mesa contributors.
- `tools/linux/wsl-mesa/prebuilt/ubuntu-26.04-amd64/wsl-mesa.tar.zst` is an
  **unmodified upstream build** of Mesa 25.3.6 (gallium `llvmpipe`/`d3d12`,
  Vulkan `swrast`/`dzn`), produced by the in-repo `build/build-mesa.sh` with no
  source modifications. It contains Mesa's own binaries only; LLVM (used by
  `llvmpipe`) is an external `apt` dependency installed on the guest and is
  **not** bundled in this repository. The surrounding `tools/linux/wsl-mesa/`
  scripts and configs are AppSandbox's own code (MIT).

## tools/vdd/ThirdParty/wdf/ — Microsoft Windows Driver Framework (WDF)

- **License:** Microsoft Software License Terms for the Windows Driver Kit
  (redistributable SDK headers). See
  <https://learn.microsoft.com/en-us/legal/windows-sdk/>.
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/webview2/ — Microsoft Edge WebView2 SDK

- **License:** Microsoft Software License Terms — Microsoft Edge WebView2 SDK
  (redistributable). See
  <https://learn.microsoft.com/en-us/microsoft-edge/webview2/>.
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/devcon/devcon.exe — Microsoft DevCon utility

- **License:** Microsoft Software License Terms for the Windows Driver Kit
  (redistributable tool).
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/vigem/ViGEmClient/ — ViGEm client library (virtual game controllers)

- **License:** MIT (`SPDX: MIT`). License text:
  [`vendor/vigem/ViGEmClient/LICENSE`](vendor/vigem/ViGEmClient/LICENSE).
- **Copyright:** Copyright (c) Nefarius Software Solutions e.U. and contributors.
- Source compiled into `appsandbox-input.exe` (guest), unmodified.

## vendor/vigem/ViGEmBus/ViGEmBus_Setup.exe — ViGEmBus driver installer

- **License:** BSD 3-Clause (`SPDX: BSD-3-Clause`). License text:
  [`vendor/vigem/ViGEmBus/LICENSE`](vendor/vigem/ViGEmBus/LICENSE).
- **Copyright:** Copyright (c) 2016-2020, Nefarius Software Solutions e.U.
- Nefarius' signed v1.22.0 installer, redistributed unmodified (renamed) and
  installed into Windows guests by the agent.

## vendor/qemu-ivshmem/ — QEMU + its runtime dependencies (bundled macOS VMM)

- **License:** QEMU — GNU General Public License, version 2 (`SPDX: GPL-2.0`);
  GLib and gettext/libintl — GNU Lesser General Public License, version 2.1
  (`SPDX: LGPL-2.1`); PCRE2 — BSD ("PCRE2 Licence"); EDK2/TianoCore guest UEFI
  firmware — BSD-2-Clause-Patent (`SPDX: BSD-2-Clause-Patent`).
- **Copyright:** the QEMU, GLib, GNU gettext, PCRE2 (University of Cambridge),
  and TianoCore/EDK2 contributors.
- The macOS host runs Windows guests on a vendored, patched build of **QEMU
  11.0.1** (ivshmem-plain on HVF) together with its GLib / gettext / PCRE2
  runtime and the EDK2 UEFI firmware, embedded under
  `AppSandbox.app/Contents/Resources/qemu/`. QEMU runs as a separate process
  (aggregated with, not linked into, the app); the LGPL dylibs are dynamically
  linked (LGPL §6 relink clause satisfied). Per-component notices, the GPL/LGPL
  written offer, and corresponding-source pointers are in
  [`vendor/qemu-ivshmem/LICENSES/NOTICE.md`](vendor/qemu-ivshmem/LICENSES/NOTICE.md)
  and [`vendor/qemu-ivshmem/WRITTEN_OFFER.txt`](vendor/qemu-ivshmem/WRITTEN_OFFER.txt)
  (both also shipped in the app bundle); `vendor/qemu-ivshmem/fetch-sources.sh`
  retrieves the corresponding source tarballs.

## vendor/virtio-win/ — NetKVM (Red Hat VirtIO Ethernet Adapter)

- **License:** BSD 3-Clause (`SPDX: BSD-3-Clause`). License text:
  [`vendor/virtio-win/virtio-win_license.txt`](vendor/virtio-win/virtio-win_license.txt).
- **Copyright:** Copyright (c) Red Hat, Inc. and the virtio-win contributors.
- `vendor/virtio-win/netkvm-arm64.zip` is the unmodified, WHQL-signed ARM64
  NetKVM virtio-net guest driver, JIT-staged into a Mac-hosted Windows guest for
  NAT networking. Redistributed unmodified; the license text is staged alongside
  the driver in the guest.

---

## AppSandbox's own Linux kernel module (not third-party)

`tools/linux/asb_drm/` is **AppSandbox's own** code, intentionally dual-licensed
under **MIT OR GPL-2.0** (`SPDX-License-Identifier: (GPL-2.0 OR MIT)`) and
declaring `MODULE_LICENSE("Dual MIT/GPL")`. The dual license lets the module
resolve GPL-only kernel symbols (`EXPORT_SYMBOL_GPL` DRM/KMS helpers) when it is
built into or loaded by the Linux kernel, while keeping the source available
under MIT for everyone else. It is listed here only for clarity — it is covered
by AppSandbox's own copyright, not a third party's.
