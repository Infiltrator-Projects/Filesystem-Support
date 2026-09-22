// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.hpp"

#include <gio/gio.h>

#include <memory>
#include <utility>
#include <vector>

namespace filesystem_support {
namespace {

struct InstallJob {
    GSubprocess* process = nullptr;
    InstallCompletion completion;
};

void install_finished(GObject* source_object,
                      GAsyncResult* result,
                      gpointer user_data)
{
    std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(user_data));
    GError* error = nullptr;
    const gboolean succeeded = g_subprocess_wait_check_finish(
        G_SUBPROCESS(source_object), result, &error);

    const std::string message = succeeded
        ? "Installation completed successfully."
        : (error != nullptr ? error->message : "Package installation failed.");

    if (job->completion) {
        job->completion(succeeded, message);
    }

    g_clear_error(&error);
    g_clear_object(&job->process);
}

} // namespace

void install_packages_async(const std::vector<std::string>& packages,
                            InstallCompletion completion)
{
    if (packages.empty()) {
        completion(true, "Nothing needs to be installed.");
        return;
    }

    gchar* pkexec = g_find_program_in_path("pkexec");
    gchar* apt_get = g_find_program_in_path("apt-get");
    if (pkexec == nullptr || apt_get == nullptr) {
        g_free(pkexec);
        g_free(apt_get);
        completion(false, "pkexec or apt-get is not available.");
        return;
    }

    std::vector<std::string> owned;
    owned.reserve(packages.size() + 4U);
    owned.emplace_back(pkexec);
    owned.emplace_back(apt_get);
    owned.emplace_back("install");
    owned.emplace_back("-y");
    for (const auto& package : packages) {
        owned.push_back(package);
    }
    g_free(pkexec);
    g_free(apt_get);

    std::vector<const gchar*> argv;
    argv.reserve(owned.size() + 1U);
    for (const auto& value : owned) {
        argv.push_back(value.c_str());
    }
    argv.push_back(nullptr);

    GError* error = nullptr;
    GSubprocess* process = g_subprocess_newv(
        argv.data(), G_SUBPROCESS_FLAGS_NONE, &error);

    if (process == nullptr) {
        const std::string message =
            error != nullptr ? error->message : "Unable to start package installation.";
        g_clear_error(&error);
        completion(false, message);
        return;
    }

    auto* job = new InstallJob;
    job->process = process;
    job->completion = std::move(completion);
    g_subprocess_wait_check_async(process, nullptr, install_finished, job);
}

} // namespace filesystem_support
