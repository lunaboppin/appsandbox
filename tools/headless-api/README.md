# AppSandbox headless SDK (`asb.py`)

A small, dependency-free Python client for driving AppSandbox programmatically —
create, configure, snapshot, and run GPU-enabled VMs without the GUI. This README
is the usage/API reference.

```python
import asb

c = asb.connect()                      # auto-discovers the running daemon
c.create(name="dev", osType="Linux", imagePath=r"C:\ISOs\ubuntu.iso",
         ramMb=4096, cpuCores=2, adminUser="user", adminPass="test123")
c.wait_online("dev")                   # blocks through build + install + boot
print(c.ssh_info("dev"))               # {'host': '127.0.0.1', 'port': ..., ...}
c.shutdown("dev")                      # graceful ACPI power-off
c.delete_vm("dev")
```

---

## How it works

`appsandbox.exe --headless` starts a **single-owner daemon**. It hosts the
AppSandbox core (`appsandbox_core.dll`) for the lifetime of the process and
exposes it as a Docker-style local HTTP/JSON API. The daemon *owns* the
VMs: they run as long as it runs (exit terminates them), and the API is just a
remote control for the same core the GUI drives.

- **Loopback only.** The listener binds `http://127.0.0.1:<port>` via
  `http.sys`. Nothing is exposed off-box.
