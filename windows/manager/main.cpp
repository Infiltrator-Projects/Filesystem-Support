// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <commctrl.h>

#include "catalog.hpp"
#include "platform_support.hpp"

#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"InfiltratorFilesystemSupportWindow";
constexpr wchar_t kWindowTitle[] = L"Filesystem Support";
constexpr int kListId = 1001;
constexpr int kInstallId = 1002;
constexpr int kRemoveId = 1003;
constexpr int kStatusId = 1004;

HWND g_list = nullptr;
HWND g_install = nullptr;
HWND g_remove = nullptr;
HWND g_status = nullptr;

std::wstring widen(const std::string_view value)
{
    if (value.empty()) {
        return {};
    }

    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return L"<invalid UTF-8>";
    }

    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) {
        return L"<invalid UTF-8>";
    }
    return result;
}

void add_column(const int index, const int width, const wchar_t* title)
{
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(g_list, index, &column);
}

void set_subitem(const int row, const int column, const std::wstring& text)
{
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(g_list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
}

void populate_catalog()
{
    const auto& entries = filesystem_support::catalog();

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const std::wstring name = widen(entry.name);

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        const int row = ListView_InsertItem(g_list, &item);
        if (row < 0) {
            continue;
        }

        set_subitem(row, 1, widen(entry.family));
        set_subitem(
            row, 2,
            widen(filesystem_support::native_implementation_state_label(
                filesystem_support::windows_native_state(entry.id))));
    }
}

void update_selection()
{
    const int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (selected < 0) {
        EnableWindow(g_install, FALSE);
        EnableWindow(g_remove, FALSE);
        SetWindowTextW(
            g_status,
            L"Select a filesystem to see its Windows implementation state.");
        return;
    }

    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(g_list, &item)) {
        return;
    }

    const auto& entries = filesystem_support::catalog();
    const size_t index = static_cast<size_t>(item.lParam);
    if (index >= entries.size()) {
        return;
    }

    const auto& entry = entries[index];
    const auto state = filesystem_support::windows_native_state(entry.id);
    const bool installable = filesystem_support::windows_native_installable(entry.id);

    EnableWindow(g_install, installable ? TRUE : FALSE);
    EnableWindow(g_remove, FALSE);

    std::wstring message = widen(entry.name);
    message += L": ";
    message += widen(filesystem_support::native_implementation_state_label(state));
    if (state == filesystem_support::NativeImplementationState::InProgress) {
        message += L" - not installable until the shared canonical engine is qualified.";
    } else if (!installable) {
        message += L" - Windows support has not been implemented yet.";
    }
    SetWindowTextW(g_status, message.c_str());
}

void layout_controls(HWND window)
{
    RECT client{};
    GetClientRect(window, &client);

    constexpr int margin = 12;
    constexpr int button_width = 100;
    constexpr int button_height = 30;
    constexpr int status_height = 42;
    constexpr int gap = 8;

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int list_height = height - (margin * 3) - button_height - status_height;

    MoveWindow(g_list, margin, margin, width - margin * 2,
               list_height > 80 ? list_height : 80, TRUE);

    const int controls_y = margin + (list_height > 80 ? list_height : 80) + gap;
    MoveWindow(g_install, margin, controls_y, button_width, button_height, TRUE);
    MoveWindow(g_remove, margin + button_width + gap, controls_y,
               button_width, button_height, TRUE);
    MoveWindow(g_status, margin, controls_y + button_height + gap,
               width - margin * 2, status_height, TRUE);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
            GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(
            g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        add_column(0, 210, L"Filesystem");
        add_column(1, 180, L"Family");
        add_column(2, 170, L"Windows support");

        g_install = CreateWindowExW(
            0, L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallId)),
            GetModuleHandleW(nullptr), nullptr);
        g_remove = CreateWindowExW(
            0, L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveId)),
            GetModuleHandleW(nullptr), nullptr);
        g_status = CreateWindowExW(
            0, L"STATIC",
            L"Windows filesystem modules are being migrated. No module is installable yet.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)),
            GetModuleHandleW(nullptr), nullptr);

        EnableWindow(g_install, FALSE);
        EnableWindow(g_remove, FALSE);
        populate_catalog();
        layout_controls(window);
        return 0;
    }
    case WM_SIZE:
        layout_controls(window);
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header && header->idFrom == kListId &&
            header->code == LVN_ITEMCHANGED) {
            update_selection();
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kInstallId || LOWORD(wparam) == kRemoveId) {
            update_selection();
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    if (!InitCommonControlsEx(&controls)) {
        return 1;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&window_class)) {
        return 1;
    }

    HWND window = CreateWindowExW(
        0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 620,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        return 1;
    }

    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
