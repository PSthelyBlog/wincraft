# WinCraft OS

A small, open-source operating system whose native application format is the
Windows program. It boots straight into a Windows-style desktop; `.exe` and
`.msi` files run when you open them, installers put their shortcuts in the
Start menu, and there is nothing else on the disk.

- **213 MB** ISO, boots on BIOS and UEFI
- about **80 MB of RAM** in use at the idle desktop
- runs live from the ISO, or installs to disk in under a minute
- 64-bit and 32-bit Windows programs
- a built-in assistant that knows WinCraft and can look at this computer,
  using an AI model you choose (optional)

**Status: 0.3, a working preview.** Read [What it is not](#what-it-is-not)
before relying on it.

![The WinCraft desktop: Start menu, Notepad and Minesweeper](docs/screenshot.png)

## Download

Get `wincraft-0.3-x86_64.iso` from the [releases page](../../releases) and
check it against the `.sha256` file published next to it:

```sh
sha256sum -c wincraft-0.3-x86_64.iso.sha256
```

Then boot it in a virtual machine. This is the configuration it was tested
with (4 GB of RAM, 4 CPUs):

```sh
qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 4096 -smp 4 \
    -device VGA -device qemu-xhci -device usb-tablet -device usb-kbd \
    -nic user,model=virtio-net-pci -cdrom wincraft-0.3-x86_64.iso
```

To share the clipboard (text, both ways) with the VM, add QEMU's clipboard
channel and turn it on in its window:

```sh
    -chardev qemu-vdagent,id=vda,clipboard=on -device virtio-serial-pci \
    -device virtserialport,chardev=vda,name=com.redhat.spice.0 -display gtk,clipboard=on
```

Guest drivers for VirtualBox, VMware and Hyper-V are built into the kernel,
but only QEMU has been tried. Nothing touches your disks unless you run
*Install WinCraft OS* from the desktop.

## How it works

WinCraft does not reimplement Windows. It combines the two open-source
projects that already do the hard parts, and writes everything in between:

| Layer | What | Origin |
|---|---|---|
| Kernel | Linux 6.18 LTS, one monolithic image, no modules, no firmware blobs. Built with NT synchronisation primitives (`ntsync`) and with PE executables registered as a native binary format | built from source, config in `kernel/` |
| Windows API | Wine 11 (WoW64 build: 64- and 32-bit programs) | binary package, plus the fixes in `wine/patches` |
| Display | X.Org server, Mesa (software OpenGL) | binary packages |
| **Init** | `wcinit`: PID 1, service supervision, shutdown, a small privilege broker | **written for WinCraft** (`src/init`) |
| **Window manager** | `wcwm`: stacking layers, focus, minimise/maximise, work area. Draws nothing: Windows programs frame their own windows | **written for WinCraft** (`src/wm`) |
| **Shell** | `wcshell.exe`: desktop, taskbar, Start menu with search, Run, *Download a Program*, installer front end. A native Win32 program | **written for WinCraft** (`src/shell`) |
| **Assistant** | `wcagent.exe`: *WinCraft Assistant*, a chat about WinCraft and this computer with the AI model you choose. A native Win32 program | **written for WinCraft** (`src/agent`) |
| **Boot, install, build** | early userspace, live/installed boot, disk installer, image assembler | **written for WinCraft** |

The userland is not a Linux distribution with a desktop on top. The build
takes binary packages from Arch Linux, then copies out only the files the
system needs (`tools/assemble-rootfs.py`), verifying that every shared-library
dependency still resolves. There is no package manager, no systemd, no shell
you ever have to see.

The system is one read-only, compressed image (`wincraft/system.sfs`) with a
writable layer on top: RAM for a live session, a data partition once
installed. An update is a new image file; your programs and files live in the
other layer.

## What works (tested)

Verified in QEMU, booting the ISO, unless noted:

- Boot via Limine on **BIOS** and on **UEFI** (OVMF), to the desktop.
- Desktop shell: Start menu with type-to-search, taskbar with running
  windows, minimise / maximise / restore, drag, Run dialog (Win+R), context
  menus, Windows key, clock.
- Networking: wired, DHCP, DNS; HTTPS with certificate validation.
- **Install to disk**, boot from the installed disk (BIOS), files persisting
  across reboots.
- Shut down and restart from the Start menu.
- Third-party software (tested in the native desktop session, `./wc desktop`):
  the **7-Zip** installer downloaded with *Download a Program*, installed,
  appeared in the Start menu and ran; both the 64-bit and the 32-bit builds.
- **Firefox 158**, downloaded and installed with *Download a Program* in a
  live session with 8 GB of RAM: browsing works and YouTube plays video.
- **Clipboard shared with QEMU** (`./wc run`'s window): text copied on the
  host pastes into Windows programs in the VM and back, including non-ASCII
  text and texts over 10 KB; it keeps working when the desktop session
  restarts. Also from a Wayland desktop, into an installed system.
- **WinCraft Assistant**, with a real model (Claude Sonnet 5.5 through
  Anthropic's API, in a live session): setup, chats that look things up on
  the computer. Against a scripted test server speaking both API formats:
  every tool, every permission dialog (allow and refuse), stopping a request,
  errors.

## What it is not

Be clear-eyed about these before you depend on it:

- **It does not run "anything Windows runs".** Compatibility is Wine's.
  Most desktop applications and a large share of games work; kernel drivers,
  kernel-level anti-cheat and DRM, UWP/Store apps and some copy protection do
  not. Check a program at <https://appdb.winehq.org>.
- **No GPU acceleration to speak of.** Everything tested so far is
  software-rendered (OpenGL through llvmpipe): fine for applications, far too
  slow for modern 3D games. Intel graphics have a kernel driver and may be
  accelerated, but that is untested; AMD and NVIDIA cards run unaccelerated on
  the firmware framebuffer. There is no Vulkan (so no Direct3D 12).
- **No Wi-Fi, no Bluetooth.** Both need firmware blobs and supporting
  services that are not in the image. Wired Ethernet only.
- **No web browser included.** Use *Download a Program* → *Get Firefox*, and
  give a live session 8 GB of RAM for it: 4 GB is not enough.
- **No media codecs** (Media Foundation / DirectShow playback), no printing.
- **.NET programs** need Wine Mono, which Wine offers to download on first use.
- Notification-area icons appear in a small floating window above the clock
  rather than inside the taskbar.
- The clock is UTC and the keyboard layout is US unless set at boot
  (`wc.kbd=fr`); there is no settings panel for either yet.
- Untested: real hardware, booting from a USB stick, sound output, UEFI boot
  of an *installed* disk. Secure Boot is not supported (the boot loader is
  unsigned).
- It has had no security review. Treat it as a single-user machine.

## WinCraft Assistant

*WinCraft Assistant* answers questions about WinCraft OS and about this
computer: why a program does not start or stops responding, what is
installed, how something works here. It uses an AI model from a provider you
choose, so it needs your own account with one, or a model server on your
network; WinCraft itself sends nothing anywhere until you set it up.

- **Setting it up.** Open *WinCraft Assistant* on the desktop, pick a
  provider, enter your API key, press *Load models*, choose one, *Test*,
  *Save*. It then has its own Start menu entry. Providers: Anthropic
  (Claude), OpenAI, OpenRouter, and any server with an OpenAI-compatible API,
  such as Ollama, llama.cpp or LM Studio on another computer (no key needed).
- **What it knows.** WinCraft's own documentation for the exact version it
  ships in (this README, the changelog, the notes on the Wine fixes) and a
  troubleshooting guide, plus an overview of this computer taken when the
  chat starts: memory, storage, network, installed and running programs,
  recent errors in the system logs.
- **What it can do.** Look things up with a fixed set of tools: running
  programs and their windows, installed programs, the system logs, the
  registry, the network, crash reports, storage. Each look-up is listed in the
  chat. Looking inside a program that is not responding (with Wine's
  debugger) asks first. Ending a program, changing a Wine DLL override,
  setting a value under HKEY_CURRENT_USER and downloading and running an
  installer each ask every time, in a dialog the model cannot answer.
- **What it cannot do.** Run commands, act as administrator, change system
  settings, install WinCraft, read your documents, browse the web.
- **Privacy.** What you type and what it looks up are sent to the provider
  you chose. *Chat > What the assistant can see* shows exactly what is sent
  at the start of a chat, and *Chat > Activity log* records every look-up and
  every permission asked. Settings, the activity log and chat transcripts are
  kept in `%APPDATA%\WinCraft Assistant`. The API key is kept there too,
  readable only by you, unless you untick *Remember the key*; in a live
  session all of it is lost at restart.

## Run it from a build

After [building](#build-it), the same ISO is in `out/`:

```sh
./wc run                 # boots out/wincraft-0.3-x86_64.iso in a QEMU window
./wc run --uefi          # same, UEFI firmware
./wc run --disk build/vm/disk.qcow2:20G     # with a blank disk to install to
./wc run --no-iso --disk build/vm/disk.qcow2   # boot the installed disk
```

`./wc run` shares the clipboard with the VM (text only): `wcclip` in the
guest keeps QEMU's clipboard channel and the desktop's clipboard in step.

QEMU uses KVM when `/dev/kvm` exists. Without it (virtualisation disabled in
the firmware setup) it falls back to software emulation, which works but is
roughly ten times slower.

To put it on a USB stick, write the ISO to the whole device
(`dd if=out/wincraft-0.3-x86_64.iso of=/dev/sdX bs=4M`, **erasing it**).

### Boot options

Edit the entry in the boot menu (`e`) to add these to the command line:

| Option | Effect |
|---|---|
| `wc.toram` | copy the system image to RAM; the medium can be removed |
| `wc.nodata` | ignore the data partition for this boot (a clean session) |
| `wc.kbd=<layout>` | keyboard layout, e.g. `fr`, `de`, `gb` |
| `wc.shell` | root shells on the second console (Ctrl+Alt+F2) and serial port |
| `nomodeset` | use the firmware framebuffer |

### Installing

Open **Install WinCraft OS** on the desktop, pick a disk, confirm. **The whole
disk is erased.** Resulting layout (MBR, bootable on BIOS and UEFI):

1. FAT32 `WINCRAFT` — boot loader, kernel, the system image
2. ext4 `WINCRAFT_DATA` — everything you install or change

## Build it

Nothing is installed on the host and no root access is needed: the build runs
in a rootless Arch Linux toolbox under `build/`, entered through user
namespaces.

```sh
./wc build        # toolbox, kernel, root filesystem, ISO  ->  out/
```

Host requirements: bash, curl, tar, zstd, util-linux (`unshare`),
`newuidmap`/`newgidmap` and a subuid/subgid range for your user. About 12 GB
of disk and, the first time, a few gigabytes of downloads. The kernel takes
about four minutes on 16 cores; everything else about five.

Individual steps: `./wc kernel | stage | wine | rootfs | iso`. `./wc desktop` runs
just the desktop session natively on a virtual display — the fast way to work
on the shell or the window manager (see `tools/xtest`). `./wc clean` removes
all build output.

### Patching Wine

Wine is taken from the Arch package, but fixes WinCraft needs before a Wine
release has them go in `wine/patches`. The build applies them to the upstream
source of the same release (`wine/VERSION`, checked against
`wine/SHA256SUMS`), rebuilds only what they touch, about half a minute, and
lays that over the packaged files.

- Windows-side modules and the Wine server can be patched this way. A patch
  to the server's protocol, to shared headers or to a module with a Unix-side
  part is refused: those have to match the rest of the package.
- The build stops when Arch moves to a newer Wine than `wine/VERSION`: bump
  the version and checksum, and drop the patches the new release already has.
- Each patch says why it exists and where WineHQ stands on the same problem,
  and stays only until a Wine release makes it unnecessary.
- WineHQ does not accept LLM-generated code. A patch written with an LLM, as
  the current ones were, cannot be sent there; it points to the matching
  WineHQ bug or merge request instead, when there is one.

## Layout

```
wc                     build and run driver
kernel/                kernel version and configuration fragment
initramfs/init         early userspace: find the image, assemble the root
boot/limine.conf       boot menu of the ISO
src/init/wcinit.c      PID 1
src/wm/wcwm.c          window manager
src/shell/wcshell.c    desktop shell, download tool, installer front end
src/agent/             WinCraft Assistant: chat window, model APIs, tools,
                       and its working rules and troubleshooting guide
src/tools/wcctl.c      talk to wcinit (power off, privileged helpers)
src/tools/wcclip.c     share the clipboard with a virtual machine's host
rootfs/packages.txt    packages the image is cut from
rootfs/overlay/        WinCraft's own system files
rootfs/wincraft.reg    registry defaults for the Windows environment
wine/                  pinned Wine release and WinCraft's patches to it
tools/                 toolbox entry, image assembler, build steps, VM runner,
                       test harnesses (xtest, qmp.py), component manifest,
                       the assistant's knowledge pack (assistant-knowledge.sh)
```

Each build also writes `out/wincraft-<version>-components.txt`: every
third-party package with a file in the image, with its version, licence and
upstream source.

## License

WinCraft's own code is MIT licensed (see `LICENSE`); the patches in
`wine/patches` are changes to Wine and carry Wine's license, LGPL-2.1+. A
built image also contains Linux (GPL-2.0), Wine (LGPL-2.1+) and other
components under their own licenses (see `NOTICE`).

WinCraft OS is not affiliated with or endorsed by Microsoft. "Windows" is a
trademark of Microsoft Corporation, used here only to describe compatibility.
