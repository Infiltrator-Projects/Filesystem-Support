// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include "platform_support.hpp"

#include <iostream>
#include <string_view>

namespace {

int fail(const char* message)
{
    std::cerr << "windows_catalog_test: " << message << '\n';
    return 1;
}

} // namespace

int main()
{
    const auto& entries = filesystem_support::catalog();
    if (entries.empty()) {
        return fail("catalogue is empty");
    }

    bool found_ext2 = false;
    for (const auto& entry : entries) {
        const auto state = filesystem_support::windows_native_state(entry.id);
        const bool installable =
            filesystem_support::windows_native_installable(entry.id);

        if (entry.id == std::string_view("ext2")) {
            found_ext2 = true;
            if (state != filesystem_support::NativeImplementationState::InProgress) {
                return fail("EXT2 must remain in progress until shared-engine qualification");
            }
        } else if (state != filesystem_support::NativeImplementationState::NotImplemented) {
            std::cerr << "windows_catalog_test: unexpected Windows implementation state for "
                      << entry.id << '\n';
            return 1;
        }

        if (installable) {
            std::cerr << "windows_catalog_test: unqualified filesystem is installable: "
                      << entry.id << '\n';
            return 1;
        }
    }

    if (!found_ext2) {
        return fail("EXT2 proving implementation is missing");
    }

    return 0;
}
