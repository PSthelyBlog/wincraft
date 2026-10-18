#!/bin/sh
# WinCraft OS system bring-up: run once by wcinit, before services start.

hostname -F /etc/hostname
ip link set lo up

mkdir -p /run/wincraft /run/user/1000 /run/lock /tmp/.X11-unix
chown 1000:1000 /run/user/1000
chmod 700 /run/user/1000
chmod 1777 /tmp/.X11-unix
: > /run/resolv.conf
: > /run/wincraft/services

# A live session: the writable layer is memory, not a WINCRAFT_DATA partition.
grep -q '^wincraft / overlay ' /proc/mounts &&
    ! grep -q ' /live/data ' /proc/mounts && : > /run/wincraft/live

# Boot options (see README): wc.shell, wc.kbd=<layout>
shell= serial=
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        wc.kbd=*)  echo "${arg#wc.kbd=}" > /run/wincraft/kbd ;;
        wc.shell)  shell=1 ;;
        console=ttyS0*) serial=1 ;;
    esac
done
# A virtual machine started for development (./wc run --shell) asks for the
# same shells with an SMBIOS OEM string from its host, so that it can boot
# through its own boot menu like any computer.
grep -q -a 'wincraft\.shell' /sys/firmware/dmi/tables/DMI 2>/dev/null && shell=1 serial=1
if [ -n "$shell" ]; then
    # Root shells for rescue and development, only when asked for.
    echo "tty2 respawn exec getty -n -l /bin/sh 38400 tty2 linux" >> /run/wincraft/services
    [ -n "$serial" ] &&
        echo "ttyS0 respawn exec getty -n -l /bin/sh -L 115200 ttyS0 vt100" >> /run/wincraft/services
fi

sysctl -q -p /etc/sysctl.conf

# Windows executables are a native executable format: the kernel hands any
# PE binary ("MZ" magic) to the Windows runtime, so ./setup.exe just runs.
mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc 2>/dev/null
echo ':windows:M::MZ::/usr/bin/wine:' > /proc/sys/fs/binfmt_misc/register 2>/dev/null

# Device manager: populates /dev permissions and the input device database
# the display server relies on.
/usr/lib/systemd/systemd-udevd --daemon >/dev/null 2>&1
udevadm trigger --action=add --type=subsystems
udevadm trigger --action=add --type=devices
udevadm settle --timeout=20

# A virtual machine that shares its clipboard (QEMU's qemu-vdagent, SPICE)
# has a port for it: keep it in step with the desktop's clipboard.
for port in /sys/class/virtio-ports/*; do
    [ "$(cat "$port/name" 2>/dev/null)" = com.redhat.spice.0 ] &&
        echo "clipboard respawn exec /usr/bin/wcclip /dev/${port##*/}" >> /run/wincraft/services
done

# Compressed swap in RAM keeps small machines usable.
if [ -b /dev/zram0 ]; then
    half=$(awk '/MemTotal/ {print int($2 / 2)}' /proc/meminfo)
    echo "${half}K" > /sys/block/zram0/disksize &&
        mkswap /dev/zram0 >/dev/null && swapon -p 100 /dev/zram0
fi

# Sound cards power up muted: set sensible playback levels.
for card in /proc/asound/card[0-9]*; do
    [ -d "$card" ] || continue
    for control in Master Headphone Speaker PCM Front 'Line Out'; do
        amixer -q -c "${card##*card}" sset "$control" 80% unmute 2>/dev/null
    done
done

hwclock -s -u 2>/dev/null
mount -a 2>/dev/null
exit 0
