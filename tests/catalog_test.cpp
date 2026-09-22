// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"

#include <cassert>
#include <string_view>

int main()
{
    using filesystem_support::catalog;
    using filesystem_support::catalog_is_valid;

    assert(catalog_is_valid());
    assert(catalog().size() >= 20U);

    bool found_affs = false;
    bool found_xfs = false;
    bool found_zfs = false;

    for (const auto& entry : catalog()) {
        if (entry.id == std::string_view("affs")) {
            found_affs = true;
            assert(entry.name.find("Amiga") != std::string_view::npos);
        }
        if (entry.id == std::string_view("xfs")) {
            found_xfs = true;
            assert(entry.name.find("XFS") != std::string_view::npos);
        }
        if (entry.id == std::string_view("zfs")) {
            found_zfs = true;
            assert(!entry.packages.empty());
        }
    }

    assert(found_affs);
    assert(found_xfs);
    assert(found_zfs);
    return 0;
}
