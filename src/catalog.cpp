// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include "native_filesystems.generated.hpp"

#include <infiltratr/core.h>

#include <unordered_set>

namespace filesystem_support {
namespace {

bool safe_catalog_token(const std::string_view value)
{
    if (value.empty()) {
        return false;
    }

    for (const unsigned char character : value) {
        if (infiltratr_ascii_is_alnum(character) ||
            character == '-' || character == '_' ||
            character == '.' || character == '+') {
            continue;
        }
        return false;
    }

    return true;
}

const NativeFilesystemPolicy* native_policy_for(const std::string_view id)
{
    for (const auto& policy : kNativeFilesystemPolicies) {
        if (policy.filesystem_id == id) {
            return &policy;
        }
    }
    return nullptr;
}

bool provider_shape_is_valid(const FilesystemDescriptor& entry)
{
    switch (entry.provider) {
    case SupportProvider::Kernel:
        return !entry.modules.empty() && entry.packages.empty();
    case SupportProvider::KernelWithUserspace:
    case SupportProvider::Dkms:
        return !entry.modules.empty() && !entry.packages.empty();
    case SupportProvider::Userspace:
        return entry.modules.empty() && !entry.packages.empty();
    case SupportProvider::ToolsOnly:
        return entry.modules.empty() && !entry.packages.empty() &&
               entry.access == AccessMode::ToolsOnly;
    }
    return false;
}

} // namespace

const std::vector<FilesystemDescriptor>& catalog()
{
    // Catalogue identity is the on-media filesystem format, never a provider,
    // transport, overlay, archive helper, encryption container or virtual
    // namespace. Project-native Linux membership and module names are applied
    // from the generated deployment policy so they have exactly one source.
    static const std::vector<FilesystemDescriptor> entries = [] {
        std::vector<FilesystemDescriptor> values = {
            {"infiltratorfs", "InfiltratorFS", "InfiltratorOS",
             "InfiltratorOS native copy-on-write filesystem.",
             {"infiltratorfs"}, {"infiltratorfs"}, AccessMode::ReadWrite,
             SupportProvider::Dkms,
             "Native Linux VFS/DKMS provider from the InfiltratorFS project; this is a first-class InfiltratorOS root-filesystem target."},

            // Native and mainstream local disk filesystems.
            {"ext2", "EXT2", "Linux", "Second Extended Filesystem.",
             {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,
             "Project-native EXT2 implementation and Linux module; no external EXT userspace provider."},
            {"ext3", "EXT3", "Linux", "Third Extended Filesystem.",
             {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,
             "Project-native EXT3 implementation and Linux module, kept separate from EXT2 and EXT4."},
            {"ext4", "EXT4", "Linux", "Fourth Extended Filesystem.",
             {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,
             "Project-native EXT4 implementation and Linux module; no external EXT userspace provider."},
            {"xfs", "SGI XFS", "Unix / Linux",
             "High-performance journaling filesystem originally developed by Silicon Graphics.",
             {"xfs"}, {"xfsprogs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace, ""},
            {"btrfs", "Btrfs", "Linux",
             "Copy-on-write Linux filesystem with checksums, snapshots and subvolumes.",
             {"btrfs"}, {"btrfs-progs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace, ""},
            {"f2fs", "F2FS", "Linux",
             "Flash-Friendly File System for block-addressed flash storage.",
             {"f2fs"}, {"f2fs-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace, ""},
            {"jfs", "IBM JFS", "Unix / Linux",
             "IBM journaling filesystem supported by the Linux JFS driver.",
             {"jfs"}, {"jfsutils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace, ""},
            {"nilfs2", "NILFS2", "Linux",
             "Log-structured filesystem with continuous snapshotting.",
             {"nilfs2"}, {"nilfs-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace, ""},
            {"reiserfs", "ReiserFS", "Linux / Legacy",
             "Legacy journaling disk filesystem.",
             {"reiserfs"}, {"reiserfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace,
             "The Linux driver is deprecated and may be absent from newer kernels; the format remains a real local disk filesystem target for compatibility work."},
            {"minix", "Minix filesystem", "Unix / Legacy",
             "Filesystem family used by Minix and early Linux systems.",
             {"minix"}, {"util-linux"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,
             "Debian's util-linux supplies mkfs.minix and fsck.minix."},
            {"bcachefs", "Bcachefs", "Linux",
             "Copy-on-write local-storage filesystem integrated into newer Linux kernels.",
             {"bcachefs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,
             "Debian trixie stable does not ship bcachefs-tools; current availability therefore depends on the running kernel and formatter qualification is pending."},

            // Shared-disk and pooled local-storage formats. These remain disk
            // formats even when their normal deployment uses multiple hosts.
            {"gfs2", "GFS2", "Shared-disk / Linux",
             "Red Hat Global File System 2 on-disk format.",
             {"gfs2"}, {"gfs2-utils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace, ""},
            {"ocfs2", "OCFS2", "Shared-disk / Linux",
             "Oracle Cluster File System 2 on-disk format.",
             {"ocfs2"}, {"ocfs2-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace, ""},
            {"zfs", "OpenZFS", "Unix / Linux",
             "ZFS pooled copy-on-write local-storage filesystem.",
             {"zfs"}, {"zfsutils-linux", "zfs-dkms"}, AccessMode::ReadWrite, SupportProvider::Dkms,
             "ZFS is represented once by format identity; zfs-fuse is not a second filesystem entry."},

            // Historical workstation, Unix and other local disk formats.
            {"adfs", "Acorn ADFS", "Acorn / RISC OS",
             "Acorn Disc Filing System used by RISC OS and earlier Acorn systems.",
             {"adfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,
             "Support depends on whether the running Debian kernel was built with the ADFS driver."},
            {"ofs", "Amiga OFS", "Amiga",
             "Classic Commodore Amiga Original File System as an independent native module.",
             {}, {}, AccessMode::Mixed, SupportProvider::Kernel,
             "Project-native Linux OFS module; directory-cache variants remain conservative/read-only where required."},
            {"ffs", "Amiga FFS", "Amiga",
             "Classic Commodore Amiga Fast File System as an independent native module.",
             {}, {}, AccessMode::Mixed, SupportProvider::Kernel,
             "Project-native Linux FFS module; directory-cache variants remain conservative/read-only where required."},
            {"sfs", "Amiga SFS", "Amiga",
             "Amiga Smart File System as an independent native Linux module.",
             {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,
             "Project-native Linux SFS module implementing the SFS on-disk format."},
            {"sfs2", "Amiga SFS2", "Amiga",
             "Amiga Smart File System 2 as an independent native Linux module.",
             {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,
             "Project-native Linux SFS2 module implementing the SFS2 on-disk format independently."},
            {"pfs3", "Amiga PFS3", "Amiga",
             "Amiga Professional File System 3 disk format.",
             {}, {}, AccessMode::ReadOnly, SupportProvider::Kernel,
             "Project-native PFS3 adapter is intentionally read-only until writable metadata updates are fully qualified."},
            {"befs", "BeOS BeFS", "BeOS / Haiku",
             "BeOS filesystem disk format.",
             {"befs"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel,
             "Current Linux BeFS support is primarily read-only; the format itself remains a local disk filesystem."},
            {"bfs", "SCO BFS", "Unix / Legacy",
             "SCO/UnixWare Boot File System disk format.",
             {"bfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel, ""},
            {"efs", "SGI EFS", "SGI / Unix",
             "Silicon Graphics Extent File System used before XFS.",
             {"efs"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel, ""},
            {"hpfs", "OS/2 HPFS", "IBM / OS/2",
             "High Performance File System used by IBM OS/2.",
             {"hpfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,
             "Current Linux write capability depends on kernel configuration."},
            {"qnx4", "QNX4 filesystem", "QNX",
             "QNX4 local disk filesystem.",
             {"qnx4"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel, ""},
            {"qnx6", "QNX6 filesystem", "QNX",
             "QNX6 Power-Safe local disk filesystem.",
             {"qnx6"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel, ""},
            {"sysv", "System V / Xenix / Coherent FS", "Unix / Legacy",
             "System V-derived local disk filesystem family.",
             {"sysv"}, {}, AccessMode::Mixed, SupportProvider::Kernel, ""},
            {"ufs", "UFS / BSD FFS", "Unix / BSD",
             "Unix File System family used by BSD, Sun and other Unix systems.",
             {"ufs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,
             "Current Linux write support is limited and depends on the exact UFS variant."},
            {"omfs", "OMFS", "Embedded / Legacy",
             "Optimized MPEG File System local disk format used by some embedded media devices.",
             {"omfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel, ""},

            // Microsoft and DOS disk formats.
            {"fat", "FAT12 / FAT16 / FAT32", "Microsoft / DOS",
             "Classic DOS and Windows FAT disk filesystems.",
             {"vfat", "msdos"}, {"dosfstools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,
             "FAT is one format-family entry; fusefat is only an alternative provider and is not separately catalogued."},
            {"exfat", "exFAT", "Microsoft",
             "Microsoft removable/local-storage filesystem.",
             {"exfat"}, {"exfatprogs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,
             "exFAT is represented once; exfat-fuse is not a second filesystem entry."},
            {"ntfs", "NTFS", "Microsoft",
             "Windows NT on-disk filesystem.",
             {}, {"ntfs-3g"}, AccessMode::Userspace, SupportProvider::Userspace,
             "NTFS is represented once. NTFS-3G is the current conservative Debian access provider; ntfs3 is a provider, not a second filesystem identity."},

            // Apple disk formats.
            {"hfs", "Apple HFS", "Apple / Classic Mac",
             "Classic Macintosh Hierarchical File System.",
             {"hfs"}, {"hfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace,
             "Debian trixie uses hfsprogs with the kernel driver."},
            {"hfsplus", "Apple HFS+", "Apple",
             "Mac OS Extended / HFS Plus filesystem.",
             {"hfsplus"}, {"hfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace,
             "Journaled or feature-rich volumes can restrict current Linux write support."},
            {"apfs", "Apple APFS", "Apple",
             "Apple File System on-disk format.",
             {"apfs"}, {"apfs-dkms", "apfsprogs"}, AccessMode::Experimental, SupportProvider::Dkms,
             "APFS has one catalogue identity. The current Debian kernel provider is experimental; FUSE/DKMS provider names are implementation details rather than separate filesystems."},

            // Other writable/local block-device formats.
            {"udf", "UDF", "Optical / Local storage",
             "Universal Disk Format, including writable block-device media.",
             {"udf"}, {"udftools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,
             "UDF remains in scope because it is a persistent on-media filesystem format and can be used on writable block devices; ISO9660 and immutable image filesystems are out of setup-root scope."},
            {"vmfs", "VMware VMFS3 / VMFS5", "Virtualisation / Disk",
             "VMware VMFS3/VMFS5 on-disk filesystem format.",
             {}, {"vmfs-tools"}, AccessMode::ReadOnly, SupportProvider::Userspace,
             "Current Debian access is read-only; VMFS remains in scope because it is a real persistent disk filesystem."},
            {"vmfs6", "VMware VMFS6", "Virtualisation / Disk",
             "VMware VMFS6 on-disk filesystem format.",
             {}, {"vmfs6-tools"}, AccessMode::ReadOnly, SupportProvider::Userspace,
             "Current Debian access is read-only; VMFS6 remains in scope because it is a real persistent disk filesystem."},
            {"cpm", "CP/M filesystems", "Retro",
             "CP/M local disk filesystem family.",
             {}, {"cpmtools"}, AccessMode::ToolsOnly, SupportProvider::ToolsOnly,
             "Current support is tools-only; the format remains a legitimate future native/root-capability target rather than a tools product identity."},
            {"fosfat", "Smaky filesystem (Fosfat)", "Retro",
             "Smaky local disk filesystem format.",
             {}, {"fosfat"}, AccessMode::ReadOnly, SupportProvider::Userspace,
             "Current Debian access is read-only userspace; the catalogue identity is the disk format, not FUSE itself."}
        };

        for (auto& entry : values) {
            const NativeFilesystemPolicy* policy = native_policy_for(entry.id);
            if (policy == nullptr) {
                continue;
            }
            entry.modules = {policy->module};
            entry.project_native_linux = true;
        }
        return values;
    }();

    return entries;
}

const char* access_mode_label(const AccessMode mode)
{
    switch (mode) {
    case AccessMode::ReadWrite:
        return "Read / write";
    case AccessMode::ReadOnly:
        return "Read-only";
    case AccessMode::Mixed:
        return "Variant / kernel dependent";
    case AccessMode::Userspace:
        return "Userspace / FUSE";
    case AccessMode::ToolsOnly:
        return "Userspace tools";
    case AccessMode::Experimental:
        return "Experimental";
    }
    return "Unknown";
}

const char* support_provider_label(const SupportProvider provider)
{
    switch (provider) {
    case SupportProvider::Kernel:
        return "Kernel";
    case SupportProvider::KernelWithUserspace:
        return "Kernel + userspace";
    case SupportProvider::Dkms:
        return "DKMS driver";
    case SupportProvider::Userspace:
        return "Userspace / FUSE";
    case SupportProvider::ToolsOnly:
        return "Tools only";
    }
    return "Unknown";
}

bool package_is_catalogued(const std::string_view package)
{
    if (package.empty()) {
        return false;
    }

    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.packages) {
            if (candidate == package) {
                return true;
            }
        }
    }

    return false;
}

bool module_is_catalogued(const std::string_view module)
{
    if (module.empty()) {
        return false;
    }

    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.modules) {
            if (candidate == module) {
                return true;
            }
        }
    }

    return false;
}

bool linux_native_module_is_managed(const std::string_view filesystem_id,
                                    const std::string_view module)
{
    if (filesystem_id.empty() || module.empty()) {
        return false;
    }

    for (const auto& policy : kNativeFilesystemPolicies) {
        if (policy.filesystem_id == filesystem_id && policy.module == module) {
            return true;
        }
    }

    return false;
}

std::vector<std::string_view> catalogue_entries_using_package(
    const std::string_view package)
{
    std::vector<std::string_view> entries;

    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.packages) {
            if (candidate == package) {
                entries.push_back(entry.id);
                break;
            }
        }
    }

    return entries;
}

bool catalog_is_valid()
{
    std::unordered_set<std::string_view> ids;
    std::size_t native_entries = 0U;

    for (const auto& entry : catalog()) {
        if (entry.id.empty() || entry.name.empty() || entry.family.empty() ||
            entry.description.empty()) {
            return false;
        }

        if (!ids.insert(entry.id).second) {
            return false;
        }

        if (!safe_catalog_token(entry.id) ||
            !provider_shape_is_valid(entry)) {
            return false;
        }

        if (entry.project_native_linux) {
            ++native_entries;
            const NativeFilesystemPolicy* policy = native_policy_for(entry.id);
            if (policy == nullptr || entry.modules.size() != 1U ||
                entry.modules.front() != policy->module ||
                (entry.provider != SupportProvider::Kernel &&
                 entry.provider != SupportProvider::KernelWithUserspace)) {
                return false;
            }
        }

        for (const auto package : entry.packages) {
            if (!safe_catalog_token(package)) {
                return false;
            }
        }

        for (const auto module : entry.modules) {
            if (!safe_catalog_token(module)) {
                return false;
            }
        }
    }

    if (native_entries != kNativeFilesystemPolicies.size()) {
        return false;
    }
    for (const auto& policy : kNativeFilesystemPolicies) {
        if (!safe_catalog_token(policy.filesystem_id) ||
            !safe_catalog_token(policy.module) ||
            ids.find(policy.filesystem_id) == ids.end()) {
            return false;
        }
    }

    return true;
}

} // namespace filesystem_support
