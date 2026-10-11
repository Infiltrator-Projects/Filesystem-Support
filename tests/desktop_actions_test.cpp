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
            action.busy_label != "Installing…" ||
            action.failure_title != "Installation failed") {
            return fail("package installation leaked or lost orchestration policy");
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
            action.failure_title != "Unload module first" ||
            action.message.empty()) {
            return fail("DKMS removal interlock is not owned by desktop orchestration");
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
            action.busy_label != "Installing native…" ||
            action.failure_title != "Native module installation failed") {
            return fail("native installation is not hidden behind desktop orchestration");
        }

        probe.project_native_installed = true;
        action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Execute ||
            action.busy_label != "Removing native…" ||
            action.failure_title != "Native module removal failed") {
            return fail("native removal is not hidden behind desktop orchestration");
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
            action.busy_label != "Loading…" ||
            action.failure_title != "Module load failed") {
            return fail("module loading is not hidden behind desktop orchestration");
        }

        probe.kernel_state = KernelState::LoadableLoaded;
        action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::Execute ||
            action.busy_label != "Unloading…" ||
            action.failure_title != "Module unload failed") {
            return fail("module unloading is not hidden behind desktop orchestration");
        }

        probe.kernel_state = KernelState::BuiltIn;
        action = prepare_module_desktop_action(entry, probe);
        if (action.disposition != DesktopActionDisposition::None) {
            return fail("non-actionable module state produced an executable desktop action");
        }
    }

    return 0;
}
