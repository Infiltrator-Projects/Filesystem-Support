// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_actions.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(const char* message)
{
    std::cerr << "desktop_actions_test: " << message << '\n';
    return 1;
}

filesystem_support::FilesystemDescriptor descriptor(
    const std::string_view id,
    const filesystem_support::SupportProvider provider,
    std::vector<std::string_view> modules,
    std::vector<std::string_view> packages,
    const bool native = false)
{
    return {
        id,
        id,
        "Test",
        "Test descriptor",
        std::move(modules),
        std::move(packages),
        filesystem_support::AccessMode::ReadWrite,
        provider,
        {},
        native
    };
}

} // namespace

int main()
{
    using namespace filesystem_support;

    {
        auto entry = descriptor(
            "userspace-test", SupportProvider::Userspace, {}, {"test-package"});
        ProbeResult probe;
        probe.missing_packages = {"test-package"};
        const DesktopAction action = prepare_package_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Execute ||
            action.kind != DesktopActionKind::InstallPackages ||
            action.packages != probe.missing_packages ||
            action.busy_label != "Installing…" ||
            action.failure_title != "Installation failed") {
            return fail("package installation command is not explicit and inspectable");
        }
    }

    {
        auto entry = descriptor(
            "userspace-remove", SupportProvider::Userspace, {}, {"test-package"});
        ProbeResult probe;
        const DesktopAction action = prepare_package_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::PlanningRemoval ||
            action.kind != DesktopActionKind::None ||
            action.packages != std::vector<std::string>{"test-package"} ||
            action.busy_label != "Checking removal…" ||
            action.failure_title != "Removal planning failed") {
            return fail("package removal planning is not an explicit non-blocking state");
        }
    }

    {
        auto entry = descriptor(
            "dkms-test", SupportProvider::Dkms,
            {"dkms-test"}, {"dkms-test-package"});
        ProbeResult probe;
        probe.module_name = "dkms-test";
        probe.kernel_state = KernelState::LoadableLoaded;
        const DesktopAction action = prepare_package_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Blocked ||
            action.kind != DesktopActionKind::None ||
            !action.packages.empty() ||
            action.failure_title != "Unload module first" ||
            action.message.empty()) {
            return fail("DKMS removal interlock leaked an executable command");
        }
    }

    {
        auto entry = descriptor(
            "ofs", SupportProvider::Kernel, {"ofs"}, {}, true);
        ProbeResult probe;
        probe.module_name = "ofs";
        probe.project_native_installed = false;
        DesktopAction action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Execute ||
            action.kind != DesktopActionKind::InstallNativeModule ||
            action.filesystem_id != "ofs" || action.module != "ofs" ||
            action.busy_label != "Installing native…" ||
            action.failure_title != "Native module installation failed") {
            return fail("native installation command is not explicit and inspectable");
        }

        probe.project_native_installed = true;
        action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Execute ||
            action.kind != DesktopActionKind::RemoveNativeModule ||
            action.filesystem_id != "ofs" || action.module != "ofs" ||
            action.busy_label != "Removing native…" ||
            action.failure_title != "Native module removal failed") {
            return fail("native removal command is not explicit and inspectable");
        }
    }

    {
        auto entry = descriptor(
            "kernel-test", SupportProvider::Kernel, {"kernel-test"}, {});
        ProbeResult probe;
        probe.module_name = "kernel-test";
        probe.kernel_state = KernelState::LoadableUnloaded;
        DesktopAction action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Execute ||
            action.kind != DesktopActionKind::LoadModule ||
            action.module != "kernel-test" ||
            action.busy_label != "Loading…" ||
            action.failure_title != "Module load failed") {
            return fail("module load command is not explicit and inspectable");
        }

        probe.kernel_state = KernelState::LoadableLoaded;
        action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Execute ||
            action.kind != DesktopActionKind::UnloadModule ||
            action.module != "kernel-test" ||
            action.busy_label != "Unloading…" ||
            action.failure_title != "Module unload failed") {
            return fail("module unload command is not explicit and inspectable");
        }

        probe.kernel_state = KernelState::BuiltIn;
        action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::None ||
            action.kind != DesktopActionKind::None ||
            !action.module.empty()) {
            return fail("non-actionable module state produced command payload");
        }
    }

    {
        bool completion_called = false;
        bool completion_success = true;
        std::string completion_message;
        execute_desktop_action_async(
            DesktopAction {},
            [&](const bool success, const std::string& message) {
                completion_called = true;
                completion_success = success;
                completion_message = message;
            });
        if (!completion_called || completion_success || completion_message.empty()) {
            return fail("invalid desktop action did not fail closed");
        }
    }

    return 0;
}
