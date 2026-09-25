// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"

#include <glib.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace filesystem_support {
namespace {

struct CommandResult {
    int exit_status = -1;
    std::string output;
};

struct ProbeCache {
    bool package_inventory_loaded = false;
    bool module_inventory_loaded = false;
    std::unordered_set<std::string> installed_packages;
    std::unordered_set<std::string> available_packages;
    std::unordered_set<std::string> unavailable_packages_checked;
    std::unordered_set<std::string> registered_filesystems;
    std::unordered_set<std::string> builtin_modules;
    std::unordered_set<std::string> loadable_modules;
    std::unordered_map<std::string, KernelState> module_states;
    std::unordered_map<std::string, std::string> preferred_module_files;
    std::string kernel_release;
};

ProbeCache& probe_cache()
{
    static ProbeCache cache;
    return cache;
}

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
    ProbeCache& cache = probe_cache();
    if (!cache.kernel_release.empty()) {
        return cache.kernel_release;
    }

    struct utsname identity {};
    if (uname(&identity) == 0) {
        cache.kernel_release = identity.release;
    }
    return cache.kernel_release;
}

std::string normalize_module_name(std::string name)
{
    std::replace(name.begin(), name.end(), '-', '_');
    return name;
}

std::string module_name_from_path(std::string path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        path.erase(0U, slash + 1U);
    }

    for (const char* const suffix : {".zst", ".xz", ".gz"}) {
        const std::size_t length = std::char_traits<char>::length(suffix);
        if (path.size() >= length &&
            path.compare(path.size() - length, length, suffix) == 0) {
            path.erase(path.size() - length);
            break;
        }
    }

    constexpr const char* module_suffix = ".ko";
    constexpr std::size_t module_suffix_length = 3U;
    if (path.size() >= module_suffix_length &&
        path.compare(
            path.size() - module_suffix_length,
            module_suffix_length,
            module_suffix) == 0) {
        path.erase(path.size() - module_suffix_length);
    }

    return normalize_module_name(path);
}

void load_registered_filesystems(ProbeCache* cache)
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

        if (!last.empty()) {
            cache->registered_filesystems.insert(
                normalize_module_name(last));
        }
    }
}

void load_module_file(const std::string& path,
                      const bool dependency_file,
                      std::unordered_set<std::string>* destination)
{
    std::ifstream stream(path);
    std::string line;

    while (std::getline(stream, line)) {
        if (dependency_file) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                line.erase(colon);
            }
        }

        const std::string module = module_name_from_path(line);
        if (!module.empty()) {
            destination->insert(module);
        }
    }
}

void load_module_inventory()
{
    ProbeCache& cache = probe_cache();
    if (cache.module_inventory_loaded) {
        return;
    }
    cache.module_inventory_loaded = true;

    load_registered_filesystems(&cache);

    const std::string release = running_kernel_release();
    if (release.empty()) {
        return;
    }

    const std::string base = "/lib/modules/" + release + "/";
    load_module_file(
        base + "modules.builtin", false, &cache.builtin_modules);
    load_module_file(
        base + "modules.dep", true, &cache.loadable_modules);
}

bool module_loaded_now(const std::string_view module)
{
    load_module_inventory();
    const std::string normalized =
        normalize_module_name(std::string(module));
    const ProbeCache& cache = probe_cache();

    if (cache.registered_filesystems.find(normalized) !=
        cache.registered_filesystems.end()) {
        return true;
    }

    const std::string sys_module = "/sys/module/" + normalized;
    return g_file_test(sys_module.c_str(), G_FILE_TEST_IS_DIR);
}

