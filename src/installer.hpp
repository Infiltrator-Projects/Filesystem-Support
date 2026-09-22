// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace filesystem_support {

using InstallCompletion = std::function<void(bool, const std::string&)>;

void install_packages_async(const std::vector<std::string>& packages,
                            InstallCompletion completion);

} // namespace filesystem_support
