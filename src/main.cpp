// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include "installer.hpp"
#include "probe.hpp"

#include <gtk/gtk.h>

#include <infiltratr/core.h>
#include <infiltratr/design.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = filesystem_support;

namespace {

const InfiltratrProjectInfo kProjectInfo = {
    sizeof(InfiltratrProjectInfo),
    INFILTRATR_PROJECT_INFO_ABI,
    "Filesystem Support",
    "filesystem-support",
    "org.infiltrator.FilesystemSupport",
    FILESYSTEM_SUPPORT_VERSION,
    "Infiltrator-Projects/Filesystem-Support",
    "cmake",
    "Shannon Smith",
    "https://github.com/Infiltrator-Projects/Filesystem-Support",
    "GPL-3.0-or-later",
    "Install, remove and inspect filesystem support on Debian systems.",
    "drive-harddisk",
    "Copyright (c) 2026 Shannon Smith"
};

struct AppState;

struct RowState {
    AppState* app = nullptr;
    const fs::FilesystemDescriptor* descriptor = nullptr;
    fs::ProbeResult probe;
    GtkWidget* row = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* detail = nullptr;
    GtkWidget* package_button = nullptr;
    GtkWidget* module_button = nullptr;
};

struct AppState {
    GtkWidget* window = nullptr;
    GtkWidget* summary = nullptr;
    GtkWidget* ready_count = nullptr;
    GtkWidget* installable_count = nullptr;
    GtkWidget* unavailable_count = nullptr;
    std::vector<std::unique_ptr<RowState>> rows;
};

std::string searchable_text(const fs::FilesystemDescriptor& descriptor)
{
    return std::string(descriptor.name) + " " +
           std::string(descriptor.family) + " " +
           std::string(descriptor.description) + " " +
           fs::support_provider_label(descriptor.provider);
}

void update_summary(AppState* state)
{
    std::size_t ready = 0U;
    std::size_t installable = 0U;
    std::size_t unavailable = 0U;

    for (const auto& row : state->rows) {
        if (row->probe.state == fs::SupportState::Ready) {
            ++ready;
        } else if (row->probe.state == fs::SupportState::Installable) {
            ++installable;
        } else {
            ++unavailable;
        }
    }

    if (state->ready_count != nullptr) {
        const std::string value = std::to_string(ready);
        gtk_label_set_text(GTK_LABEL(state->ready_count), value.c_str());
    }
    if (state->installable_count != nullptr) {
        const std::string value = std::to_string(installable);
        gtk_label_set_text(GTK_LABEL(state->installable_count), value.c_str());
    }
    if (state->unavailable_count != nullptr) {
        const std::string value = std::to_string(unavailable);
        gtk_label_set_text(GTK_LABEL(state->unavailable_count), value.c_str());
    }
    if (state->summary != nullptr) {
        const std::string text =
            std::to_string(state->rows.size()) +
            " filesystem definitions checked against this system.";
        gtk_label_set_text(GTK_LABEL(state->summary), text.c_str());
    }
}

const char* install_label(const fs::FilesystemDescriptor& descriptor)
{
    switch (descriptor.provider) {
    case fs::SupportProvider::Userspace:
    case fs::SupportProvider::Dkms:
        return "Install support";
    case fs::SupportProvider::KernelWithUserspace:
        return "Install userspace";
    case fs::SupportProvider::ToolsOnly:
        return "Install tools";
    case fs::SupportProvider::Kernel:
        return "No package";
    }
    return "Install";
}

const char* remove_label(const fs::FilesystemDescriptor& descriptor)
{
    switch (descriptor.provider) {
    case fs::SupportProvider::Userspace:
    case fs::SupportProvider::Dkms:
        return "Remove support";
    case fs::SupportProvider::KernelWithUserspace:
        return "Remove userspace";
    case fs::SupportProvider::ToolsOnly:
        return "Remove tools";
    case fs::SupportProvider::Kernel:
        return "No package";
    }
    return "Remove";
}

void set_status_semantics(GtkWidget* label, const fs::SupportState state)
{
    GtkStyleContext* context = gtk_widget_get_style_context(label);
    gtk_style_context_add_class(context, "fs-status");
    gtk_style_context_remove_class(context, "fs-status-ready");
    gtk_style_context_remove_class(context, "fs-status-installable");
    gtk_style_context_remove_class(context, "fs-status-incomplete");
    gtk_style_context_remove_class(context, "fs-status-unavailable");

    switch (state) {
    case fs::SupportState::Ready:
        gtk_style_context_add_class(context, "fs-status-ready");
        break;
    case fs::SupportState::Installable:
        gtk_style_context_add_class(context, "fs-status-installable");
        break;
    case fs::SupportState::Incomplete:
        gtk_style_context_add_class(context, "fs-status-incomplete");
        break;
    case fs::SupportState::Unavailable:
        gtk_style_context_add_class(context, "fs-status-unavailable");
        break;
    }
}

void set_button_semantics(GtkWidget* button,
                          const bool primary,
                          const bool destructive)
{
    GtkStyleContext* context = gtk_widget_get_style_context(button);
    gtk_style_context_add_class(context, "fs-action");
    gtk_style_context_remove_class(context, "primary-action");
    gtk_style_context_remove_class(context, "danger-action");
    if (primary) {
        gtk_style_context_add_class(context, "primary-action");
    } else if (destructive) {
        gtk_style_context_add_class(context, "danger-action");
    }
}

void refresh_row(RowState* row)
{
    row->probe = fs::probe(*row->descriptor);

    gtk_label_set_text(
        GTK_LABEL(row->status),
        fs::support_state_label(row->probe.state));
    set_status_semantics(row->status, row->probe.state);

    std::string detail =
        std::string("Support path: ") +
        fs::support_provider_label(row->descriptor->provider);

    if (!row->descriptor->modules.empty()) {
        if (row->descriptor->project_native_linux) {
            detail += "  •  Infiltrator native: ";
            if (!row->probe.project_native_installed) {
                detail += "not installed";
            } else if (!row->probe.project_native_selected) {
                detail += "installed, but not selected by kernel";
            } else {
                detail += fs::kernel_state_label(row->probe.kernel_state);
            }
        } else {
            detail += "  •  Kernel: ";
            detail += fs::kernel_state_label(row->probe.kernel_state);
        }
    }

    detail += ". ";
    detail += row->probe.detail;
    gtk_label_set_text(GTK_LABEL(row->detail), detail.c_str());

    if (row->descriptor->packages.empty()) {
        gtk_widget_hide(row->package_button);
    } else {
        gtk_widget_show(row->package_button);

        const bool missing_packages = !row->probe.missing_packages.empty();
        const bool unavailable_packages =
            !row->probe.unavailable_packages.empty();

        if (missing_packages) {
            gtk_button_set_label(
                GTK_BUTTON(row->package_button),
                install_label(*row->descriptor));
            gtk_widget_set_sensitive(
                row->package_button,
                unavailable_packages ? FALSE : TRUE);
            set_button_semantics(
                row->package_button, !unavailable_packages, false);
        } else if (unavailable_packages) {
            gtk_button_set_label(
                GTK_BUTTON(row->package_button),
                "Package unavailable");
            gtk_widget_set_sensitive(row->package_button, FALSE);
            set_button_semantics(row->package_button, false, false);
        } else {
            gtk_button_set_label(
                GTK_BUTTON(row->package_button),
                remove_label(*row->descriptor));
            gtk_widget_set_sensitive(row->package_button, TRUE);
            set_button_semantics(row->package_button, false, true);
        }
    }

    if (row->descriptor->modules.empty()) {
        gtk_widget_hide(row->module_button);
        return;
    }

    gtk_widget_show(row->module_button);

    if (row->descriptor->project_native_linux) {
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            row->probe.project_native_installed
                ? "Remove native"
                : "Install native");
        gtk_widget_set_sensitive(row->module_button, TRUE);
        set_button_semantics(
            row->module_button,
            !row->probe.project_native_installed,
            row->probe.project_native_installed);
        return;
    }

