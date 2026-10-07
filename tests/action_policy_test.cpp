// SPDX-License-Identifier: GPL-3.0-or-later
#include "action_policy.hpp"
#include "catalog.hpp"
#include "installer.hpp"

#include <iostream>
#include <string>

namespace {

int fail(const char* message)
{
    std::cerr << "action_policy_test: " << message << '\n';
    return 1;
}

const filesystem_support::FilesystemDescriptor* find_entry(
    const std::string_view id)
{
    for (const auto& entry : filesystem_support::catalog()) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    using namespace filesystem_support;

    if (!package_is_catalogued("util-linux") ||
        package_is_catalogued("definitely-not-catalogued")) {
        return fail("package allowlist is not deny-by-default");
    }

    if (!module_is_catalogued("ofs") ||
        !module_is_catalogued("ffs") ||
        module_is_catalogued("affs") ||
        module_is_catalogued("definitely-not-catalogued")) {
        return fail("module allowlist is not deny-by-default");
    }

    const RemovalPlan empty_plan = plan_package_removal({});
    if (empty_plan.allowed) {
        return fail("empty package removal was allowed");
    }

    const RemovalPlan unknown_plan =
        plan_package_removal({"definitely-not-catalogued"});
    if (unknown_plan.allowed) {
        return fail("unknown package removal was allowed");
    }

    bool install_callback = false;
    bool install_success = true;
    install_packages_async(
        {"definitely-not-catalogued"},
        [&install_callback, &install_success](
            const bool success, const std::string&) {
            install_callback = true;
            install_success = success;
        });
    if (!install_callback || install_success) {
        return fail("invalid install was not denied synchronously");
    }

    bool module_callback = false;
    bool module_success = true;
    load_module_async(
        "definitely-not-catalogued",
        [&module_callback, &module_success](
            const bool success, const std::string&) {
            module_callback = true;
            module_success = success;
        });
    if (!module_callback || module_success) {
        return fail("invalid module action was not denied synchronously");
    }

    bool native_callback = false;
    bool native_success = true;
    install_native_module_async(
        "not-managed",
        "not-managed",
        [&native_callback, &native_success](
            const bool success, const std::string&) {
            native_callback = true;
            native_success = success;
        });
    if (!native_callback || native_success) {
        return fail("unmanaged native module action was not denied synchronously");
    }

    const auto hfs_users = catalogue_entries_using_package("hfsprogs");
    if (hfs_users.size() != 2U) {
        return fail("shared package impact mapping is incorrect");
    }

    const FilesystemDescriptor* ext3 = find_entry("ext3");
    const FilesystemDescriptor* apfs = find_entry("apfs");
    const FilesystemDescriptor* hfs = find_entry("hfs");
    if (ext3 == nullptr || apfs == nullptr || hfs == nullptr) {
        return fail("policy fixtures are missing from the catalogue");
    }

    ProbeResult native_missing;
    native_missing.module_name = "ext3";
    native_missing.kernel_state = KernelState::Missing;
    const ModuleActionPolicy install_native =
        module_action_policy(*ext3, native_missing);
    if (install_native.action != ModuleActionKind::InstallNative ||
        !install_native.button.visible || !install_native.button.enabled ||
        !install_native.button.primary || install_native.button.destructive ||
        install_native.button.label != "Install native") {
        return fail("native install presentation leaked or changed semantics");
    }

    ProbeResult native_installed = native_missing;
    native_installed.project_native_installed = true;
    native_installed.project_native_selected = true;
    native_installed.kernel_state = KernelState::LoadableLoaded;
    const ModuleActionPolicy remove_native =
        module_action_policy(*ext3, native_installed);
    if (remove_native.action != ModuleActionKind::RemoveNative ||
        !remove_native.button.enabled || !remove_native.button.destructive ||
        remove_native.button.label != "Remove native") {
        return fail("native remove presentation leaked or changed semantics");
    }

    ProbeResult hfs_missing;
    hfs_missing.missing_packages = {"hfsprogs"};
    const PackageActionPolicy install_userspace =
        package_action_policy(*hfs, hfs_missing);
    if (install_userspace.action != PackageActionKind::Install ||
        !install_userspace.button.enabled || !install_userspace.button.primary ||
        install_userspace.button.label != "Install userspace") {
        return fail("userspace install policy is incorrect");
    }

    ProbeResult apfs_ready;
    apfs_ready.kernel_state = KernelState::LoadableLoaded;
    const PackageActionPolicy remove_apfs =
        package_action_policy(*apfs, apfs_ready);
    if (remove_apfs.action != PackageActionKind::Remove ||
        !remove_apfs.button.destructive ||
        !remove_apfs.removal_requires_module_unload ||
        remove_apfs.button.label != "Remove support") {
        return fail("DKMS removal guard is not represented by policy");
    }

    ProbeResult unavailable = hfs_missing;
    unavailable.unavailable_packages = {"hfsprogs"};
    const PackageActionPolicy blocked =
        package_action_policy(*hfs, unavailable);
    if (blocked.action != PackageActionKind::None ||
        blocked.button.enabled || blocked.button.label != "Package unavailable") {
        return fail("unavailable package policy is incorrect");
    }

    const std::string native_detail =
        support_detail_text(*ext3, native_installed);
    if (native_detail.find("Infiltrator native") == std::string::npos ||
        native_detail.find("Kernel module loaded") == std::string::npos) {
        return fail("support detail policy lost native state");
    }

    return 0;
}
