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
    gcc -O2 -Wall -Wextra -Wno-unused-parameter -o $B/native/wcclip $W/src/tools/wcclip.c -lX11 -lXfixes
    strip $B/native/*
}

# ----------------------------------------------------------------- wine
# Wine itself comes from the Arch package. The patches in wine/patches are
# applied to the upstream source of the same release, and only what they touch
# is rebuilt (with the package's own build options) and laid over the packaged
# files in $B/wine-overlay: Windows-side modules, which are nearly all of Wine,
# and the Wine server.
#
# A patch to the server's protocol, to shared headers or to a module with a
# Unix-side part needs a full Wine build, because those have to match the rest
# of the package; this step refuses it rather than build a mismatched half.
step_wine() {
    local out=$B/wine-overlay src=$B/wine-src bld=$B/wine-build
    local patches=($W/wine/patches/*.patch)
    [ -e "${patches[0]}" ] || { rm -rf $out $out.stamp; return 0; }
    [ -d $S/usr ] || step_stage

    local ver staged stamp
    ver=$(cat $W/wine/VERSION)
    staged=$(pacman --root $S --dbpath $S/var/lib/pacman -Q wine | cut -d' ' -f2)
    if [ "${staged%-*}" != "${ver/-rc/rc}" ]; then
        echo "wine: the staged package is $staged, but wine/VERSION pins $ver." >&2
        echo "      Set wine/VERSION and wine/SHA256SUMS to the new release and check" >&2
        echo "      that every patch in wine/patches still applies and is still needed." >&2
        exit 1
    fi
    stamp=$({ cat $W/wine/VERSION $W/wine/SHA256SUMS "${patches[@]}"; declare -f step_wine; } |
            sha256sum | cut -d' ' -f1)
    [ "$(cat $out.stamp 2>/dev/null)" = "$stamp" ] && [ -d $out ] && return 0

    local tar=wine-$ver.tar.xz series=${ver%%.*}.x
    case "$ver" in *.0|*.0.*|*.0-rc*) series=${ver%%.*}.0 ;; esac
    mkdir -p $B/dl/wine
    if [ ! -f $B/dl/wine/$tar ]; then
        say "Fetching Wine $ver"
        curl -fL -o $B/dl/wine/$tar https://dl.winehq.org/wine/source/$series/$tar
    fi
    grep -q " $tar\$" $W/wine/SHA256SUMS ||
        { echo "wine: wine/SHA256SUMS has no entry for $tar" >&2; exit 1; }
    (cd $B/dl/wine && grep " $tar\$" $W/wine/SHA256SUMS | sha256sum -c --quiet -)

    say "Patching Wine $ver"
    rm -rf $src $bld $out $out.stamp
    mkdir -p $src $bld
    tar -C $src --strip-components=1 -xf $B/dl/wine/$tar
    local p
    for p in "${patches[@]}"; do
        patch -d $src -p1 -s --no-backup-if-mismatch < "$p" ||
            { echo "wine: $(basename "$p") does not apply to Wine $ver" >&2; exit 1; }
    done

    # What to rebuild: the dlls/<name> and programs/<name> directories the
    # patches touch (tests are not built), and the server. The server's
    # protocol is generated into files shared with the rest of Wine.
    local dirs bad
    dirs=$(sed -n 's|^+++ b/||p' "${patches[@]}" | cut -f1 | awk -F/ '
        ($1 == "dlls" || $1 == "programs") && NF >= 3 { if ($3 != "tests") print $1 "/" $2; next }
        $1 == "server" && NF == 2 && $2 !~ /^(protocol\.def|request_handlers\.h|request_trace\.h)$/ { print "server"; next }
        { print "!" $0 }' | sort -u)
    bad=$(echo "$dirs" | sed -n 's/^!//p')
    if [ -n "$bad" ]; then
        echo "wine: these patched files need a full Wine build:" >&2
        echo "$bad" | sed 's/^/        /' >&2
        exit 1
    fi

    local d module arch targets=()
    for d in $dirs; do
        if [ $d = server ]; then
            targets+=(server/wineserver)
            continue
        fi
        module=$(sed -n 's/^MODULE *= *//p' $src/$d/Makefile.in)
        [ -n "$module" ] || { echo "wine: $d does not build a module" >&2; exit 1; }
        if grep -q '^UNIXLIB' $src/$d/Makefile.in; then
            echo "wine: $module has a Unix-side part, which needs a full Wine build" >&2
            exit 1
        fi
        for arch in x86_64 i386; do
            if [ -e $S/usr/lib/wine/$arch-windows/$module ]; then
                targets+=($d/$arch-windows/$module)
            fi
        done
    done
    [ ${#targets[@]} -gt 0 ] || { echo "wine: the patches touch no module of the staged package" >&2; exit 1; }

    say "Building $(printf '%s\n' "${targets[@]##*/}" | sort -u | xargs) for Wine $ver"
    # The options the Arch package is built with, so that the rebuilt files
    # match the ones they replace: its configure options and cross-compiler
    # flags, and makepkg's flags for the host side (the server).
    local cflags ldflags
    cflags=$(bash -c '. /etc/makepkg.conf && printf %s "$CFLAGS"')
    ldflags=$(bash -c '. /etc/makepkg.conf && printf %s "$LDFLAGS"')
    (cd $bld && CFLAGS="$cflags" LDFLAGS="$ldflags" CROSSCFLAGS="-O2 -pipe -g" CROSSLDFLAGS="-Wl,-O1" \
        $src/configure --prefix=/usr \
        --libdir=/usr/lib --disable-tests --enable-archs=x86_64,i386 --enable-build-id &&
        make -j"$(nproc)" "${targets[@]}") > $B/wine-build.log 2>&1 ||
        { tail -30 $B/wine-build.log; exit 1; }

    local t strip
    for t in "${targets[@]}"; do
        if [ $t = server/wineserver ]; then
            mkdir -p $out/usr/bin
            strip --strip-all -o $out/usr/bin/wineserver $bld/$t
            chmod --reference=$S/usr/bin/wineserver $out/usr/bin/wineserver
            if grep -aq /dev/ntsync $S/usr/bin/wineserver && ! grep -aq /dev/ntsync $out/usr/bin/wineserver; then
                echo "wine: the rebuilt wineserver lacks ntsync support (no linux/ntsync.h in the toolbox?)" >&2
                exit 1
            fi
            continue
        fi
        arch=${t%-windows/*}; arch=${arch##*/}
        case $arch in
            i386) strip=i686-w64-mingw32-strip ;;
            *)    strip=x86_64-w64-mingw32-strip ;;
        esac
        mkdir -p $out/usr/lib/wine/$arch-windows
        $strip --strip-unneeded -o $out/usr/lib/wine/$arch-windows/${t##*/} $bld/$t
        chmod --reference=$S/usr/lib/wine/$arch-windows/${t##*/} $out/usr/lib/wine/$arch-windows/${t##*/}
    done
    echo "$stamp" > $out.stamp
}

# --------------------------------------------------------------- rootfs
# mshtml is disabled so that creating the prefix does not stop to offer the
# Wine Gecko download. mscoree stays on: the image ships Wine Mono in
# /usr/share/wine/mono, and wineboot sets it up from there, without asking.
in_root() {    # run a command chrooted into $R as the desktop user
    chroot --userspec=1000:1000 $R /usr/bin/env -i \
        HOME=/home/user USER=user LOGNAME=user PATH=/usr/bin LANG=C.UTF-8 \
        XDG_RUNTIME_DIR=/run/user/1000 WINEPREFIX=/home/user/.wine WINEDEBUG=-all \
        WINEDLLOVERRIDES="mshtml=;winemenubuilder.exe=d" "$@"
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
    if [ -d $W/src/agent ]; then
        say "Building WinCraft Assistant"
        make -s -C $W/src/agent OUT=$B/agent
        install -m755 $B/agent/wcagent.exe $R/usr/lib/wincraft/
        mkdir -p $R/usr/share/wincraft/assistant
        bash $W/tools/assistant-knowledge.sh > $R/usr/share/wincraft/assistant/knowledge.md
    fi
    [ -d $W/assets ] && rsync -a $W/assets/ $R/usr/share/wincraft/

    step_wine
    if [ -d $B/wine-overlay ]; then
        say "Applying patched Wine modules"
        rsync -a $B/wine-overlay/ $R/
    fi

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
    python3 $W/tools/manifest.py $S $R "$KVER" $W/wine/patches > $W/out/wincraft-$VERSION-components.txt
}

for step in "$@"; do
    case "$step" in
        kernel|stage|native|wine|rootfs|iso|manifest) step_$step ;;
        all) step_kernel; step_stage; step_rootfs; step_iso ;;
        *) echo "unknown step: $step" >&2; exit 2 ;;
    esac
done
