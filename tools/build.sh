#!/bin/bash
# WinCraft OS build steps. Runs inside the toolbox (see ../wc and tools/tb).
set -euo pipefail

W=/work
B=$W/build
S=$B/rootfs.arch        # staged Arch packages (pacman-managed)
R=$B/rootfs             # the root filesystem that ships
I=$B/iso                # ISO tree
VERSION=$(cat $W/VERSION)
KVER=$(cat $W/kernel/VERSION)
KSRC=$B/linux-$KVER
ISO=$W/out/wincraft-$VERSION-x86_64.iso

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

# --------------------------------------------------------------- kernel
step_kernel() {
    if [ ! -d "$KSRC" ]; then
        say "Fetching Linux $KVER"
        mkdir -p $B/dl/kernel
        local base=https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x
        curl -fL -o $B/dl/kernel/linux-$KVER.tar.xz $base/linux-$KVER.tar.xz
        curl -fsSL -o $B/dl/kernel/sha256sums.asc $base/sha256sums.asc
        (cd $B/dl/kernel && grep " linux-$KVER.tar.xz\$" sha256sums.asc | sha256sum -c -)
        tar -C $B -xf $B/dl/kernel/linux-$KVER.tar.xz
    fi
    say "Configuring Linux $KVER"
    cd "$KSRC"
    make -s x86_64_defconfig
    scripts/kconfig/merge_config.sh -m .config $W/kernel/wincraft.config >/dev/null
    make -s olddefconfig
    local missing=0
    while IFS= read -r line; do
        grep -qxF "$line" .config || { echo "  not applied: $line"; missing=1; }
    done < <(grep -E '^CONFIG_[A-Z0-9_]+=' $W/kernel/wincraft.config)
    [ $missing = 0 ] || { echo "kernel configuration fragment did not fully apply"; exit 1; }
    say "Building Linux $KVER"
    make -j"$(nproc)" bzImage
}

# ---------------------------------------------------------------- stage
step_stage() {
    say "Staging Arch packages"
    mkdir -p $S/var/lib/pacman $S/proc $S/dev $S/sys
    mountpoint -q $S/proc || mount -t proc proc $S/proc
    mountpoint -q $S/dev || mount --rbind /dev $S/dev
    mountpoint -q $S/sys || mount --rbind /sys $S/sys
    pacman --root $S --dbpath $S/var/lib/pacman --config $W/rootfs/pacman.conf \
        --cachedir /var/cache/pacman/pkg --gpgdir /etc/pacman.d/gnupg \
        -Sy --noconfirm --needed $(grep -v '^#' $W/rootfs/packages.txt | xargs) \
        > $B/stage.log 2>&1 || { tail -30 $B/stage.log; exit 1; }
    umount -R $S/proc $S/dev $S/sys 2>/dev/null || true
}

