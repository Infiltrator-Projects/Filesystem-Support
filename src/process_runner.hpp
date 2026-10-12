// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glib.h>

#include <string>
#include <vector>

namespace filesystem_support {

struct CommandResult {
    bool spawned = false;
    int exit_status = -1;
    int wait_status = -1;
    std::string stdout_text;
    std::string stderr_text;

    bool succeeded() const noexcept
    {
        return spawned && exit_status == 0;
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
// the stable C locale and output/status capture. Spawn and wait failures retain
// a diagnostic so callers do not collapse an execution failure into an empty
// generic error.
inline CommandResult run_command(const std::vector<std::string>& arguments)
{
    CommandResult result;
    if (arguments.empty()) {
        result.stderr_text = "No command was supplied.";
        return result;
    }

    gchar* program = g_find_program_in_path(arguments.front().c_str());
    if (program == nullptr) {
        result.stderr_text =
            "Executable not found in PATH: " + arguments.front();
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
    GError* spawn_error = nullptr;

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
        &spawn_error);

    result.spawned = spawned != FALSE;
    result.wait_status = wait_status;
    if (stdout_text != nullptr) {
        result.stdout_text = stdout_text;
    }
    if (stderr_text != nullptr) {
        result.stderr_text = stderr_text;
    }

    if (!result.spawned) {
        if (result.stderr_text.empty() && spawn_error != nullptr) {
            result.stderr_text = spawn_error->message;
        }
    } else {
        GError* wait_error = nullptr;
        if (g_spawn_check_wait_status(wait_status, &wait_error)) {
            result.exit_status = 0;
        } else {
            result.exit_status = 1;
            if (result.stderr_text.empty() && wait_error != nullptr) {
                result.stderr_text = wait_error->message;
            }
        }
        g_clear_error(&wait_error);
    }

    g_strfreev(environment);
    g_clear_error(&spawn_error);
    g_free(stdout_text);
    g_free(stderr_text);
    return result;
}

} // namespace filesystem_support
