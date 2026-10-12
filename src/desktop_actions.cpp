// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_actions.hpp"

#include "action_policy.hpp"
#include "installer.hpp"

#include <utility>

namespace filesystem_support {
namespace {

bool executable_disposition(const DesktopActionDisposition disposition)
{
    return disposition == DesktopActionDisposition::Execute ||
           disposition == DesktopActionDisposition::ConfirmRemoval;
}

void fail_invalid_action(DesktopActionCompletion completion)
{
    if (completion) {
        completion(false, "The prepared desktop action is not executable.");
    }
}

void finish_removal_plan(DesktopAction action,
                         RemovalPlan plan,
                         DesktopActionPrepared completion)
{
    if (!completion) {
        return;
    }

    if (!plan.allowed) {
        action.disposition = DesktopActionDisposition::Blocked;
        action.kind = DesktopActionKind::None;
        action.failure_title = "Removal blocked";
        action.message = std::move(plan.reason);
        action.packages.clear();
        completion(std::move(action));
        return;
    }

    if (plan.planned_packages.empty()) {
        action.disposition = DesktopActionDisposition::Refresh;
        action.kind = DesktopActionKind::None;
        action.packages.clear();
        completion(std::move(action));
        return;
    }

    action.disposition = DesktopActionDisposition::ConfirmRemoval;
    action.kind = DesktopActionKind::RemovePackages;
    action.busy_label = "Removing…";
    action.failure_title = "Removal failed";
    action.planned_packages = std::move(plan.planned_packages);
    action.affected_entries = std::move(plan.affected_entries);
    completion(std::move(action));
}

} // namespace

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
        action.disposition = DesktopActionDisposition::Execute;
        action.kind = DesktopActionKind::InstallPackages;
        action.busy_label = "Installing…";
        action.failure_title = "Installation failed";
        action.packages = probe.missing_packages;
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

    action.disposition = DesktopActionDisposition::PlanningRemoval;
    action.busy_label = "Checking removal…";
    action.failure_title = "Removal planning failed";
    action.packages.reserve(descriptor.packages.size());
    for (const auto package : descriptor.packages) {
        action.packages.emplace_back(package);
    }
    return action;
}

void prepare_package_desktop_action_async(
    const FilesystemDescriptor& descriptor,
    const ProbeResult& probe,
    DesktopActionPrepared completion)
{
    DesktopAction action = prepare_package_desktop_action(descriptor, probe);
    if (!completion) {
        return;
    }

    if (action.disposition != DesktopActionDisposition::PlanningRemoval) {
        completion(std::move(action));
        return;
    }

    const std::vector<std::string> packages = action.packages;
    plan_package_removal_async(
        packages,
        [action = std::move(action),
         completion = std::move(completion)](RemovalPlan plan) mutable {
            finish_removal_plan(
                std::move(action), std::move(plan), std::move(completion));
        });
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
    action.module = probe.module_name;

    switch (policy.action) {
    case ModuleActionKind::InstallNative:
        action.disposition = DesktopActionDisposition::Execute;
        action.kind = DesktopActionKind::InstallNativeModule;
        action.busy_label = "Installing native…";
        action.failure_title = "Native module installation failed";
        action.filesystem_id = std::string(descriptor.id);
        break;
    case ModuleActionKind::RemoveNative:
        action.disposition = DesktopActionDisposition::Execute;
        action.kind = DesktopActionKind::RemoveNativeModule;
        action.busy_label = "Removing native…";
        action.failure_title = "Native module removal failed";
        action.filesystem_id = std::string(descriptor.id);
        break;
    case ModuleActionKind::Load:
        action.disposition = DesktopActionDisposition::Execute;
        action.kind = DesktopActionKind::LoadModule;
        action.busy_label = "Loading…";
        action.failure_title = "Module load failed";
        break;
    case ModuleActionKind::Unload:
        action.disposition = DesktopActionDisposition::Execute;
        action.kind = DesktopActionKind::UnloadModule;
        action.busy_label = "Unloading…";
        action.failure_title = "Module unload failed";
        break;
    case ModuleActionKind::None:
        action.module.clear();
        break;
    }

    return action;
}

void execute_desktop_action_async(
    DesktopAction action,
    DesktopActionCompletion completion)
{
    if (!executable_disposition(action.disposition)) {
        fail_invalid_action(std::move(completion));
        return;
    }

    switch (action.kind) {
    case DesktopActionKind::InstallPackages:
        if (action.packages.empty()) {
            fail_invalid_action(std::move(completion));
            return;
        }
        install_packages_async(action.packages, std::move(completion));
        return;
    case DesktopActionKind::RemovePackages:
        if (action.packages.empty() || action.planned_packages.empty()) {
            fail_invalid_action(std::move(completion));
            return;
        }
        remove_packages_async(
            action.packages,
            action.planned_packages,
            std::move(completion));
        return;
    case DesktopActionKind::InstallNativeModule:
        if (action.filesystem_id.empty() || action.module.empty()) {
            fail_invalid_action(std::move(completion));
            return;
        }
        install_native_module_async(
            action.filesystem_id, action.module, std::move(completion));
        return;
    case DesktopActionKind::RemoveNativeModule:
        if (action.filesystem_id.empty() || action.module.empty()) {
            fail_invalid_action(std::move(completion));
            return;
        }
        remove_native_module_async(
            action.filesystem_id, action.module, std::move(completion));
        return;
    case DesktopActionKind::LoadModule:
        if (action.module.empty()) {
            fail_invalid_action(std::move(completion));
            return;
        }
        load_module_async(action.module, std::move(completion));
        return;
    case DesktopActionKind::UnloadModule:
        if (action.module.empty()) {
            fail_invalid_action(std::move(completion));
            return;
        }
        unload_module_async(action.module, std::move(completion));
        return;
    case DesktopActionKind::None:
        fail_invalid_action(std::move(completion));
        return;
    }

    fail_invalid_action(std::move(completion));
}

} // namespace filesystem_support
