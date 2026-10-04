# Using the VM window

Everything below happens in the window that shows a running Windows VM (the
"IDD Display" window). Most options are in the window's **menu**: right-click
the title bar, or press **Alt+Space** while the window is active.

| Menu item | What it does |
|---|---|
| Mute audio | Stops the VM's sound from playing on your PC |
| Share microphone with the VM | Lets programs in the VM hear your microphone |
| Microphone source ▸ | Which of your PC's microphones the VM hears |
| Immersive mode | Sends Alt+Tab, the Windows key and other shortcuts to the VM |
| Fullscreen (Ctrl+Alt+Enter) | Borderless fullscreen on the current monitor |
| VM resolution ▸ | The VM's screen resolution |
| VM refresh rate ▸ | The VM's refresh rate (60–240 Hz) |
| VM displays ▸ | How many screens the VM has (1–4) |
| Test Mode… | Lets the VM load App Sandbox's newer drivers (see below) |
| Show Log | A log window for checking what's happening |

Settings are saved per VM.

---

## Before you start: updates and Test Mode

**Helpers update by themselves.** App Sandbox copies the latest VM-side programs into
the VM every time **you start the VM from App Sandbox**. A restart from inside Windows
doesn't do this. After updating App Sandbox, shut the VM down fully and start it again.

**Some features need the newer drivers, and those need Test Mode.** The newer
display and audio drivers in this build are *test-signed*, and Windows in the VM only
loads test-signed drivers when it's in **Test Mode**:

| Needs Test Mode | Works without it |
|---|---|
| VM displays (more than one) | Mouse and keyboard, Immersive mode |
| VM resolution | Fullscreen |
| VM refresh rate above 60 Hz | Game controllers |
| Microphone | Drag and drop files |

### Turning on Test Mode

- **New VM:** tick **Test Mode** when you create the VM. It's with the other Windows options.
- **Existing VM:**
  1. Open the VM window menu, choose **Test Mode…** and confirm.
  2. Shut the VM down and start it again from App Sandbox.
  3. The VM turns Test Mode on and **restarts itself once** (you'll see a short countdown).
  4. When it comes back, it installs the newer display and audio drivers automatically.
     The screen may flicker once while that happens.

Test Mode turns off Secure Boot for that VM and allows test-signed drivers in it, so the
VM is a little less protected against tampered boot files and drivers. It can't be
turned off again from the menu, because the display driver would then stop loading.

How to check: in the VM, `C:\Windows\AppSandbox\agent.log` shows
`Test Mode: on` and then `Drivers: AppSandboxVDD updated` / `Drivers: AppSandboxVAD updated`.

---

## Mouse

There's nothing to set up. The mouse switches mode by itself:

- **On the desktop**, your pointer moves freely in and out of the window.
- **In a game** that hides the pointer (first-person mouse-look), the window takes the mouse
  automatically: the pointer disappears and is kept inside the picture, and
  movement goes to the game as raw motion.
- It lets go as soon as the game shows its pointer again (menus, Esc), when you
  Alt+Tab away, or when you click another window. Click the picture to go back in.
- **Ctrl+Alt+Del** always reaches your PC.

## Keyboard and Immersive mode

Keys go to the VM while its window is active. Normally your PC keeps shortcuts such as
**Alt+Tab** and the **Windows key**. Turn on **Immersive mode** to send them to the VM instead:
useful for games and full-screen work. Ctrl+Alt+Del still goes to your PC.

## Fullscreen

**Ctrl+Alt+Enter** (or **Fullscreen** in the menu) fills the monitor the window is on, with
no borders. Press it again to go back to a window. The shortcut isn't sent to the VM.

For a sharp 1:1 picture, combine it with **VM resolution ▸ Match host monitor**.

## Resolution and refresh rate *(Test Mode)*

- **VM resolution ▸ Match host monitor** makes the VM use the same resolution as the
  monitor its window is on, and follows it when you move the window to another monitor.
  You can also pick a fixed size, from 1280×720 up to 3840×2160 and 5120×1440.
  **Keep guest setting** (the default) leaves whatever Windows in the VM chose.
- **VM refresh rate ▸ Match host monitor** (the default) runs the VM at your monitor's
  refresh rate, up to 240 Hz, so games aren't capped at 60 fps. You can also pick a
  fixed rate.

Resolutions the VM can't use are ignored, and the VM keeps its current one.

## Multiple displays *(Test Mode)*

**VM displays ▸ 2 / 3 / 4** gives the VM more screens. Each extra screen opens in its own
window, which you can move to another monitor and make fullscreen (Ctrl+Alt+Enter).
The mouse moves between the windows as if they were real screens.

- To remove a screen, close its window or pick a lower number.
- Extra screens use the same resolution and refresh settings as the main window.
- To arrange the screens inside the VM, use **Settings ▸ System ▸ Display** in the VM.

If an extra window stays black, open **Show Log** in the main window. A line such as
`Display 2: waiting for the VM to add it` means the VM doesn't have the newer display
driver yet: turn on Test Mode (above).

## Game controllers

Plug an Xbox controller (or any XInput controller) into your PC. In the VM it appears as an
**Xbox 360 controller**, with rumble.

- The controller only drives the VM while one of its windows is the active window. When
  you switch away, the game in the VM sees the sticks centred and no buttons pressed.
- The first time, the VM installs the free **ViGEmBus** controller driver by itself. It's
  properly signed, so this doesn't need Test Mode. If a controller doesn't show up, start
  the VM once more after the install.
- PlayStation controllers need Steam Input or DS4Windows to present them as Xbox controllers.

To check: run `joy.cpl` in the VM.

## Microphone *(Test Mode)*

1. Turn on **Share microphone with the VM**. It's off by default.
2. Optional: under **Microphone source**, pick which of your PC's microphones to use. The
   default is whatever Windows uses as your default recording device.
3. In the VM, programs should record from **Microphone (App Sandbox Microphone)**. If that
   isn't the VM's default, choose it in the program's settings or in **Settings ▸ System ▸ Sound**.

Your PC's microphone is **only opened while a program in the VM is actually recording**
(Windows on your PC shows the microphone-in-use icon only then). If the VM has no
"App Sandbox Microphone", it doesn't have the newer audio driver yet: turn on Test Mode.

## Drag and drop files

Drag files or folders from Explorer onto the VM window. They're copied to the **Desktop**
of the user signed in to the VM. A name that already exists gets " (2)" added.

- One drop at a time; drop again once the first has finished.
- Copying only goes from your PC to the VM.
- **Show Log** reports how many files were copied.

---

## Troubleshooting

**Show Log** (main VM window) is the first place to look.

| You see | Meaning |
|---|---|
| `Input connected (guest caps 0xFF)` | The VM runs the current helpers |
| `guest caps` lower than `0xFF` | The VM still runs older helpers: shut it down fully and start it from App Sandbox |
| `Display 2: waiting for the VM to add it` | The newer display driver isn't installed: turn on Test Mode |
| `this VM has no App Sandbox Microphone yet` | The newer audio driver isn't installed: turn on Test Mode |
| `File drop: the VM is not accepting files` | Older helpers: restart the VM from App Sandbox |

Logs inside the VM, in `C:\Windows\AppSandbox\`:
`agent.log` (driver and controller installs, Test Mode), `input.log` (mouse, keyboard,
displays, controllers, file drops), `audio.log` (sound and microphone). The display
driver logs to `C:\ProgramData\AppSandboxVDD.log`.
