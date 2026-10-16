// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include "process_runner.hpp"

#include <glib.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace filesystems {
namespace {

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
    std::string kernel_release;
    std::string package_inventory_error;
    std::string module_inventory_error;
};

struct NativeModuleObservation {
    bool reliable = false;
    bool installed = false;
    bool current = false;
    bool selected = false;
    std::string error;
};

ProbeCache& probe_cache()
{
    /*
     * Probing is intentionally cache-heavy, but the public probe helpers are
     * not restricted to one caller thread. Keep each worker's inventory local
     * to that thread so a future concurrent scan cannot race cache mutation or
     * invalidate another worker's snapshot.
     */
    thread_local ProbeCache cache;
    return cache;
}

void append_error(std::string* target, const std::string& message)
{
    if (target == nullptr || message.empty()) {
        return;
    }
    if (!target->empty()) {
        *target += " ";
    }
    *target += message;
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

bool load_registered_filesystems(ProbeCache* cache)
{
    std::ifstream stream("/proc/filesystems");
    if (!stream.is_open()) {
        return false;
    }

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
    return !stream.bad();
}

bool load_module_file(const std::string& path,
                      const bool dependency_file,
                      std::unordered_set<std::string>* destination)
{
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return false;
    }

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
    return !stream.bad();
}

void load_module_inventory()
{
    ProbeCache& cache = probe_cache();
    if (cache.module_inventory_loaded) {
        return;
    }
    cache.module_inventory_loaded = true;

    if (!load_registered_filesystems(&cache)) {
        append_error(
            &cache.module_inventory_error,
            "Unable to read the running kernel filesystem registry.");
    }

    const std::string release = running_kernel_release();
    if (release.empty()) {
        append_error(
            &cache.module_inventory_error,
            "Unable to determine the running kernel release.");
        return;
    }

    const std::string base = "/lib/modules/" + release + "/";
    if (!load_module_file(
            base + "modules.builtin", false, &cache.builtin_modules)) {
        append_error(
            &cache.module_inventory_error,
            "Unable to read modules.builtin for the running kernel.");
    }
    if (!load_module_file(
            base + "modules.dep", true, &cache.loadable_modules)) {
        append_error(
            &cache.module_inventory_error,
            "Unable to read modules.dep for the running kernel.");
    }
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
        "-f=${Package}\t${db:Status-Status}\n"
    });

    std::string line;
    if (installed.succeeded()) {
        std::istringstream installed_stream(installed.stdout_text);
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
    } else {
        append_error(
            &cache.package_inventory_error,
            "Unable to read the installed Debian package inventory.");
    }

    const auto available = run_command({"apt-cache", "pkgnames"});
    if (available.succeeded()) {
        std::istringstream available_stream(available.stdout_text);
        while (std::getline(available_stream, line)) {
            line = trim_ascii_whitespace(std::move(line));
            if (!line.empty()) {
                cache.available_packages.insert(std::move(line));
            }
        }
    } else {
        append_error(
            &cache.package_inventory_error,
            "Unable to read the configured Debian repository inventory.");
    }
}

std::string native_state_helper_path()
{
    static const char* const candidates[] = {
        "/usr/lib/filesystems/native-module-helper",
        "/usr/local/lib/filesystems/native-module-helper"
    };

    for (const char* const candidate : candidates) {
        if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR) &&
            g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
            return candidate;
        }
    }
    return {};
}

