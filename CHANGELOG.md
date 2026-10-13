# Changelog

## 0.2 — 2026-10-13

Fixes found while running Firefox on 0.1.

- Programs that open many handles no longer break under `ntsync`: the desktop
  session raises the open-file limit that Wine's server was running into. On
  0.1 this made Firefox lose tabs and helper processes, and could take the
  Wine server down with the shell.
- *Download a Program* identifies itself as Windows 10, so *Get Firefox*
  fetches the current Firefox instead of the old ESR 115 branch.
- Firefox no longer crashes on YouTube: Wine's `userenv.dll` gains the
  function that recent Firefox versions look for.
- Fixes to Wine like that one can be carried in `wine/patches` until a Wine
  release has them; see "Patching Wine" in the README.

Tested with Firefox 158 installed through *Download a Program*: YouTube plays
video. A live session with the current Firefox does not fit in 4 GB of RAM;
give the virtual machine 8 GB.

## 0.1 — 2026-10-13

First public preview.

- Boots from a 213 MB ISO on BIOS and UEFI, live or installed to disk.
- Linux 6.18 LTS kernel built without modules or firmware blobs, with `ntsync`
  and Windows executables registered as a native binary format.
- `wcinit`: init, service supervision, clean shutdown, privilege broker.
- `wcwm`: window manager with stacking layers, click-to-focus,
  minimise / maximise / full screen, cascading placement and a work area that
  keeps maximised windows above the taskbar.
- `wcshell`: desktop with icons, taskbar, Start menu with type-to-search,
  Run dialog, *Download a Program* and the *Install WinCraft OS* front end.
- Read-only system image with a writable layer: RAM for a live session, a
  `WINCRAFT_DATA` partition once installed.
- Wine 11 (WoW64) for 64-bit and 32-bit Windows programs; wired networking
  with DHCP; software-rendered OpenGL.

Known limitations are listed in the README under "What it is not".
