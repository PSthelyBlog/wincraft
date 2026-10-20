# Changelog

## 0.7.13 — 2026-10-20

The WinCraft Agent can build four settings that WinCraft lacked.

- **Key repeat, screen sleep, pointer speed and left-handed mouse
  buttons.** These belong to the display server: Windows programs follow
  it, and Wine keeps the Windows settings for them without applying them.
  The image now has the display server's own tools for them, `xset` and
  `xinput` (about 100 KB), so the agent can build a feature that sets them
  at each login. Turning the feature off undoes it at the next restart.
  Left-handed buttons made this way work in every Windows program, which
  Windows' own setting does not do in Wine. Pointer speed has no effect on
  a virtual machine's tablet pointer.
- WinCraft Assistant now offers the agent for these four requests, instead
  of saying WinCraft cannot change them.
- Everything else is as in 0.7.12.

## 0.7.12 — 2026-10-20

A fix for the WinCraft Agent.

- **The agent tests times of day in your local time.** Its sandbox starts
  every command the way a restart does, before the boot hooks have run, so
  a time zone set by a feature is missing there and the sandbox's clock
  reads UTC. An agent building something for a time of day could test it
  against the wrong clock, or convert times by hand and be off by an hour or
  two, and again after a daylight saving change. Its rules now say so, ask
  for features that read the local time, and show how to test in the
  computer's time zone.
- Everything else is as in 0.7.11.

## 0.7.11 — 2026-10-20

A security fix for the WinCraft Agent's sandbox. Update if you use the agent.

- **The sandbox can no longer reach your screen.** The display server also
  listened on a socket that the sandbox could reach, without access control,
  so a command run there could connect to your display: read it, change its
  layout or keyboard mapping, show windows on it, and, with a tool it
  downloaded, type and click in your session, the agent's own approval
  dialogs included. Nothing in WinCraft's own tests did more than read the
  screen's size, and nothing shows that it happened. The display server now
  accepts only programs outside the sandbox.
- The agent's rules say that a window opened in the sandbox cannot be seen,
  so the agent tests a program's other effects instead.
- Everything else is as in 0.7.10.

## 0.7.10 — 2026-10-20

A fix for WinCraft Assistant.

- **The assistant no longer changes Windows settings that do nothing in
  WinCraft.** Wine keeps many of them in the registry without applying them:
  asked to swap the mouse buttons, the assistant set `SwapMouseButtons`,
  which Wine reports but never applies to the clicks; asked for a wider text
  cursor, it set `CaretWidth`, which Wine's text boxes ignore. Its guide now
  lists these settings, found by going through every Windows setting Wine
  keeps: pointer speed, trails and size, the submenu delay, focus following
  the mouse, most visual effects, the screen saver and power times, and
  others. It also lists the values Wine writes for itself, and what WinCraft
  draws on its own: the desktop, the taskbar and the Start menu. For these
  the assistant now says that WinCraft cannot change them yet.
- Everything else is as in 0.7.9.

## 0.7.9 — 2026-10-19

A fix for WinCraft Assistant.

- **"Make the text bigger" changes the right setting.** The assistant raised
  a value Wine keeps for itself (`LogPixels` under
  `HKEY_CURRENT_USER\Software\Wine\Fonts`), which Wine sets back on its own,
  and said the text was bigger. Asked again, it took a machine-wide value it
  cannot change for the real setting and gave up. Its guide now names
  `LogPixels` under `HKEY_CURRENT_USER\Control Panel\Desktop`, and says the
  new size shows only after WinCraft restarts: Wine fixes the screen's size
  settings when the desktop starts, so a program started again before that
  looks the same.
- Its guide also says that how fast a held key repeats is the display
  server's, not Windows': the Windows values for it do nothing in WinCraft.
- Everything else is as in 0.7.8.

## 0.7.8 — 2026-10-19

A fix for the WinCraft Agent's sandbox.