- **Discovery file.** On startup the daemon writes `host.json`
  (`%ProgramData%\AppSandbox\`) with the endpoint, port, and a random per-run
  bearer token. `asb.connect()` reads it, so you never hard-code a port or token.
- **Single instance.** A global mutex guarantees
  one AppSandbox at a time — GUI or daemon (a second `--headless` exits with
  status 2). One owner, one source of truth.
- **Serialized core.** The HTTP receive loop *is* the single command thread:
  one request runs to completion before the next, so all core calls are
  serialized — no interleaving, no races against the core.
- **Capabilities, not API forks.** `version()["capabilities"]` advertises what
  the host supports (snapshots, templates); unsupported routes
  return `501`. Everything else — status model, validation, SSE, SSH key
  deploy — is identical.

### Security model — read this before "hardening" it

The API is **admin-gated, and that gate is functional, not a security boundary.**
Creating, starting, and snapshotting VMs go through the Host Compute Service
(HCS), which requires elevation; binding the `http.sys` URL likewise needs
elevation. So the daemon must run elevated, and only an administrator can launch
it. The bearer token exists to stop *other processes* from stumbling onto the
port, not to defend against an authenticated administrator on the same machine —
that is explicitly **out of scope**. The threat model is single-user. Do not add
auth layers, DACL tightening, or token ceremony that the local single-user model
doesn't justify.

---

## Platform support at a glance

The host runs **Windows** and **Linux** (Ubuntu) guests. Guest support is
enforced in create-validation (wrong `osType` → `400`). Snapshots & branches,
templates (`isTemplate`, create-from-template), the display window
(`/vms/{n}/display`), GPU, NAT/network modes, SSH server, SSH-key auto-deploy
and SSE events are all supported. `snapshots`/`templates` are advertised in
`version()["capabilities"]` (unsupported routes → `501`).

## Requirements

| Requirement | Why |
|---|---|
| **Windows 11, build 22000+** | The bundled virtual display (VDD) and audio (VAD) drivers are Windows-11-only (their INFs target `10.0…22000`; the VDD uses the Win11 UMDF reflector). |
| **Administrator** | HCS VM operations and the `http.sys` bind both require elevation. |
| **`VirtualMachinePlatform` feature** | The HCS backend the core uses. Enable with `dism /online /Enable-Feature /FeatureName:VirtualMachinePlatform /All` then reboot. |

A missing prerequisite makes `--headless` **fail fast with a clear message** and
a non-zero exit — it never half-starts:

- not elevated → "must be run as Administrator…"; unsupported OS →
  "requires Windows 11, build 22000 or newer"; feature off → "requires the
  'VirtualMachinePlatform' Windows feature…"
- a second instance (GUI or daemon already running) → exit 2

---

## Getting started

1. **Start the daemon**:
   ```
   # elevated shell
   appsandbox.exe --headless
   ```
   It runs in the foreground-less GUI subsystem. Diagnostics go to
   `headless.log` next to the discovery file.

2. **Connect** from Python (stdlib only — no `pip install`):
   ```python
   import asb
   c = asb.connect()
   print(c.version())   # {'product': 'AppSandbox', 'version': '0.1.4', ...}
   ```

`asb.py` has no third-party dependencies; copy it next to your script, or add
`tools/headless-api` to `sys.path` (the examples do the latter).

---

## VM lifecycle

A VM's `state` is **derived** from the core's flags, so it's always live —
there's no cached status to go stale:

```
building → stopped → installing → booting → online → stopping → stopped
   │           ▲          (running = true ─────────────┘)
   └─ vhdx build         agent connects at "online"
```

| state | meaning |
|---|---|
| `building` | the VHDX is being created/staged (the build worker holds the disk) |
| `stopped` | powered off |
| `installing` | powered on, guest OS still installing (`installComplete` = false) |
| `failed` | powered on, first-boot setup on a Linux guest can't finish; the reason is in `installError` |
| `booting` | installed and powered on, guest agent not yet connected |
| `online` | guest agent connected — fully up |
| `stopping` | graceful shutdown requested, not yet powered off |

`create()` is **async and auto-starts** the VM, so a fresh create walks
`building → installing → booting → online` on its own. Watch it with
`wait()`/`status()` (poll-based and miss-proof) or `events()` (push, for UIs).
`wait()` raises `RuntimeError` with the reason if the VM reaches `failed`.

**Linux first-boot failures.** The guest reports its setup over COM1, and the
current step shows in `installStatus`. If the guest agent can't be built, the VM
goes to `failed` as soon as setup ends. If the guest goes quiet, it goes there
when a deadline passes: 15 minutes with no sign of setup starting, 10 minutes of
silence during setup, or 15 minutes after setup with no agent. The longest wait is
about an hour. The console output is in `firstboot-console.log` in the VM's
folder, and `installError` stays set after the VM is stopped. Starting the VM again
clears it until the guest reports again, and the agent connecting clears it for
good. To retry a failed setup, delete the VM and create it again.

> **Create VMs sequentially, not all at once.** A create runs a VHDX build that
> prefetches the build dependencies (over the network for Linux) and
> stages them. Firing multiple creates simultaneously can make those concurrent
> prefetch/staging steps race, and a build may fail (`iso-patch.exe exited with code
> 1`) and get removed. It's generally advised to let one create clear its build phase
> (or wait a short interval, e.g. ~30 s) before starting the next.

---

## API surface

`connect()` returns a `Client`. Query methods return parsed JSON; lifecycle
methods return `(http_status, body)` so you can branch on the status code.

### Queries
| Method | Returns |
|---|---|
| `version()` | `{product, version, apiVersion, hostOs, capabilities}` |
| `host()` | `{hostCores, hostRamMb, freeGb, vmCores, vmRamMb, vmHddGb}` (capacity + what running/declared VMs use) |
| `list()` | list of VM status objects |
| `status(name)` | one VM's status (raises `KeyError` on 404) |
| `ssh_info(name)` | `{host, port, user, sshState, enabled, keyDeployed}` (loopback-forwarded SSH; `sshState 4` = ready + key deployed) |
| `templates()` | `[{name, osType}, …]` |
| `snapshots(name)` | list of `{index, name, branchCount, branches:[…]}` |
| `snapshots_full(name)` | the above **plus** `current: {snapIndex, branchIndex}` |

A **status object** has: `name, osType, state, running, agentOnline,
installComplete, installError, installStatus, building, progress, sshState,
sshPort, ramMb, hddGb, cpuCores,
gpuMode, networkMode, displayOpen, displayView`.

### Lifecycle  *(return `(status, body)`)*
| Method | Effect |
|---|---|
| `create(**cfg)` | create + auto-start (validated; see below) |
| `edit(name, **fields)` | change config — **VM must be stopped** (else 409) |
| `start(name, snap_index=-1, branch_index=-1, branch_name=None)` | power on; optionally boot a snapshot/branch |
| `shutdown(name)` | graceful ACPI power-off |
| `stop(name)` | force power-off (pull the plug) |
| `delete_vm(name)` | remove the VM and its disk (refused 409 while building) |
| `delete_template(name)` | remove a template |
| `shutdown_daemon(force=False)` | stop the daemon (refused 409 if VMs are active unless `force=True`) |

### Snapshots & branches *(return `(status, body)`)*
| Method | Effect |
|---|---|
| `snap_take(name, snapname=None)` | snapshot — **VM must be stopped** (else 409) |
| `snap_rename(name, index, new_name, branch_index=-1)` | rename a snapshot (or a branch with `branch_index`) |
| `snap_delete(name, index)` | delete a snapshot |
| `snap_delete_branch(name, index, branch_index)` | delete one branch |

A **branch** is created by *starting from* a snapshot with a new name:
`start(name, snap_index=2, branch_name="experiment")`. This mirrors the GUI,
which has no other way to fork a snapshot.

**Base branches** are branches forked directly off the base disk rather than a
named snapshot — address them with `snap_index = -2`. `snapshots_full(name)`
returns them under a `baseBranches` key (alongside `snapshots`), and you create /
rename / delete them the same way: `start(name, snap_index=-2, branch_name=...)`,
`snap_rename(name, -2, new_name, branch_index=b)`, `snap_delete_branch(name, -2, b)`.
The base itself is **not** a deletable snapshot — `snap_delete(name, -2)` is
refused (deleting the base disk is "delete the VM"); only its branches delete.

### Events (SSE)
`events()` is a generator yielding parsed dicts as the daemon pushes them. It
**blocks** — run it in a thread. Event shapes:

```json
{"event": "vmStateChanged", "name": "dev", "running": true}
{"event": "vmProgress",     "name": "dev", "progress": 42, "staging": false}
{"event": "alert",          "message": "..."}
```

Events are a convenience for live UIs. **For control flow, prefer
`wait()`/`status()`** — they read the current state on each call, so a dropped
connection can never make you miss a transition.

### Waiting
- `wait(name, target_states, timeout=300, interval=2)` — poll until `state` is in
  `target_states` (a string or set); raises `TimeoutError`.
- `wait_online(name, timeout=900)` — shorthand for `wait(name, {"online"})`.

`asb.RUNNING_STATES == {"booting", "online"}` — the powered-on states.

### Display

Open the VM's **display window** — the same view the GUI's monitor button shows —
on the **daemon's local desktop**. It renders on demand; the daemon never
auto-opens it (good in the GUI, wrong for a CLI).

The window shows one of two sources and switches between them by itself:

- **Hyper-V console** (`"console"`) — the VM's synthetic video framebuffer, served
  by `vmwp.exe` on `\\.\pipe\<vm>.BasicSession` (the "basic session" Hyper-V's
  own VMConnect uses), rendered with the RDP client control. It works from
  firmware / boot loader onward with no guest cooperation, so it is what a
  **booting** VM shows: UEFI, the boot loader, Windows setup/OOBE, kernel and
  cloud-init output.
- **IDD stream** (`"idd"`) — the guest's virtual monitor (the IddCx driver on
  Windows, `asb_drm` on Linux) over the frame channel, with input, clipboard and
  audio. Needs the guest agent and display driver up.

| Method | Returns |
|---|---|
| `display_status(name)` | `{open, ready, canOpen, view, following}` — poll this; **no window is opened by polling** |
| `display_ready(name)` | `bool` — shorthand for `display_status()["ready"]` (IDD stream available) |
| `open_display(name, view=None)` | `(status, body)` — open (or focus / switch) the window; `body` has `view` and `following` |
| `close_display(name)` | `(status, body)` — close it |

```python
c.start("dev")
c.open_display("dev")                # booting: the Hyper-V console (boot screen) appears ...
c.wait_online("dev")
# ... and the same window switches itself to the IDD stream once the agent
# reports the display driver up: display_status("dev")["view"] -> "idd"
c.open_display("dev", view="console")  # manual switch back to the Hyper-V console
c.close_display("dev")              # or the user just closes it with the [X]
```

- **`POST /v1/vms/{name}/display`** takes an optional body `{"view": "auto" |
  "console" | "idd"}`:
  - `auto` (default): IDD if `ready`, otherwise the Hyper-V console, which then
    **follows** the boot — it switches to IDD on its own as soon as `ready` turns
    true, opening the IDD window where the console was.
  - `console`: the Hyper-V console, staying there.
  - `idd`: the IDD stream; refused with `409 display_not_ready` until `ready`.
  On an already-open window, `console` / `idd` switch it (applied within a
  fraction of a second) and `auto` just focuses it. The response is
  `{"ok":true,"displayOpen":true,"view":"console"|"idd","following":bool}`.
  An unknown `view` is `400 bad_view`.
- **`ready` means the IDD stream is available.** It is `running && agentOnline &&
  idd_ready`, where `idd_ready` is the **guest agent's own report** that the display
  driver is up (`idd_status:ok`) — a latched flag, **not** a probe: reading it touches no
  window and no frame channel, so polling can't disturb the display. (Connecting to the
  frame channel to "check" would itself become the display's single consumer.)
  `canOpen` is just `running` — a booting VM can be opened before `ready` and
  shows the console. `view` is what an open window shows (`null` when closed);
  `following` is true while it is on the console waiting to switch to IDD.
- **In the window itself** the user can switch too: the console window's toolbar
  has **Switch to VM Display** (if the guest display isn't up yet, the window
  switches as soon as it is), and the IDD window's system menu (right-click the
  title bar) has **Show Hyper-V Screen (boot console)**.
- **`displayOpen` / `displayView`** (in every status object) reflect the *live*
  window, however it closed — `close_display`, the window's `[X]`, VM delete, and
  daemon exit all drive them to `false` / `null`. `open_display` on an
  already-open VM just focuses the window (foregrounding it).
- **Local desktop only.** The window shows on the session the daemon runs in. A
  non-interactive session (a service/SSH daemon with no visible desktop) can't
  show one, so `open_display` returns `409 no_display` rather than spawning an
  invisible window. Multiple VMs' displays can be open at once.
- **On a Windows guest the IDD display follows the window.** When the window
  settles at a new size (the end of a drag, or just after maximize or snap) the
  guest display takes exactly that client size, odd sizes and portrait included,
  and goes back to 1920x1080 only until the window asks again after a guest
  reboot. The driver changes size by making its monitor depart and arrive with
  that one mode, because Windows reads a monitor's modes when it arrives, so the
  screen blanks for a moment. Limits: 640x480 at the small end (Windows offers no
  smaller mode) and 8192 per side and 7680x4320 worth of pixels at the large end
  (what a frame may cost in buffers and link throughput); past either the picture
  is scaled to fit. If the guest can't take a size (a driver from before this, or
  a size Windows won't use) it stays where it was, scaled to the window, and the
  driver returns to the last size that worked by itself. Ubuntu guests follow the
  window the same way, through `asb_drm`; one built before this keeps its size and
  scales.
- The Hyper-V console only shows what the guest draws on the synthetic video
  adapter: once the Windows agent disables that adapter in favour of the IDD
  monitor, the console stays dark, so use the IDD view for a running desktop.
- Serves both Windows and Linux guests.

---

### Remote: Moonlight and browser

Both are off until turned on, per VM. A browser session or a Moonlight stream controls
the VM, so the responses say in `reach` exactly who can get in.

```python
c.web_set("dev", True)                 # new key; c.web_status("dev")["url"] opens it
c.web_set("dev", True, lan=True)       # adds HTTPS for other devices
c.stream_set("dev", True)              # Sunshine in the guest, this PC only, CPU encoding
c.stream_status("dev")["guest"]        # {'phase': 'ready', 'running': True, ...}
c.stream_pair("dev", "1234")           # the PIN Moonlight shows
```

| Call | Route |
|---|---|
| `stream_status(name)` | `GET /vms/{n}/stream` |
| `stream_set(name, enabled, exposure, software_encoder)` | `POST /vms/{n}/stream` `{enabled, exposure, softwareEncoder}` |
| `stream_pair(name, pin, client)` | `POST /vms/{n}/stream/pair` `{pin, name}` |
| `stream_unpair(name)` | `POST /vms/{n}/stream/unpair` |
| `web_status(name)` | `GET /vms/{n}/web` |
| `web_set(name, enabled, lan)` | `POST /vms/{n}/web` `{enabled, lan}` |

**Moonlight.** The guest downloads the official Sunshine MSI at setup and checks it
against a pinned SHA-256; Sunshine (GPL-3) is never shipped with App Sandbox. A NAT VM
is reached through a relay in the app, which lives and dies with it and listens on
127.0.0.1 unless you choose `network`, which adds a firewall rule for private and domain
networks. A bridged VM is on the network directly. Windows guests only for now; `unsupported`
gives the reason on others. Gamepads use ViGEmBus, which its author no longer maintains.

`softwareEncoder` defaults to on. Setting it off lets Sunshine probe the GPU's encoder through
GPU-PV, where [Sunshine #2141](https://github.com/LizardByte/Sunshine/issues/2141) has reports of
the *host* crashing with NVENC on some cards and drivers.

**Browser.** The VM's display channel is encoded on the host to H.264 (the host GPU's encoder if
it has one) and decoded by WebCodecs, on one WebSocket with sound, input and text clipboard.
It isn't WebRTC: that adds a jitter buffer and native dependencies, and this path works from the
sign-in screen with nothing installed in the guest. Being TCP, a lossy link stalls rather than
degrades; the encoder skips frames when the link falls behind. The display window pauses while
a browser is connected, and only one browser can hold a VM: the newest wins. No gamepad.

What protects it: a new 256-bit access key per turn-on (never saved), exchanged for an
HttpOnly, SameSite=Strict cookie tied to one VM; failed logins limited per address; Host
and Origin checked on every request; 127.0.0.1 by default, HTTPS with a self-signed certificate
and a fingerprint to compare on the network. Malware lab VMs are refused for both features.

## Create config & validation

The daemon validates create/edit input **exactly like the GUI** (it mirrors the
JS guards in `web/app.js`), so the API rejects precisely what the UI would
rather than forwarding bad input to the core.

| Key | Notes |
|---|---|
| `name` | guest hostname; **set at create, not editable later**. Letters/digits/hyphens; not all-digits; no leading/trailing hyphen. Windows ≤15 chars (NetBIOS); Linux ≤63 and lowercase. Must be unique across VMs *and* templates. |
| `osType` | `"Linux"` or `"Windows"` |
| `imagePath` | path to an install ISO — **required unless** `templateName` is given |
| `templateName` | create from a Windows template instead of an ISO |
| `ramMb` | ≥512, **2 MB-aligned** (even). The SDK rounds down to even for you. |
| `hddGb` | ≥1 |
| `cpuCores` | ≥1 |
| `gpuMode` | `0` None · `1` Default (paravirtual) · `2` Try all |
| `networkMode` | `0` None · `1` NAT · `2` External · `3` Internal |
| `netAdapter` | host adapter name (for External mode) |
| `adminUser` | required for a normal create. Linux: ≤32, starts `[a-z_]`, body `[a-z0-9_-]`. Windows: ≤20, none of `"\/[]:;|=,+*?<>`, no trailing `.`, not a reserved name (CON, PRN, …). |
| `adminPass` | required on Linux (≤255 bytes); optional on Windows |
| `testMode` | skip interactive setup where supported |
| `sshEnabled` | provision SSH + forward a loopback port |
| `preinstallApps` | comma-separated apps to install after the first start: winget IDs on Windows, apt or snap names on Ubuntu |
| `sshDeployKey` | deploy the AppSandbox public key for password-less login (**requires `sshEnabled`**; rejected `400` otherwise) |
| `isTemplate` | build a template (Windows only; can't be built from another template) |

`edit()` accepts `ramMb`, `cpuCores`, `gpuMode`, `networkMode` with the same
range rules, and **only while the VM is stopped**. `name` cannot be changed.

### SSH key deploy (password-less login)

With `sshEnabled=True, sshDeployKey=True`, the daemon generates an **AppSandbox
ed25519 keypair** once (under `%ProgramData%\AppSandbox\ssh\`) and the guest agent
writes the public key into the guest's `authorized_keys` (Windows:
`administrators_authorized_keys` + the strict ACL sshd needs; Linux:
`~/.ssh/authorized_keys`). The deployed key is saved in the VM's config. Once it
lands, `sshInfo` reports `keyDeployed: true` and `sshState: 4` (ready + key
deployed), and you can connect with no password:

```python
import asb, subprocess
c = asb.connect()
i = c.ssh_info("dev")                       # {'sshState': 4, 'keyDeployed': True, 'port': ..., 'user': ...}
subprocess.run(["ssh", "-i", asb.key_path(), "-p", str(i["port"]),
                "%s@127.0.0.1" % i["user"], "hostname"])
```

`asb.key_path()` returns the private key path. (`sshState` values: 1 installing,
2 ready, 3 failed, 4 ready + key deployed.)

---

## HTTP status / error model

| Status | When |
|---|---|
| `200 OK` | a query, or a successful `edit` |
| `202 Accepted` | an async action accepted (create/start/stop/snapshot/…) — watch `status`/`events` for the outcome |
| `400 Bad Request` | input failed validation — body has `{"error":{"code":"invalid_arg","message":…}}` |
| `401 Unauthorized` | missing/bad bearer token (only `version` is open) |
| `404 Not Found` | unknown VM/route |
| `405 Method Not Allowed` | wrong verb for the route |
| `409 Conflict` | state conflict: edit/snapshot while running (`vm_running`), delete while building (`vm_building`), daemon shutdown with active VMs (`vms_active`, lists them), or open-display refused (`no_display` no interactive desktop · `not_running` · `display_not_ready` `view: "idd"` requested before the agent reported the display driver up) |
| `500 Internal Server Error` | the core returned a failure (`error.hr`) |
| `501 Not Implemented` | the feature isn't supported on this host (`not_supported`); mirror of `capabilities` |

---

## Examples

Runnable scripts in [`examples/`](examples):

| Script | Shows |
|---|---|
| `list_vms.py` | connect, `version`, `host`, `list`, `templates` (read-only overview) |
| `start_vm.py` | `start` + poll to `online` |
| `stop_vm.py` | graceful `shutdown` vs `--force` `stop` |
| `full_lifecycle.py` | the whole surface: create → wait → `ssh_info` → snapshot → edit → branch → delete-branch → delete (self-contained) |
| `stream_events.py` | live SSE event stream |
| `admin.py` | operator/maintenance: `version`, `host`, `templates`, `delete_template`, `shutdown_daemon` (destructive actions gated behind flags) |

---

## Tests

The test suite lives in [`tests/`](tests/) and is **self-contained**: it assumes
no existing configuration, creates the throwaway `brk-*` VMs it needs, and
deletes them at the end (so it never leaves disk behind or touches your VMs).

Point it at your install ISOs first (env vars; or drop the ISOs in `tests/`,
which is the default), then run the harness:

```
set ASB_ISO_LINUX=C:\path\to\ubuntu-desktop-amd64.iso
set ASB_ISO_WINDOWS=C:\path\to\Win11_x64.iso
python tests\run_all.py
```

`run_all.py` runs the no-VM static checks once (`test_host_and_validation.py`:
host reporting vs the real machine + create-validation rejections), then spawns
the full per-VM lifecycle (`vm_lifecycle.py`) **concurrently** across specs —
Linux and Windows at the same time — so multi-VM concurrency and every code path
are proven on both OSes simultaneously. The per-VM lifecycle also covers SSH key
auto-deploy: each VM is built with `sshDeployKey` and, once online, the suite
asserts `keyDeployed`/`sshState 4` and performs a key-only SSH login (no
password). `test_windows_template.py` covers the Windows template build →
create-from-template → delete cycle. Default guest credentials are
`user` / `test123`. See [`tests/README.md`](tests/README.md) for the file-by-file
layout.
