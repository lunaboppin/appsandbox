<img width="1028" height="446" alt="App Sandbox" src="https://github.com/user-attachments/assets/f66dcaff-201e-4cf5-8e23-bc480338f353" />

# App Sandbox

App Sandbox is a virtual machine app for Windows that's focused on performance and ease of use. Point it at a Windows or Ubuntu ISO and it builds the disk, runs an unattended install and boots the VM, with GPU acceleration, sound, clipboard and a fast display out of the box.

It runs on **Windows 11 Home or Pro (x64), without Hyper-V**, and is free and open source (MIT), with prebuilt EV-signed binaries on the [Releases](https://github.com/jamesstringer90/appsandbox/releases) page.

## Features

### Virtual machines
- **Windows 11 and Ubuntu 26.04 LTS** guests, installed with zero touch from your own ISO. Windows VMs get the **Pro** edition from the ISO.
- **Apps on first boot:** pick apps when creating a VM. On Windows that's Claude, Steam, VS Code, Notepad++, Everything, 7-Zip and more, or any winget ID, installed after the first sign-in. On Ubuntu it's any apt or snap name, installed once the VM has internet access.
- **Tidy new installs:** new Windows VMs start with a clean taskbar (just Edge and File Explorer, no search box, widgets or Copilot).
- **Checkpoints and branches:** freeze a VM's state and fork independent working copies from it.
- **Disposable VMs:** every start begins from a clean checkpoint and throws away the last session's changes.
- **Run in Sandbox:** right-click any file in Explorer (under **Show more options** on Windows 11) to copy it into a VM and open it there on a throwaway checkpoint. When you're done, discard rolls the VM back and keep merges the changes. Your original file is only ever read, and shared drives and anything sent over the network aren't rolled back.
- **Templates** for creating Windows VMs from a prepared state instead of reinstalling.
- **Per-VM management:** installer media, storage relocation, grow-only disk resizing, Secure Boot and display preferences. The whole VM tree can be placed on any fixed NTFS/ReFS volume.
- **Backstage session:** like ScreenConnect's Backstage, open a SYSTEM shell in a Windows VM's hidden session 0, in a window of its own, from the display window's menu. The signed-in user, or the sign-in screen, never sees it. It has a taskbar with Command Prompt, PowerShell, Task Manager, Registry Editor and more, plus an Explorer file browser, and everything opened from it runs there too.
- **Remote access, off until you turn it on per VM:** use a VM from a web browser (video, sound, keyboard, mouse, clipboard), on this PC or over HTTPS on your network, with a key shown in Manage. Or stream it to Moonlight: the guest downloads the official Sunshine and you enter the PIN in the app. In Moonlight, **Desktop** works at the sign-in screen and scales to your device; **Desktop (resize to this device)** matches your device's size and needs someone signed in. Windows VMs only for Moonlight.
- **Nested virtualization,** so Docker, WSL and Claude Cowork run inside the VM.

### Display, input and sound
- **Virtual display (IddCx) at 60 Hz that follows the window:** resize the display window and the guest takes exactly that size. Frames are sent as compact NV12 when most of the screen changes (games, video) and losslessly otherwise.
- **Game mode:** the mouse is captured when a game hides the cursor, raw relative mouse input is passed through, and Ctrl+Alt releases it. Keyboard hotkeys can be passed to the guest too.
- **2-channel audio** from the VM, using App Sandbox's own audio driver or, when Secure Boot blocks it, VB-CABLE, installed automatically.
- **Copy and paste** between host and guest.

### GPU
- **GPU acceleration via paravirtualization (GPU-PV):** the host's own GPU is shared with the VM, the same way WSL2 does it, with no passthrough or second GPU. Windows guests get DirectX 12, OpenGL, Vulkan, CUDA and OpenCL; Ubuntu guests the same minus DirectX.
- **Hardware video decode and encode.**

### Shared drives and storage
- **Shared drives** live on a small hidden **shared appliance** VM (Ubuntu with Samba, or Windows Server Core) and appear in every VM, as drive letters on Windows and under `/mnt/appsandbox/<letter>` and in the Files sidebar on Ubuntu, read/write or read-only, with per-VM opt-outs.
- **Mount Host** maps a shared drive in File Explorer on your PC too.

### Networking
- **NAT, external, internal or no network** per VM.
- **WireGuard VPN:** import a `.conf` from your VPN provider and pick it as a VM's network. That VM's traffic goes only through the tunnel; if the tunnel drops, the VM goes offline instead of leaking.
- **SSH** into any VM over a Hyper-V socket, with no network needed.

### AppSandbox domain (optional)
With the Windows Server Core appliance, App Sandbox can run its own **Active Directory domain**:
- **Domain dialog:** manage users and groups (any password is accepted), including who is an administrator on the VMs.
- **Managed VMs:** create a VM as *Managed*, or switch an existing one, and it joins the domain; domain users sign in to it.
- **Roaming files:** each user's Desktop, Documents, Downloads and Pictures live on their own **H:** home drive on the appliance, so they follow them to every managed VM, disposable ones included.
- **Per-group shared drives:** limit a shared drive to domain groups, so only members get it.

### Malware Lab
The **Malware Lab** button builds an isolated, disposable Windows VM for running suspicious files: no shared drives, domain or clipboard, and network off (or through a VPN). Start an analysis, run the sample, and stop it to get an HTML report like an online sandbox's, with:
- a screenshot timeline;
- a process tree with command lines;
- every file created, modified or deleted;
- every registry key and value added or changed, with autostart locations highlighted;
- every network connection.

### Headless API
`appsandbox.exe --headless` starts a local HTTP/JSON API (Docker-style, on `127.0.0.1`), and the dependency-free Python SDK (`asb.py`) drives it: create GPU-accelerated VMs, SSH in over an auto-deployed key, snapshot and branch, open the live display, and run several at once. See [`tools/headless-api/`](tools/headless-api/) for the full reference and examples.

## Requirements

- Windows 11 (Home or Pro) on an x64 processor, with the **Virtual Machine Platform** feature (App Sandbox offers to enable it).
- A Windows 10/11 or Ubuntu ISO for each VM you create.
- For the shared appliance or domain: a Windows Server ISO (for the Server Core appliance), or nothing extra for the Ubuntu one.

## Installing and updating

Download `AppSandbox-<version>-win-x64-setup.exe` from [Releases](https://github.com/lunaboppin/appsandbox/releases) and run it. It installs to Program Files, adds a Start Menu shortcut (and a desktop one if you tick it) and an entry in Apps & features. No driver or service is installed on your PC: the drivers it carries are copied into your VMs when they're provisioned.

**Updates:** App Sandbox checks GitHub once a day, and when a newer release exists it shows a notice at the top of the window. The **Updates** button shows your version, checks on demand, and has the settings for automatic checks and for including pre-releases (off by default).

**Closing the window** leaves App Sandbox running in the notification area, along with your VMs. Click the tray icon or launch the app again to bring the window back; right-click the icon and choose **Exit** to really quit, which stops every VM.

**Your VMs are safe from updates.** Installing exits App Sandbox, and exiting stops every VM, so an update never starts while a VM is running or being built. You're told which VMs are in the way; the update downloads and then waits, and installs by itself once they've all been shut down. App Sandbox then reopens.

**What's checked:** a download must match the SHA-256 GitHub reports for it and carry an intact signature from a certificate pinned in App Sandbox, and its file version must be the one the release advertises. Anything else is deleted and not run.

**Uninstalling** from Apps & features asks whether to also delete your VMs and settings, and keeps them unless you say otherwise. A copy unzipped by hand or built from source doesn't update itself; it links to the release page instead.

See [`tools/installer/README.md`](tools/installer/README.md) to build the installer or make a release.

## Why it doesn't need Hyper-V

App Sandbox doesn't use Hyper-V or Hyper-V Manager. It creates and runs VMs through the Windows **Host Compute System (HCS)** and **Host Compute Network (HCN)** APIs, which need only *Virtual Machine Platform*, the same Windows feature WSL2 uses. Hyper-V is limited to Windows 11 Pro and Enterprise; Virtual Machine Platform is available on Home too, so you don't need a Pro licence on the host.

## What you can use it for

- A full Windows or Ubuntu **desktop** with GPU acceleration, for building and testing your software on a clean install.
- **Gaming** in a VM, with GPU-PV, game mode and low-latency display.
- Running a program kept apart from your main OS and files, in a disposable VM or the Malware Lab.
- A small **multi-user setup:** several people, each with their own domain account and files, on shared VMs.
- Running a computer-use AI agent (such as Claude's computer use) in its own VM. Several agents can run at once, each in its own VM.
- Scripting the VM lifecycle from code with the headless API and Python SDK, for example on a self-hosted CI runner.

## How it works

This repo also aims to be a working example of creating full desktop VMs programmatically with the HCS/HCN APIs:
- **VMs:** App Sandbox submits a hand-built HCS machine document to `computecore.dll` / `computenetwork.dll`, the layer that also underlies WSL2 and Windows Sandbox.
- **GPU:** acceleration uses GPU paravirtualization (GPU-PV) to share the host's installed GPU with the guest.
- **Display and sound:** a custom **IddCx** indirect display driver and virtual audio device carry the screen and sound.
- **Guest channels:** clipboard, audio, input, SSH and the guest agent run over **Hyper-V sockets**.
- **Disks:** guest disks are built by an in-repo tool supporting ext4, squashfs, qcow2 and VHDX.
- **Linux GPU:** Ubuntu guests reach the GPU through a custom DRM/KMS kernel module (`asb_drm`), Microsoft's WSL2 `dxgkrnl` and a custom Mesa build.
- **App:** native C, with an HTML/JS UI in WebView2.

## Tips

- **[Windows] Display menu:** the small tab on the top edge of the VM's display (drag it along the edge to move it) opens a menu for mouse capture, game mode, hotkeys, muting audio, the Backstage session and the log. The same options are in the title bar's right-click menu.
- **[Windows] Remote access:** need high-performance remote desktop into your VM? [Phaze](https://phaze.app) works well.
- **[Windows] Check GPU-PV:** run `gpu-test.exe` ([gpu-test.zip](https://github.com/jamesstringer90/appsandbox/releases/download/v0.1.2/gpu-test.zip)) inside a Windows VM. It shows six rotating cubes, one each for D3D9, D3D10, D3D11, D3D12, OpenGL and Vulkan; any API that fails won't show its cube. Some games and apps still don't detect GPU-PV correctly even when every API works.

## License

AppSandbox is licensed under the [MIT License](LICENSE) — Copyright (c) 2026 James Stringer.

It bundles third-party components that are **not** covered by the MIT license and keep their own terms, most notably the Microsoft WSL2 `dxgkrnl` GPU driver (GPL-2.0) and the embedded XZ decoder (0BSD). See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full list. AppSandbox's own Linux DRM kernel module (`tools/linux/asb_drm/`) is dual-licensed MIT OR GPL-2.0 so it can use the kernel's GPL-only symbols.

VB-CABLE, which App Sandbox can download into a VM for sound, is donationware by [VB-Audio](https://vb-cable.com) and is not bundled.

## Acknowledgements

Beyond my own experience building [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV), I found [NanaBox](https://github.com/M2Team/NanaBox) to be a really helpful resource for understanding HCS.

## Author

[James Stringer](https://www.linkedin.com/in/jamesstringerphotography/) — author of [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV).
