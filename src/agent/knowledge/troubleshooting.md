# Troubleshooting guide

## A program is "Not Responding"

1. `list_programs`: find it. Windows that stopped answering are marked
   NOT RESPONDING. Note its pid and memory use.
2. Check memory in the overview. A live session keeps everything in memory;
   a large program (Firefox) needs 8 GB there.
3. If the person wants to know why: `program_backtrace` (asks permission).
   A thread waiting in win32u's NtUserMsgWaitForMultipleObjectsEx or
   NtUserGetMessage is idle in its message loop, which is normal. A main
   thread that stays in another wait (RtlWaitOnAddress,
   NtWaitForSingleObject...) is waiting for something else in the program.
4. Firefox: WinCraft 0.3 fixed the Firefox startup hang (Wine's
   GetProcessMitigationPolicy, see the patch notes). If Firefox still hangs,
   look at memory and at `read_log` desktop.
5. To get going again: `end_program` (asks permission), then start the
   program again. Anything unsaved in it is lost.

## A program does not start, or closes at once

- `read_log` with name desktop and problems_only: Wine writes its errors
  there. "err:module:import_dll Library X not found" means a DLL the program
  needs is missing (often a Visual C++ runtime or .NET); "unimplemented
  function" means Wine lacks something the program calls.
- `list_crash_reports` for dump files the program left.
- Common fixes the person can do: install the runtime the program needs
  (its vendor's Visual C++ redistributable; for a program built for .NET 5
  or later, the Microsoft .NET runtime it names. .NET Framework programs
  run on Wine Mono, which is built in); choose another Windows version for
  that program in *Compatibility Settings* (Applications tab); a DLL
  override only when it is known which DLL is at fault.
- Kernel drivers, kernel-level anti-cheat, some DRM and copy protection,
  UWP/Store apps cannot work.

## Installing and removing programs

- *Download a Program* (desktop icon and Start menu) takes the web address
  of an installer (.exe or .msi), saves it in Downloads and runs it. Its
  *Get Firefox* button fetches Firefox.
- `download_and_run` does the same from the chat, after asking.
- Installers put their shortcuts in the Start menu, as on Windows.
- Remove programs with *Add or Remove Programs* in the Start menu.
- In a live session, everything installed is lost at restart. *Install
  WinCraft OS* keeps it, on a disk.

## Network

- Wired Ethernet with DHCP only; there is no Wi-Fi or Bluetooth.
- `check_network`: no gateway means no connection (cable, or the virtual
  machine's network setting); a failing name lookup means DNS does not work.
- In a QEMU virtual machine with QEMU's default network (the README's
  command, and `./wc run`), WinCraft is on a private network, 10.0.2.x,
  behind the host. Connections out work: to the internet, to other machines
  on the host's network, and to the host itself at 10.0.2.2. Only
  connections into WinCraft from elsewhere need port forwarding in QEMU.

## Display, graphics, sound

- Graphics: in a virtual machine with a virtio GPU and virgl, as `./wc run`
  starts it, OpenGL runs on the host's graphics card (the renderer is named
  "virgl"). Elsewhere it is software OpenGL (llvmpipe): fine for
  applications, slow for 3D games. No Vulkan, so no Direct3D 12.
- There is no display or sound settings panel yet. Sound works in a virtual
  machine (QEMU's Intel HD Audio, played by the host's sound server) and is
  untested on real hardware. When a program plays but nothing is heard in a
  virtual machine, check the host's volume, and its "WinCraft OS" stream,
  first.
- Text size in Windows programs (DPI) is the dword `LogPixels` under
  `HKEY_CURRENT_USER\Control Panel\Desktop`, which `set_registry_value` can
  set: 96 is 100%, 120 is 125%, 144 is 150%, 192 is 200%. When it is not set,
  the size is 96. It takes effect only after WinCraft restarts (from the
  Start menu): Wine fixes the screen's DPI when the desktop starts, so until
  then a program started again still shows its text at the old size.
  - The machine-wide `LogPixels` under
    `HKEY_LOCAL_MACHINE\System\CurrentControlSet\Hardware Profiles\Current\Software\Fonts`
    is used only while the per-user value is not set.
  - Do not change `LogPixels` or `Codepages` directly under
    `HKEY_CURRENT_USER\Software\Wine\Fonts`. They are Wine's record of what it
    last set its fonts up for, and Wine writes them back itself.

## Keyboard layout, clock

- The keyboard layout is chosen at boot with the option `wc.kbd=fr` (or de,
  gb...): press `e` on the boot menu entry and add it to the command line.
  There is no settings panel for it yet.
