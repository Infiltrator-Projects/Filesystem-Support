// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include "filesystem_identity.hpp"
#include "linux_support_policy.hpp"
#include "native_filesystems.generated.hpp"

#include <infiltratr/core.h>

#include <unordered_set>
#include <utility>

namespace filesystems {
namespace {

bool safe_catalog_token(const std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    for (const unsigned char character : value) {
        if (infiltratr_ascii_is_alnum(character) ||
            character == '-' || character == '_' ||
            character == '.' || character == '+') {
            continue;
        }
        return false;
    }
    return true;
}

const NativeFilesystemPolicy* native_policy_for(const std::string_view id)
{
    for (const auto& policy : kNativeFilesystemPolicies) {
        if (policy.filesystem_id == id) {
            return &policy;
        }
    }
    return nullptr;
}

const LinuxSupportPolicy* linux_policy_for(const std::string_view id)
{
    for (const auto& policy : linux_support_policies()) {
        if (policy.filesystem_id == id) {
            return &policy;
        }
    }
    return nullptr;
}

bool provider_shape_is_valid(const FilesystemDescriptor& entry)
{
    switch (entry.provider) {
    case SupportProvider::Kernel:
        return !entry.modules.empty() && entry.packages.empty();
    case SupportProvider::KernelWithUserspace:
    case SupportProvider::Dkms:
        return !entry.modules.empty() && !entry.packages.empty();
    case SupportProvider::Userspace:
        return entry.modules.empty() && !entry.packages.empty();
    case SupportProvider::ToolsOnly:
        return entry.modules.empty() && !entry.packages.empty() &&
               entry.access == AccessMode::ToolsOnly;
    }
    return false;
}

} // namespace

const std::vector<FilesystemDescriptor>& catalog()
{
    // Linux support is a join between stable format identity and Linux-only
    // deployment/access policy. Package or module churn therefore cannot alter
    // the cross-platform identity contract.
    static const std::vector<FilesystemDescriptor> entries = [] {
        std::vector<FilesystemDescriptor> values;
        values.reserve(filesystem_identities().size());

        for (const auto& identity : filesystem_identities()) {
            const LinuxSupportPolicy* policy = linux_policy_for(identity.id);
            if (policy == nullptr) {
                return std::vector<FilesystemDescriptor> {};
            }

            FilesystemDescriptor entry {
                identity.id,
                identity.name,
                identity.family,
                identity.description,
                policy->modules,
                policy->packages,
                policy->access,
                policy->provider,
                identity.note
            };

            const NativeFilesystemPolicy* native = native_policy_for(identity.id);
            if (native != nullptr) {
                entry.modules = {native->module};
                entry.project_native_linux = true;
            }
            values.push_back(std::move(entry));
        }
        return values;
    }();

    return entries;
}

const char* access_mode_label(const AccessMode mode)
{
    switch (mode) {
    case AccessMode::ReadWrite: return "Read / write";
    case AccessMode::ReadOnly: return "Read-only";
    case AccessMode::Mixed: return "Variant / kernel dependent";
    case AccessMode::Userspace: return "Userspace / FUSE";
    case AccessMode::ToolsOnly: return "Userspace tools";
    case AccessMode::Experimental: return "Experimental";
    }
    return "Unknown";
}

const char* support_provider_label(const SupportProvider provider)
{
    switch (provider) {
    case SupportProvider::Kernel: return "Kernel";
    case SupportProvider::KernelWithUserspace: return "Kernel + userspace";
    case SupportProvider::Dkms: return "DKMS driver";
    case SupportProvider::Userspace: return "Userspace / FUSE";
    case SupportProvider::ToolsOnly: return "Tools only";
    }
    return "Unknown";
}

bool package_is_catalogued(const std::string_view package)
{
    if (package.empty()) {
        return false;
    }
    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.packages) {
            if (candidate == package) {
                return true;
            }
        }
    }
    return false;
}

bool module_is_catalogued(const std::string_view module)
{
    if (module.empty()) {
        return false;
    }
    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.modules) {
            if (candidate == module) {
                return true;
            }
        }
    }
    return false;
}

bool linux_native_module_is_managed(const std::string_view filesystem_id,
                                    const std::string_view module)
{
    if (filesystem_id.empty() || module.empty()) {
        return false;
    }
    for (const auto& policy : kNativeFilesystemPolicies) {
        if (policy.filesystem_id == filesystem_id && policy.module == module) {
            return true;
        }
    }
    return false;
}

std::vector<std::string_view> catalogue_entries_using_package(
    const std::string_view package)
{
    std::vector<std::string_view> entries;
    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.packages) {
            if (candidate == package) {
                entries.push_back(entry.id);
                break;
            }
        }
    }
    return entries;
}

bool catalog_is_valid()
{
    const auto& identities = filesystem_identities();
    const auto& policies = linux_support_policies();
    if (catalog().size() != identities.size() || policies.size() != identities.size()) {
        return false;
    }

    std::unordered_set<std::string_view> identity_ids;
    for (const auto& identity : identities) {
        if (identity.id.empty() || identity.name.empty() ||
            identity.family.empty() || identity.description.empty() ||
            !safe_catalog_token(identity.id) ||
            !identity_ids.insert(identity.id).second) {
            return false;
        }
    }

    std::unordered_set<std::string_view> policy_ids;
    for (const auto& policy : policies) {
        if (!safe_catalog_token(policy.filesystem_id) ||
            identity_ids.find(policy.filesystem_id) == identity_ids.end() ||
            !policy_ids.insert(policy.filesystem_id).second) {
            return false;
        }
        for (const auto package : policy.packages) {
            if (!safe_catalog_token(package)) {
                return false;
            }
        }
        for (const auto module : policy.modules) {
            if (!safe_catalog_token(module)) {
                return false;
            }
        }
    }

    std::unordered_set<std::string_view> ids;
    std::size_t native_entries = 0U;
    for (const auto& entry : catalog()) {
        if (!ids.insert(entry.id).second || !provider_shape_is_valid(entry)) {
            return false;
        }
        if (entry.project_native_linux) {
            ++native_entries;
            const NativeFilesystemPolicy* policy = native_policy_for(entry.id);
            if (policy == nullptr || entry.modules.size() != 1U ||
                entry.modules.front() != policy->module ||
                (entry.provider != SupportProvider::Kernel &&
                 entry.provider != SupportProvider::KernelWithUserspace)) {
                return false;
            }
        }
        for (const auto package : entry.packages) {
            if (!safe_catalog_token(package)) {
                return false;
            }
        }
        for (const auto module : entry.modules) {
            if (!safe_catalog_token(module)) {
                return false;
            }
        }
    }

    if (native_entries != kNativeFilesystemPolicies.size()) {
        return false;
    }
    for (const auto& policy : kNativeFilesystemPolicies) {
        if (!safe_catalog_token(policy.filesystem_id) ||
            !safe_catalog_token(policy.module) ||
            ids.find(policy.filesystem_id) == ids.end()) {
            return false;
        }
    }
    return true;
}

} // namespace filesystems