    switch (row->probe.kernel_state) {
    case fs::KernelState::NotApplicable:
        gtk_widget_hide(row->module_button);
        break;
    case fs::KernelState::BuiltIn:
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            "Built into kernel");
        gtk_widget_set_sensitive(row->module_button, FALSE);
        set_button_semantics(row->module_button, false, false);
        break;
    case fs::KernelState::LoadableUnloaded:
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            "Load module");
        gtk_widget_set_sensitive(row->module_button, TRUE);
        set_button_semantics(row->module_button, true, false);
        break;
    case fs::KernelState::LoadableLoaded:
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            "Unload module");
        gtk_widget_set_sensitive(row->module_button, TRUE);
        set_button_semantics(row->module_button, false, true);
        break;
    case fs::KernelState::Missing:
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            row->descriptor->provider == fs::SupportProvider::Dkms
                ? "Driver not installed"
                : "Requires different kernel");
        gtk_widget_set_sensitive(row->module_button, FALSE);
        set_button_semantics(row->module_button, false, false);
        break;
    }
}

void refresh_all(AppState* state)
{
    fs::invalidate_probe_cache();
    for (const auto& row : state->rows) {
        refresh_row(row.get());
    }
    update_summary(state);
}

void show_message(GtkWindow* parent,
                  GtkMessageType type,
                  const char* title,
                  const std::string& message)
{
    GtkWidget* dialog = gtk_message_dialog_new(
        parent,
        GTK_DIALOG_MODAL,
        type,
        GTK_BUTTONS_CLOSE,
        "%s",
        title);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

bool confirm_removal(GtkWindow* parent,
                     const fs::RemovalPlan& plan)
{
    std::ostringstream body;
    body << "Debian's simulation plans to remove:";

    for (const auto& package : plan.planned_packages) {
        body << "\n  • " << package;
    }

    if (!plan.affected_entries.empty()) {
        body << "\n\nFilesystem Support entries affected:";
        for (const auto& entry : plan.affected_entries) {
            body << "\n  • " << entry;
        }
    }

    body << "\n\nNo automatic autoremove will be run.";

    GtkWidget* dialog = gtk_message_dialog_new(
        parent,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE,
        "%s",
        "Remove filesystem support?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s", body.str().c_str());
    gtk_dialog_add_buttons(
        GTK_DIALOG(dialog),
        "_Cancel",
        GTK_RESPONSE_CANCEL,
        "_Remove",
        GTK_RESPONSE_ACCEPT,
        nullptr);

    const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_ACCEPT;
}

void package_clicked(GtkButton*, gpointer user_data)
{
    auto* row = static_cast<RowState*>(user_data);
    if (row == nullptr || row->app == nullptr) {
        return;
    }

    if (!row->probe.missing_packages.empty()) {
        gtk_widget_set_sensitive(row->package_button, FALSE);
        gtk_button_set_label(GTK_BUTTON(row->package_button), "Installing…");

        fs::install_packages_async(
            row->probe.missing_packages,
            [row](const bool success, const std::string& message) {
                refresh_all(row->app);
                if (!success) {
                    show_message(
                        GTK_WINDOW(row->app->window),
                        GTK_MESSAGE_ERROR,
                        "Installation failed",
                        message);
                }
            });
        return;
    }

    if (row->descriptor->provider == fs::SupportProvider::Dkms &&
        row->probe.kernel_state == fs::KernelState::LoadableLoaded) {
        show_message(
            GTK_WINDOW(row->app->window),
            GTK_MESSAGE_WARNING,
            "Unload module first",
            "The DKMS driver is currently loaded. Unload the kernel module "
            "before removing its Debian driver package.");
        return;
    }

    std::vector<std::string> packages;
    packages.reserve(row->descriptor->packages.size());
    for (const auto package : row->descriptor->packages) {
        packages.emplace_back(package);
    }

    gtk_widget_set_sensitive(row->package_button, FALSE);
    gtk_button_set_label(GTK_BUTTON(row->package_button), "Checking removal…");

    const fs::RemovalPlan plan = fs::plan_package_removal(packages);
    if (!plan.allowed) {
        refresh_row(row);
        show_message(
            GTK_WINDOW(row->app->window),
            GTK_MESSAGE_WARNING,
            "Removal blocked",
            plan.reason);
        return;
    }

    if (plan.planned_packages.empty()) {
        refresh_all(row->app);
        return;
    }

    if (!confirm_removal(GTK_WINDOW(row->app->window), plan)) {
        refresh_row(row);
        return;
    }

    gtk_button_set_label(GTK_BUTTON(row->package_button), "Removing…");

    fs::remove_packages_async(
        packages,
        [row](const bool success, const std::string& message) {
            refresh_all(row->app);
            if (!success) {
                show_message(
                    GTK_WINDOW(row->app->window),
                    GTK_MESSAGE_ERROR,
                    "Removal failed",
                    message);
            }
        });
}

void module_clicked(GtkButton*, gpointer user_data)
{
    auto* row = static_cast<RowState*>(user_data);
    if (row == nullptr || row->app == nullptr ||
        row->probe.module_name.empty()) {
        return;
    }

    const std::string module = row->probe.module_name;
    gtk_widget_set_sensitive(row->module_button, FALSE);

    if (row->descriptor->project_native_linux) {
        const std::string filesystem_id(row->descriptor->id);

        if (row->probe.project_native_installed) {
            gtk_button_set_label(
                GTK_BUTTON(row->module_button), "Removing native…");
            fs::remove_native_module_async(
                filesystem_id,
                module,
                [row](const bool success, const std::string& message) {
                    refresh_all(row->app);
                    if (!success) {
                        show_message(
                            GTK_WINDOW(row->app->window),
                            GTK_MESSAGE_ERROR,
                            "Native module removal failed",
                            message +
                                "\n\nUnmount any filesystem using this "
                                "driver before removing it.");
                    }
                });
        } else {
            gtk_button_set_label(
                GTK_BUTTON(row->module_button), "Installing native…");
            fs::install_native_module_async(
                filesystem_id,
                module,
                [row](const bool success, const std::string& message) {
                    refresh_all(row->app);
                    if (!success) {
                        show_message(
                            GTK_WINDOW(row->app->window),
                            GTK_MESSAGE_ERROR,
                            "Native module installation failed",
                            message);
                    }
                });
        }
        return;
    }

    if (row->probe.kernel_state == fs::KernelState::LoadableUnloaded) {
        gtk_button_set_label(GTK_BUTTON(row->module_button), "Loading…");
        fs::load_module_async(
            module,
            [row](const bool success, const std::string& message) {
                refresh_all(row->app);
                if (!success) {
                    show_message(
                        GTK_WINDOW(row->app->window),
                        GTK_MESSAGE_ERROR,
                        "Module load failed",
                        message);
                }
            });
        return;
    }

    if (row->probe.kernel_state == fs::KernelState::LoadableLoaded) {
        gtk_button_set_label(GTK_BUTTON(row->module_button), "Unloading…");
        fs::unload_module_async(
            module,
            [row](const bool success, const std::string& message) {
                refresh_all(row->app);
                if (!success) {
                    show_message(
                        GTK_WINDOW(row->app->window),
                        GTK_MESSAGE_ERROR,
                        "Module unload failed",
                        message +
                            "\n\nThe module may be in use by a mounted filesystem.");
                }
            });
    }
}

void search_changed(GtkSearchEntry* entry, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    const char* query = gtk_entry_get_text(GTK_ENTRY(entry));

    for (const auto& row : state->rows) {
        const std::string haystack = searchable_text(*row->descriptor);
        const bool visible =
            query == nullptr || *query == '\0' ||
            infiltratr_ascii_contains_ci(haystack.c_str(), query);
        gtk_widget_set_visible(row->row, visible);
    }
}

void about_clicked(GtkButton*, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    const gchar* authors[] = {"Shannon Smith", nullptr};
    gtk_show_about_dialog(
        GTK_WINDOW(state->window),
        "program-name", kProjectInfo.program_name,
        "version", kProjectInfo.version,
        "comments", kProjectInfo.comments,
        "website", kProjectInfo.website,
        "license-type", GTK_LICENSE_GPL_3_0,
        "authors", authors,
        "logo-icon-name", kProjectInfo.icon_name,
        nullptr);
}

GtkWidget* create_row(AppState* state,
                      const fs::FilesystemDescriptor& descriptor,
                      RowState* row)
{
    row->app = state;
    row->descriptor = &descriptor;

    GtkWidget* list_row = gtk_list_box_row_new();
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(list_row), "fs-row");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(outer), "fs-row-content");
    gtk_widget_set_size_request(right, 156, -1);

    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(list_row), outer);

    const std::string title_text =
        std::string(descriptor.name) + "  ·  " +
        std::string(descriptor.family);
    gchar* escaped = g_markup_escape_text(title_text.c_str(), -1);
    const std::string title_markup = "<b>" + std::string(escaped) + "</b>";
    g_free(escaped);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), title_markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(title), "fs-row-title");

    GtkWidget* description =
        gtk_label_new(std::string(descriptor.description).c_str());
    gtk_label_set_xalign(GTK_LABEL(description), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(description), "fs-row-description");

    const std::string capability =
        std::string("Capability: ") +
        fs::access_mode_label(descriptor.access) +
        "  •  Provider: " +
        fs::support_provider_label(descriptor.provider) +
        (descriptor.note.empty()
             ? ""
             : " — " + std::string(descriptor.note));

    GtkWidget* note = gtk_label_new(capability.c_str());
    gtk_label_set_xalign(GTK_LABEL(note), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(note), "dim-label");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(note), "fs-row-meta");

    row->detail = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(row->detail), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(row->detail), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(row->detail), "fs-row-detail");

    gtk_box_pack_start(GTK_BOX(left), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), description, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), row->detail, FALSE, FALSE, 0);

    row->status = gtk_label_new("");
    gtk_widget_set_halign(row->status, GTK_ALIGN_END);

    row->package_button = gtk_button_new_with_label("Checking…");
    set_button_semantics(row->package_button, false, false);
    g_signal_connect(
        row->package_button,
        "clicked",
        G_CALLBACK(package_clicked),
        row);

    row->module_button = gtk_button_new_with_label("Checking kernel…");
    set_button_semantics(row->module_button, false, false);
    g_signal_connect(
        row->module_button,
        "clicked",
        G_CALLBACK(module_clicked),
        row);

    gtk_box_pack_start(GTK_BOX(right), row->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), row->package_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), row->module_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), left, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(outer), right, FALSE, FALSE, 0);

    row->row = list_row;
    refresh_row(row);
    return list_row;
}

