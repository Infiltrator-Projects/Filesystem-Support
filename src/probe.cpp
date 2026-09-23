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

std::string trim_ascii_whitespace(std::string value)
{
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    std::size_t first = 0U;
    while (first < value.size() &&
           (value[first] == '\n' || value[first] == '\r' ||
            value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }

    return value.substr(first);
}

std::string running_kernel_release()
{
    const auto result = run_command({"uname", "-r"});
    if (result.exit_status != 0) {
        return {};
    }
    return trim_ascii_whitespace(result.output);
}

std::string project_native_module_path(const std::string_view module)
{
    const std::string release = running_kernel_release();
    if (release.empty() || module.empty()) {
        return {};
    }

    return "/lib/modules/" + release +
           "/updates/infiltrator/" + std::string(module) + ".ko";
}

std::string preferred_module_filename(const std::string_view module)
{
    const auto result = run_command({
        "modinfo",
        "-F",
        "filename",
        std::string(module)
    });
    if (result.exit_status != 0) {
        return {};
    }
    return trim_ascii_whitespace(result.output);
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

bool project_native_module_installed(const std::string_view module)
{
    const std::string path = project_native_module_path(module);
    return !path.empty() &&
           g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR);
}

bool project_native_module_selected(const std::string_view module)
{
    const std::string path = project_native_module_path(module);
    if (path.empty()) {
        return false;
    }

    return preferred_module_filename(module) == path;
}

ProbeResult probe(const FilesystemDescriptor& descriptor)
{
    ProbeResult result;

    result.project_native_module = descriptor.project_native_linux;
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

    if (descriptor.project_native_linux) {
        if (descriptor.modules.size() == 1U) {
            result.module_name = std::string(descriptor.modules.front());
            result.project_native_installed =
                project_native_module_installed(descriptor.modules.front());
            result.project_native_selected =
                result.project_native_installed &&
                project_native_module_selected(descriptor.modules.front());
        }

        result.module_requirement_met =
            result.project_native_installed &&
            result.project_native_selected &&
            (result.kernel_state == KernelState::LoadableLoaded ||
             result.kernel_state == KernelState::LoadableUnloaded);
    } else {
        result.module_requirement_met =
            result.kernel_state == KernelState::NotApplicable ||
            result.kernel_state == KernelState::BuiltIn ||
            result.kernel_state == KernelState::LoadableLoaded ||
            result.kernel_state == KernelState::LoadableUnloaded;
    }

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

    if (descriptor.project_native_linux &&
        !result.project_native_installed) {
        result.state = SupportState::Installable;
        result.detail =
            "The Infiltrator native kernel module is available to install "
            "for the running kernel.";
        if (!result.missing_packages.empty()) {
            result.detail += " Optional/administration packages are also available: " +
                             join_names(result.missing_packages) + ".";
        }
    } else if (descriptor.project_native_linux &&
               result.project_native_installed &&
               !result.project_native_selected) {
        result.state = SupportState::Incomplete;
        result.detail =
            "The Infiltrator native module is installed, but the kernel is "
            "selecting another implementation. Remove the conflict before "
            "claiming native support.";
    } else if (result.module_requirement_met && result.package_requirement_met) {
        result.state = SupportState::Ready;
        result.detail = descriptor.project_native_linux
            ? "The Infiltrator native kernel module is installed for this kernel."
            : "Support is installed on this Debian system.";
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
