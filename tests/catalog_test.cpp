// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"

#include <iostream>
#include <string_view>

namespace {

int fail(const char* message)
{
    std::cerr << "catalog_test: " << message << '\n';
    return 1;
}

} // namespace

int main()
{
    using filesystem_support::catalog;
    using filesystem_support::catalog_is_valid;

    if (!catalog_is_valid()) {
        return fail("catalogue validation failed");
    }

    if (catalog().size() < 100U) {
        return fail("expanded Debian catalogue contains fewer than 100 entries");
    }

    bool found_affs = false;
    bool found_adfs = false;
    bool found_apfs_fuse = false;
    bool found_apfs_dkms = false;
    bool found_gfs2 = false;
    bool found_openafs = false;
    bool found_sshfs = false;
    bool found_vmfs = false;
    bool found_zfs = false;
    bool found_zonefs = false;
    bool found_tmfs = false;
    bool found_fosfat = false;
    bool found_moosefs = false;
    bool hfs_uses_removed_package = false;

    for (const auto& entry : catalog()) {
        if (entry.id == std::string_view("affs")) {
            found_affs = true;
        } else if (entry.id == std::string_view("adfs")) {
            found_adfs = true;
        } else if (entry.id == std::string_view("apfs-fuse")) {
            found_apfs_fuse = true;
        } else if (entry.id == std::string_view("apfs-dkms")) {
            found_apfs_dkms = true;
        } else if (entry.id == std::string_view("gfs2")) {
            found_gfs2 = true;
        } else if (entry.id == std::string_view("openafs")) {
            found_openafs = true;
        } else if (entry.id == std::string_view("sshfs")) {
            found_sshfs = true;
        } else if (entry.id == std::string_view("vmfs")) {
            found_vmfs = true;
        } else if (entry.id == std::string_view("zfs")) {
            found_zfs = true;
        } else if (entry.id == std::string_view("zonefs")) {
            found_zonefs = true;
        } else if (entry.id == std::string_view("tmfs")) {
            found_tmfs = true;
        } else if (entry.id == std::string_view("fosfat")) {
            found_fosfat = true;
        } else if (entry.id == std::string_view("moosefs")) {
            found_moosefs = true;
        }

        if (entry.id == std::string_view("hfs")) {
            for (const auto package : entry.packages) {
                if (package == std::string_view("hfsutils")) {
                    hfs_uses_removed_package = true;
                }
            }
        }
    }

    if (!found_affs || !found_adfs) {
        return fail("Amiga or Acorn legacy filesystem coverage is missing");
    }
    if (!found_apfs_fuse || !found_apfs_dkms) {
        return fail("both conservative and experimental APFS paths are required");
    }
    if (!found_gfs2 || !found_openafs) {
        return fail("cluster/distributed filesystem coverage is incomplete");
    }
    if (!found_sshfs || !found_vmfs || !found_zfs) {
        return fail("network, virtualisation or pooled filesystem coverage is incomplete");
    }
    if (!found_zonefs || !found_tmfs || !found_fosfat || !found_moosefs) {
        return fail("Debian stable specialist filesystem coverage is incomplete");
    }
    if (hfs_uses_removed_package) {
        return fail("HFS still references hfsutils, which is not in Debian trixie stable");
    }

    return 0;
}