std::string rgb_hex(const uint32_t rgb)
{
    char text[8];
    g_snprintf(text, sizeof(text), "#%06x",
               static_cast<unsigned int>(rgb & 0x00ffffffU));
    return text;
}

bool system_prefers_dark()
{
    GtkSettings* settings = gtk_settings_get_default();
    if (settings == nullptr) {
        return false;
    }

    gboolean prefer_dark = FALSE;
    gchar* theme_name = nullptr;
    g_object_get(
        settings,
        "gtk-application-prefer-dark-theme", &prefer_dark,
        "gtk-theme-name", &theme_name,
        nullptr);

    bool dark = prefer_dark != FALSE;
    if (!dark && theme_name != nullptr) {
        dark = infiltratr_ascii_contains_ci(theme_name, "dark");
    }
    g_free(theme_name);
    return dark;
}

void install_common_theme()
{
    const InfiltratrThemePalette* palette =
        infiltratr_theme_resolve(
            INFILTRATR_THEME_SYSTEM, system_prefers_dark());
    const InfiltratrDesignMetrics* metrics = infiltratr_design_metrics();
    const InfiltratrTypography* typography = infiltratr_typography();
    GdkScreen* screen = gdk_screen_get_default();

    if (palette == nullptr || metrics == nullptr ||
        typography == nullptr || screen == nullptr) {
        return;
    }

    const std::string background = rgb_hex(palette->background_rgb);
    const std::string card = rgb_hex(palette->card_rgb);
    const std::string surface = rgb_hex(palette->surface_rgb);
    const std::string input = rgb_hex(palette->input_rgb);
    const std::string border = rgb_hex(palette->border_rgb);
    const std::string text = rgb_hex(palette->text_rgb);
    const std::string title = rgb_hex(palette->title_rgb);
    const std::string muted = rgb_hex(palette->muted_rgb);
    const std::string subtle = rgb_hex(palette->subtle_rgb);
    const std::string accent = rgb_hex(palette->neutral_accent_rgb);
    const std::string accent_foreground =
        rgb_hex(palette->accent_foreground_rgb);
    const std::string accent_hover = rgb_hex(palette->accent_hover_rgb);
    const std::string success = rgb_hex(palette->success_rgb);
    const std::string warning = rgb_hex(palette->warning_rgb);
    const std::string fault = rgb_hex(palette->fault_rgb);
    const std::string titlebar = rgb_hex(palette->titlebar_rgb);
    const std::string hover = rgb_hex(palette->card_hover_rgb);
    const std::string surface_hover = rgb_hex(palette->surface_hover_rgb);
    const std::string operation = rgb_hex(palette->operation_rgb);
    const std::string status_border = rgb_hex(palette->status_border_rgb);
    const std::string success_border = rgb_hex(palette->success_border_rgb);
    const std::string warning_border = rgb_hex(palette->warning_border_rgb);

    std::ostringstream css;
    css
        << "window { background: " << background << "; color: " << text
        << "; font-family: \"" << typography->ui_family << "\", "
        << typography->gtk_fallback << "; font-weight: "
        << typography->ui_regular_weight << "; }\n"
        << "headerbar { background: " << titlebar << "; color: " << title
        << "; border-bottom: 1px solid " << border << "; padding: 4px 8px; }\n"
        << "headerbar .title { font-family: \"" << typography->brand_family
        << "\"; font-weight: " << typography->brand_weight
        << "; font-size: 17px; }\n"
        << ".app-root, .fs-scroller, .fs-scroller viewport { background: "
        << background << "; }\n"
        << ".page-hero { background: " << card << "; border: 1px solid "
        << border << "; border-radius: " << metrics->panel_radius
        << "px; padding: 18px 20px; }\n"
        << ".page-icon { background: " << surface << "; color: " << accent
        << "; border: 1px solid " << border << "; border-radius: "
        << metrics->card_radius << "px; padding: 10px; }\n"
        << ".page-kicker { color: " << accent
        << "; font-size: 10px; font-weight: " << typography->ui_bold_weight
        << "; letter-spacing: 0.11em; }\n"
        << ".page-title { color: " << title << "; font-family: \""
        << typography->brand_family << "\"; font-size: 26px; font-weight: "
        << typography->brand_weight << "; }\n"
        << ".page-summary { color: " << muted << "; font-size: 12px; }\n"
        << ".stat-card { background: " << card << "; border: 1px solid "
        << border << "; border-radius: " << metrics->card_radius
        << "px; padding: 10px 14px; }\n"
        << ".stat-caption { color: " << subtle
        << "; font-size: 10px; font-weight: " << typography->ui_bold_weight
        << "; }\n"
        << ".stat-value { color: " << title
        << "; font-size: 19px; font-weight: " << typography->ui_bold_weight
        << "; }\n"
        << ".stat-ready .stat-value { color: " << success << "; }\n"
        << ".stat-installable .stat-value { color: " << accent << "; }\n"
        << ".stat-unavailable .stat-value { color: " << warning << "; }\n"
        << ".fs-search { background: " << input << "; color: " << text
        << "; border: 1px solid " << border << "; border-radius: "
        << metrics->control_radius << "px; padding: 8px 12px; }\n"
        << ".fs-search:focus { border-color: " << accent << "; }\n"
        << ".fs-list, .fs-list row { background: transparent; }\n"
        << ".fs-row { background: " << card << "; border: 1px solid "
        << border << "; border-radius: " << metrics->card_radius
        << "px; margin: 0 0 8px 0; }\n"
        << ".fs-row:hover { background: " << hover << "; }\n"
        << ".fs-row-title { color: " << title
        << "; font-size: 14px; font-weight: " << typography->ui_bold_weight
        << "; }\n"
        << ".fs-row-description { color: " << text << "; font-size: 11px; }\n"
        << ".fs-row-meta { color: " << subtle << "; font-size: 10px; }\n"
        << ".fs-row-detail { color: " << muted << "; font-size: 10px; }\n"
        << ".fs-status { background: " << surface << "; color: " << muted
        << "; border: 1px solid " << status_border << "; border-radius: "
        << metrics->small_radius << "px; padding: 4px 8px; font-size: 10px; "
        << "font-weight: " << typography->ui_bold_weight << "; }\n"
        << ".fs-status-ready { color: " << success << "; border-color: "
        << success_border << "; }\n"
        << ".fs-status-installable { color: " << accent
        << "; border-color: " << accent << "; }\n"
        << ".fs-status-incomplete { color: " << warning
        << "; border-color: " << warning_border << "; }\n"
        << ".fs-status-unavailable { color: " << fault << "; }\n"
        << "button.fs-action, button.header-action { background: " << operation
        << "; color: " << text << "; border: 1px solid " << border
        << "; border-radius: " << metrics->control_radius
        << "px; padding: 6px 10px; }\n"
        << "button.fs-action:hover, button.header-action:hover { background: "
        << surface_hover << "; }\n"
        << "button.primary-action { background: " << accent << "; color: "
        << accent_foreground << "; border-color: " << accent << "; }\n"
        << "button.primary-action:hover { background: " << accent_hover << "; }\n"
        << "button.danger-action { color: " << fault << "; }\n"
        << "button:disabled { opacity: 0.58; }\n";

    const std::string css_text = css.str();
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        provider,
        css_text.c_str(),
        static_cast<gssize>(css_text.size()),
        nullptr);
    gtk_style_context_add_provider_for_screen(
        screen,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 50U);
    g_object_unref(provider);
}

