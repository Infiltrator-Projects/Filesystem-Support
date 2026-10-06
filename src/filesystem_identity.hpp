// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>
#include <vector>

namespace filesystem_support {

// Cross-platform filesystem identity.  This deliberately contains no Linux
// package names, kernel modules, provider classes, privilege policy or
// installation state.  Windows, EFI and setup consumers use this contract.
struct FilesystemIdentity {
    std::string_view id;
    std::string_view name;
    std::string_view family;
    std::string_view description;
    std::string_view note;
};

const std::vector<FilesystemIdentity>& filesystem_identities();

} // namespace filesystem_support