- **A command in the sandbox is really stopped after 300 seconds**, with
  everything it started. A test the agent ran looped for 27 minutes, and
  every later command of the task waited behind it: the command was the
  first process of the sandbox's process list, which ignores the signal
  meant to stop it.
- **Closing the agent's window ends its sandbox at once**, even while a
  command is running; before, it waited for that command to finish.
- The agent's rules warn that BusyBox's shell runs its own `setpriv`,
  `unshare`, `date`, `sleep` and others before those of `PATH`: the
  agent's first nightly-shutdown feature called a `setpriv` without the
  option it needed, and its tests, which replaced it, did not show it.
- Everything else is as in 0.7.7.

## 0.7.7 — 2026-10-19

A fix for WinCraft Assistant and the WinCraft Agent.

- **An answer cut off at the length limit no longer ends a task without a
  word.** The WinCraft Agent stopped silently while writing a nightly
  shutdown service: its answer reached the 4,096-token limit in the
  middle of writing a file, and the cut-off answer passed for a finished
  one. Answers may now be 16,384 tokens long. When one is cut off anyway,
  the chat says so, nothing in it is run (a cut-off file could be
  incomplete), and the model is asked for the rest in smaller steps.
- The agent's rules ask for long scripts to be written in parts.
- Everything else is as in 0.7.6.

## 0.7.6 — 2026-10-19

A fix for WinCraft Assistant and the WinCraft Agent.

- **A model that takes more than 21 seconds to start answering no longer
  fails** with "Could not reach the provider (error 10060)". Wine's WinHTTP
  waits only 21 seconds for the first bytes of an answer unless told
  otherwise, and the 300 seconds the assistant asked for did not cover that
  wait. The WinCraft Agent hit it twice in a row while writing a service
  that shuts the computer down every night; it now waits up to 300 seconds.
- **A failed request is recorded in the activity log** (*Chat > Activity
  log*) with its size, the step that failed (connecting, waiting for the
  answer, reading it) and how long it took.
- Everything else is as in 0.7.5.

## 0.7.5 — 2026-10-19

Windows programs in your language, and a sturdier clock.

- **A sixth folder for features: `environment.d`.** Its `KEY=value` lines
  are variables for the whole desktop session, set before Wine starts, so
  every Windows program inherits them: a language, a sound card
  (`ALSA_CARD`), a frame counter for games (`GALLIUM_HUD=fps`), a mapping
  for an unknown gamepad (`SDL_GAMECONTROLLERCONFIG`). The variables the
  session depends on (`PATH`, `HOME`, `DISPLAY`, `WINEPREFIX`, `LD_*` and a
  few others) are refused, and the boot menu's *without your features*
  entry leaves them all out.
- **Windows programs can be in your language.** `LANG=fr_FR.UTF-8` shows
  them in French, with French dates and numbers: Notepad becomes
  *Bloc-notes*. Wine has the translations but ignores a language whose
  locale data is missing, and the image had only `C.UTF-8`; it now has
  17 European locales (de_AT, de_CH, de_DE, en_GB, es_ES, es_MX, fr_BE,
  fr_CA, fr_CH, fr_FR, it_IT, nl_BE, nl_NL, pl_PL, pt_BR, pt_PT, sv_SE),
  built from the image's own glibc. WinCraft's own windows stay in English.
- **The clock recovers sooner.** The time service asks four `pool.ntp.org`
  servers instead of one address found at start: after a host's sleep, the
  single server it had stopped answering for eight minutes, and the clock
  stayed eight seconds off until it came back.
- WinCraft Assistant's overview gives the language of Windows programs.
- Everything else is as in 0.7.4.

## 0.7.4 — 2026-10-18

A security fix for the WinCraft Agent's sandbox.

- **The kernel's settings are out of the sandbox's reach.** In 0.7 to
  0.7.3 the sandbox shared `/proc/sys` with the computer, and its root
  could change kernel settings of the running system, which the
  capabilities it lacks did not cover. Testing a feature that names the
  computer, the agent renamed the running computer for a moment, then
  named it back. Now each command in the sandbox has its own computer name,
  `/proc/sys` and the other kernel setting areas are read-only, and the
  `/proc` files that act on the machine are masked; if any of that cannot
  be set up, the command does not run.
