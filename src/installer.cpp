// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.hpp"

#include "catalog.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace filesystem_support {
namespace {

struct CommandResult {
    int exit_status = -1;
    std::string output;
};

struct AsyncJob {
    GSubprocess* process = nullptr;
    ActionCompletion completion;
    std::string success_message;
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
            result.output = stdout_text;
        }
        if (stderr_text != nullptr && result.exit_status != 0) {
            if (!result.output.empty()) {
                result.output += '\n';
            }
            result.output += stderr_text;
        }
    }

    g_strfreev(environment);
    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
    return result;
}

bool package_is_protected(const std::string& package, std::string* reason)
{
    const auto result = run_command({
        "dpkg-query",
        "-W",
        "-f=${Essential}\n${Protected}\n${Priority}\n",
        package
    });

    if (result.exit_status != 0) {
        if (reason != nullptr) {
            *reason = "Unable to inspect Debian protection metadata for " +
                      package + ".";
        }
        return true;
    }

    std::istringstream stream(result.output);
    std::string essential;
    std::string protected_field;
    std::string priority;
    std::getline(stream, essential);
    std::getline(stream, protected_field);
    std::getline(stream, priority);

    if (essential == "yes" || protected_field == "yes" ||
        priority == "required") {
        if (reason != nullptr) {
            *reason = package +
                      " is marked Essential, Protected, or required by Debian.";
        }
        return true;
    }

    return false;
}

std::vector<std::string> parse_removals(const std::string& output)
{
    std::vector<std::string> packages;
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.rfind("Remv ", 0U) != 0U) {
            continue;
        }

        std::istringstream parser(line.substr(5U));
        std::string package;
        parser >> package;
        if (!package.empty() &&
            std::find(packages.begin(), packages.end(), package) ==
                packages.end()) {
            packages.push_back(package);
        }
    }

    return packages;
}

void async_finished(GObject* source_object,
                    GAsyncResult* result,
                    gpointer user_data)
{
    std::unique_ptr<AsyncJob> job(static_cast<AsyncJob*>(user_data));
    GError* error = nullptr;
    const gboolean succeeded = g_subprocess_wait_check_finish(
        G_SUBPROCESS(source_object), result, &error);

    const std::string message = succeeded
        ? job->success_message
        : (error != nullptr ? error->message : "The requested operation failed.");

    if (job->completion) {
        job->completion(succeeded, message);
    }

    g_clear_error(&error);
    g_clear_object(&job->process);
}

void run_privileged_async(const std::vector<std::string>& arguments,
                          std::string success_message,
                          ActionCompletion completion)
{
    if (arguments.empty()) {
        completion(false, "No privileged command was supplied.");
        return;
    }

    gchar* pkexec = g_find_program_in_path("pkexec");
    gchar* executable = nullptr;
    if (g_path_is_absolute(arguments.front().c_str())) {
        if (g_file_test(arguments.front().c_str(), G_FILE_TEST_IS_REGULAR) &&
            g_file_test(arguments.front().c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
            executable = g_strdup(arguments.front().c_str());
        }
    } else {
        executable = g_find_program_in_path(arguments.front().c_str());
    }

    if (pkexec == nullptr || executable == nullptr) {
        g_free(pkexec);
        g_free(executable);
        completion(false, "pkexec or the required system command is unavailable.");
        return;
    }

    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1U);
    owned.emplace_back(pkexec);
    owned.emplace_back(executable);
    for (std::size_t index = 1U; index < arguments.size(); ++index) {
        owned.push_back(arguments[index]);
    }
    g_free(pkexec);
    g_free(executable);

    std::vector<const gchar*> argv;
    argv.reserve(owned.size() + 1U);
    for (const auto& value : owned) {
        argv.push_back(value.c_str());
    }
    argv.push_back(nullptr);

    GError* error = nullptr;
    GSubprocessLauncher* launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);

    GSubprocess* process = g_subprocess_launcher_spawnv(
        launcher, argv.data(), &error);
    g_object_unref(launcher);

    if (process == nullptr) {
        const std::string message =
            error != nullptr ? error->message : "Unable to start privileged operation.";
        g_clear_error(&error);
        completion(false, message);
        return;
    }

    auto* job = new AsyncJob;
    job->process = process;
    job->completion = std::move(completion);
    job->success_message = std::move(success_message);

    g_subprocess_wait_check_async(process, nullptr, async_finished, job);
}

bool packages_are_catalogued(const std::vector<std::string>& packages,
                             std::string* reason)
{
    if (packages.empty()) {
        if (reason != nullptr) {
            *reason = "No packages were supplied.";
        }
        return false;
    }

    for (const auto& package : packages) {
        if (!package_is_catalogued(package)) {
            if (reason != nullptr) {
                *reason = "Refusing non-catalogued package: " + package + ".";
            }
            return false;
        }
    }

    return true;
}

