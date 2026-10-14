// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glib.h>

#include <array>
#include <string>
#include <sys/wait.h>
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

// Resolve observation/planning tools independently of the caller's PATH. The
// process runner is used to manufacture support state and destructive package
// plans; allowing an unprivileged environment to substitute dpkg-query,
// apt-cache, modinfo or another helper would make those observations less
// trustworthy than the privileged execution path that later consumes them.
// Absolute command identities remain exact. Bare system-tool names are resolved
// only through the conventional immutable system locations below; callers that
// own an application helper already pass its validated absolute path.
inline std::string trusted_command_path(const std::string& command)
{
    if (command.empty()) {
        return {};
    }

    if (g_path_is_absolute(command.c_str())) {
        return g_file_test(command.c_str(), G_FILE_TEST_IS_REGULAR) &&
               g_file_test(command.c_str(), G_FILE_TEST_IS_EXECUTABLE)
            ? command
            : std::string();
    }

    if (command.find('/') != std::string::npos) {
        return {};
    }

    static constexpr std::array<const char*, 4> kTrustedSystemDirectories = {
        "/usr/bin", "/usr/sbin", "/bin", "/sbin"
    };
    for (const char* const directory : kTrustedSystemDirectories) {
        const std::string candidate =
            std::string(directory) + "/" + command;
        if (g_file_test(candidate.c_str(), G_FILE_TEST_IS_REGULAR) &&
            g_file_test(candidate.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
            return candidate;
        }
    }
    return {};
}

// Single authority for synchronous child-process execution used by probing and
// package planning. Callers own command/argument policy; this utility owns only
// trusted process resolution, launch, the stable C locale and output/status
// capture. Spawn and wait failures retain a diagnostic so callers do not
// collapse an execution failure into an empty generic error. Exact normal exit
// codes are preserved for callers that distinguish an expected tool result
// from an execution failure.
inline CommandResult run_command(const std::vector<std::string>& arguments)
{
    CommandResult result;
    if (arguments.empty()) {
        result.stderr_text = "No command was supplied.";
        return result;
    }

    const std::string program = trusted_command_path(arguments.front());
    if (program.empty()) {
        result.stderr_text =
            "Executable not found in trusted system locations: " +
            arguments.front();
        return result;
    }

    std::vector<std::string> owned = arguments;
    owned.front() = program;

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
        if (WIFEXITED(wait_status)) {
            result.exit_status = WEXITSTATUS(wait_status);
        }

        if (result.exit_status != 0) {
            GError* wait_error = nullptr;
            (void)g_spawn_check_wait_status(wait_status, &wait_error);
            if (result.stderr_text.empty() && wait_error != nullptr) {
                result.stderr_text = wait_error->message;
            }
            g_clear_error(&wait_error);
        }
    }

    g_strfreev(environment);
    g_clear_error(&spawn_error);
    g_free(stdout_text);
    g_free(stderr_text);
    return result;
}

} // namespace filesystem_support
