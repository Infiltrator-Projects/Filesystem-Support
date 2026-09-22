// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
#include <vector>

namespace filesystem_support {
enum class AccessMode { ReadWrite, ReadOnly, Mixed, Userspace };

struct FilesystemDescriptor {
    std::string_view id;
    std::string_view name;
    std::string_view family;
    std::string_view description;
    std::vector<std::string_view> modules;
    std::vector<std::string_view> packages;
    AccessMode access;
    std::string_view note;
};

const std::vector<FilesystemDescriptor>& catalog();
const char* access_mode_label(AccessMode mode);
bool catalog_is_valid();
} // namespace filesystem_support