std::string native_module_helper_path()
{
    static const char* const candidates[] = {
        "/usr/lib/infiltrator-filesystem-support/native-module-helper",
        "/usr/local/lib/infiltrator-filesystem-support/native-module-helper"
    };

    for (const char* const path : candidates) {
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
            g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) {
            return path;
        }
    }

    return {};
}

} // namespace

RemovalPlan plan_package_removal(const std::vector<std::string>& packages)
{
    RemovalPlan plan;

    if (!packages_are_catalogued(packages, &plan.reason)) {
        return plan;
    }

    std::vector<std::string> arguments = {
        "apt-get",
        "-s",
        "-o",
        "Debug::NoLocking=1",
        "remove"
    };
    arguments.insert(arguments.end(), packages.begin(), packages.end());

    const auto simulation = run_command(arguments);
    if (simulation.exit_status != 0) {
        plan.reason =
            "Debian could not safely simulate this removal.\n" +
            simulation.output;
        return plan;
    }

    plan.planned_packages = parse_removals(simulation.output);

    if (plan.planned_packages.empty()) {
        plan.allowed = true;
        plan.reason = "Nothing is currently installed for removal.";
        return plan;
    }

    std::unordered_set<std::string> affected_entries;
    for (const auto& package : plan.planned_packages) {
        if (!package_is_catalogued(package)) {
            plan.reason =
                "Removal would also remove unrelated Debian package " +
                package + "; refusing the operation.";
            return plan;
        }

        std::string protection_reason;
        if (package_is_protected(package, &protection_reason)) {
            plan.reason = std::move(protection_reason);
            return plan;
        }

        for (const auto id : catalogue_entries_using_package(package)) {
            affected_entries.emplace(id);
        }
    }

    plan.affected_entries.assign(
        affected_entries.begin(), affected_entries.end());
    std::sort(plan.affected_entries.begin(), plan.affected_entries.end());
    plan.allowed = true;
    plan.reason =
        "Debian simulation confirms that only catalogue-managed filesystem "
        "packages would be removed.";
    return plan;
}

void install_packages_async(const std::vector<std::string>& packages,
                            ActionCompletion completion)
{
    std::string reason;
    if (!packages_are_catalogued(packages, &reason)) {
        completion(false, reason);
        return;
    }

    std::vector<std::string> arguments = {
        "apt-get",
        "install",
        "-y"
    };
    arguments.insert(arguments.end(), packages.begin(), packages.end());

    run_privileged_async(
        arguments,
        "Installation completed successfully.",
        std::move(completion));
}

void remove_packages_async(const std::vector<std::string>& packages,
                           ActionCompletion completion)
{
    const RemovalPlan plan = plan_package_removal(packages);
    if (!plan.allowed) {
        completion(false, plan.reason);
        return;
    }

    if (plan.planned_packages.empty()) {
        completion(true, "Nothing is currently installed for removal.");
        return;
    }

    std::vector<std::string> arguments = {
        "apt-get",
        "remove",
        "-y"
    };
    arguments.insert(arguments.end(), packages.begin(), packages.end());

    run_privileged_async(
        arguments,
        "Removal completed successfully.",
        std::move(completion));
}

void load_module_async(const std::string& module,
                       ActionCompletion completion)
{
    if (!module_is_catalogued(module)) {
        completion(false, "Refusing non-catalogued kernel module.");
        return;
    }

    run_privileged_async(
        {"modprobe", module},
        "Kernel module loaded successfully.",
        std::move(completion));
}

void unload_module_async(const std::string& module,
                         ActionCompletion completion)
{
    if (!module_is_catalogued(module)) {
        completion(false, "Refusing non-catalogued kernel module.");
        return;
    }

    run_privileged_async(
        {"modprobe", "-r", module},
        "Kernel module unloaded successfully.",
        std::move(completion));
}

void install_native_module_async(const std::string& filesystem_id,
                                 const std::string& module,
                                 ActionCompletion completion)
{
    if (!linux_native_module_is_managed(filesystem_id, module)) {
        completion(false, "Refusing unmanaged native filesystem module.");
        return;
    }

    const std::string helper = native_module_helper_path();
    if (helper.empty()) {
        completion(
            false,
            "The Filesystem Support native-module helper is not installed. "
            "Reinstall Filesystem Support from the current package.");
        return;
    }

    run_privileged_async(
        {helper, "install", filesystem_id, module},
        "Native kernel module installed and loaded successfully.",
        std::move(completion));
}

void remove_native_module_async(const std::string& filesystem_id,
                                const std::string& module,
                                ActionCompletion completion)
{
    if (!linux_native_module_is_managed(filesystem_id, module)) {
        completion(false, "Refusing unmanaged native filesystem module.");
        return;
    }

    const std::string helper = native_module_helper_path();
    if (helper.empty()) {
        completion(
            false,
            "The Filesystem Support native-module helper is not installed. "
            "Reinstall Filesystem Support from the current package.");
        return;
    }

    run_privileged_async(
        {helper, "remove", filesystem_id, module},
        "Native kernel module removed successfully.",
        std::move(completion));
}

} // namespace filesystem_support