- The agent's rules say that a kernel setting cannot take effect in the
  sandbox, so a hook that changes one has to be checked another way.
- Everything else is as in 0.7.3.

## 0.7.3 — 2026-10-18

WinCraft Assistant sees WinCraft's own services, and the clock as it is.

- **The assistant's overview of the computer lists WinCraft's services**
  (time sync, network, logging, the desktop, and those features add), with
  whether each is running, and the features added to the computer. Its
  tools see only Windows programs: asked to keep the clock in sync, it had
  said that WinCraft lacked time sync, which it has always had. wcinit now
  records which process each service is in
  `/run/wincraft/services.status`, since a script's process is only `sh`.
- **The overview gives the clock as programs see it**: local date and time,
  the offset from UTC and the time zone. The assistant had said the clock
  showed UTC on a computer whose time zone feature set Paris time.
- **The `time` service keeps `ntpd`'s messages** in its log,
  `/run/log/time.log`, instead of discarding them.
- The README says that the clock follows internet time.
- Everything else is as in 0.7.2.

## 0.7.2 — 2026-10-18

The WinCraft Agent is asked to build features that undo themselves.

- **Disabling or removing a feature the agent makes should bring the
  computer back to how it was.** Its rules now ask that hooks write nothing
  lasting outside the feature's folder: what they make goes in `/run`,
  which is empty at every start, and where a system path is needed, a fixed
  link into `/run` leads nowhere once the feature is off. Registry
  settings, which stay after a feature is removed, are kept for when
  nothing else works, and the feature's description says so. The agent is
  asked to test the undo in its sandbox too.
- Found with the agent's time zone feature: its first version linked
  `/etc/localtime` straight to the zone file, so turning it off would have
  kept Paris time. Refused and asked, the agent rebuilt it the way the
  rules now describe; after a restart, Linux and Windows programs showed
  Paris time.
- Everything else is as in 0.7.1.

## 0.7.1 — 2026-10-18

A fix for the WinCraft Assistant and the WinCraft Agent.

