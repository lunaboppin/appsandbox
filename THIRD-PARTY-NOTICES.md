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

## Inno Setup (the installer's setup program)

- **License:** the Inno Setup License (zlib-style), <https://jrsoftware.org/files/is/license.txt>.
- **Copyright:** Copyright (C) 1997-2026 Jordan Russell. Portions Copyright (C) 2000-2026 Martijn Laan.
- The App Sandbox setup program and uninstaller are built with Inno Setup
  (<https://jrsoftware.org/isinfo.php>). It is a build tool and is not in this repository.

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
