// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace filesystem_support {

enum class NativeImplementationState {
    NotImplemented,
    InProgress,
    Qualified
};

NativeImplementationState windows_native_state(std::string_view filesystem_id);
const char* native_implementation_state_label(NativeImplementationState state);
bool windows_native_installable(std::string_view filesystem_id);

} // namespace filesystem_support