NativeModuleObservation query_native_module_state(
    const std::string_view filesystem_id)
{
    NativeModuleObservation observation;
    const std::string helper = native_state_helper_path();
    if (helper.empty()) {
        observation.error =
            "The installed native-module state authority is unavailable.";
        return observation;
    }

    const auto status = run_command({
        helper,
        "status",
        std::string(filesystem_id)
    });
    if (!status.succeeded()) {
        observation.error = status.combined_output();
        if (observation.error.empty()) {
            observation.error = "Native-module state query failed.";
        }
        return observation;
    }

    std::istringstream stream(trim_ascii_whitespace(status.stdout_text));
    std::string presence;
    std::string freshness;
    std::string selection;
    std::string extra;
    if (!std::getline(stream, presence, '\t') ||
        !std::getline(stream, freshness, '\t') ||
        !std::getline(stream, selection, '\t') ||
        std::getline(stream, extra, '\t')) {
        observation.error = "Native-module state authority returned malformed data.";
        return observation;
    }

    if (presence == "absent") {
        if (freshness != "stale" || selection != "unselected") {
            observation.error = "Native-module absent state was inconsistent.";
            return observation;
        }
        observation.reliable = true;
        return observation;
    }

    if (presence != "installed" ||
        (freshness != "current" && freshness != "stale") ||
        (selection != "selected" && selection != "unselected")) {
        observation.error = "Native-module state authority returned unknown state.";
        return observation;
    }

    observation.reliable = true;
    observation.installed = true;
    observation.current = freshness == "current";
    observation.selected = selection == "selected";
    return observation;
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
     * pass. Retain the exact query as a fallback for virtual/edge package
     * names. A failed primary inventory is reported by probe() and therefore
     * can never become an actionable false absence.
     */
    const auto result = run_command({
        "apt-cache",
        "--no-all-versions",
        "show",
        name
    });
    const bool available =
        result.succeeded() && !result.stdout_text.empty();

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
        if (filename.succeeded() && !filename.stdout_text.empty()) {
            state = filename.stdout_text.find("builtin") != std::string::npos
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

ProbeResult probe(const FilesystemDescriptor& descriptor)
{
    ProbeResult result;
    NativeModuleObservation native_observation;

    result.project_native_module = descriptor.project_native_linux;
    result.module_requirement_met = descriptor.modules.empty();
    result.kernel_state = descriptor.modules.empty()
        ? KernelState::NotApplicable
        : KernelState::Missing;

    if (!descriptor.modules.empty()) {
        load_module_inventory();
    }

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
        native_observation = query_native_module_state(descriptor.id);
        if (descriptor.modules.size() == 1U) {
            result.module_name = std::string(descriptor.modules.front());
        }
        if (native_observation.reliable) {
            result.project_native_installed = native_observation.installed;
            result.project_native_current = native_observation.current;
            result.project_native_selected = native_observation.selected;
        }

        result.module_requirement_met =
            native_observation.reliable &&
            result.project_native_installed &&
            result.project_native_current &&
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
    if (!descriptor.packages.empty()) {
        load_package_inventory();
    }
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

    const ProbeCache& cache = probe_cache();
    const bool module_probe_failed =
        !descriptor.modules.empty() && !cache.module_inventory_error.empty();
    const bool package_probe_failed =
        !descriptor.packages.empty() && !cache.package_inventory_error.empty();
    const bool native_probe_failed =
        descriptor.project_native_linux && !native_observation.reliable;
    if (module_probe_failed || package_probe_failed || native_probe_failed) {
        result.reliable = false;
        result.state = SupportState::Incomplete;
        result.module_requirement_met = descriptor.modules.empty();
        result.package_requirement_met = descriptor.packages.empty();
        result.missing_packages.clear();
        result.unavailable_packages.clear();
        result.repository_packages_available = !package_probe_failed;
        result.detail = "Support state could not be verified.";
        if (module_probe_failed) {
            result.detail += " " + cache.module_inventory_error;
        }
        if (package_probe_failed) {
            result.detail += " " + cache.package_inventory_error;
        }
        if (native_probe_failed) {
            result.detail += " " + native_observation.error;
        }
        result.detail += " Actions are disabled until a successful refresh.";
        return result;
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
               !result.project_native_current) {
        result.state = SupportState::Incomplete;
        result.detail =
            "The installed native module was built from different packaged "
            "source. Update native support before treating it as current.";
    } else if (descriptor.project_native_linux &&
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
            ? "The current Infiltrator native kernel module is installed for this kernel."
            : "Support is ready on this Debian system.";
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
        return "Ready";
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

} // namespace filesystems
