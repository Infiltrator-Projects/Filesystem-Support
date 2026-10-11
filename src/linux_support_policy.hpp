// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalog.hpp"

#include <string_view>
#include <vector>

namespace filesystem_support {

struct LinuxSupportPolicy {
    std::string_view filesystem_id;
    std::vector<std::string_view> modules;
    std::vector<std::string_view> packages;
    AccessMode access;
    SupportProvider provider;
};

inline const std::vector<LinuxSupportPolicy>& linux_support_policies()
{
    static const std::vector<LinuxSupportPolicy> policies = {
        {"infiltratorfs", {"infiltratorfs"}, {"infiltratorfs"}, AccessMode::ReadWrite, SupportProvider::Dkms},
        {"ext2", {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel},
        {"ext3", {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel},
        {"ext4", {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel},
        {"xfs", {"xfs"}, {"xfsprogs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"btrfs", {"btrfs"}, {"btrfs-progs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"f2fs", {"f2fs"}, {"f2fs-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"jfs", {"jfs"}, {"jfsutils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"nilfs2", {"nilfs2"}, {"nilfs-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"reiserfs", {"reiserfs"}, {"reiserfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace},
        {"minix", {"minix"}, {"util-linux"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"bcachefs", {"bcachefs"}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"gfs2", {"gfs2"}, {"gfs2-utils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"ocfs2", {"ocfs2"}, {"ocfs2-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"zfs", {"zfs"}, {"zfsutils-linux", "zfs-dkms"}, AccessMode::ReadWrite, SupportProvider::Dkms},
        {"adfs", {"adfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"ofs", {}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"ffs", {}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"sfs", {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel},
        {"sfs2", {}, {}, AccessMode::ReadWrite, SupportProvider::Kernel},
        {"pfs3", {}, {}, AccessMode::ReadOnly, SupportProvider::Kernel},
        {"befs", {"befs"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel},
        {"bfs", {"bfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"efs", {"efs"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel},
        {"hpfs", {"hpfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"qnx4", {"qnx4"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel},
        {"qnx6", {"qnx6"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel},
        {"sysv", {"sysv"}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"ufs", {"ufs"}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"omfs", {"omfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel},
        {"fat", {"vfat", "msdos"}, {"dosfstools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"exfat", {"exfat"}, {"exfatprogs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"ntfs", {}, {"ntfs-3g"}, AccessMode::Userspace, SupportProvider::Userspace},
        {"hfs", {"hfs"}, {"hfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace},
        {"hfsplus", {"hfsplus"}, {"hfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace},
        {"apfs", {"apfs"}, {"apfs-dkms", "apfsprogs"}, AccessMode::Experimental, SupportProvider::Dkms},
        {"udf", {"udf"}, {"udftools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace},
        {"vmfs", {}, {"vmfs-tools"}, AccessMode::ReadOnly, SupportProvider::Userspace},
        {"vmfs6", {}, {"vmfs6-tools"}, AccessMode::ReadOnly, SupportProvider::Userspace},
        {"cpm", {}, {"cpmtools"}, AccessMode::ToolsOnly, SupportProvider::ToolsOnly},
        {"fosfat", {}, {"fosfat"}, AccessMode::ReadOnly, SupportProvider::Userspace}
    };
    return policies;
}

} // namespace filesystem_support
