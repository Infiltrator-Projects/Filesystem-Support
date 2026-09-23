// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"

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

} // namespace

int main(int argc, char** argv)
{
    using filesystem_support::catalog;
    using filesystem_support::catalog_is_valid;

    if (!catalog_is_valid()) {
        return fail("catalogue validation failed");
    }

    if (catalog().size() != 108U) {
        return fail("catalogue size is not the documented 108 entries");
    }

    bool found_affs = false;
    bool found_ofs = false;
    bool found_ffs = false;
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
    std::size_t kernel_only = 0U;
    std::size_t kernel_with_userspace = 0U;
    std::size_t dkms = 0U;
    std::size_t userspace = 0U;
    std::size_t tools_only = 0U;

    for (const auto& entry : catalog()) {
        if (entry.id == std::string_view("affs")) {
            found_affs = true;
        } else if (entry.id == std::string_view("ofs")) {
            found_ofs = true;
        } else if (entry.id == std::string_view("ffs")) {
            found_ffs = true;
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

    if (!found_affs || !found_ofs || !found_ffs || !found_adfs) {
        return fail("native/legacy Amiga or Acorn filesystem coverage is missing");
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
    if (kernel_only != 19U || kernel_with_userspace != 29U ||
        dkms != 3U || userspace != 53U || tools_only != 4U) {
        return fail("support-provider classification counts changed unexpectedly");
    }
    if (!filesystem_support::package_is_catalogued("zfs-dkms") ||
        filesystem_support::package_is_catalogued("definitely-not-a-package")) {
        return fail("catalogue package allowlist is inconsistent");
    }
    if (!filesystem_support::module_is_catalogued("affs") ||
        !filesystem_support::module_is_catalogued("ofs") ||
        !filesystem_support::module_is_catalogued("ffs") ||
        filesystem_support::module_is_catalogued("definitely-not-a-module")) {
        return fail("catalogue module allowlist is inconsistent");
    }

    if (!filesystem_support::linux_native_module_is_managed("ext3", "ext3") ||
        !filesystem_support::linux_native_module_is_managed("ofs", "ofs") ||
        !filesystem_support::linux_native_module_is_managed("ffs", "ffs") ||
        filesystem_support::linux_native_module_is_managed("affs", "affs") ||
        filesystem_support::linux_native_module_is_managed(
            "definitely-not-a-filesystem", "ext3")) {
        return fail("project-native Linux module policy is inconsistent");
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
