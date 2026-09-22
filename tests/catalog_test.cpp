// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"

#include <iostream>
#include <string_view>

namespace {

int fail(const char* message)
{
    std::cerr << "catalog_test: " << message << '\n';
    return 1;
}

} // namespace

int main()
{
    using filesystem_support::catalog;
    using filesystem_support::catalog_is_valid;

    if (!catalog_is_valid()) {
        return fail("catalogue validation failed");
    }
    if (catalog().size() < 20U) {
        return fail("initial catalogue contains fewer than 20 filesystems");
    }

    bool found_affs = false;
    bool found_xfs = false;
    bool found_zfs = false;

    for (const auto& entry : catalog()) {
        if (entry.id == std::string_view("affs")) {
            found_affs = true;
            if (entry.name.find("Amiga") == std::string_view::npos) {
                return fail("AFFS entry is missing its Amiga identity");
            }
        }
        if (entry.id == std::string_view("xfs")) {
            found_xfs = true;
            if (entry.name.find("XFS") == std::string_view::npos) {
                return fail("XFS entry is missing its XFS identity");
            }
        }
        if (entry.id == std::string_view("zfs")) {
            found_zfs = true;
            if (entry.packages.empty()) {
                return fail("OpenZFS entry has no installation package");
            }
        }
    }

    if (!found_affs) {
        return fail("AFFS entry is missing");
    }
    if (!found_xfs) {
        return fail("XFS entry is missing");
    }
    if (!found_zfs) {
        return fail("OpenZFS entry is missing");
    }

    return 0;
}