- How soon and how fast a held key repeats is decided by the display server
  (X), not by Windows settings. Wine keeps `KeyboardDelay` and `KeyboardSpeed`
  under `HKEY_CURRENT_USER\Control Panel\Keyboard`, but does not use them
  with WinCraft's display server, so changing them does nothing. WinCraft has
  no setting for key repeat, but the WinCraft Agent can build a feature that
  sets it at each login with the display server's own tool (`xset`).
- The clock shows UTC; there is no time zone setting yet.

## Windows settings that do nothing here

Wine keeps many Windows settings in the registry without applying them, and
WinCraft draws parts of the screen itself. Such a value can be read and
written like any other, so finding it proves nothing. When a request comes
down to one of these, do not write the value: say that WinCraft cannot change
it yet, or, for the display server's settings at the end of this list, offer
the WinCraft Agent.

- Kept by Wine but not applied by Wine's own windows and controls (a program
  that reads the value itself may still follow it):
  - under `HKEY_CURRENT_USER\Control Panel\Mouse`: `MouseSensitivity`,
    `MouseSpeed`, `MouseThreshold1` and `MouseThreshold2` (pointer speed and
    acceleration belong to the display server, below), `MouseTrails`,
    `SnapToDefaultButton`, and `SwapMouseButtons`
    (Wine reports the buttons as swapped but does not swap the clicks; the
    display server can, below);
  - under `Control Panel\Desktop`: `MenuShowDelay`, `CaretWidth` (Wine's text
    boxes always draw a 1-pixel cursor), `FocusBorderWidth`,
    `FocusBorderHeight`, `ForegroundLockTimeout`, `ForegroundFlashCount`,
    `WheelScrollChars`, `ClickLockTime`, `ScreenSaveTimeOut`,
    `ScreenSaveActive`, `LowPowerActive`, `PowerOffActive`, `DragFullWindows`
    (ordinary windows always move with their contents), and the effects in
    `UserPreferencesMask` other than gradient title bars and flat menus: menu,
    tooltip and selection fades, drop shadows, the pointer shadow, underlined
    shortcut letters, hot tracking, smooth list scrolling, ClickLock;
  - `IconSpacing` and `IconVerticalSpacing` under
    `Control Panel\Desktop\WindowMetrics`;
  - the keyboard preference, screen reader, ShowSounds and audio description
    flags under `Control Panel\Accessibility`.
- Not read at all: the pointer scheme and size (`Control Panel\Cursors`,
  `CursorBaseSize` included). The Windows 10 and 11 accent colour
  (`Software\Microsoft\Windows\DWM`) colours only WinCraft's own taskbar
  and menus (below); title bars take their colours from `ActiveTitle` and
  `GradientActiveTitle` in `Control Panel\Colors`.
- Values Wine writes for itself, which look like settings:
  - under `Software\Wine\Fonts`: `LogPixels`, `Codepages`, and the
    `External Fonts` and `Cache` keys;
  - under `Software\Microsoft\Windows\CurrentVersion\ThemeManager`: the
    `Control Panel\Colors` key, `NonClientMetrics` and `IconTitleFont`, a copy
    taken when the theme was switched on. If the theme is switched off, Wine
    puts that copy back over `Control Panel\Colors`, undoing colour changes
    made since.
  - `Control Panel\International` is a real setting (date, time and number
    formats), but Wine rewrites it whenever the language of Windows programs
    changes, so changes made there are lost then.
- Drawn or handled by WinCraft: the desktop, the taskbar, the Start menu and
  window focus. They follow only the settings in "WinCraft's desktop and
  taskbar" below; Explorer's other settings (its taskbar position in
  `StuckRects3`, desktop icon size or spacing, combining taskbar buttons) do
  nothing here yet.
- Belonging to the display server, which Windows programs follow: key
  repeat, screen blanking and sleep, pointer speed, and left-handed mouse
  buttons. WinCraft has no setting for them, but the WinCraft Agent can build
  a feature that sets them at each login with the display server's own tools
  (`xset`, `xinput`); turning the feature off undoes it after a restart. The
  pointer's speed has no effect on a virtual machine's tablet pointer.

## WinCraft's desktop and taskbar

WinCraft's desktop, taskbar and window manager follow these settings, all
under `HKEY_CURRENT_USER`, which `set_registry_value` can change or delete.
They apply at once: WinCraft watches these keys, so nothing needs a restart.
Change them yourself; the WinCraft Agent is not needed.

