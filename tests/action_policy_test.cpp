// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include "installer.hpp"

#include <iostream>
#include <string>

namespace {

int fail(const char* message)
{
    std::cerr << "action_policy_test: " << message << '\n';
    return 1;
}

} // namespace

int main()
{
    using namespace filesystem_support;

    if (!package_is_catalogued("util-linux") ||
        package_is_catalogued("definitely-not-catalogued")) {
        return fail("package allowlist is not deny-by-default");
    }

    if (!module_is_catalogued("affs") ||
        module_is_catalogued("definitely-not-catalogued")) {
        return fail("module allowlist is not deny-by-default");
    }

    const RemovalPlan empty_plan = plan_package_removal({});
    if (empty_plan.allowed) {
        return fail("empty package removal was allowed");
    }

    const RemovalPlan unknown_plan =
        plan_package_removal({"definitely-not-catalogued"});
    if (unknown_plan.allowed) {
        return fail("unknown package removal was allowed");
    }

    bool install_callback = false;
    bool install_success = true;
    install_packages_async(
        {"definitely-not-catalogued"},
        [&install_callback, &install_success](
            const bool success, const std::string&) {
            install_callback = true;
            install_success = success;
        });
    if (!install_callback || install_success) {
        return fail("invalid install was not denied synchronously");
    }

    bool module_callback = false;
    bool module_success = true;
    load_module_async(
        "definitely-not-catalogued",
        [&module_callback, &module_success](
            const bool success, const std::string&) {
            module_callback = true;
            module_success = success;
        });
    if (!module_callback || module_success) {
        return fail("invalid module action was not denied synchronously");
    }

    bool native_callback = false;
    bool native_success = true;
    install_native_module_async(
        "affs",
        "affs",
        [&native_callback, &native_success](
            const bool success, const std::string&) {
            native_callback = true;
            native_success = success;
        });
    if (!native_callback || native_success) {
        return fail("unmanaged native module action was not denied synchronously");
    }

    const auto hfs_users = catalogue_entries_using_package("hfsprogs");
    if (hfs_users.size() != 2U) {
        return fail("shared package impact mapping is incorrect");
    }

    return 0;
}
