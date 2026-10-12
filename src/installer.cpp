// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.hpp"

#include "catalog.hpp"
#include "process_runner.hpp"

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

struct AsyncJob {
    GSubprocess* process = nullptr;
    ActionCompletion completion;
    std::string success_message;
};

struct RemovalPlanJob {
    std::vector<std::string> packages;
    RemovalPlanCompletion completion;
};

bool package_is_protected(const std::string& package, std::string* reason)
{
    const auto result = run_command({
        "dpkg-query",
        "-W",
        "-f=${Essential}\n${Protected}\n${Priority}\n",
        package
    });

    if (!result.succeeded()) {
        if (reason != nullptr) {
            *reason = "Unable to inspect Debian protection metadata for " +
                      package + ".";
        }
        return true;
    }

    std::istringstream stream(result.stdout_text);
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

std::string diagnostic_output(const gchar* stdout_text,
                              const gchar* stderr_text)
{
    std::string output;

    if (stderr_text != nullptr && *stderr_text != '\0') {
        output = stderr_text;
    }
    if (stdout_text != nullptr && *stdout_text != '\0') {
        if (!output.empty()) {
            output += '\n';
        }
        output += stdout_text;
    }

    /*
     * Compiler output can be large. Keep the end of the diagnostic because
     * make/modprobe/signing errors and the helper's rollback reason are
     * emitted last.
     */
    constexpr std::size_t kMaximumDiagnosticBytes = 12000U;
    if (output.size() > kMaximumDiagnosticBytes) {
        output =
            "[Earlier build output omitted]\n...\n" +
            output.substr(output.size() - kMaximumDiagnosticBytes);
    }

    return output;
}

void async_finished(GObject* source_object,
                    GAsyncResult* result,
                    gpointer user_data)
{
    std::unique_ptr<AsyncJob> job(static_cast<AsyncJob*>(user_data));
    auto* process = G_SUBPROCESS(source_object);
    gchar* stdout_text = nullptr;
    gchar* stderr_text = nullptr;
    GError* error = nullptr;

    const gboolean communicated = g_subprocess_communicate_utf8_finish(
        process,
        result,
        &stdout_text,
        &stderr_text,
        &error);

    const gboolean succeeded =
        communicated && g_subprocess_get_successful(process);

    std::string message;
    if (succeeded) {
        message = job->success_message;
    } else {
        message = diagnostic_output(stdout_text, stderr_text);
        if (message.empty() && error != nullptr) {
            message = error->message;
        }
        if (message.empty()) {
            if (g_subprocess_get_if_exited(process)) {
                message =
                    "Privileged helper exited with code " +
                    std::to_string(g_subprocess_get_exit_status(process)) + ".";
            } else if (g_subprocess_get_if_signaled(process)) {
                message =
                    "Privileged helper was terminated by signal " +
                    std::to_string(g_subprocess_get_term_sig(process)) + ".";
            } else {
                message = "The requested operation failed.";
            }
        }
    }

    if (job->completion) {
        job->completion(succeeded, message);
    }

    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
    g_clear_object(&job->process);
}

bool executable_file(const char* path)
{
    return path != nullptr &&
           g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
           g_file_test(path, G_FILE_TEST_IS_EXECUTABLE);
}

std::string trusted_privileged_executable(const std::string& requested)
{
    if (requested == "apt-get") {
        return executable_file("/usr/bin/apt-get")
            ? "/usr/bin/apt-get"
            : std::string();
    }

    if (requested == "modprobe") {
        for (const char* const candidate : {
                 "/usr/sbin/modprobe", "/sbin/modprobe"}) {
            if (executable_file(candidate)) {
                return candidate;
            }
        }
        return {};
    }

    /* Absolute helpers are selected by fixed application policy before this
     * point (for example native_module_helper_path()). Never search PATH for a
     * privileged executable supplied by the unprivileged process environment. */
    if (g_path_is_absolute(requested.c_str()) &&
        executable_file(requested.c_str())) {
        return requested;
    }

    return {};
}

void run_privileged_async(const std::vector<std::string>& arguments,
                          std::string success_message,
                          ActionCompletion completion)
{
    if (arguments.empty()) {
        completion(false, "No privileged command was supplied.");
        return;
    }

    constexpr const char* kPkexec = "/usr/bin/pkexec";
    const std::string executable =
        trusted_privileged_executable(arguments.front());

    if (!executable_file(kPkexec) || executable.empty()) {
        completion(
            false,
            "The trusted privileged helper or required system command is unavailable.");
        return;
    }

    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1U);
    owned.emplace_back(kPkexec);
    owned.emplace_back(executable);
    for (std::size_t index = 1U; index < arguments.size(); ++index) {
        owned.push_back(arguments[index]);
    }

    std::vector<const gchar*> argv;
    argv.reserve(owned.size() + 1U);
    for (const auto& value : owned) {
        argv.push_back(value.c_str());
    }
    argv.push_back(nullptr);

    GError* error = nullptr;
    GSubprocessLauncher* launcher =
        g_subprocess_launcher_new(
            static_cast<GSubprocessFlags>(
                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                G_SUBPROCESS_FLAGS_STDERR_PIPE));
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

    g_subprocess_communicate_utf8_async(
        process,
        nullptr,
        nullptr,
        async_finished,
        job);
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
        if (executable_file(path)) {
            return path;
        }
    }

    return {};
}

