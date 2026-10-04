#!/usr/bin/env python3
"""Exercise the real desktop functions used by Disks and Nemo."""
import argparse
import ctypes
import ctypes.util
import os
from pathlib import Path
import subprocess
import tempfile
import sys


def check_preload_restored():
    expected = os.environ.get("FSUPPORT_TEST_PRELOAD")
    if expected is None:
        return
    assert os.environ.get("LD_PRELOAD") == (None if expected == "unset" else expected)
    assert "INFILTRATOR_DISKS_PRELOAD_SET" not in os.environ
    assert "INFILTRATOR_DISKS_PRELOAD_VALUE" not in os.environ
    assert "INFILTRATOR_NEMO_PRELOAD_SET" not in os.environ
    assert "INFILTRATOR_NEMO_PRELOAD_VALUE" not in os.environ


def check_gio(kind):
    """Verify the filesystem::type value queried by Nemo through GIO."""
    os.environ["FSUPPORT_TEST_MOUNT_TYPE"] = kind
    gio_path = ctypes.util.find_library("gio-2.0")
    gobject_path = ctypes.util.find_library("gobject-2.0")
    assert gio_path and gobject_path
    gio = ctypes.CDLL(gio_path, mode=ctypes.RTLD_GLOBAL)
    gobject = ctypes.CDLL(gobject_path, mode=ctypes.RTLD_GLOBAL)
    process = ctypes.CDLL(None)

    gio.g_file_new_for_path.argtypes = [ctypes.c_char_p]
    gio.g_file_new_for_path.restype = ctypes.c_void_p
    query = process.g_file_query_filesystem_info
    query.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
                      ctypes.POINTER(ctypes.c_void_p)]
    query.restype = ctypes.c_void_p
    gio.g_file_info_get_attribute_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    gio.g_file_info_get_attribute_string.restype = ctypes.c_char_p
    gobject.g_object_unref.argtypes = [ctypes.c_void_p]
    gobject.g_object_unref.restype = None

    file_object = gio.g_file_new_for_path(b"/")
    assert file_object
    error = ctypes.c_void_p()
    info = query(file_object, b"filesystem::type", None, ctypes.byref(error))
    try:
        assert not error.value, "GIO filesystem query returned an error"
        assert info
        value = gio.g_file_info_get_attribute_string(info, b"filesystem::type")
        assert value, "filesystem::type was not returned"
        actual = value.decode()
        assert actual == kind, (kind, actual)
    finally:
        if info:
            gobject.g_object_unref(info)
        gobject.g_object_unref(file_object)
    print(f"GIO filesystem::type={kind}: PASS")


def check(stock_only=False, modules_root=Path("/lib/modules"), enabled=None):
    modules = modules_root / os.uname().release / "updates/infiltrator"
    stock_path = os.environ.get("UDISKS_STOCK_LIBRARY") or ctypes.util.find_library("udisks2")
    assert stock_path, "Install the stock libudisks2-0 package"
    ctypes.CDLL(stock_path, mode=ctypes.RTLD_GLOBAL)
    client = ctypes.CDLL(None)
    glib = ctypes.CDLL(ctypes.util.find_library("glib-2.0"))
    display = client.udisks_client_get_id_for_display
    display.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                        ctypes.c_char_p, ctypes.c_int]
    display.restype = ctypes.c_void_p
    glib.g_free.argtypes = [ctypes.c_void_p]
    glib.g_free.restype = None

    def name(usage, kind, version, long_name):
        value = display(None, usage.encode(), kind.encode(), version.encode(), long_name)
        assert value, (usage, kind, version)
        try:
            return ctypes.string_at(value).decode()
        finally:
            glib.g_free(value)

    cases = (
        ("ofs", "DOS/0", "Amiga Original File System", "Amiga OFS"),
        ("ffs", "DOS/1", "Amiga Fast File System", "Amiga FFS"),
        ("sfs", "3", "Amiga Smart File System", "Amiga SFS"),
        ("sfs2", "4", "Amiga Smart File System 2", "Amiga SFS2"),
        ("pfs3", "PFS/1", "Amiga Professional File System 3", "Amiga PFS3"),
    )
    for kind, version, long_name, short_name in cases:
        if stock_only or (enabled is not None and kind not in enabled) or not (modules / f"{kind}.ko").is_file():
            assert name("filesystem", kind, version, True) == f"Unknown ({kind} {version})"
            assert name("filesystem", kind, "", True) == f"Unknown ({kind})"
            assert name("filesystem", kind, version, False) == kind
            continue
        qualifier = f"version {version}" if kind in ("sfs", "sfs2") else version
        assert name("filesystem", kind, version, True) == f"{long_name} ({qualifier})"
        assert name("filesystem", kind, version, False) == short_name
        assert name("filesystem", kind, "", True) == long_name
        assert name("filesystem", kind, "", False) == short_name

    assert name("filesystem", "ext4", "1.0", True) == "Ext4 (version 1.0)"
    assert name("filesystem", "ext4", "", False) == "Ext4"
    assert name("filesystem", "fsupport-test-unknown", "", True) == "Unknown (fsupport-test-unknown)"
    assert name("crypto", "ofs", "DOS/0", True) == "Unknown (ofs DOS/0)"
    print("UDisks filesystem display names: PASS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path)
    parser.add_argument("--preloaded", action="store_true")
    parser.add_argument("--modules-root", type=Path, default=Path("/lib/modules"))
    parser.add_argument("--stock", action="store_true")
    parser.add_argument("--enabled")
    parser.add_argument("--gio-type")
    args = parser.parse_args()
    os.environ["LANGUAGE"] = "C"
    os.environ["LC_ALL"] = "C"
    check_preload_restored()

    if args.gio_type:
        check_gio(args.gio_type)
        return

    if args.package:
        def field(name):
            return subprocess.check_output(["dpkg-deb", "-f", str(args.package), name], text=True).strip()
        assert field("Package") == "infiltrator-filesystem-support"
        assert field("Conflicts") == "infiltrator-filesystem-support-udisks"
        assert "infiltrator-filesystem-support-udisks" not in field("Depends")
        for name in ("Provides", "Replaces", "Breaks"):
            assert not field(name), (name, field(name))
        assert "libudisks2-0" in field("Depends")
        assert "gnome-disk-utility" in field("Depends")
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["dpkg-deb", "-R", str(args.package), directory], check=True)
            root = Path(directory)
            templates = root / "usr/lib/infiltrator-filesystem-support/desktop/templates"
            for kind in ("ofs", "ffs", "sfs", "sfs2", "pfs3"):
                assert (templates / f"{kind}.so").is_file()
                assert (templates / f"{kind}.rules").is_file()
                rule = (templates / f"{kind}.rules").read_text()
                assert f"--filesystem {kind} /dev/%k" in rule
            assert (templates / "gnome-disks").is_file()
            assert (templates / "nemo").is_file()
            assert not (root / "usr/lib/infiltrator-filesystem-support/desktop/active").exists()
            assert not list(root.glob("usr/lib/*/libudisks*"))
            assert not list(root.glob("usr/share/doc/libudisks*"))
            assert not (root / "usr/bin/gnome-disks").exists()
            assert not (root / "usr/bin/nemo").exists()
            assert not list(root.glob("usr/lib/udev/rules.d/*infiltrator*"))
            assert "desktop-integration sync" in (root / "DEBIAN/postinst").read_text()
        print("Main package contains inactive, separate desktop templates: PASS")
    else:
        check(stock_only=args.stock, modules_root=args.modules_root,
              enabled=None if args.enabled is None else set(filter(None, args.enabled.split(','))))


if __name__ == "__main__":
    main()
