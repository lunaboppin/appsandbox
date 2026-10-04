# Parsec-style input and high-refresh display — plan

Branch: `immersive-input`

## Goals

1. Mouse input that works in games without a capture hotkey and without
   pinning the host cursor to the VM window during normal desktop use.
2. Keyboard input that reaches games correctly, plus an **Immersive mode**
   toggle that also sends Alt+Tab, the Windows key and similar shortcuts to
   the VM.
3. Input that keeps working through UAC prompts, lock and unlock, logon, and
   VM startup, without closing and reopening the display.
4. Let games run above 60 fps. The VM display matches the host monitor's
   refresh rate instead of being fixed at 1080p60.

## Why the earlier `relative-mouse` branch didn't work well

- Capture was manual (Pause/Ctrl+Alt+G). Once captured, the host cursor
  stayed locked until the user remembered the key.
- The guest helper only re-attached to the active desktop after SendInput
  failed with `ERROR_ACCESS_DENIED`. A thread on the wrong desktop often
  fails *silently* (UIPI/desktop isolation give no error), so the UAC
  secure desktop never got input until the display was reopened.
- The host input socket was reconnected only as a side effect of frame
  traffic, and packets were dropped on `WSAEWOULDBLOCK`. A restarted guest
  helper or a busy socket left input dead or keys stuck.

## How Parsec does it, and what we copy

Parsec's host watches the cursor on the machine being controlled. While a
cursor is visible, the client sends **absolute** positions and the local
cursor moves freely in and out of the window. When the remote app **hides
its cursor** (every first-person game does this in mouse-look), the client
switches to **relative** mode: it reads raw mouse deltas, hides the local
cursor and keeps it inside the window. When the game shows a cursor again
(menus, Esc, Alt+Tab), the client switches back on its own.

We already get the guest cursor's visibility and position from the IddCx
hardware cursor (`CursorHeader.visible`, `x`, `y`). Until now they were
mostly ignored.

## Design

### Host (`src/backend_win/vm_display_idd.c`)

**Dedicated input thread.** It owns the input connection (port 3):
- Connects, performs the `IRDY` handshake, then reads the guest's optional
  capability packet. Old and Linux helpers don't send one, so they get
  absolute-only behavior.
- Sends blocking, in batches, from an ordered queue. Consecutive moves are
  merged as they're queued: absolute moves replace each other, relative
  deltas add up. Nothing is dropped, so key-ups always arrive.
- Detects when the guest side closes (helper respawned by the agent) by
  polling for readability, and reconnects within about 250 ms.

**Automatic mouse mode.**
- Absolute mode (guest cursor visible): same as today, but sent through the
  queue without the 8 ms throttle.
- Relative mode starts automatically when **all** of these are true: the
  guest cursor is hidden, the display window is active, the host pointer is
  over the VM picture (or the user clicks it), and the guest helper
  supports relative motion.
  - Raw Input (`WM_INPUT`) supplies device deltas with no pointer
    acceleration.
  - The host cursor is hidden and clipped to the render area only while
    relative mode is active.
- Relative mode ends automatically when the guest shows its cursor again.
  The host cursor is placed exactly where the guest cursor is, so there is
  no jump.
- It also ends when the window loses activation (Alt+Tab or Win on the
  host, or clicking another app). It comes back on the next click.
- There's no hotkey. Ctrl+Alt+Del is always a guaranteed way out.

**Extra mouse buttons.** Side buttons (X1/X2) and horizontal wheel.

**Immersive mode.** The renamed "Transmit Keyboard Hotkeys" system-menu
item (same per-VM setting, so existing preferences carry over). Win, Alt+Tab,
Alt+Esc, Ctrl+Esc, PrintScreen and the Menu key go to the VM.

**Refresh rate.** A system-menu submenu: *Match host monitor* (default), or
60/120/144/165/240 Hz. On every input (re)connect, and when the window moves
to another monitor, the host tells the guest helper which rate to use.

**Frame presentation.**
- Flip-model swap chain with tearing allowed (when supported), so the host
  never waits on its own vsync.
- The window title (which forced a synchronous repaint on every frame) is
  updated at most twice a second.

### Guest input helper (`tools/agent/appsandbox-input.c`)

- **Follows the input desktop.** A watcher thread waits on the
  `WinSta0_DesktopSwitch` event. Before injecting, the helper checks the
  flag, plus a 250 ms safety re-check and a forced check after any
  SendInput failure. If the input desktop changed (Default, Winlogon/UAC,
  Screen-saver), it re-attaches. The helper already runs as SYSTEM in the
  console session, so it is allowed to inject into the UAC secure desktop.
- **Newest connection wins.** When the host reconnects, the stale
  connection is shut down instead of blocking the accept loop.
- **Releases everything on disconnect.** Keys and buttons still held by a
  connection that drops are released.
- **Scancode keyboard injection** (`KEYEVENTF_SCANCODE`). DirectInput and
  raw-input games see real make codes. Pause and NumLock keep VK injection
  because their scancodes are ambiguous.
- **New packets:**
  - `MOUSE_MOVE_REL` (4)
  - `MOUSE_HWHEEL` (5)
  - X1/X2 buttons (3/4)
  - `SET_REFRESH` (0x20)
  - The capability reply `GUEST_CAPS` (0x80), sent right after `IRDY`.
    Older hosts never read past `IRDY`, so the extra bytes are harmless.
- **SET_REFRESH:** calls `ChangeDisplaySettingsExW` on the VDD monitor with
  the closest supported rate.

### Virtual display driver (`tools/vdd`)

- Offers 1920x1080 at 60, 75, 120, 144, 165 and 240 Hz. 60 Hz stays
  preferred so first boot is unchanged. The host chooses a rate after
  connecting.
- EDID range limits widened to 24–240 Hz so the OS doesn't filter the new
  modes.
- Frames go out in one or two transport sends instead of one per row (1080
  syscalls per full frame before). Dirty rectangles are packed into a
  contiguous buffer.

## Compatibility

| Host | Guest helper | Result |
|---|---|---|
| New | New | Full feature set |
| New | Old (or Linux) | No caps packet → absolute mode only, as before |
| Old / macOS host | New | Old packet types unchanged; caps bytes ignored |

The VDD changes need a re-signed driver (test-signing or the EV pipeline).
The input changes work without them; the VM just stays at 60 Hz.

## Deploying to an existing VM

Guest binaries are installed when the VM is provisioned. For an existing VM,
copy the new `appsandbox-input.exe` (and, for high refresh, the rebuilt and
signed VDD package) into `C:\Windows\AppSandbox\` in the guest. Then restart
the agent service, or reboot.

## Test checklist

1. Desktop use: the cursor moves freely in and out of the window with no
   capture, and clicks land where the guest arrow is.
2. FPS game: mouse-look turns smoothly and isn't limited by the window
   edge. Opening the menu frees the cursor exactly where the guest arrow
   is.
3. Alt+Tab on the host during a game: the cursor is released at once.
   Clicking back into the game resumes mouse-look.
4. Immersive on: Alt+Tab and Win go to the guest. Ctrl+Alt+Del still
   reaches the host.
5. UAC: run something as admin. The prompt can be clicked and typed into
   without reopening the display. After Win+L and unlocking, input works.
6. Close and reopen the display mid-keypress: no stuck keys in the guest.
7. Reboot the VM with the display open: input works once the desktop is up.
8. Refresh: on a 144 Hz host, guest Display settings shows 144 Hz, and a
   game with vsync off reaches about 144 presented fps.
9. Side mouse buttons and horizontal scrolling work in the guest.
