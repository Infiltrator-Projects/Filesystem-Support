// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalog.hpp"
#include "probe.hpp"

#include <functional>
#include <string>
#include <vector>

namespace filesystem_support {

using DesktopActionCompletion =
    std::function<void(bool, const std::string&)>;

enum class DesktopActionDisposition {
    None,
    Execute,
    ConfirmRemoval,
    Refresh,
    Blocked
};

struct DesktopAction {
    DesktopActionDisposition disposition = DesktopActionDisposition::None;
    std::string busy_label;
    std::string failure_title;
    std::string message;
    std::vector<std::string> planned_packages;
    std::vector<std::string> affected_entries;

private:
    std::function<void(DesktopActionCompletion)> executor;

    friend DesktopAction prepare_package_desktop_action(
        const FilesystemDescriptor&, const ProbeResult&);
    friend DesktopAction prepare_module_desktop_action(
        const FilesystemDescriptor&, const ProbeResult&);
    friend void execute_desktop_action_async(
        DesktopAction, DesktopActionCompletion);
};

DesktopAction prepare_package_desktop_action(
    const FilesystemDescriptor& descriptor,
    const ProbeResult& probe);

DesktopAction prepare_module_desktop_action(
    const FilesystemDescriptor& descriptor,
    const ProbeResult& probe);

void execute_desktop_action_async(
    DesktopAction action,
    DesktopActionCompletion completion);

} // namespace filesystem_support
