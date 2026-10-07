// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalog.hpp"
#include "probe.hpp"

#include <string>
#include <string_view>

namespace filesystem_support {

enum class PackageActionKind {
    None,
    Install,
    Remove
};

enum class ModuleActionKind {
    None,
    InstallNative,
    RemoveNative,
    Load,
    Unload
};

struct ButtonPolicy {
    bool visible = false;
    bool enabled = false;
    bool primary = false;
    bool destructive = false;
    std::string_view label;
};

struct PackageActionPolicy {
    PackageActionKind action = PackageActionKind::None;
    ButtonPolicy button;
    bool removal_requires_module_unload = false;
};

struct ModuleActionPolicy {
    ModuleActionKind action = ModuleActionKind::None;
    ButtonPolicy button;
};

std::string support_detail_text(const FilesystemDescriptor& descriptor,
                                const ProbeResult& probe);
PackageActionPolicy package_action_policy(const FilesystemDescriptor& descriptor,
                                          const ProbeResult& probe);
ModuleActionPolicy module_action_policy(const FilesystemDescriptor& descriptor,
                                        const ProbeResult& probe);

} // namespace filesystem_support
