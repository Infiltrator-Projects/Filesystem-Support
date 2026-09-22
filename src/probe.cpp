// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"

#include <glib.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace filesystem_support {
namespace {

struct CommandResult {
    int exit_status = -1;
    std::string output;
};

CommandResult run_command(const std::vector<std::string>& arguments)
{
    CommandResult result;
    if (arguments.empty()) {
        return result;
    }

    gchar* program = g_find_program_in_path(arguments.front().c_str());
    if (program == nullptr) {
        return result;
    }

    std::vector<std::string> owned = arguments;
    owned.front() = program;
    g_free(program);

    std::vector<gchar*> argv;
    argv.reserve(owned.size() + 1U);
    for (auto& argument : owned) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    gchar* stdout_text = nullptr;
    gchar* stderr_text = nullptr;
    gint wait_status = -1;
    GError* error = nullptr;

    const gboolean spawned = g_spawn_sync(
        nullptr,
        argv.data(),
        nullptr,
        G_SPAWN_DEFAULT,
        nullptr,
        nullptr,
        &stdout_text,
        &stderr_text,
        &wait_status,
        &error);

    if (spawned) {
        result.exit_status =
            g_spawn_check_wait_status(wait_status, nullptr) ? 0 : 1;
        if (stdout_text != nullptr) {
            result.output = stdout_text;
        }
    }

    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
    return result;
}

bool filesystem_registered(const std::string_view name)
{
    std::ifstream stream("/proc/filesystems");
    std::string line;

    while (std::getline(stream, line)) {
        std::istringstream parser(line);
        std::string token;
        std::string last;

        while (parser >> token) {
            last = token;
        }

        if (last == name) {
            return true;
        }
    }

    return false;
}

std::string join_names(const std::vector<std::string>& names)
{
    std::string joined;

    for (const auto& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }

    return joined;
}

} // namespace

bool package_installed(const std::string_view package)
{
    if (package.empty()) {
        return false;
    }

    const auto result = run_command({
        "dpkg-query",
        "-W",
        "-f=${db:Status-Status}",
        std::string(package)
    });

    return result.exit_status == 0 && result.output == "installed";
}

bool package_available(const std::string_view package)
{
    if (package.empty()) {
        return false;
    }

    const auto result = run_command({
        "apt-cache",
        "--no-all-versions",
        "show",
        std::string(package)
    });

    return result.exit_status == 0 && !result.output.empty();
}

KernelState module_state(const std::string_view module)
{
    if (module.empty()) {
        return KernelState::Missing;
    }

    const auto filename = run_command({
        "modinfo",
        "-F",
        "filename",
        std::string(module)
    });

    if (filename.exit_status == 0 && !filename.output.empty()) {
        if (filename.output.find("builtin") != std::string::npos) {
            return KernelState::BuiltIn;
        }

        const std::string sys_module =
            "/sys/module/" + std::string(module);
        if (filesystem_registered(module) ||
            g_file_test(sys_module.c_str(), G_FILE_TEST_IS_DIR)) {
            return KernelState::LoadableLoaded;
        }

        return KernelState::LoadableUnloaded;
    }

    const std::string sys_module =
        "/sys/module/" + std::string(module);
    if (filesystem_registered(module) ||
        g_file_test(sys_module.c_str(), G_FILE_TEST_IS_DIR)) {
        return KernelState::BuiltIn;
    }

    return KernelState::Missing;
}

bool module_available(const std::string_view module)
{
    return module_state(module) != KernelState::Missing;
}

ProbeResult probe(const FilesystemDescriptor& descriptor)
{
    ProbeResult result;

    result.module_requirement_met = descriptor.modules.empty();
    result.kernel_state = descriptor.modules.empty()
        ? KernelState::NotApplicable
        : KernelState::Missing;

    for (const auto module : descriptor.modules) {
        const KernelState state = module_state(module);

        const bool better =
            state == KernelState::BuiltIn ||
            (state == KernelState::LoadableLoaded &&
             result.kernel_state != KernelState::BuiltIn) ||
            (state == KernelState::LoadableUnloaded &&
             result.kernel_state == KernelState::Missing);

        if (better) {
            result.kernel_state = state;
            result.module_name = std::string(module);
        }
    }

    result.module_requirement_met =
        result.kernel_state == KernelState::NotApplicable ||
        result.kernel_state == KernelState::BuiltIn ||
        result.kernel_state == KernelState::LoadableLoaded ||
        result.kernel_state == KernelState::LoadableUnloaded;

    result.package_requirement_met = true;
    for (const auto package : descriptor.packages) {
        if (package_installed(package)) {
            continue;
        }

        result.package_requirement_met = false;

        if (package_available(package)) {
            result.missing_packages.emplace_back(package);
        } else {
            result.repository_packages_available = false;
            result.unavailable_packages.emplace_back(package);
        }
    }

    if (result.module_requirement_met && result.package_requirement_met) {
        result.state = SupportState::Ready;
        result.detail = "Support is installed on this Debian system.";
    } else if (!result.unavailable_packages.empty()) {
        result.state = SupportState::Unavailable;
        result.detail =
            "Not available from the configured Debian repositories: " +
            join_names(result.unavailable_packages) + ".";
    } else if (!result.missing_packages.empty()) {
        result.state = SupportState::Installable;
        result.detail = result.module_requirement_met
            ? "Kernel support is present; Debian packages are available to install."
            : "Debian packages are available; install them and the driver will be re-checked.";
    } else if (!result.module_requirement_met) {
        result.state = SupportState::Unavailable;
        result.detail =
            "The running Debian kernel does not expose the required filesystem driver.";
    } else {
        result.state = SupportState::Incomplete;
        result.detail = "Filesystem support is only partially available.";
    }

    return result;
}

const char* support_state_label(const SupportState state)
{
    switch (state) {
    case SupportState::Ready:
        return "Installed";
    case SupportState::Installable:
        return "Available";
    case SupportState::Incomplete:
        return "Incomplete";
    case SupportState::Unavailable:
        return "Unavailable";
    }
    return "Unknown";
}

const char* kernel_state_label(const KernelState state)
{
    switch (state) {
    case KernelState::NotApplicable:
        return "Not applicable";
    case KernelState::BuiltIn:
        return "Built into kernel";
    case KernelState::LoadableUnloaded:
        return "Kernel module available";
    case KernelState::LoadableLoaded:
        return "Kernel module loaded";
    case KernelState::Missing:
        return "Requires different kernel";
    }
    return "Unknown";
}

} // namespace filesystem_support
