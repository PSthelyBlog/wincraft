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

- Graphics are software OpenGL (llvmpipe): fine for applications, slow for
  3D games. No Vulkan, so no Direct3D 12.
- There is no display or sound settings panel yet. Sound output is untested.

## Keyboard layout, clock

- The keyboard layout is chosen at boot with the option `wc.kbd=fr` (or de,
  gb...): press `e` on the boot menu entry and add it to the command line.
  There is no settings panel for it yet.
- The clock shows UTC; there is no time zone setting yet.

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
