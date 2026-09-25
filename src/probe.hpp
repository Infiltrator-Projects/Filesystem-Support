// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalog.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace filesystem_support {

enum class SupportState {
    Ready,
    Installable,
    Incomplete,
    Unavailable
};

enum class KernelState {
    NotApplicable,
    BuiltIn,
    LoadableUnloaded,
    LoadableLoaded,
    Missing
};

struct ProbeResult {
    SupportState state = SupportState::Unavailable;
    bool module_requirement_met = false;
    bool package_requirement_met = false;
    KernelState kernel_state = KernelState::NotApplicable;
    std::string module_name;
    bool project_native_module = false;
    bool project_native_installed = false;
    bool project_native_selected = false;
    bool repository_packages_available = true;
    std::vector<std::string> missing_packages;
    std::vector<std::string> unavailable_packages;
    std::string detail;
};

void invalidate_probe_cache();
bool package_installed(std::string_view package);
bool package_available(std::string_view package);
bool module_available(std::string_view module);
bool project_native_module_installed(std::string_view module);
bool project_native_module_selected(std::string_view module);
KernelState module_state(std::string_view module);
ProbeResult probe(const FilesystemDescriptor& descriptor);
const char* support_state_label(SupportState state);
const char* kernel_state_label(KernelState state);

} // namespace filesystem_support