void destroy_removal_plan(gpointer data)
{
    delete static_cast<RemovalPlan*>(data);
}

void removal_plan_job_destroy(gpointer data)
{
    delete static_cast<RemovalPlanJob*>(data);
}

void removal_plan_worker(GTask* task,
                         gpointer,
                         gpointer task_data,
                         GCancellable*)
{
    auto* job = static_cast<RemovalPlanJob*>(task_data);
    auto* plan = new RemovalPlan(plan_package_removal(job->packages));
    g_task_return_pointer(task, plan, destroy_removal_plan);
}

void removal_plan_finished(GObject*, GAsyncResult* result, gpointer)
{
    auto* task = G_TASK(result);
    auto* job = static_cast<RemovalPlanJob*>(g_task_get_task_data(task));
    GError* error = nullptr;
    std::unique_ptr<RemovalPlan> plan(
        static_cast<RemovalPlan*>(g_task_propagate_pointer(task, &error)));

    if (job != nullptr && job->completion) {
        if (plan != nullptr) {
            job->completion(std::move(*plan));
        } else {
            RemovalPlan failed;
            failed.reason = error != nullptr && error->message != nullptr
                ? error->message
                : "Debian removal planning did not complete.";
            job->completion(std::move(failed));
        }
    }

    g_clear_error(&error);
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
    if (!simulation.succeeded()) {
        plan.reason =
            "Debian could not safely simulate this removal.\n" +
            simulation.combined_output();
        return plan;
    }

    plan.planned_packages = parse_removals(simulation.stdout_text);

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

void plan_package_removal_async(const std::vector<std::string>& packages,
                                RemovalPlanCompletion completion)
{
    if (!completion) {
        return;
    }

    GTask* task = g_task_new(nullptr, nullptr, removal_plan_finished, nullptr);
    auto* job = new RemovalPlanJob {packages, std::move(completion)};
    g_task_set_task_data(task, job, removal_plan_job_destroy);
    g_task_run_in_thread(task, removal_plan_worker);
    g_object_unref(task);
}

bool removal_plan_matches_approval(
    const std::vector<std::string>& approved_packages,
    const std::vector<std::string>& current_packages)
{
    std::vector<std::string> approved = approved_packages;
    std::vector<std::string> current = current_packages;
    std::sort(approved.begin(), approved.end());
    std::sort(current.begin(), current.end());
    return approved == current;
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

void remove_packages_async(
    const std::vector<std::string>& packages,
    const std::vector<std::string>& approved_planned_packages,
    ActionCompletion completion)
{
    plan_package_removal_async(
        packages,
        [packages,
         approved_planned_packages,
         completion = std::move(completion)](RemovalPlan plan) mutable {
            if (!plan.allowed) {
                completion(false, plan.reason);
                return;
            }

            if (!removal_plan_matches_approval(
                    approved_planned_packages, plan.planned_packages)) {
                completion(
                    false,
                    "The Debian removal plan changed after confirmation. "
                    "Nothing was removed; review and confirm the new plan.");
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
        });
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
