# Changelog

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
