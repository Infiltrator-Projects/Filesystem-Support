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

    const std::string text =
        std::to_string(ready) + " ready  •  " +
        std::to_string(installable) + " installable  •  " +
        std::to_string(unavailable) + " unavailable/incomplete";
    gtk_label_set_text(GTK_LABEL(state->summary), text.c_str());
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

void refresh_row(RowState* row)
{
    row->probe = fs::probe(*row->descriptor);

    gtk_label_set_text(
        GTK_LABEL(row->status),
        fs::support_state_label(row->probe.state));

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
        } else if (unavailable_packages) {
            gtk_button_set_label(
                GTK_BUTTON(row->package_button),
                "Package unavailable");
            gtk_widget_set_sensitive(row->package_button, FALSE);
        } else {
            gtk_button_set_label(
                GTK_BUTTON(row->package_button),
                remove_label(*row->descriptor));
            gtk_widget_set_sensitive(row->package_button, TRUE);
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
        break;
    case fs::KernelState::LoadableUnloaded:
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            "Load module");
        gtk_widget_set_sensitive(row->module_button, TRUE);
        break;
    case fs::KernelState::LoadableLoaded:
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            "Unload module");
        gtk_widget_set_sensitive(row->module_button, TRUE);
        break;
    case fs::KernelState::Missing:
        gtk_button_set_label(
            GTK_BUTTON(row->module_button),
            row->descriptor->provider == fs::SupportProvider::Dkms
                ? "Driver not installed"
                : "Requires different kernel");
        gtk_widget_set_sensitive(row->module_button, FALSE);
        break;
    }
}

void refresh_all(AppState* state)
{
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

    GtkWidget* description =
        gtk_label_new(std::string(descriptor.description).c_str());
    gtk_label_set_xalign(GTK_LABEL(description), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);

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

    row->detail = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(row->detail), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(row->detail), TRUE);

    gtk_box_pack_start(GTK_BOX(left), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), description, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), row->detail, FALSE, FALSE, 0);

    row->status = gtk_label_new("");
    gtk_widget_set_halign(row->status, GTK_ALIGN_END);

    row->package_button = gtk_button_new_with_label("Checking…");
    g_signal_connect(
        row->package_button,
        "clicked",
        G_CALLBACK(package_clicked),
        row);

    row->module_button = gtk_button_new_with_label("Checking kernel…");
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

void activate(GtkApplication* application, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state->window != nullptr) {
        gtk_window_present(GTK_WINDOW(state->window));
        return;
    }

    const InfiltratrDesignMetrics* metrics = infiltratr_design_metrics();
    const gint padding =
        metrics != nullptr
            ? static_cast<gint>(metrics->screen_padding)
            : 18;
    const gint spacing =
        metrics != nullptr
            ? static_cast<gint>(metrics->section_spacing)
            : 12;

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
        "Manage Debian filesystem packages and kernel modules");
    gtk_header_bar_set_show_close_button(
        GTK_HEADER_BAR(header), TRUE);

    GtkWidget* about = gtk_button_new_with_label("About");
    g_signal_connect(
        about, "clicked", G_CALLBACK(about_clicked), state);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about);
    gtk_window_set_titlebar(GTK_WINDOW(state->window), header);

    GtkWidget* root =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
    gtk_container_set_border_width(
        GTK_CONTAINER(root), static_cast<guint>(padding));
    gtk_container_add(GTK_CONTAINER(state->window), root);

    GtkWidget* search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search),
        "Search filesystems, providers, vendors or families…");
    g_signal_connect(
        search,
        "search-changed",
        G_CALLBACK(search_changed),
        state);

    state->summary = gtk_label_new("Detecting support…");
    gtk_label_set_xalign(GTK_LABEL(state->summary), 0.0F);

    GtkWidget* scroller =
        gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(
        GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scroller), list);

    gtk_box_pack_start(GTK_BOX(root), search, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(root), state->summary, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(root), scroller, TRUE, TRUE, 0);

    for (const auto& descriptor : fs::catalog()) {
        auto row = std::make_unique<RowState>();
        gtk_container_add(
            GTK_CONTAINER(list),
            create_row(state, descriptor, row.get()));
        state->rows.push_back(std::move(row));
    }

    update_summary(state);
    gtk_widget_show_all(state->window);

    for (const auto& row : state->rows) {
        refresh_row(row.get());
    }
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
