// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glib.h>

#include <string>
#include <vector>

namespace filesystem_support {

struct CommandResult {
    int exit_status = -1;
    std::string stdout_text;
    std::string stderr_text;

    bool succeeded() const noexcept
    {
        return exit_status == 0;
    }

    std::string combined_output() const
    {
        if (stderr_text.empty()) {
            return stdout_text;
        }
        if (stdout_text.empty()) {
            return stderr_text;
        }
        return stdout_text + '\n' + stderr_text;
    }
};

// Single authority for synchronous child-process execution used by probing and
// package planning. Callers own policy; this utility owns only process launch,
// the stable C locale and output/status capture.
inline CommandResult run_command(const std::vector<std::string>& arguments)
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

    gchar** environment = g_get_environ();
    environment = g_environ_setenv(environment, "LC_ALL", "C", TRUE);

    gchar* stdout_text = nullptr;
    gchar* stderr_text = nullptr;
    gint wait_status = -1;
    GError* error = nullptr;

    const gboolean spawned = g_spawn_sync(
        nullptr,
        argv.data(),
        environment,
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
            result.stdout_text = stdout_text;
        }
        if (stderr_text != nullptr) {
            result.stderr_text = stderr_text;
        }
    }

    g_strfreev(environment);
    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
    return result;
}

} // namespace filesystem_support