# --------------------------------------------------------------- native
# The host-side programs WinCraft adds: init, its control tool, and the
# window manager.
step_native() {
    mkdir -p $B/native
    gcc -O2 -static -Wall -Wextra -o $B/native/wcinit $W/src/init/wcinit.c
    gcc -O2 -Wall -Wextra -o $B/native/wcctl $W/src/tools/wcctl.c
    gcc -O2 -Wall -Wextra -Wno-unused-parameter -o $B/native/wcwm $W/src/wm/wcwm.c -lX11
    strip $B/native/*
}

# --------------------------------------------------------------- rootfs
in_root() {    # run a command chrooted into $R as the desktop user
    chroot --userspec=1000:1000 $R /usr/bin/env -i \
        HOME=/home/user USER=user LOGNAME=user PATH=/usr/bin LANG=C.UTF-8 \
        XDG_RUNTIME_DIR=/run/user/1000 WINEPREFIX=/home/user/.wine WINEDEBUG=-all \
        WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" "$@"
}

mount_root() {
    mountpoint -q $R/proc || mount -t proc proc $R/proc
    mountpoint -q $R/dev || mount --rbind /dev $R/dev
    mountpoint -q $R/tmp || mount -t tmpfs -o mode=1777 tmpfs $R/tmp
    mountpoint -q $R/run || mount -t tmpfs -o mode=755 tmpfs $R/run
    mkdir -p $R/run/user/1000 && chown 1000:1000 $R/run/user/1000
}

umount_root() {
    umount -R $R/proc $R/dev $R/tmp $R/run 2>/dev/null || true
}

step_rootfs() {
    [ -d $S/usr ] || step_stage
    say "Assembling root filesystem"
    umount_root
    rm -rf $R
    python3 $W/tools/assemble-rootfs.py $S $R

    say "Applying WinCraft system files"
    rm -rf $R/etc/{arch-release,crypttab,locale.gen,securetty,subuid,subgid,gshadow,profile.d,skel,mtab,resolv.conf,host.conf,gai.conf,rpc}
    rm -rf $R/usr/lib/os-release $R/srv $R/opt $R/boot $R/usr/local $R/usr/src $R/var/{spool,mail,opt,games,empty,local}
    rsync -a $W/rootfs/overlay/ $R/
    ln -sf /proc/self/mounts $R/etc/mtab
    ln -sf /run/resolv.conf $R/etc/resolv.conf
    ln -sf ../../etc/os-release $R/usr/lib/os-release
    mkdir -p $R/live/{medium,system,data} $R/media $R/home/user $R/root \
             $R/usr/lib/wincraft/priv.d $R/var/{log,cache,lib,tmp}
    chmod 600 $R/etc/shadow
    chmod 750 $R/root
    echo "$VERSION" > $R/etc/wincraft/version

    say "Building native components"
    step_native
    install -m755 $B/native/* $R/usr/bin/
    ln -sf wcinit $R/usr/bin/init
    for applet in $($R/usr/bin/busybox --list); do
        [ -e $R/usr/bin/$applet ] || ln -s busybox $R/usr/bin/$applet
    done

    if [ -d $W/src/shell ]; then
        say "Building the WinCraft shell"
        make -s -C $W/src/shell OUT=$B/shell
        install -m755 $B/shell/*.exe $R/usr/lib/wincraft/
    fi
    [ -d $W/assets ] && rsync -a $W/assets/ $R/usr/share/wincraft/

    say "Generating caches"
    ldconfig -r $R
    mount_root
    chroot $R fc-cache -s >/dev/null

    say "Creating the Windows environment (Wine prefix)"
    chown -R 1000:1000 $R/home/user
    in_root wineboot --init >/dev/null 2>&1
    in_root wineserver -w
    if [ -f $W/rootfs/wincraft.reg ]; then
        cp $W/rootfs/wincraft.reg $R/tmp/wincraft.reg
        in_root wine regedit /S 'Z:\tmp\wincraft.reg' >/dev/null 2>&1
    fi
    if [ -x $R/usr/lib/wincraft/wcshell.exe ]; then
        in_root wine /usr/lib/wincraft/wcshell.exe /setup >/dev/null 2>&1
    fi
    in_root wineserver -w
    umount_root
    rm -rf $R/home/user/.cache $R/home/user/.local $R/home/user/.config
    say "Root filesystem: $(du -sh $R | cut -f1)"
}

# ------------------------------------------------------------------ iso
step_iso() {
    [ -f $KSRC/arch/x86/boot/bzImage ] || step_kernel
    [ -x $R/usr/bin/wcinit ] || step_rootfs

    say "Building initramfs"
    rm -rf $B/initramfs && mkdir -p $B/initramfs/bin
    cp $S/usr/bin/busybox $B/initramfs/bin/busybox
    install -m755 $W/initramfs/init $B/initramfs/init
    (cd $B/initramfs && find . | LC_ALL=C sort | cpio --quiet -o -H newc -R 0:0 | zstd -q -19) > $B/initramfs.img

    say "Compressing system image"
    rm -rf $I && mkdir -p $I/boot/limine $I/EFI/BOOT $I/wincraft
    umount_root
    mksquashfs $R $I/wincraft/system.sfs -noappend -quiet -no-progress \
        -comp zstd -Xcompression-level 19 -b 1M -no-xattrs

    say "Building ISO"
    cp $KSRC/arch/x86/boot/bzImage $I/boot/vmlinuz
    cp $B/initramfs.img $I/boot/initramfs.img
    cp $W/boot/limine.conf $I/boot/limine/limine.conf
    cp /usr/share/limine/{limine-bios.sys,limine-bios-cd.bin,limine-uefi-cd.bin} $I/boot/limine/
    cp /usr/share/limine/{BOOTX64.EFI,BOOTIA32.EFI} $I/EFI/BOOT/
    echo "$VERSION" > $I/wincraft/version
    mkdir -p $W/out
    xorriso -as mkisofs -quiet -R -r -J -V WINCRAFT \
        -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
        -hfsplus -apm-block-size 2048 \
        --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
        --protective-msdos-label \
        $I -o $ISO
    limine bios-install $ISO >/dev/null 2>&1
    (cd $W/out && sha256sum "$(basename $ISO)" > "$(basename $ISO).sha256")
    step_manifest
    say "ISO ready: out/$(basename $ISO) ($(du -h $ISO | cut -f1))"
}

# ------------------------------------------------------------- manifest
# What the image is made of, for credit and for finding the matching sources.
step_manifest() {
    mkdir -p $W/out
    python3 $W/tools/manifest.py $S $R "$KVER" > $W/out/wincraft-$VERSION-components.txt
}

for step in "$@"; do
    case "$step" in
        kernel|stage|native|rootfs|iso|manifest) step_$step ;;
        all) step_kernel; step_stage; step_rootfs; step_iso ;;
        *) echo "unknown step: $step" >&2; exit 2 ;;
    esac
done
