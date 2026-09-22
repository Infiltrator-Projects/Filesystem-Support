// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_support.hpp"

namespace filesystem_support {

NativeImplementationState windows_native_state(const std::string_view filesystem_id)
{
    // EXT2 is the proving implementation for the shared-engine architecture.
    // It is deliberately not installable until the canonical engine has
    // replaced both the Linux-specific and former ExtFS implementations.
    if (filesystem_id == "ext2") {
        return NativeImplementationState::InProgress;
    }

    return NativeImplementationState::NotImplemented;
}

const char* native_implementation_state_label(const NativeImplementationState state)
{
    switch (state) {
    case NativeImplementationState::NotImplemented:
        return "Not implemented";
    case NativeImplementationState::InProgress:
        return "In progress";
    case NativeImplementationState::Qualified:
        return "Available";
    }
    return "Unknown";
}

bool windows_native_installable(const std::string_view filesystem_id)
{
    return windows_native_state(filesystem_id) == NativeImplementationState::Qualified;
}

} // namespace filesystem_support
