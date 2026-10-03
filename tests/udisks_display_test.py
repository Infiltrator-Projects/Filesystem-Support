#!/usr/bin/env python3
"""Exercise the real client function used by both GNOME Disks display paths."""
import argparse
import ctypes
import ctypes.util
import os
from pathlib import Path
import subprocess
import tempfile
import sys


def check(stock_only=False):
    # Resolve the same dynamic symbol as Disks: preloaded extension first,
    # then the distribution client for non-Amiga identifiers.
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
        if stock_only:
            assert name("filesystem", kind, version, True) == f"Unknown ({kind} {version})"
            continue
        qualifier = f"version {version}" if kind in ("sfs", "sfs2") else version
        assert name("filesystem", kind, version, True) == f"{long_name} ({qualifier})"
        assert name("filesystem", kind, version, False) == short_name
        assert name("filesystem", kind, "", True) == long_name
        assert name("filesystem", kind, "", False) == short_name
        print(f"{kind}: {name('filesystem', kind, version, True)} / {short_name}")

    # Existing identifiers retain their stock names, and a truly unrecognised
    # format must still use the normal fallback rather than an Amiga label.
    assert name("filesystem", "ext4", "1.0", True) == "Ext4 (version 1.0)"
    assert name("filesystem", "ext4", "", False) == "Ext4"
    assert name("filesystem", "fsupport-test-unknown", "", True) == "Unknown (fsupport-test-unknown)"
    assert name("crypto", "ofs", "DOS/0", True) == "Unknown (ofs DOS/0)"
    print("UDisks filesystem display names: PASS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path)
    parser.add_argument("--preloaded", action="store_true")
    parser.add_argument("--stock", action="store_true")
    args = parser.parse_args()
    os.environ["LANGUAGE"] = "C"
    os.environ["LC_ALL"] = "C"
    expected = os.environ.get("FSUPPORT_TEST_PRELOAD")
    if expected is not None:
        assert os.environ.get("LD_PRELOAD") == (None if expected == "unset" else expected)
        assert "INFILTRATOR_DISKS_PRELOAD_SET" not in os.environ
        assert "INFILTRATOR_DISKS_PRELOAD_VALUE" not in os.environ
    if args.package:
        def field(name):
            return subprocess.check_output(["dpkg-deb", "-f", str(args.package), name], text=True).strip()
        assert field("Package") == "infiltrator-filesystem-support-udisks"
        for name in ("Provides", "Conflicts", "Replaces", "Breaks"):
            assert not field(name), (name, field(name))
        assert "libudisks2-0" in field("Depends")
        assert "gnome-disk-utility" in field("Depends")
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["dpkg-deb", "-R", str(args.package), directory], check=True)
            root = Path(directory)
            shim = root / "usr/lib/infiltrator-filesystem-support/udisks-amiga-names.so"
            assert shim.is_file()
            assert not list(root.glob("usr/lib/*/libudisks*"))
            assert not list(root.glob("usr/share/doc/libudisks*"))
            assert (root / "usr/bin/gnome-disks").is_file()
            for script in ("preinst", "postrm"):
                assert "dpkg-divert --package infiltrator-filesystem-support-udisks" in (root / "DEBIAN" / script).read_text()
            env = dict(os.environ, LD_PRELOAD=str(shim))
            subprocess.run([sys.executable, __file__, "--preloaded"], env=env, check=True)
            for was_set, previous, expected in (("no", "", "unset"), ("yes", "libm.so.6", "libm.so.6")):
                isolated = dict(env, INFILTRATOR_DISKS_PRELOAD_SET=was_set,
                                INFILTRATOR_DISKS_PRELOAD_VALUE=previous,
                                FSUPPORT_TEST_PRELOAD=expected)
                subprocess.run([sys.executable, __file__, "--preloaded"], env=isolated, check=True)
    else:
        check(stock_only=args.stock)


if __name__ == "__main__":
    main()