- Desktop picture: `Control Panel\Desktop`, `Wallpaper` (text: the full
  Windows or Linux path of a BMP, JPG, PNG or GIF picture, such as
  `C:\users\user\Pictures\sea.jpg`; empty for none) and `WallpaperStyle`
  (text: `10` fill the screen, the default; `6` fit; `2` stretch; `0` centre,
  or tile with `TileWallpaper` = `1`).
- Plain desktop colour, used when `Wallpaper` is empty: `Control Panel\Colors`,
  `Background` (text, `R G B`). WinCraft's own value, `11 21 48`, means its
  usual blue background.
- Light taskbar and menus: `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`,
  `SystemUsesLightTheme` (dword 1; 0 for dark, the default).
- Accent colour of the taskbar and menus: `Software\Microsoft\Windows\DWM`,
  `AccentColor` (dword `0xAABBGGRR`: green 0,154,60 is `0xFF3C9A00`).
- Seconds on the clock: `Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced`,
  `ShowSecondsInSystemClock` (dword 1).
- Open desktop icons with one click: `Software\WinCraft\Shell`,
  `SingleClickOpen` (dword 1). WinCraft's own value: Windows keeps this
  setting in a form the registry tool cannot write.
- Hide the taskbar until the pointer reaches the screen's edge:
  `Software\WinCraft\Shell`, `TaskbarAutoHide` (dword 1). WinCraft's own
  value, for the same reason.
- The taskbar at the top of the screen: `Software\WinCraft\Shell`,
  `TaskbarPosition` (dword 1; 3, or not set, for the bottom: Windows'
  numbers for the edges). The Start menu opens below it and maximized windows
  fill the rest. WinCraft's own value, for the same reason. The left and
  right edges are not supported yet.
- Focus follows the mouse: `Control Panel\Mouse`, `ActiveWindowTracking`
  (dword 1), and how long the pointer must rest on a window first:
  `Control Panel\Desktop`, `ActiveWndTrkTimeout` (dword, milliseconds; 0 at
  once). As on Windows, the window gets the keyboard without coming to the
  front, and an open menu stays open.
- Programs' notification-area icons show in the taskbar, beside the clock,
  with their tooltips and balloons; a click goes to the program, a
  right-click opens its menu. Their menus work with the mouse only (Wine
  gives the keyboard only to a program's visible windows). Wine's own
  floating tray window is turned off (`Software\Wine\Explorer`,
  `ShowSystray` dword 0). There is nothing to set.
- Programs set to start with Windows start at each login, after the taskbar:
  the `Run` and `RunOnce` values under `Software\Microsoft\Windows\CurrentVersion`
  (in `HKEY_CURRENT_USER` and `HKEY_LOCAL_MACHINE`) and the shortcuts in the
  Startup folders. Wine's `wineboot` runs them, once per session. To stop
  one, use the program's own option ("start with Windows") or remove its
  value with Registry Editor: `set_registry_value` refuses the `Run` keys.
  The boot menu's *without your features* entry starts none of them.
- Alt+Tab switches windows: the window in front first, then the others, most
  recently used first and minimized ones last. Tab moves on, Shift+Tab back,
  releasing Alt switches, Esc cancels. There is nothing to set.
- WinCraft's defaults, to put things back: `Wallpaper` empty, `Background`
  `11 21 48`, `ActiveWindowTracking` dword 0; every other value above is not
  set, so delete it. Deleting `AccentColor` brings back WinCraft's own blue.

## Files and drives

- C: is the Windows side of the person's home (Documents, Downloads,
  Program Files). Z: shows the whole underlying Linux file system.
- *File Explorer* is on the desktop and in the Start menu.
- The list of running programs shows each program's file. Wine's own
  programs are in C:\windows, WinCraft's in Z:\usr\lib\wincraft, and what
  the person installed or downloaded is elsewhere on C:, usually under
  C:\Program Files or Downloads. A program whose file is anywhere else, such
  as Z:\tmp or Z:\run, is not part of WinCraft and was not installed in
  Windows: say where it is, and do not guess what it does from its name.

## Live session or installed

- The overview says which. Live: started from the ISO or USB, all changes in
  memory, lost at restart. Installed: changes are kept on the data
  partition.
- *Install WinCraft OS* (desktop icon in a live session) erases the WHOLE
  disk chosen, then makes it bootable on BIOS and UEFI.
- There is no update tool yet: a new WinCraft release is a new system image.

## Power

- Shut down and restart from the Start menu.