- **A request to the AI provider no longer fails when a tool returns
  bytes that are not text.** When a command's output, a log or a page held
  them, for example a search that read the system's own programs, the
  provider refused the request ("The request body is not valid JSON:
  invalid unicode code point"), and every later one in the same chat. Such
  bytes are now sent as the replacement character �. Found by the
  agent's first time zone task.
- The agent's rules ask it not to print binary files, which only waste its
  output.
- Everything else is as in 0.7.

## 0.7 — 2026-10-18

WinCraft can now grow on one computer: *features* add what it lacks, and
the WinCraft Agent builds them.

- **Features.** A feature adds to WinCraft, on one computer, something it
  does not do yet: a setting it lacks, something to do at every start or
  login, registry settings for every program. It is a folder of root's in
  `/var/lib/wincraft/features` whose files are linked into folders the
  system image reads: `settings.d`, `hooks/boot.d`, `hooks/session.d`,
  `services.d` and `registry.d` under `/etc/wincraft`. No system file is
  replaced, so features survive updates. A hook that fails or hangs is
  logged and skipped, and never stops the computer from starting.
- ***WinCraft Features*** in the Start menu lists them, with what each does,
  who made it and for what request, and disables, enables or removes them.
  The boot menu's ***WinCraft OS (without your features)*** entry starts
  without any of them, to repair one that keeps the computer from working.
- **The WinCraft Agent.** When WinCraft lacks what you need, WinCraft
  Assistant says what is missing and, if you allow it, hands the task to the
  agent, which opens in its own window:
  - it shows you its plan first, and nothing runs until you approve it;
  - it works as administrator in a sandbox, a copy of the system in memory
    that is thrown away at the end, with the network but without disks,
    your files, or the power to mount, make devices or leave it; every
    command it runs is listed in its window;
  - it shows you the feature's files in full and every other change it
    made, and only if you approve is the feature added and enabled.

  Tested with Claude Sonnet 5.5: asked for a French keyboard that stays, it
  built a feature that applies the layout at every start. The agent uses the
  provider set up in WinCraft Assistant, and takes more requests than a chat.
  Any program running as you can drive its sandbox: see the README's
  security note.
- **Long requests for permission** (the agent's plan and result, a long
  task) open in a window that scrolls, where Enter refuses.
- For `./wc run` (building from source): `tools/qmp.py` no longer hangs when
  a screenshot has to come from the guest.
- The kernel, Wine and the Arch packages are those of 0.6.1.

## 0.6.2 — 2026-10-18

Installing and using WinCraft, improved after a fresh install from start to
finish.

- **An installed WinCraft starts from its disk**, even with the installation
  medium still in the computer. Its boot menu names its own two partitions by
  UUID; before, it looked for a label that the medium carries too. An
  installation CD is ejected as the computer restarts.
- **The installer can leave room on the disk.** *Space for WinCraft* gives it
  less than the whole disk, from 5 GB, and leaves the rest empty, and *Data
  partition name* names its data partition. The whole disk is still erased
  first. When there is only one disk, it is selected already.
- ***Install WinCraft OS* only appears where it can be used**: on the
  desktop and in the Start menu of a session started from the installation
  medium, no longer on an installed system.
- **Start menu folders.** A program that installs several shortcuts in a
  folder of its own gets one Start menu entry, which opens in place; a folder
  with a single shortcut shows just that program. Search still finds what is
  inside folders.
- **Sound works**, tested in a virtual machine: SuperTuxKart 1.5 plays with
  its sound on the host's speakers.
- An installed disk is now tested on UEFI firmware too.
- For a virtual machine started with `./wc run` (building from source):
  - **3D acceleration**: a virtio GPU with virgl draws the guest's OpenGL on
    the host's graphics card, so SuperTuxKart plays well. The guest takes the
    window's size when it starts. `--no-gl` gives the old VGA adapter.
  - **`--gamepads`** hands the game controllers connected when the VM starts
    to it; Windows programs see an Xbox controller as an XInput gamepad.
  - **The VM boots through its firmware, disk first**, like a computer:
    install, restart, and the installed system comes up. `--shell` adds the
    development root shell without changing that.
- The kernel, Wine and the Arch packages are those of 0.6.1.

## 0.6.1 — 2026-10-17

Security fixes, and the first release built from a pinned package snapshot.

- **Linux 6.18.57**, the newest point release of the 6.18 long-term series,
  with its fixes. 0.6 had 6.18.56.
- **Arch packages as of 2026-10-16**, three days newer than 0.6's:
  - freetype2 2.15.0, the font renderer, which reads fonts from programs and
    web pages;
  - new builds of zlib and the X server.
- Wine stays at 11.19. Arch moved to Wine 11.20 on 2026-10-17; that will
  come in a release of its own, with WinCraft's Wine patches checked against
  it.
- **Builds are reproducible from now on.** The Arch packages come from the
  Arch Linux Archive for a fixed date, set in `rootfs/pacman.conf`.
- **`./wc security`** lists the published security fixes a release lacks,
  and a weekly job runs it on the latest release. See "Security updates" in
  the README.

## 0.6 — 2026-10-17

WinCraft Assistant can check the web, and its answers are easier to read.

- **The assistant reads web pages, after asking.** It offered the version of
  a program it remembered: VLC 3.0.21 when 3.0.24 was out, a LibreOffice two
  years old. Now it reads the program's official page first, and takes the
  version and the download address from there. It also finds Wine's
  compatibility reports for a program on Wine's AppDB.
  - The first page of a chat asks you; you can allow the rest of that chat.
    Every page read is listed in the chat.
  - It reads only https pages on the public internet, never addresses on
    your own network, even when a site redirects there, and keeps no
    cookies.
  - A page's text counts as data, not instructions, and is cut at about
    6,000 tokens.
  - *Chat > Settings* can switch this off.
- **Answers show as rich text.** Lists, bold and italics show as such, file
  paths in a fixed-width font, and a table as a list. Web addresses are
  links: a click opens them in the browser you installed, such as Firefox
  from *Download a Program*.
- **Permission dialogs show the whole address.** Wine's dialogs dropped
  every "&" and cut off long addresses, so a download's address could show
  wrongly.
- **The assistant knows QEMU's default network.** WinCraft can connect out
  from it to your network, and to the host at 10.0.2.2.

Tested with Claude Sonnet 5.5 through Anthropic's API on an installed
system. The assistant found VLC 3.0.24 on VideoLAN's page and installed it
over 3.0.21. It installed PuTTY 0.85 from PuTTY's site. It read Wine's AppDB
ratings for four programs, and a link in its answer opened in Firefox.

## 0.5 — 2026-10-17

Fixes found while using 0.4, most of them through WinCraft Assistant.

- **.NET Framework programs run.** Wine Mono, Wine's stand-in for the .NET
  Framework, is now built in. On 0.4 a program such as KeePass 2 exited
  without a message: Wine offers to download Wine Mono only while it sets up
  its Windows folder, which happens when the image is built, and the README
  was wrong to say otherwise. Programs for .NET 5 and later still need
  Microsoft's .NET runtime, and say which one.
- **The assistant no longer runs a web page as an installer.** It downloaded
  as a web browser, and some sites answer a browser with a download page:
  asked for VLC, it saved VideoLAN's page as the installer and could not tell
  why it would not start. It now downloads under its own name, runs only
  Windows programs and .msi packages, deletes anything else unrun, and says
  what it got instead.
- **The assistant shows where each running program's file is**, and reports a
  program it does not know as such instead of guessing from the name. Wine's
  `lsass.exe` is no longer listed as a program you started.
- **The assistant sets a Wine library override for any program name**, such as
  `notepad++.exe` or `My Game (x86).exe`. It refused some names before, and
  garbled names with accents.

Tested with Claude Sonnet 5.5 through Anthropic's API: the assistant
downloaded and started VLC's installer and LibreOffice's .msi package. KeePass
2.61 runs in a live session with nothing to install.

## 0.4 — 2026-10-16

- **WinCraft Assistant.** A chat about WinCraft OS and this computer, with
  an AI model from a provider you choose: Anthropic, OpenAI, OpenRouter, or a
  model server on your network such as Ollama. It knows WinCraft from the
  documentation built into this version and looks things up on the computer
  as it goes: running and installed programs, the system logs, the network,
  crash reports. Ending a program, changing a Wine library override or a
  registry setting, and downloading an installer wait for your permission.
  Open *WinCraft Assistant* on the desktop to set it up.
- **The clipboard is shared with QEMU.** Text copied on the host pastes into
  Windows programs in the virtual machine, and back. `./wc run` does it; the
  README has the options for your own QEMU command.
- `tools/vm --iso FILE` boots an ISO other than the one in `out/`.

Tested with Claude Sonnet 5.5 through Anthropic's API, and against a scripted
server for both API formats.

## 0.3 — 2026-10-15

Fixes found while using 0.2 from day to day.

- Firefox no longer starts "Not Responding" at random. When Firefox asks
  Windows which security restrictions apply to it, Wine reported success
  without filling in the answer, so Firefox read leftover memory. When that
  said "locked down", it stopped reading window messages for good. This
  happened in about half of all starts.
- The notification area no longer disappears a few seconds after the last
  File Explorer window is closed. Wine's server took the desktop for
  abandoned and closed Wine's desktop process while the shell was still on it.
- `wine/patches` can carry fixes to the Wine server too, not only to
  Windows-side modules.

Tested with Firefox 158: it answered on 10 of 10 fresh starts, where it hung
on 3 of 5 before.

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
