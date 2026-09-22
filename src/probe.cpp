// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"

#include <glib.h>

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

bool module_available(const std::string_view module)
{
    if (module.empty()) {
        return false;
    }

    const auto result = run_command({
        "modinfo",
        "-n",
        std::string(module)
    });
    return result.exit_status == 0 && !result.output.empty();
}

ProbeResult probe(const FilesystemDescriptor& descriptor)
{
    ProbeResult result;

    result.module_requirement_met = descriptor.modules.empty();
    for (const auto module : descriptor.modules) {
        if (module_available(module)) {
            result.module_requirement_met = true;
            break;
        }
    }

    result.package_requirement_met = true;
    for (const auto package : descriptor.packages) {
        if (!package_installed(package)) {
            result.package_requirement_met = false;
            result.missing_packages.emplace_back(package);
        }
    }

    if (result.module_requirement_met && result.package_requirement_met) {
        result.state = SupportState::Ready;
        result.detail = "Support is installed on this system.";
    } else if (!result.missing_packages.empty()) {
        result.state = SupportState::Installable;
        result.detail = result.module_requirement_met
            ? "Kernel support is present; additional userspace tools can be installed."
            : "Required support is incomplete; install the distribution package(s) and re-check.";
    } else if (!result.module_requirement_met) {
        result.state = SupportState::Unavailable;
        result.detail = "The running kernel does not expose the required filesystem driver.";
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

} // namespace filesystem_support
