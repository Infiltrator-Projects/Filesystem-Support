// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>
#include <vector>

namespace filesystem_support {

enum class AccessMode {
    ReadWrite,
    ReadOnly,
    Mixed,
    Userspace,
    ToolsOnly,
    Experimental
};

enum class SupportProvider {
    Kernel,
    KernelWithUserspace,
    Dkms,
    Userspace,
    ToolsOnly
};

struct FilesystemDescriptor {
    std::string_view id;
    std::string_view name;
    std::string_view family;
    std::string_view description;
    std::vector<std::string_view> modules;
    std::vector<std::string_view> packages;
    AccessMode access;
    SupportProvider provider;
    std::string_view note;
    bool project_native_linux = false;
};

const std::vector<FilesystemDescriptor>& catalog();
const char* access_mode_label(AccessMode mode);
const char* support_provider_label(SupportProvider provider);
bool package_is_catalogued(std::string_view package);
bool module_is_catalogued(std::string_view module);
bool linux_native_module_is_managed(std::string_view filesystem_id,
                                    std::string_view module);
std::vector<std::string_view> catalogue_entries_using_package(
    std::string_view package);
bool catalog_is_valid();

} // namespace filesystem_support
