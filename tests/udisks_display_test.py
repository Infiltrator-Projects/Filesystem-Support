#!/usr/bin/env python3
"""Exercise the real client function used by both GNOME Disks display paths."""
import argparse
import ctypes
import ctypes.util
import os
from pathlib import Path
import subprocess
import tempfile


def check(library_path):
    client = ctypes.CDLL(str(library_path))
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
    parser.add_argument("--library", type=Path)
    args = parser.parse_args()
    os.environ["LANGUAGE"] = "C"
    if args.package:
        package = subprocess.check_output(["dpkg-deb", "-f", str(args.package), "Package"], text=True).strip()
        assert package == "infiltrator-filesystem-support-udisks", package
        for field in ("Provides", "Conflicts", "Replaces"):
            value = subprocess.check_output(["dpkg-deb", "-f", str(args.package), field], text=True)
            assert "libudisks2-0" in value, (field, value)
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["dpkg-deb", "-x", str(args.package), directory], check=True)
            libraries = list(Path(directory).glob("usr/lib/*/libudisks2.so.0"))
            assert len(libraries) == 1, libraries
            check(libraries[0])
    else:
        check(args.library or ctypes.util.find_library("udisks2"))


if __name__ == "__main__":
    main()
