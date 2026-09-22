// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include "installer.hpp"
#include "probe.hpp"

#include <gtk/gtk.h>

#include <infiltratr/core.h>
#include <infiltratr/design.h>

#include <memory>
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
    "Install and inspect filesystem support on Linux Mint and compatible systems.",
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
    GtkWidget* button = nullptr;
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
           std::string(descriptor.description);
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
        std::to_string(ready) + " installed  •  " +
        std::to_string(installable) + " available to add  •  " +
        std::to_string(unavailable) + " unavailable/incomplete";
    gtk_label_set_text(GTK_LABEL(state->summary), text.c_str());
}

void refresh_row(RowState* row)
{
    row->probe = fs::probe(*row->descriptor);
    gtk_label_set_text(GTK_LABEL(row->status),
                       fs::support_state_label(row->probe.state));
    gtk_label_set_text(GTK_LABEL(row->detail), row->probe.detail.c_str());

    const bool installable =
        row->probe.state == fs::SupportState::Installable &&
        !row->probe.missing_packages.empty();

    gtk_widget_set_sensitive(row->button, installable);
    gtk_button_set_label(
        GTK_BUTTON(row->button),
        installable ? "Install support" :
        row->probe.state == fs::SupportState::Ready ? "Installed" :
        "Not installable");
}

void show_error(GtkWindow* parent, const std::string& message)
{
    GtkWidget* dialog = gtk_message_dialog_new(
        parent,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE,
        "%s",
        "Installation failed");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void install_clicked(GtkButton*, gpointer user_data)
{
    auto* row = static_cast<RowState*>(user_data);
    if (row == nullptr || row->app == nullptr ||
        row->probe.missing_packages.empty()) {
        return;
    }

    gtk_widget_set_sensitive(row->button, FALSE);
    gtk_button_set_label(GTK_BUTTON(row->button), "Installing…");

    fs::install_packages_async(
        row->probe.missing_packages,
        [row](const bool success, const std::string& message) {
            refresh_row(row);
            update_summary(row->app);
            if (!success) {
                show_error(GTK_WINDOW(row->app->window), message);
            }
        });
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
        std::string(descriptor.name) + "  ·  " + std::string(descriptor.family);
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
        std::string("Capability: ") + fs::access_mode_label(descriptor.access) +
        (descriptor.note.empty() ? "" : " — " + std::string(descriptor.note));
    GtkWidget* note = gtk_label_new(capability.c_str());
    gtk_label_set_xalign(GTK_LABEL(note), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");

    row->detail = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(row->detail), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(row->detail), TRUE);

    gtk_box_pack_start(GTK_BOX(left), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), description, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), row->detail, FALSE, FALSE, 0);

    row->status = gtk_label_new("");
    gtk_widget_set_halign(row->status, GTK_ALIGN_END);

    row->button = gtk_button_new_with_label("Checking…");
    g_signal_connect(row->button, "clicked", G_CALLBACK(install_clicked), row);

    gtk_box_pack_start(GTK_BOX(right), row->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), row->button, FALSE, FALSE, 0);
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
        metrics != nullptr ? static_cast<gint>(metrics->screen_padding) : 18;
    const gint spacing =
        metrics != nullptr ? static_cast<gint>(metrics->section_spacing) : 12;

    state->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state->window), kProjectInfo.program_name);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 900, 680);
    gtk_window_set_icon_name(GTK_WINDOW(state->window), kProjectInfo.icon_name);

    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), kProjectInfo.program_name);
    gtk_header_bar_set_subtitle(
        GTK_HEADER_BAR(header),
        "Add filesystem support without remembering package names");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);

    GtkWidget* about = gtk_button_new_with_label("About");
    g_signal_connect(about, "clicked", G_CALLBACK(about_clicked), state);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about);
    gtk_window_set_titlebar(GTK_WINDOW(state->window), header);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
    gtk_container_set_border_width(GTK_CONTAINER(root),
                                   static_cast<guint>(padding));
    gtk_container_add(GTK_CONTAINER(state->window), root);

    GtkWidget* search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search),
        "Search filesystems, vendors or families…");
    g_signal_connect(search, "search-changed",
                     G_CALLBACK(search_changed), state);

    state->summary = gtk_label_new("Detecting support…");
    gtk_label_set_xalign(GTK_LABEL(state->summary), 0.0F);

    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scroller), list);

    gtk_box_pack_start(GTK_BOX(root), search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), state->summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroller, TRUE, TRUE, 0);

    for (const auto& descriptor : fs::catalog()) {
        auto row = std::make_unique<RowState>();
        gtk_container_add(GTK_CONTAINER(list),
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
    g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
    const int status =
        g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
