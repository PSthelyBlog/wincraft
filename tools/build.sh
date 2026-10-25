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

# -------------------------------------------------------------- version
# Some files carry the version themselves rather than read VERSION: the boot
# menu, the system's name for itself, and WinCraft's Windows programs with
# their resources and manifests. A release changes them all; this stops the
# build when one was missed.
version_lines() {
    local v=${VERSION//./\\.} four c f
    four=$(echo "$VERSION.0.0.0" | cut -d. -f1-4)    # Windows wants four numbers: 0.8 -> 0.8.0.0
    c=${four//./,} four=${four//./\\.}
    cat <<EOF
boot/limine.conf ^interface_branding: WinCraft OS $v\$
rootfs/overlay/etc/issue ^WinCraft OS $v \\(
rootfs/overlay/etc/os-release ^PRETTY_NAME="WinCraft OS $v"\$
rootfs/overlay/etc/os-release ^VERSION="$v"\$
rootfs/overlay/etc/os-release ^VERSION_ID=$v\$
src/shell/wcshell.c ^#define WC_VERSION +L"$v"\$
src/agent/agent.h ^#define WC_VERSION +L"$v"\$
EOF
    for f in src/shell/wcshell src/agent/wcagent; do
        cat <<EOF
$f.rc ^FILEVERSION $c\$
$f.rc ^PRODUCTVERSION $c\$
$f.rc "FileVersion", "$v"\$
$f.rc "ProductVersion", "$v"\$
$f.manifest name="WinCraft\.[A-Za-z]+" version="$four"
EOF
    done
}

check_version() {
    local file pattern bad=0
    while read -r file pattern; do
        grep -Eq -- "$pattern" $W/$file ||
            { echo "version: $file does not say $VERSION where it should (/$pattern/)" >&2; bad=1; }
    done < <(version_lines)
    [ $bad = 0 ] || { echo "version: set the files above to VERSION ($VERSION)" >&2; exit 1; }
}

# --------------------------------------------------------------- kernel
step_kernel() {
    if [ ! -d "$KSRC" ]; then
        say "Fetching Linux $KVER"
        mkdir -p $B/dl/kernel
        local base=https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x
        curl -fL -o $B/dl/kernel/linux-$KVER.tar.xz $base/linux-$KVER.tar.xz
        # the checksums from kernel.org itself: the CDN's copy can lag days
        # behind a new release
        curl -fsSL -o $B/dl/kernel/sha256sums.asc \
            https://www.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/sha256sums.asc
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
# The Arch Linux Archive date rootfs/pacman.conf takes packages from. The stage
# records the date it was made from; a stage from another date is made again
# from nothing, so that it holds exactly that day's packages.
snapshot() {
    sed -n 's|^Server *= *https://archive\.archlinux\.org/repos/\([0-9]*/[0-9]*/[0-9]*\)/.*|\1|p' \
        $W/rootfs/pacman.conf | sort -u
}

stage_ready() {
    [ -d $S/usr ] && [ "$(cat $S/var/lib/pacman/wincraft-snapshot 2>/dev/null)" = "$(snapshot)" ]
}

step_stage() {
    local date m
    date=$(snapshot)
    if [ -z "$date" ] || [ "$(echo "$date" | wc -l)" != 1 ]; then
        echo "stage: the Server lines in rootfs/pacman.conf must name one archive date" >&2
        exit 1
    fi
    if [ -d $S/usr ] && ! stage_ready; then
        say "Snapshot is now $date: staging again from nothing"
        umount -R $S/proc $S/dev $S/sys 2>/dev/null || true
        for m in proc dev sys; do
            if mountpoint -q $S/$m; then
                echo "stage: $S/$m is still mounted; not deleting the stage" >&2
                exit 1
            fi
        done
        rm -rf $S
    fi
    say "Staging Arch packages as of $date"
    mkdir -p $S/var/lib/pacman $S/proc $S/dev $S/sys
    mountpoint -q $S/proc || mount -t proc proc $S/proc
    mountpoint -q $S/dev || mount --rbind /dev $S/dev
    mountpoint -q $S/sys || mount --rbind /sys $S/sys
    pacman --root $S --dbpath $S/var/lib/pacman --config $W/rootfs/pacman.conf \
        --cachedir /var/cache/pacman/pkg --gpgdir /etc/pacman.d/gnupg \
        -Sy --noconfirm --needed $(grep -v '^#' $W/rootfs/packages.txt | xargs) \
        > $B/stage.log 2>&1 || { tail -30 $B/stage.log; exit 1; }
    umount -R $S/proc $S/dev $S/sys 2>/dev/null || true
    echo "$date" > $S/var/lib/pacman/wincraft-snapshot
}

# --------------------------------------------------------------- native
# The host-side programs WinCraft adds: init, its control tool, and the
# window manager.
step_native() {
    mkdir -p $B/native
    gcc -O2 -static -Wall -Wextra -o $B/native/wcinit $W/src/init/wcinit.c
    gcc -O2 -Wall -Wextra -o $B/native/wcctl $W/src/tools/wcctl.c
    gcc -O2 -Wall -Wextra -Wno-unused-parameter -o $B/native/wcwm $W/src/wm/wcwm.c \
        -lX11 -lXi -lXext -lXcomposite -lXdamage -lXfixes -lXrender
    gcc -O2 -Wall -Wextra -Wno-unused-parameter -o $B/native/wcclip $W/src/tools/wcclip.c -lX11 -lXfixes
    strip $B/native/*
}

# ----------------------------------------------------------------- wine
# Wine itself comes from the Arch package. The patches in wine/patches are
# applied to the upstream source of the same release, and only what they touch
# is rebuilt (with the package's own build options) and laid over the packaged
# files in $B/wine-overlay: Windows-side modules, which are nearly all of Wine,
# the Unix-side libraries of modules (such as winex11.so), and the Wine server.
#
# A patch to the server's protocol or to shared headers needs a full Wine build,
# because those have to match the rest of the package; this step refuses it
# rather than build a mismatched half. A rebuilt Unix-side library must need
# the same libraries as the packaged one, and no newer glibc than the stage's.
step_wine() {
    local out=$B/wine-overlay src=$B/wine-src bld=$B/wine-build
    local patches=($W/wine/patches/*.patch)
    [ -e "${patches[0]}" ] || { rm -rf $out $out.stamp; return 0; }
    stage_ready || step_stage

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

    # What to rebuild, for each file the patches touch: in dlls/<name> and
    # programs/<name> (tests are not built), the module's Windows side, or its
    # Unix-side library for a file built into it ("#pragma makedep unix"), and
    # both for a header of a module that has one; the server for its own files.
    # The server's protocol is generated into files shared with the rest of Wine.
    local f d module unixlib arch dirs="" bad="" targets=()
    for f in $(sed -n 's|^+++ b/||p' "${patches[@]}" | cut -f1 | sort -u); do
        case $f in
            dlls/*/tests/*|programs/*/tests/*) ;;
            dlls/*/*|programs/*/*)
                d=$(echo $f | cut -d/ -f1-2)
                if grep -q '^#pragma makedep unix' $src/$f 2>/dev/null; then
                    dirs+=" $d:unix"
                elif [ "${f%.h}" != "$f" ] && grep -q '^UNIXLIB' $src/$d/Makefile.in; then
                    dirs+=" $d $d:unix"
                else
                    dirs+=" $d"
                fi
                ;;
            server/protocol.def|server/request_handlers.h|server/request_trace.h|server/*/*) bad+=" $f" ;;
            server/*) dirs+=" server" ;;
            *) bad+=" $f" ;;
        esac
    done
    if [ -n "$bad" ]; then
        echo "wine: these patched files need a full Wine build:" >&2
        printf '        %s\n' $bad >&2
        exit 1
    fi

    for d in $(printf '%s\n' $dirs | sort -u); do
        if [ $d = server ]; then
            targets+=(server/wineserver)
            continue
        fi
        module=$(sed -n 's/^MODULE *= *//p' $src/${d%:unix}/Makefile.in)
        [ -n "$module" ] || { echo "wine: ${d%:unix} does not build a module" >&2; exit 1; }
        if [ "${d%:unix}" != "$d" ]; then
            d=${d%:unix}
            unixlib=$(sed -n 's/^UNIXLIB *= *//p' $src/$d/Makefile.in)
            [ -n "$unixlib" ] && [ -e $S/usr/lib/wine/x86_64-unix/$unixlib ] ||
                { echo "wine: $module has no Unix-side library in the staged package" >&2; exit 1; }
            targets+=($d/$unixlib)
            continue
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
    # flags, and makepkg's flags for the host side (the server, the Unix-side
    # libraries).
    local cflags ldflags
    cflags=$(bash -c '. /etc/makepkg.conf && printf %s "$CFLAGS"')
    ldflags=$(bash -c '. /etc/makepkg.conf && printf %s "$LDFLAGS"')
    (cd $bld && CFLAGS="$cflags" LDFLAGS="$ldflags" CROSSCFLAGS="-O2 -pipe -g" CROSSLDFLAGS="-Wl,-O1" \
        $src/configure --prefix=/usr \
        --libdir=/usr/lib --disable-tests --enable-archs=x86_64,i386 --enable-build-id &&
        make -j"$(nproc)" "${targets[@]}") > $B/wine-build.log 2>&1 ||
        { tail -30 $B/wine-build.log; exit 1; }

    local t strip lib pkg newer
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
        if [ "${t%.so}" != "$t" ]; then
            lib=$out/usr/lib/wine/x86_64-unix/${t##*/} pkg=$S/usr/lib/wine/x86_64-unix/${t##*/}
            mkdir -p ${lib%/*}
            strip --strip-unneeded -o $lib $bld/$t
            chmod --reference=$pkg $lib
            # It must load where the packaged one does: the toolbox's packages
            # can be newer than the stage's.
            if [ "$(objdump -p $lib | awk '$1 == "NEEDED"' | sort)" != "$(objdump -p $pkg | awk '$1 == "NEEDED"' | sort)" ]; then
                echo "wine: the rebuilt ${t##*/} needs other libraries than the packaged one" >&2
                exit 1
            fi
            newer=$(comm -23 <(objdump -T $lib | grep -o 'GLIBC_[0-9.]*' | sort -u) \
                             <(objdump -p $S/usr/lib/libc.so.6 $S/usr/lib/libm.so.6 | grep -o 'GLIBC_[0-9.]*' | sort -u))
            if [ -n "$newer" ]; then
                echo "wine: the rebuilt ${t##*/} needs a newer glibc than the stage's ($(echo $newer))" >&2
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

# The root filesystem's /dev is the host's own (mount_root): an image made
# with it still mounted would carry the host's devices and the shared memory
# of its programs, and "rm -rf $R" would reach into them. In the toolbox's
# user namespace the host's mounts under /dev are locked to it, so it can be
# detached only as a whole (umount -l), not taken apart (umount -R failed on
# /dev/mqueue, and images were built with the host's /dev in them). Nothing
# may still run in $R: the processes chrooted into it are
# waited for (and ended after ten seconds), and the build stops if anything
# is left mounted in $R.
chrooted() {    # the processes whose root is $R
    local p
    for p in /proc/[0-9]*; do
        [ "$(readlink "$p/root" 2>/dev/null)" = "$R" ] && echo "${p#/proc/}"
    done
    return 0
}

umount_root() {
    local n=0 left
    while [ -n "$(chrooted)" ]; do
        n=$((n + 1))
        if [ $n -gt 50 ]; then
            chrooted | xargs -r kill -KILL 2>/dev/null || true
            sleep 1
            break
        fi
        sleep 0.2
    done
    umount -l $R/proc $R/dev $R/tmp $R/run 2>/dev/null || true
    left=$(awk -v r="$R/" 'index($2, r) == 1 { print $2 }' /proc/self/mounts)
    if [ -n "$left" ]; then
        echo "rootfs: still mounted in $R, which must not reach the image:" >&2
        echo "$left" | sed 's/^/        /' >&2
        exit 1
    fi
}

# The locales of rootfs/locales.txt, for the language a feature can give the
# session (README, "Features"). pacman leaves the locale sources out of the
# stage, so they come from the stage's own glibc package, and the stage's
# localedef builds them: the data always matches the image's glibc.
build_locales() {
    local v pkg src l
    v=$(sed -n '/^%VERSION%/{n;p}' $S/var/lib/pacman/local/glibc-[0-9]*/desc)
    pkg=/var/cache/pacman/pkg/glibc-$v-x86_64.pkg.tar.zst
    [ -f "$pkg" ] || { echo "rootfs: $pkg is missing (stage again)" >&2; exit 1; }
    say "Building locales"
    src=$B/i18n
    rm -rf $src
    mkdir -p $src
    bsdtar -xf "$pkg" -C $src usr/share/i18n
    for l in $(grep -v '^#' $W/rootfs/locales.txt); do
        I18NPATH=$src/usr/share/i18n $S/usr/lib/ld-linux-x86-64.so.2 --library-path $S/usr/lib \
            $S/usr/bin/localedef --no-archive -c -i $l -f UTF-8 $R/usr/lib/locale/$l.utf8 ||
            { echo "rootfs: could not build the locale $l" >&2; exit 1; }
    done
    rm -rf $src
}

step_rootfs() {
    check_version
    stage_ready || step_stage
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
    build_locales

    say "Building native components"
    step_native
    install -m755 $B/native/* $R/usr/bin/
    # They are installed after the root filesystem's libraries were chosen
    # (assemble-rootfs.py, NATIVE): every library they need must be there.
    local f lib
    for f in $B/native/*; do
        for lib in $(objdump -p $f | awk '$1 == "NEEDED" { print $2 }'); do
            [ -e $R/usr/lib/$lib ] ||
                { echo "native: ${f##*/} needs $lib, which the root filesystem lacks (tools/assemble-rootfs.py, NATIVE)" >&2; exit 1; }
        done
    done
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
        for k in assistant:knowledge agent:agent reference:reference; do
            python3 $W/tools/assistant-knowledge.py ${k%%:*} > $R/usr/share/wincraft/assistant/${k#*:}.md
        done
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
    check_version
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
    # and even so, nothing of a filesystem mounted in it (umount_root)
    mksquashfs $R $I/wincraft/system.sfs -noappend -quiet -no-progress -one-file-system \
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
