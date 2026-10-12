#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# QEMU init used by Amiga runtime qualification. The first boot exercises
# ordinary read/write semantics, then deliberately reboots with writable
# filesystems still mounted after sync. The second boot proves that committed
# state survives that unclean shutdown and remains mountable/readable.
set -eu

fail()
{
    echo "AMIGA-RUNTIME-FAIL: $*" > /dev/ttyS0
    poweroff -f
    exit 1
}

mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail devtmpfs

for fs in ofs ffs sfs sfs2 pfs3; do
    insmod "/$fs.ko" || fail "insmod $fs"
done

crash_phase_armed()
{
    mount -t ofs /dev/vda /mnt/ofs || fail "crash probe mount ofs"
    if test -f /mnt/ofs/.ifs-crash-phase; then
        umount /mnt/ofs || fail "crash probe unmount ofs"
        return 0
    fi
    umount /mnt/ofs || fail "initial probe unmount ofs"
    return 1
}

recover_after_unclean_shutdown()
{
    for pair in 'ofs /dev/vda' 'ffs /dev/vdb' 'sfs /dev/vdc' 'sfs2 /dev/vdd'; do
        set -- $pair
        fs="$1"
        dev="$2"
        mnt="/mnt/$fs"
        mount -t "$fs" "$dev" "$mnt" || fail "crash recovery mount $fs"
        test "$(cat "$mnt/.ifs-crash-stable")" = "committed-$fs" ||
            fail "crash recovery committed data $fs"
        rm -f "$mnt/.ifs-crash-stable" "$mnt/.ifs-crash-phase" ||
            fail "crash recovery cleanup $fs"
        sync
        umount "$mnt" || fail "crash recovery unmount $fs"
        echo "AMIGA-CRASH-RECOVERY-PASS: $fs" > /dev/ttyS0
    done

    echo "AMIGA-RUNTIME-PASS" > /dev/ttyS0
    poweroff -f
}

if crash_phase_armed; then
    recover_after_unclean_shutdown
fi

qualify_rw()
{
    fs="$1"; dev="$2"; mnt="/mnt/$fs"
    mount -t "$fs" "$dev" "$mnt" || fail "mount $fs"
    echo "runtime-$fs" > "$mnt/new.txt" || fail "create $fs"
    mkdir "$mnt/dir" || fail "mkdir $fs"
    mv "$mnt/new.txt" "$mnt/dir/renamed.txt" || fail "rename $fs"
    ln "$mnt/dir/renamed.txt" "$mnt/hard.txt" || fail "hardlink $fs"
    ln -s dir/renamed.txt "$mnt/soft.txt" || fail "symlink $fs"
    test "$(cat "$mnt/hard.txt")" = "runtime-$fs" || fail "readback $fs"
    sync
    umount "$mnt" || fail "unmount $fs"
    mount -t "$fs" "$dev" "$mnt" || fail "remount $fs"
    test "$(cat "$mnt/dir/renamed.txt")" = "runtime-$fs" || fail "remount-read $fs"
    rm "$mnt/hard.txt" "$mnt/soft.txt" "$mnt/dir/renamed.txt" || fail "unlink $fs"
    rmdir "$mnt/dir" || fail "rmdir $fs"
    sync
    umount "$mnt" || fail "final-unmount $fs"
}