void load_package_inventory()
{
    ProbeCache& cache = probe_cache();
    if (cache.package_inventory_loaded) {
        return;
    }
    cache.package_inventory_loaded = true;

    const auto installed = run_command({
        "dpkg-query",
        "-W",
        "-f=\${Package}\t\${db:Status-Status}\n"
    });

    std::istringstream installed_stream(installed.output);
    std::string line;
    while (std::getline(installed_stream, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }

        std::string package = line.substr(0U, tab);
        std::string status = trim_ascii_whitespace(line.substr(tab + 1U));
        if (!package.empty() && status == "installed") {
            cache.installed_packages.insert(std::move(package));
        }
    }

    const auto available = run_command({"apt-cache", "pkgnames"});
    std::istringstream available_stream(available.output);
    while (std::getline(available_stream, line)) {
        line = trim_ascii_whitespace(std::move(line));
        if (!line.empty()) {
            cache.available_packages.insert(std::move(line));
        }
    }
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
    ProbeCache& cache = probe_cache();
    const std::string name(module);
    const auto found = cache.preferred_module_files.find(name);
    if (found != cache.preferred_module_files.end()) {
        return found->second;
    }

    const auto result = run_command({
        "modinfo",
        "-F",
        "filename",
        name
    });
    const std::string filename =
        result.exit_status == 0
            ? trim_ascii_whitespace(result.output)
            : std::string();
    cache.preferred_module_files.emplace(name, filename);
    return filename;
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

void invalidate_probe_cache()
{
    probe_cache() = ProbeCache {};
}

bool package_installed(const std::string_view package)
{
    if (package.empty()) {
        return false;
    }

    load_package_inventory();
    const ProbeCache& cache = probe_cache();
    return cache.installed_packages.find(std::string(package)) !=
           cache.installed_packages.end();
}

bool package_available(const std::string_view package)
{
    if (package.empty()) {
        return false;
    }

    load_package_inventory();
    ProbeCache& cache = probe_cache();
    const std::string name(package);

    if (cache.available_packages.find(name) !=
        cache.available_packages.end()) {
        return true;
    }
    if (cache.unavailable_packages_checked.find(name) !=
        cache.unavailable_packages_checked.end()) {
        return false;
    }

    /*
     * apt-cache pkgnames covers normal binary packages in one fast inventory
     * pass. Retain the old exact query as a fallback for virtual/edge package
     * names instead of spawning it once for every catalogue row.
     */
    const auto result = run_command({
        "apt-cache",
        "--no-all-versions",
        "show",
        name
    });
    const bool available =
        result.exit_status == 0 && !result.output.empty();

    if (available) {
        cache.available_packages.insert(name);
    } else {
        cache.unavailable_packages_checked.insert(name);
    }
    return available;
}

KernelState module_state(const std::string_view module)
{
    if (module.empty()) {
        return KernelState::Missing;
    }

    load_module_inventory();
    ProbeCache& cache = probe_cache();
    const std::string normalized =
        normalize_module_name(std::string(module));

    const auto cached = cache.module_states.find(normalized);
    if (cached != cache.module_states.end()) {
        return cached->second;
    }

    KernelState state = KernelState::Missing;
    if (cache.builtin_modules.find(normalized) !=
        cache.builtin_modules.end()) {
        state = KernelState::BuiltIn;
    } else if (cache.loadable_modules.find(normalized) !=
               cache.loadable_modules.end()) {
        state = module_loaded_now(normalized)
            ? KernelState::LoadableLoaded
            : KernelState::LoadableUnloaded;
    } else if (module_loaded_now(normalized)) {
        state = KernelState::BuiltIn;
    } else {
        /*
         * modules.dep/modules.builtin are authoritative for ordinary module
         * names and avoid hundreds of modinfo processes at startup. Keep one
         * cached modinfo fallback for aliases and unusual packaging layouts.
         */
        const auto filename = run_command({
            "modinfo",
            "-F",
            "filename",
            std::string(module)
        });
        if (filename.exit_status == 0 && !filename.output.empty()) {
            state = filename.output.find("builtin") != std::string::npos
                ? KernelState::BuiltIn
                : KernelState::LoadableUnloaded;
        }
    }

    cache.module_states.emplace(normalized, state);
    return state;
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
            result.detail +=
                " Optional/administration packages are also available: " +
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
    } else if (result.module_requirement_met &&
               result.package_requirement_met) {
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
