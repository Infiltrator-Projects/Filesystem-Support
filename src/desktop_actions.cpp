// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_actions.hpp"

#include "action_policy.hpp"
#include "installer.hpp"

#include <utility>

namespace filesystem_support {

DesktopAction prepare_package_desktop_action(
    const FilesystemDescriptor& descriptor,
    const ProbeResult& probe)
{
    DesktopAction action;
    const PackageActionPolicy policy = package_action_policy(descriptor, probe);

    if (policy.action == PackageActionKind::None) {
        return action;
    }

    if (policy.action == PackageActionKind::Install) {
        const std::vector<std::string> packages = probe.missing_packages;
        action.disposition = DesktopActionDisposition::Execute;
        action.busy_label = "Installing…";
        action.failure_title = "Installation failed";
        action.executor = [packages](DesktopActionCompletion completion) {
            install_packages_async(packages, std::move(completion));
        };
        return action;
    }

    if (policy.removal_requires_module_unload) {
        action.disposition = DesktopActionDisposition::Blocked;
        action.failure_title = "Unload module first";
        action.message =
            "The DKMS driver is currently loaded. Unload the kernel module "
            "before removing its Debian driver package.";
        return action;
    }

    std::vector<std::string> packages;
    packages.reserve(descriptor.packages.size());
    for (const auto package : descriptor.packages) {
        packages.emplace_back(package);
    }

    const RemovalPlan plan = plan_package_removal(packages);
    if (!plan.allowed) {
        action.disposition = DesktopActionDisposition::Blocked;
        action.failure_title = "Removal blocked";
        action.message = plan.reason;
        return action;
    }

    if (plan.planned_packages.empty()) {
        action.disposition = DesktopActionDisposition::Refresh;
        return action;
    }

    action.disposition = DesktopActionDisposition::ConfirmRemoval;
    action.busy_label = "Removing…";
    action.failure_title = "Removal failed";
    action.planned_packages = plan.planned_packages;
    action.affected_entries = plan.affected_entries;
    action.executor = [packages = std::move(packages)](
                          DesktopActionCompletion completion) {
        remove_packages_async(packages, std::move(completion));
    };
    return action;
}

DesktopAction prepare_module_desktop_action(
    const FilesystemDescriptor& descriptor,
    const ProbeResult& probe)
{
    DesktopAction action;
    if (probe.module_name.empty()) {
        return action;
    }

    const ModuleActionPolicy policy = module_action_policy(descriptor, probe);
    const std::string module = probe.module_name;

    switch (policy.action) {
    case ModuleActionKind::InstallNative: {
        const std::string filesystem_id(descriptor.id);
        action.disposition = DesktopActionDisposition::Execute;
        action.busy_label = "Installing native…";
        action.failure_title = "Native module installation failed";
        action.executor = [filesystem_id, module](DesktopActionCompletion completion) {
            install_native_module_async(
                filesystem_id, module, std::move(completion));
        };
        break;
    }
    case ModuleActionKind::RemoveNative: {
        const std::string filesystem_id(descriptor.id);
        action.disposition = DesktopActionDisposition::Execute;
        action.busy_label = "Removing native…";
        action.failure_title = "Native module removal failed";
        action.executor = [filesystem_id, module](DesktopActionCompletion completion) {
            remove_native_module_async(
                filesystem_id, module, std::move(completion));
        };
        break;
    }
    case ModuleActionKind::Load:
        action.disposition = DesktopActionDisposition::Execute;
        action.busy_label = "Loading…";
        action.failure_title = "Module load failed";
        action.executor = [module](DesktopActionCompletion completion) {
            load_module_async(module, std::move(completion));
        };
        break;
    case ModuleActionKind::Unload:
        action.disposition = DesktopActionDisposition::Execute;
        action.busy_label = "Unloading…";
        action.failure_title = "Module unload failed";
        action.executor = [module](DesktopActionCompletion completion) {
            unload_module_async(module, std::move(completion));
        };
        break;
    case ModuleActionKind::None:
        break;
    }

    return action;
}

void execute_desktop_action_async(
    DesktopAction action,
    DesktopActionCompletion completion)
{
    if ((action.disposition != DesktopActionDisposition::Execute &&
         action.disposition != DesktopActionDisposition::ConfirmRemoval) ||
        !action.executor) {
        if (completion) {
            completion(false, "The prepared desktop action is not executable.");
        }
        return;
    }

    auto executor = std::move(action.executor);
    executor(std::move(completion));
}

} // namespace filesystem_support