void activate(GtkApplication* application, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state->window != nullptr) {
        gtk_window_present(GTK_WINDOW(state->window));
        return;
    }

    install_common_theme();

    const InfiltratrDesignMetrics* metrics = infiltratr_design_metrics();
    const gint padding =
        metrics != nullptr
            ? static_cast<gint>(metrics->screen_padding)
            : 20;
    const gint spacing =
        metrics != nullptr
            ? static_cast<gint>(metrics->section_spacing)
            : 18;

    state->window = gtk_application_window_new(application);
    gtk_window_set_title(
        GTK_WINDOW(state->window), kProjectInfo.program_name);
    gtk_window_set_default_size(
        GTK_WINDOW(state->window), 1220, 780);
    gtk_window_set_icon_name(
        GTK_WINDOW(state->window), kProjectInfo.icon_name);

    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_title(
        GTK_HEADER_BAR(header), kProjectInfo.program_name);
    gtk_header_bar_set_subtitle(
        GTK_HEADER_BAR(header),
        "Filesystem drivers and support");
    gtk_header_bar_set_show_close_button(
        GTK_HEADER_BAR(header), TRUE);

    GtkWidget* about = gtk_button_new_with_label("About");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(about), "header-action");
    g_signal_connect(
        about, "clicked", G_CALLBACK(about_clicked), state);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about);
    gtk_window_set_titlebar(GTK_WINDOW(state->window), header);

    GtkWidget* root =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(root), "app-root");
    gtk_container_set_border_width(
        GTK_CONTAINER(root), static_cast<guint>(padding));
    gtk_container_add(GTK_CONTAINER(state->window), root);

    GtkWidget* hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(hero), "page-hero");

    GtkWidget* icon_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(icon_box), "page-icon");
    GtkWidget* icon = gtk_image_new_from_icon_name(
        "drive-harddisk-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(icon_box), icon, TRUE, TRUE, 0);

    GtkWidget* identity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget* kicker = gtk_label_new("SYSTEM STORAGE");
    gtk_label_set_xalign(GTK_LABEL(kicker), 0.0F);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(kicker), "page-kicker");

    GtkWidget* page_title = gtk_label_new("Filesystem Support");
    gtk_label_set_xalign(GTK_LABEL(page_title), 0.0F);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(page_title), "page-title");

    state->summary = gtk_label_new("Detecting support…");
    gtk_label_set_xalign(GTK_LABEL(state->summary), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(state->summary), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(state->summary), "page-summary");

    gtk_box_pack_start(GTK_BOX(identity), kicker, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(identity), page_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(identity), state->summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), icon_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), identity, TRUE, TRUE, 0);

    auto make_stat_card =
        [](const char* caption,
           const char* semantic_class,
           GtkWidget** value_out) -> GtkWidget* {
            GtkWidget* card =
                gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            gtk_style_context_add_class(
                gtk_widget_get_style_context(card), "stat-card");
            gtk_style_context_add_class(
                gtk_widget_get_style_context(card), semantic_class);
            gtk_widget_set_hexpand(card, TRUE);

            GtkWidget* caption_label = gtk_label_new(caption);
            gtk_label_set_xalign(GTK_LABEL(caption_label), 0.0F);
            gtk_style_context_add_class(
                gtk_widget_get_style_context(caption_label), "stat-caption");

            GtkWidget* value = gtk_label_new("—");
            gtk_label_set_xalign(GTK_LABEL(value), 0.0F);
            gtk_style_context_add_class(
                gtk_widget_get_style_context(value), "stat-value");

            gtk_box_pack_start(
                GTK_BOX(card), caption_label, FALSE, FALSE, 0);
            gtk_box_pack_start(
                GTK_BOX(card), value, FALSE, FALSE, 0);
            *value_out = value;
            return card;
        };

    GtkWidget* stats = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(
        GTK_BOX(stats),
        make_stat_card("INSTALLED", "stat-ready", &state->ready_count),
        TRUE, TRUE, 0);
    gtk_box_pack_start(
        GTK_BOX(stats),
        make_stat_card(
            "AVAILABLE", "stat-installable", &state->installable_count),
        TRUE, TRUE, 0);
    gtk_box_pack_start(
        GTK_BOX(stats),
        make_stat_card(
            "UNAVAILABLE / INCOMPLETE",
            "stat-unavailable",
            &state->unavailable_count),
        TRUE, TRUE, 0);

    GtkWidget* search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search),
        "Search filesystems, providers, vendors or families…");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(search), "fs-search");
    g_signal_connect(
        search,
        "search-changed",
        G_CALLBACK(search_changed),
        state);

    GtkWidget* scroller =
        gtk_scrolled_window_new(nullptr, nullptr);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(scroller), "fs-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget* list = gtk_list_box_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(list), "fs-list");
    gtk_list_box_set_selection_mode(
        GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scroller), list);

    gtk_box_pack_start(GTK_BOX(root), hero, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), stats, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroller, TRUE, TRUE, 0);

    for (const auto& descriptor : fs::catalog()) {
        auto row = std::make_unique<RowState>();
        gtk_container_add(
            GTK_CONTAINER(list),
            create_row(state, descriptor, row.get()));
        state->rows.push_back(std::move(row));
    }

    update_summary(state);
    gtk_widget_show_all(state->window);
}

} // namespace

int main(int argc, char** argv)
{
    if (!infiltratr_project_info_is_valid(&kProjectInfo) ||
        !fs::catalog_is_valid()) {
        return 2;
    }

    AppState state;
    GtkApplication* application = gtk_application_new(
        kProjectInfo.application_id,
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(
        application, "activate", G_CALLBACK(activate), &state);
    const int status =
        g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
