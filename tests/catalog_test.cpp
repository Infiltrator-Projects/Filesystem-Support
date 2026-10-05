// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

int fail(const char* message)
{
    std::cerr << "catalog_test: " << message << '\n';
    return 1;
}

bool has_id(const std::string_view id)
{
    for (const auto& entry : filesystem_support::catalog()) {
        if (entry.id == id) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    using filesystem_support::catalog;
    using filesystem_support::catalog_is_valid;

    if (!catalog_is_valid()) {
        return fail("catalogue validation failed");
    }

    if (catalog().size() != 41U) {
        return fail("catalogue size is not the documented 41 disk formats");
    }

    // The product catalogue is format-centric. These are providers, transports,
    // overlays, containers, archives, device namespaces or immutable image
    // formats and must not reappear as selectable filesystem identities.
    constexpr std::array<std::string_view, 30> excluded_ids = {
        "zfs-fuse", "ntfs3", "apfs-fuse", "apfs-dkms", "overlayfs",
        "ecryptfs", "bitlocker", "luksde", "filevault", "tmfs",
        "iso9660", "squashfs", "erofs", "romfs", "cramfs",
        "fusezip", "archivemount", "guestmount", "virtiofs", "vmhgfs",
        "cifs", "nfs", "cephfs", "sshfs", "s3fs",
        "rclone", "fuse-overlayfs", "gocryptfs", "ifuse", "lxcfs"
    };
    for (const auto id : excluded_ids) {
        if (has_id(id)) {
            return fail("non-disk/provider identity leaked back into the catalogue");
        }
    }

    constexpr std::array<std::string_view, 13> required_ids = {
        "infiltratorfs", "ext2", "ext3", "ext4", "ofs", "ffs", "sfs",
        "sfs2", "pfs3", "ntfs", "apfs", "zfs", "udf"
    };
    for (const auto id : required_ids) {
        if (!has_id(id)) {
            return fail("required local disk filesystem format is missing");
        }
    }

    if (has_id("apfs-fuse") || has_id("apfs-dkms") || !has_id("apfs")) {
        return fail("APFS must have one format identity rather than provider identities");
    }
    if (has_id("ntfs3") || !has_id("ntfs")) {
        return fail("NTFS must have one format identity rather than provider identities");
    }
    if (has_id("zfs-fuse") || !has_id("zfs")) {
        return fail("ZFS must have one format identity rather than provider identities");
    }

    bool hfs_uses_removed_package = false;
    std::size_t kernel_only = 0U;
    std::size_t kernel_with_userspace = 0U;
    std::size_t dkms = 0U;
    std::size_t userspace = 0U;
    std::size_t tools_only = 0U;

    for (const auto& entry : catalog()) {
        if (entry.id == std::string_view("fuse2fs")) {
            return fail("fuse2fs must not be exposed alongside canonical EXT2/EXT3/EXT4");
        }

        if (entry.id == std::string_view("ext2") ||
            entry.id == std::string_view("ext3") ||
            entry.id == std::string_view("ext4")) {
            if (entry.provider != filesystem_support::SupportProvider::Kernel ||
                !entry.packages.empty() ||
                !entry.project_native_linux) {
                return fail("EXT2/EXT3/EXT4 must remain project-native kernel-only providers");
            }
        }

        switch (entry.provider) {
        case filesystem_support::SupportProvider::Kernel:
            ++kernel_only;
            if (entry.modules.empty() || !entry.packages.empty()) {
                return fail("kernel-only provider classification is inconsistent");
            }
            break;
        case filesystem_support::SupportProvider::KernelWithUserspace:
            ++kernel_with_userspace;
            if (entry.modules.empty() || entry.packages.empty()) {
                return fail("kernel/userspace provider classification is inconsistent");
            }
            break;
        case filesystem_support::SupportProvider::Dkms:
            ++dkms;
            if (entry.modules.empty() || entry.packages.empty()) {
                return fail("DKMS provider classification is inconsistent");
            }
            break;
        case filesystem_support::SupportProvider::Userspace:
            ++userspace;
            if (!entry.modules.empty() || entry.packages.empty()) {
                return fail("userspace provider classification is inconsistent");
            }
            break;
        case filesystem_support::SupportProvider::ToolsOnly:
            ++tools_only;
            if (!entry.modules.empty() || entry.packages.empty()) {
                return fail("tools-only provider classification is inconsistent");
            }
            break;
        }

        if (entry.id == std::string_view("hfs")) {
            for (const auto package : entry.packages) {
                if (package == std::string_view("hfsutils")) {
                    hfs_uses_removed_package = true;
                }
            }
        }
    }

    if (hfs_uses_removed_package) {
        return fail("HFS still references hfsutils, which is not in Debian trixie stable");
    }
    if (kernel_only != 19U || kernel_with_userspace != 14U ||
        dkms != 3U || userspace != 4U || tools_only != 1U) {
        return fail("support-provider classification counts changed unexpectedly");
    }

    if (filesystem_support::package_is_catalogued("e2fsprogs") ||
        !filesystem_support::package_is_catalogued("zfs-dkms") ||
        !filesystem_support::package_is_catalogued("infiltratorfs") ||
        filesystem_support::package_is_catalogued("dislocker") ||
        filesystem_support::package_is_catalogued("sshfs") ||
        filesystem_support::package_is_catalogued("definitely-not-a-package")) {
        return fail("catalogue package allowlist is inconsistent");
    }

    if (!filesystem_support::module_is_catalogued("infiltratorfs") ||
        !filesystem_support::module_is_catalogued("ofs") ||
        !filesystem_support::module_is_catalogued("ffs") ||
        !filesystem_support::module_is_catalogued("sfs") ||
        filesystem_support::module_is_catalogued("affs") ||
        filesystem_support::module_is_catalogued("overlay") ||
        filesystem_support::module_is_catalogued("cifs") ||
        filesystem_support::module_is_catalogued("definitely-not-a-module")) {
        return fail("catalogue module allowlist is inconsistent");
    }

    if (!filesystem_support::linux_native_module_is_managed("ext3", "ext3") ||
        !filesystem_support::linux_native_module_is_managed("ofs", "ofs") ||
        !filesystem_support::linux_native_module_is_managed("ffs", "ffs") ||
        !filesystem_support::linux_native_module_is_managed("sfs", "sfs") ||
        !filesystem_support::linux_native_module_is_managed("sfs2", "sfs2") ||
        !filesystem_support::linux_native_module_is_managed("pfs3", "pfs3") ||
        filesystem_support::linux_native_module_is_managed("infiltratorfs", "infiltratorfs") ||
        filesystem_support::linux_native_module_is_managed("affs", "affs") ||
        filesystem_support::linux_native_module_is_managed(
            "definitely-not-a-filesystem", "ext3")) {
        return fail("project-native Linux module policy is inconsistent");
    }

    const auto hfs_users = filesystem_support::catalogue_entries_using_package("hfsprogs");
    if (hfs_users.size() != 2U) {
        return fail("shared package impact mapping is incorrect");
    }

    if (argc != 2) {
        return fail("support-matrix path was not supplied");
    }

    std::ifstream matrix(argv[1]);
    if (!matrix) {
        return fail("support matrix could not be opened");
    }

    const std::string matrix_text(
        (std::istreambuf_iterator<char>(matrix)),
        std::istreambuf_iterator<char>());

    for (const auto& entry : catalog()) {
        const std::string marker =
            "id=\"support-" + std::string(entry.id) + "\"";
        const std::size_t first = matrix_text.find(marker);
        if (first == std::string::npos) {
            std::cerr << "catalog_test: support matrix is missing catalogue ID "
                      << entry.id << '\n';
            return 1;
        }
        if (matrix_text.find(marker, first + marker.size()) != std::string::npos) {
            std::cerr << "catalog_test: support matrix duplicates catalogue ID "
                      << entry.id << '\n';
            return 1;
        }
    }

    return 0;
}
