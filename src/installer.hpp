// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace filesystem_support {

using ActionCompletion = std::function<void(bool, const std::string&)>;

struct RemovalPlan {
    bool allowed = false;
    std::vector<std::string> planned_packages;
    std::vector<std::string> affected_entries;
    std::string reason;
};

RemovalPlan plan_package_removal(const std::vector<std::string>& packages);

void install_packages_async(const std::vector<std::string>& packages,
                            ActionCompletion completion);
void remove_packages_async(const std::vector<std::string>& packages,
                           ActionCompletion completion);
void load_module_async(const std::string& module,
                       ActionCompletion completion);
void unload_module_async(const std::string& module,
                         ActionCompletion completion);
void install_native_module_async(const std::string& filesystem_id,
                                 const std::string& module,
                                 ActionCompletion completion);
void remove_native_module_async(const std::string& filesystem_id,
                                const std::string& module,
                                ActionCompletion completion);

} // namespace filesystem_support
