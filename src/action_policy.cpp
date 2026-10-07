// SPDX-License-Identifier: GPL-3.0-or-later
#include "action_policy.hpp"

namespace filesystem_support {
namespace {

std::string_view install_label(const FilesystemDescriptor& descriptor)
{
    switch (descriptor.provider) {
    case SupportProvider::Userspace:
    case SupportProvider::Dkms:
        return "Install support";
    case SupportProvider::KernelWithUserspace:
        return "Install userspace";
    case SupportProvider::ToolsOnly:
        return "Install tools";
    case SupportProvider::Kernel:
        return "No package";
    }
    return "Install";
}

std::string_view remove_label(const FilesystemDescriptor& descriptor)
{
    switch (descriptor.provider) {
    case SupportProvider::Userspace:
    case SupportProvider::Dkms:
        return "Remove support";
    case SupportProvider::KernelWithUserspace:
        return "Remove userspace";
    case SupportProvider::ToolsOnly:
        return "Remove tools";
    case SupportProvider::Kernel:
        return "No package";
    }
    return "Remove";
}

} // namespace

std::string support_detail_text(const FilesystemDescriptor& descriptor,
                                const ProbeResult& probe)
{
    std::string detail =
        std::string("Support path: ") + support_provider_label(descriptor.provider);

    if (!descriptor.modules.empty()) {
        detail += "  •  ";
        if (descriptor.project_native_linux) {
            detail += "Infiltrator native: ";
            if (!probe.project_native_installed) {
                detail += "not installed";
            } else if (!probe.project_native_selected) {
                detail += "installed, not selected by kernel";
            } else {
                detail += kernel_state_label(probe.kernel_state);
            }
        } else {
            detail += "Kernel: ";
            detail += kernel_state_label(probe.kernel_state);
        }
    }

    if (!probe.detail.empty()) {
        detail += ". ";
        detail += probe.detail;
    }
    return detail;
}

PackageActionPolicy package_action_policy(const FilesystemDescriptor& descriptor,
                                          const ProbeResult& probe)
{
    PackageActionPolicy policy;
    if (descriptor.packages.empty()) {
        return policy;
    }

    policy.button.visible = true;
    const bool missing = !probe.missing_packages.empty();
    const bool unavailable = !probe.unavailable_packages.empty();

    if (missing) {
        if (unavailable) {
            policy.button.label = "Package unavailable";
            return policy;
        }
        policy.action = PackageActionKind::Install;
        policy.button.enabled = true;
        policy.button.primary = true;
        policy.button.label = install_label(descriptor);
        return policy;
    }

    if (unavailable) {
        policy.button.label = "Package unavailable";
        return policy;
    }

    policy.action = PackageActionKind::Remove;
    policy.button.enabled = true;
    policy.button.destructive = true;
    policy.button.label = remove_label(descriptor);
    policy.removal_requires_module_unload =
        descriptor.provider == SupportProvider::Dkms &&
        probe.kernel_state == KernelState::LoadableLoaded;
    return policy;
}

ModuleActionPolicy module_action_policy(const FilesystemDescriptor& descriptor,
                                        const ProbeResult& probe)
{
    ModuleActionPolicy policy;
    if (descriptor.modules.empty()) {
        return policy;
    }

    policy.button.visible = true;

    if (descriptor.project_native_linux) {
        policy.button.enabled = true;
        if (probe.project_native_installed) {
            policy.action = ModuleActionKind::RemoveNative;
            policy.button.destructive = true;
            policy.button.label = "Remove native";
        } else {
            policy.action = ModuleActionKind::InstallNative;
            policy.button.primary = true;
            policy.button.label = "Install native";
        }
        return policy;
    }

    switch (probe.kernel_state) {
    case KernelState::NotApplicable:
        policy.button.visible = false;
        break;
    case KernelState::BuiltIn:
        policy.button.label = "Built into kernel";
        break;
    case KernelState::LoadableUnloaded:
        policy.action = ModuleActionKind::Load;
        policy.button.enabled = true;
        policy.button.primary = true;
        policy.button.label = "Load module";
        break;
    case KernelState::LoadableLoaded:
        policy.action = ModuleActionKind::Unload;
        policy.button.enabled = true;
        policy.button.destructive = true;
        policy.button.label = "Unload module";
        break;
    case KernelState::Missing:
        policy.button.label = descriptor.provider == SupportProvider::Dkms
            ? "Driver not installed"
            : "Requires different kernel";
        break;
    }

    return policy;
}

} // namespace filesystem_support