qualify_sfs_rw()
{
    fs="$1"; dev="$2"; mnt="/mnt/$fs"
    mount -t "$fs" "$dev" "$mnt" || fail "mount $fs"
    echo "runtime-$fs" > "$mnt/new.txt" || fail "create $fs"
    test "$(cat "$mnt/new.txt")" = "runtime-$fs" || fail "write-readback $fs"
    mkdir "$mnt/dir" || fail "mkdir $fs"
    mv "$mnt/new.txt" "$mnt/dir/renamed.txt" || fail "rename $fs"
    ln -s dir/renamed.txt "$mnt/soft.txt" || fail "symlink $fs"
    test "$(cat "$mnt/soft.txt")" = "runtime-$fs" || fail "symlink-read $fs"
    sync
    umount "$mnt" || fail "unmount $fs"
    mount -t "$fs" "$dev" "$mnt" || fail "remount $fs"
    test "$(cat "$mnt/dir/renamed.txt")" = "runtime-$fs" || fail "remount-read $fs"
    test "$(cat "$mnt/soft.txt")" = "runtime-$fs" || fail "remount-symlink $fs"
    : > "$mnt/dir/renamed.txt" || fail "truncate $fs"
    test ! -s "$mnt/dir/renamed.txt" || fail "truncate-readback $fs"
    rm "$mnt/soft.txt" "$mnt/dir/renamed.txt" || fail "unlink $fs"
    rmdir "$mnt/dir" || fail "rmdir $fs"
    sync
    umount "$mnt" || fail "final-unmount $fs"
}

qualify_rw ofs /dev/vda
qualify_rw ffs /dev/vdb
qualify_sfs_rw sfs /dev/vdc
qualify_sfs_rw sfs2 /dev/vdd

mount -t pfs3 -o ro /dev/vde /mnt/pfs3 || fail "mount pfs3"
test -s /mnt/pfs3/frag || fail "fixture-read pfs3"
cat /mnt/pfs3/frag >/dev/null || fail "payload-read pfs3"
umount /mnt/pfs3 || fail "unmount pfs3"

qualify_desktop_user()
{
    fs="$1"; dev="$2"; mnt="/mnt/$fs"
    options=$(cat "/$fs.mount-options")
    mount -t "$fs" -o "$options" "$dev" "$mnt" || fail "desktop mount $fs"
    test "$(stat -c '%u:%g' "$mnt")" = '1000:1000' || fail "desktop ownership $fs"
    su -s /bin/sh -c 'test "$(id -u)" = 1000 && mkdir "$1/desktop-dir" && echo desktop-write > "$1/desktop-dir/file" && mv "$1/desktop-dir/file" "$1/desktop-dir/renamed"' desktop sh "$mnt" || fail "desktop create/write/rename $fs"
    if su -s /bin/sh -c 'echo forbidden > "$1/other-user"' other sh "$mnt"; then
        fail "unrelated user was allowed to write $fs"
    fi
    sync
    umount "$mnt" || fail "desktop unmount $fs"
    mount -t "$fs" -o "$options" "$dev" "$mnt" || fail "desktop remount $fs"
    su -s /bin/sh -c 'test "$(cat "$1/desktop-dir/renamed")" = desktop-write && rm "$1/desktop-dir/renamed" && rmdir "$1/desktop-dir"' desktop sh "$mnt" || fail "desktop persisted read/delete $fs"
    umount "$mnt" || fail "desktop final unmount $fs"
    echo "AMIGA-DESKTOP-USER-PASS: $fs" > /dev/ttyS0
}

qualify_desktop_user ofs /dev/vda
qualify_desktop_user ffs /dev/vdb
qualify_desktop_user sfs /dev/vdc
qualify_desktop_user sfs2 /dev/vdd

# Establish known committed state, then reboot without unmounting any writable
# filesystem. This verifies crash recovery of already-committed metadata/data;
# it deliberately makes no promise about unsynchronised writes.
for pair in 'ofs /dev/vda' 'ffs /dev/vdb' 'sfs /dev/vdc' 'sfs2 /dev/vdd'; do
    set -- $pair
    fs="$1"
    dev="$2"
    mnt="/mnt/$fs"
    mount -t "$fs" "$dev" "$mnt" || fail "crash arm mount $fs"
    echo "committed-$fs" > "$mnt/.ifs-crash-stable" || fail "crash arm write $fs"
    if test "$fs" = ofs; then
        echo armed > "$mnt/.ifs-crash-phase" || fail "crash arm marker ofs"
    fi
done
sync || fail "crash arm sync"
echo "AMIGA-CRASH-ARMED" > /dev/ttyS0
reboot -f
fail "forced reboot returned"
