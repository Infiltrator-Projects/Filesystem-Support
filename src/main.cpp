// SPDX-License-Identifier: GPL-3.0-or-later
#include "action_policy.hpp"
#include "catalog.hpp"
#include "installer.hpp"
#include "probe.hpp"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <infiltratr/core.h>
#include <infiltratr/design.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <utility>
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

enum class ViewFilter {
    Home = 0,
    All,
    Linux,
    Microsoft,
    Amiga,
    Apple,
    UnixBsd,
    ImagesFuse,
    NetworkVirtual,
    Native,
    Installed,
    Available,
    Attention
};

struct AppState;

struct RowState {
    AppState* app = nullptr;
    const fs::FilesystemDescriptor* descriptor = nullptr;
    fs::ProbeResult probe;
    bool probed = false;
    std::string searchable;
    GtkWidget* row = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* detail = nullptr;
    GtkWidget* package_button = nullptr;
    GtkWidget* module_button = nullptr;
};

struct AppState {
    GtkWidget* window = nullptr;
    GtkWidget* stack = nullptr;
    GtkWidget* search = nullptr;
    GtkWidget* navigation = nullptr;
    GtkWidget* all_row = nullptr;
    GtkWidget* catalogue_title = nullptr;
    GtkWidget* catalogue_summary = nullptr;
    GtkWidget* ready_count = nullptr;
    GtkWidget* installable_count = nullptr;
    GtkWidget* attention_count = nullptr;
    GtkWidget* native_count = nullptr;
    GtkWidget* coverage_count = nullptr;
    GtkWidget* refresh_button = nullptr;
    GtkWidget* scan_spinner = nullptr;
    GtkCssProvider* theme_provider = nullptr;
    ViewFilter filter = ViewFilter::Home;
    std::vector<std::unique_ptr<RowState>> rows;
    bool scan_running = false;
    bool scan_pending = false;
    bool scan_complete = false;
    bool shutting_down = false;
};

struct ScanJob {
    std::vector<fs::ProbeResult> results;
};

std::string rgb_hex(const uint32_t rgb)
{
    char text[8];
    g_snprintf(text, sizeof(text), "#%06x",
               static_cast<unsigned int>(rgb & UINT32_C(0x00ffffff)));
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
    g_object_get(settings,
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

std::string running_kernel()
{
    struct utsname identity {};
    return uname(&identity) == 0 ? identity.release : "Unknown";
}

bool contains_ci(const std::string_view value, const char* needle)
{
    if (needle == nullptr || *needle == '\0') {
        return false;
    }
    const std::string owned(value);
    return infiltratr_ascii_contains_ci(owned.c_str(), needle);
}

bool family_contains(const fs::FilesystemDescriptor& descriptor,
                     const char* needle)
{
    return contains_ci(descriptor.family, needle);
}

bool descriptor_matches_family(const fs::FilesystemDescriptor& descriptor,
                               const ViewFilter filter)
{
    switch (filter) {
    case ViewFilter::Linux:
        return family_contains(descriptor, "Linux");
    case ViewFilter::Microsoft:
        return family_contains(descriptor, "Microsoft") ||
               family_contains(descriptor, "DOS") ||
               family_contains(descriptor, "OS/2");
    case ViewFilter::Amiga:
        return family_contains(descriptor, "Amiga");
    case ViewFilter::Apple:
        return family_contains(descriptor, "Apple") ||
               family_contains(descriptor, "Mac");
    case ViewFilter::UnixBsd:
        return family_contains(descriptor, "Unix") ||
               family_contains(descriptor, "BSD") ||
               family_contains(descriptor, "SGI") ||
               family_contains(descriptor, "QNX");
    case ViewFilter::ImagesFuse:
        return family_contains(descriptor, "Image") ||
               family_contains(descriptor, "FUSE") ||
               family_contains(descriptor, "Archive") ||
               family_contains(descriptor, "Optical") ||
               family_contains(descriptor, "Overlay") ||
               family_contains(descriptor, "Encryption");
    case ViewFilter::NetworkVirtual:
        return family_contains(descriptor, "Network") ||
               family_contains(descriptor, "Distributed") ||
               family_contains(descriptor, "Cloud") ||
               family_contains(descriptor, "Virtualisation") ||
               family_contains(descriptor, "Container") ||
               family_contains(descriptor, "Device") ||
               family_contains(descriptor, "Desktop");
    default:
        return true;
    }
}

std::string searchable_text(const fs::FilesystemDescriptor& descriptor)
{
    return std::string(descriptor.id) + " " +
           std::string(descriptor.name) + " " +
           std::string(descriptor.family) + " " +
           std::string(descriptor.description) + " " +
           std::string(descriptor.note) + " " +
           fs::access_mode_label(descriptor.access) + " " +
           fs::support_provider_label(descriptor.provider);
}

const char* filter_title(const ViewFilter filter)
{
    switch (filter) {
    case ViewFilter::Home: return "Overview";
    case ViewFilter::All: return "All Filesystems";
    case ViewFilter::Linux: return "Linux Filesystems";
    case ViewFilter::Microsoft: return "Microsoft & DOS";
    case ViewFilter::Amiga: return "Amiga Filesystems";
    case ViewFilter::Apple: return "Apple Filesystems";
    case ViewFilter::UnixBsd: return "Unix & BSD";
    case ViewFilter::ImagesFuse: return "Images, FUSE & Overlays";
    case ViewFilter::NetworkVirtual: return "Network & Virtual";
    case ViewFilter::Native: return "Infiltrator Native";
    case ViewFilter::Installed: return "Ready Support";
    case ViewFilter::Available: return "Available Support";
    case ViewFilter::Attention: return "Needs Attention";
    }
    return "Filesystems";
}

const char* filter_summary(const ViewFilter filter)
{
    switch (filter) {
    case ViewFilter::Home:
        return "Live support state for this system.";
    case ViewFilter::All:
        return "Every filesystem and storage namespace in the catalogue.";
    case ViewFilter::Linux:
        return "Native Linux and Linux-integrated filesystem support.";
    case ViewFilter::Microsoft:
        return "Windows, DOS and Microsoft storage formats.";
    case ViewFilter::Amiga:
        return "Classic and modern Amiga filesystem implementations.";
    case ViewFilter::Apple:
        return "Classic Mac, HFS+, APFS and Apple storage access.";
    case ViewFilter::UnixBsd:
        return "Unix, BSD, workstation and historical filesystems.";
    case ViewFilter::ImagesFuse:
        return "Disk images, optical media, FUSE providers and overlays.";
    case ViewFilter::NetworkVirtual:
        return "Network, distributed, cloud and virtual filesystems.";
    case ViewFilter::Native:
        return "Project-native kernel modules managed by Infiltrator.";
    case ViewFilter::Installed:
        return "Support ready on this system.";
    case ViewFilter::Available:
        return "Support available to install or enable.";
    case ViewFilter::Attention:
        return "Unavailable or incomplete support requiring review.";
    }
    return "";
}

void show_message(GtkWindow* parent,
                  const GtkMessageType type,
                  const char* title,
                  const std::string& message)
{
    GtkWidget* dialog = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_CLOSE, "%s", title);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
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

void apply_button_policy(GtkWidget* button, const fs::ButtonPolicy& policy)
{
    gtk_widget_set_visible(button, policy.visible ? TRUE : FALSE);
    if (!policy.visible) {
        return;
    }
    const std::string label(policy.label);
    gtk_button_set_label(GTK_BUTTON(button), label.c_str());
    gtk_widget_set_sensitive(button, policy.enabled ? TRUE : FALSE);
    set_button_semantics(button, policy.primary, policy.destructive);
}

void render_row(RowState* row)
{
    if (row == nullptr || row->descriptor == nullptr) {
        return;
    }

    if (!row->probed) {
        gtk_label_set_text(GTK_LABEL(row->status), "Scanning…");
        gtk_style_context_add_class(
            gtk_widget_get_style_context(row->status), "fs-status");
        gtk_label_set_text(
            GTK_LABEL(row->detail),
            "Detecting package and kernel support in the background.");

        gtk_widget_set_visible(
            row->package_button,
            row->descriptor->packages.empty() ? FALSE : TRUE);
        gtk_widget_set_sensitive(row->package_button, FALSE);
        gtk_button_set_label(GTK_BUTTON(row->package_button), "Scanning…");
        set_button_semantics(row->package_button, false, false);

        gtk_widget_set_visible(
            row->module_button,
            row->descriptor->modules.empty() ? FALSE : TRUE);
        gtk_widget_set_sensitive(row->module_button, FALSE);
        gtk_button_set_label(GTK_BUTTON(row->module_button), "Scanning…");
        set_button_semantics(row->module_button, false, false);
        return;
    }

    gtk_label_set_text(
        GTK_LABEL(row->status), fs::support_state_label(row->probe.state));
    set_status_semantics(row->status, row->probe.state);

    const std::string detail =
        fs::support_detail_text(*row->descriptor, row->probe);
    gtk_label_set_text(GTK_LABEL(row->detail), detail.c_str());

    const fs::PackageActionPolicy package_policy =
        fs::package_action_policy(*row->descriptor, row->probe);
    apply_button_policy(row->package_button, package_policy.button);

    const fs::ModuleActionPolicy module_policy =
        fs::module_action_policy(*row->descriptor, row->probe);
    apply_button_policy(row->module_button, module_policy.button);
}

bool row_matches_filter(const RowState& row, const ViewFilter filter)
{
    if (row.descriptor == nullptr) {
        return false;
    }

    switch (filter) {
    case ViewFilter::Home:
        return false;
    case ViewFilter::All:
        return true;
    case ViewFilter::Native:
        return row.descriptor->project_native_linux;
    case ViewFilter::Installed:
        return row.probed && row.probe.state == fs::SupportState::Ready;
    case ViewFilter::Available:
        return row.probed && row.probe.state == fs::SupportState::Installable;
    case ViewFilter::Attention:
        return row.probed &&
               (row.probe.state == fs::SupportState::Incomplete ||
                row.probe.state == fs::SupportState::Unavailable);
    default:
        return descriptor_matches_family(*row.descriptor, filter);
    }
}

void apply_filter(AppState* state)
{
    if (state == nullptr) {
        return;
    }

    const char* query = state->search != nullptr
        ? gtk_entry_get_text(GTK_ENTRY(state->search))
        : "";

    std::size_t visible = 0U;
    for (const auto& row : state->rows) {
        bool matches = row_matches_filter(*row, state->filter);
        if (matches && query != nullptr && *query != '\0') {
            matches = infiltratr_ascii_contains_ci(row->searchable.c_str(), query);
        }
        gtk_widget_set_visible(row->row, matches ? TRUE : FALSE);
        if (matches) {
            ++visible;
        }
    }

    if (state->catalogue_title != nullptr) {
        gtk_label_set_text(
            GTK_LABEL(state->catalogue_title), filter_title(state->filter));
    }
    if (state->catalogue_summary != nullptr) {
        std::ostringstream text;
        if (state->scan_running) {
            text << "Scanning system support  •  ";
        } else {
            text << visible << " entries shown  •  ";
        }
        text << filter_summary(state->filter);
        if (query != nullptr && *query != '\0') {
            text << "  •  Search: “" << query << "”";
        }
        gtk_label_set_text(
            GTK_LABEL(state->catalogue_summary), text.str().c_str());
    }
}

void set_metric(GtkWidget* widget, const std::string& value)
{
    if (widget != nullptr) {
        gtk_label_set_text(GTK_LABEL(widget), value.c_str());
    }
}

void update_summary(AppState* state)
{
    if (state == nullptr) {
        return;
    }

    if (!state->scan_complete) {
        set_metric(state->ready_count, "—");
        set_metric(state->installable_count, "—");
        set_metric(state->attention_count, "—");
        set_metric(state->native_count, "—");
        set_metric(state->coverage_count, std::to_string(state->rows.size()));
        apply_filter(state);
        return;
    }

    std::size_t ready = 0U;
    std::size_t installable = 0U;
    std::size_t attention = 0U;
    std::size_t native_total = 0U;
    std::size_t native_installed = 0U;

    for (const auto& row : state->rows) {
        if (!row->probed) {
            continue;
        }
        switch (row->probe.state) {
        case fs::SupportState::Ready:
            ++ready;
            break;
        case fs::SupportState::Installable:
            ++installable;
            break;
        case fs::SupportState::Incomplete:
        case fs::SupportState::Unavailable:
            ++attention;
            break;
        }
        if (row->descriptor->project_native_linux) {
            ++native_total;
            if (row->probe.project_native_installed) {
                ++native_installed;
            }
        }
    }

    set_metric(state->ready_count, std::to_string(ready));
    set_metric(state->installable_count, std::to_string(installable));
    set_metric(state->attention_count, std::to_string(attention));
    set_metric(
        state->native_count,
        std::to_string(native_installed) + " / " + std::to_string(native_total));
    set_metric(state->coverage_count, std::to_string(state->rows.size()));
    apply_filter(state);
}

void set_scan_chrome(AppState* state, const bool running)
{
    if (state == nullptr) {
        return;
    }
    if (state->refresh_button != nullptr) {
        gtk_widget_set_sensitive(state->refresh_button, running ? FALSE : TRUE);
        gtk_widget_set_tooltip_text(
            state->refresh_button,
            running ? "Scanning filesystem support" : "Refresh support scan");
    }
    if (state->scan_spinner != nullptr) {
        gtk_widget_set_visible(state->scan_spinner, running ? TRUE : FALSE);
        if (running) {
            gtk_spinner_start(GTK_SPINNER(state->scan_spinner));
        } else {
            gtk_spinner_stop(GTK_SPINNER(state->scan_spinner));
        }
    }

    /* Keep package/module actions out of the probe cache while its inventory
     * is being rebuilt on the worker thread.  When the scan ends render_row()
     * restores the exact sensitivity for each entry. */
    for (const auto& row : state->rows) {
        if (running) {
            gtk_widget_set_sensitive(row->package_button, FALSE);
            gtk_widget_set_sensitive(row->module_button, FALSE);
        } else if (row->probed) {
            render_row(row.get());
        }
    }
}

void start_scan(AppState* state);

void scan_job_destroy(gpointer data)
{
    delete static_cast<ScanJob*>(data);
}

void scan_worker(GTask* task,
                 gpointer,
                 gpointer task_data,
                 GCancellable*)
{
    auto* job = static_cast<ScanJob*>(task_data);
    fs::invalidate_probe_cache();
    const auto& catalogue = fs::catalog();
    job->results.reserve(catalogue.size());
    for (const auto& descriptor : catalogue) {
        job->results.push_back(fs::probe(descriptor));
    }
    g_task_return_boolean(task, TRUE);
}

void scan_finished(GObject*, GAsyncResult* result, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state == nullptr) {
        return;
    }

    GError* error = nullptr;
    const gboolean success =
        g_task_propagate_boolean(G_TASK(result), &error);
    auto* job = static_cast<ScanJob*>(
        g_task_get_task_data(G_TASK(result)));

    state->scan_running = false;

    const bool accepted =
        !state->shutting_down && success && job != nullptr &&
        job->results.size() == state->rows.size();
    if (accepted) {
        for (std::size_t index = 0U; index < state->rows.size(); ++index) {
            state->rows[index]->probe = std::move(job->results[index]);
            state->rows[index]->probed = true;
        }
        state->scan_complete = true;
    } else if (!state->shutting_down) {
        const std::string message = error != nullptr && error->message != nullptr
            ? error->message
            : "Filesystem support scan did not complete.";
        show_message(
            GTK_WINDOW(state->window), GTK_MESSAGE_ERROR,
            "Support scan failed", message);
    }
    if (!state->shutting_down) {
        set_scan_chrome(state, false);
        if (accepted) {
            update_summary(state);
        }
    }
    g_clear_error(&error);
    if (!state->shutting_down && state->scan_pending) {
        state->scan_pending = false;
        start_scan(state);
    }
}

void start_scan(AppState* state)
{
    if (state == nullptr || state->shutting_down) {
        return;
    }
    if (state->scan_running) {
        state->scan_pending = true;
        return;
    }

    state->scan_running = true;
    state->scan_pending = false;
    set_scan_chrome(state, true);
    apply_filter(state);

    GTask* task = g_task_new(nullptr, nullptr, scan_finished, state);
    auto* job = new ScanJob;
    g_task_set_task_data(task, job, scan_job_destroy);
    g_task_run_in_thread(task, scan_worker);
    g_object_unref(task);
}

bool confirm_removal(GtkWindow* parent, const fs::RemovalPlan& plan)
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
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
        "%s", "Remove filesystem support?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog), "%s", body.str().c_str());
    gtk_dialog_add_buttons(
        GTK_DIALOG(dialog),
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Remove", GTK_RESPONSE_ACCEPT,
        nullptr);
    const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_ACCEPT;
}

void package_clicked(GtkButton*, gpointer user_data)
{
    auto* row = static_cast<RowState*>(user_data);
    if (row == nullptr || row->app == nullptr || !row->probed ||
        row->descriptor == nullptr) {
        return;
    }

    AppState* state = row->app;
    const fs::PackageActionPolicy policy =
        fs::package_action_policy(*row->descriptor, row->probe);

    if (policy.action == fs::PackageActionKind::Install) {
        gtk_widget_set_sensitive(row->package_button, FALSE);
        gtk_button_set_label(GTK_BUTTON(row->package_button), "Installing…");
        fs::install_packages_async(
            row->probe.missing_packages,
            [state](const bool success, const std::string& message) {
                if (state->shutting_down) {
                    return;
                }
                if (!success) {
                    show_message(
                        GTK_WINDOW(state->window), GTK_MESSAGE_ERROR,
                        "Installation failed", message);
                }
                start_scan(state);
            });
        return;
    }

    if (policy.action != fs::PackageActionKind::Remove) {
        render_row(row);
        return;
    }

    if (policy.removal_requires_module_unload) {
        show_message(
            GTK_WINDOW(state->window), GTK_MESSAGE_WARNING,
            "Unload module first",
            "The DKMS driver is currently loaded. Unload the kernel module before removing its Debian driver package.");
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
        render_row(row);
        show_message(
            GTK_WINDOW(state->window), GTK_MESSAGE_WARNING,
            "Removal blocked", plan.reason);
        return;
    }
    if (plan.planned_packages.empty()) {
        start_scan(state);
        return;
    }
    if (!confirm_removal(GTK_WINDOW(state->window), plan)) {
        render_row(row);
        return;
    }

    gtk_button_set_label(GTK_BUTTON(row->package_button), "Removing…");
    fs::remove_packages_async(
        packages,
        [state](const bool success, const std::string& message) {
            if (state->shutting_down) {
                return;
            }
            if (!success) {
                show_message(
                    GTK_WINDOW(state->window), GTK_MESSAGE_ERROR,
                    "Removal failed", message);
            }
            start_scan(state);
        });
}

void module_clicked(GtkButton*, gpointer user_data)
{
    auto* row = static_cast<RowState*>(user_data);
    if (row == nullptr || row->app == nullptr || !row->probed ||
        row->descriptor == nullptr || row->probe.module_name.empty()) {
        return;
    }

    AppState* state = row->app;
    const std::string module = row->probe.module_name;
    const fs::ModuleActionPolicy policy =
        fs::module_action_policy(*row->descriptor, row->probe);
    if (policy.action == fs::ModuleActionKind::None) {
        render_row(row);
        return;
    }

    gtk_widget_set_sensitive(row->module_button, FALSE);

    auto complete = [state](const char* title,
                            const bool success,
                            const std::string& message) {
        if (state->shutting_down) {
            return;
        }
        if (!success) {
            show_message(
                GTK_WINDOW(state->window), GTK_MESSAGE_ERROR, title, message);
        }
        start_scan(state);
    };

    switch (policy.action) {
    case fs::ModuleActionKind::InstallNative: {
        gtk_button_set_label(GTK_BUTTON(row->module_button), "Installing native…");
        const std::string filesystem_id(row->descriptor->id);
        fs::install_native_module_async(
            filesystem_id, module,
            [complete](const bool success, const std::string& message) {
                complete("Native module installation failed", success, message);
            });
        break;
    }
    case fs::ModuleActionKind::RemoveNative: {
        gtk_button_set_label(GTK_BUTTON(row->module_button), "Removing native…");
        const std::string filesystem_id(row->descriptor->id);
        fs::remove_native_module_async(
            filesystem_id, module,
            [complete](const bool success, const std::string& message) {
                complete("Native module removal failed", success, message);
            });
        break;
    }
    case fs::ModuleActionKind::Load:
        gtk_button_set_label(GTK_BUTTON(row->module_button), "Loading…");
        fs::load_module_async(
            module,
            [complete](const bool success, const std::string& message) {
                complete("Module load failed", success, message);
            });
        break;
    case fs::ModuleActionKind::Unload:
        gtk_button_set_label(GTK_BUTTON(row->module_button), "Unloading…");
        fs::unload_module_async(
            module,
            [complete](const bool success, const std::string& message) {
                complete("Module unload failed", success, message);
            });
        break;
    case fs::ModuleActionKind::None:
        break;
    }
}

GtkWidget* make_label(const char* text,
                      const char* css_class,
                      const float xalign = 0.0F)
{
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), xalign);
    if (css_class != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(label), css_class);
    }
    return label;
}

GtkWidget* make_icon_well(const char* icon_name,
                          const int pixel_size,
                          const char* css_class)
{
    GtkWidget* well = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* icon =
        gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), pixel_size);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(well), icon, TRUE, TRUE, 0);
    if (css_class != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(well), css_class);
    }
    return well;
}

GtkWidget* create_row(AppState* state,
                      const fs::FilesystemDescriptor& descriptor,
                      RowState* row)
{
    row->app = state;
    row->descriptor = &descriptor;
    row->searchable = searchable_text(descriptor);

    GtkWidget* list_row = gtk_list_box_row_new();
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* icon = make_icon_well(
        descriptor.project_native_linux
            ? "applications-engineering-symbolic"
            : "drive-harddisk-symbolic",
        24, "fs-icon-well");
    GtkWidget* middle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(list_row), "fs-row");
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(list_row), outer);
    gtk_widget_set_size_request(actions, 170, -1);

    GtkWidget* title = make_label(
        std::string(descriptor.name).c_str(), "fs-row-title");
    GtkWidget* family = make_label(
        std::string(descriptor.family).c_str(), "fs-family-badge");
    gtk_box_pack_start(GTK_BOX(heading), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), family, FALSE, FALSE, 0);
    if (descriptor.project_native_linux) {
        gtk_box_pack_start(
            GTK_BOX(heading),
            make_label("INFILTRATOR NATIVE", "fs-native-badge"),
            FALSE, FALSE, 0);
    }

    GtkWidget* description = make_label(
        std::string(descriptor.description).c_str(), "fs-row-description");
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);

    const std::string meta =
        std::string(fs::access_mode_label(descriptor.access)) +
        "  •  " + fs::support_provider_label(descriptor.provider);
    GtkWidget* metadata = make_label(meta.c_str(), "fs-row-meta");

    row->detail = make_label("", "fs-row-detail");
    gtk_label_set_line_wrap(GTK_LABEL(row->detail), TRUE);

    gtk_box_pack_start(GTK_BOX(middle), heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(middle), description, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(middle), metadata, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(middle), row->detail, FALSE, FALSE, 0);

    row->status = make_label("Scanning…", "fs-status", 1.0F);
    gtk_widget_set_halign(row->status, GTK_ALIGN_END);
    row->package_button = gtk_button_new_with_label("Scanning…");
    row->module_button = gtk_button_new_with_label("Scanning…");
    gtk_widget_set_no_show_all(row->package_button, TRUE);
    gtk_widget_set_no_show_all(row->module_button, TRUE);
    g_signal_connect(
        row->package_button, "clicked", G_CALLBACK(package_clicked), row);
    g_signal_connect(
        row->module_button, "clicked", G_CALLBACK(module_clicked), row);

    gtk_box_pack_start(GTK_BOX(actions), row->status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), row->module_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), row->package_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), middle, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(outer), actions, FALSE, FALSE, 0);

    if (!descriptor.note.empty()) {
        gtk_widget_set_tooltip_text(
            list_row, std::string(descriptor.note).c_str());
    }

    row->row = list_row;
    render_row(row);
    return list_row;
}

void minimize_window(GtkButton*, gpointer user_data)
{
    gtk_window_iconify(GTK_WINDOW(user_data));
}

void toggle_maximize_window(GtkButton*, gpointer user_data)
{
    GtkWindow* window = GTK_WINDOW(user_data);
    if (gtk_window_is_maximized(window)) {
        gtk_window_unmaximize(window);
    } else {
        gtk_window_maximize(window);
    }
}

void close_window(GtkButton*, gpointer user_data)
{
    gtk_window_close(GTK_WINDOW(user_data));
}

GtkWidget* make_window_control(const char* icon_name,
                               const char* tooltip,
                               const char* extra_class)
{
    GtkWidget* button =
        gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(button), "window-control");
    if (extra_class != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(button), extra_class);
    }
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

void search_changed(GtkSearchEntry*, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state == nullptr) {
        return;
    }

    const char* query = gtk_entry_get_text(GTK_ENTRY(state->search));
    if (state->filter == ViewFilter::Home &&
        query != nullptr && *query != '\0' && state->all_row != nullptr) {
        gtk_list_box_select_row(
            GTK_LIST_BOX(state->navigation), GTK_LIST_BOX_ROW(state->all_row));
        return;
    }
    apply_filter(state);
}

void refresh_clicked(GtkButton*, gpointer user_data)
{
    start_scan(static_cast<AppState*>(user_data));
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

GtkWidget* build_header(AppState* state)
{
    GtkWidget* header = gtk_header_bar_new();
    GtkWidget* brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* icon = make_icon_well(
        kProjectInfo.icon_name, 28, "header-brand-icon");
    GtkWidget* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* header_end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_widget_set_name(header, "fs-shell-header");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), FALSE);
    gtk_header_bar_set_custom_title(
        GTK_HEADER_BAR(header), gtk_label_new(""));

    gtk_box_pack_start(GTK_BOX(brand), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(copy), make_label("Filesystem Support", "header-brand-title"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(copy), make_label("Infiltrator OS", "header-brand-subtitle"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand), copy, FALSE, FALSE, 0);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), brand);

    state->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(state->search), "Search filesystems…");
    gtk_widget_set_size_request(state->search, 280, -1);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(state->search), "settings-search");
    g_signal_connect(
        state->search, "search-changed", G_CALLBACK(search_changed), state);

    state->scan_spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(state->scan_spinner, TRUE);
    state->refresh_button = make_window_control(
        "view-refresh-symbolic", "Refresh support scan", nullptr);
    GtkWidget* minimize = make_window_control(
        "window-minimize-symbolic", "Minimize", nullptr);
    GtkWidget* maximize = make_window_control(
        "window-maximize-symbolic", "Maximize / Restore", nullptr);
    GtkWidget* close = make_window_control(
        "window-close-symbolic", "Close", "window-control-close");

    g_signal_connect(
        state->refresh_button, "clicked", G_CALLBACK(refresh_clicked), state);
    g_signal_connect(
        minimize, "clicked", G_CALLBACK(minimize_window), state->window);
    g_signal_connect(
        maximize, "clicked", G_CALLBACK(toggle_maximize_window), state->window);
    g_signal_connect(
        close, "clicked", G_CALLBACK(close_window), state->window);

    gtk_box_pack_start(GTK_BOX(header_end), state->search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), state->scan_spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), state->refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), minimize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), maximize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), close, FALSE, FALSE, 0);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), header_end);
    return header;
}

GtkWidget* make_navigation_heading(const char* text)
{
    GtkWidget* row = gtk_list_box_row_new();
    GtkWidget* label = make_label(text, "nav-title");
    gtk_container_set_border_width(GTK_CONTAINER(row), 4);
    gtk_container_add(GTK_CONTAINER(row), label);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    return row;
}

GtkWidget* make_navigation_row(const char* icon_name,
                               const char* title,
                               const ViewFilter filter)
{
    GtkWidget* row = gtk_list_box_row_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget* icon = gtk_image_new_from_icon_name(
        icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
    GtkWidget* label = make_label(title, "nav-primary");

    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(row), "nav-row");
    g_object_set_data(
        G_OBJECT(row), "view-filter",
        GINT_TO_POINTER(static_cast<int>(filter) + 1));
    return row;
}

void navigation_selected(GtkListBox*, GtkListBoxRow* row, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state == nullptr || row == nullptr) {
        return;
    }

    const int encoded = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(row), "view-filter"));
    if (encoded == 0) {
        return;
    }

    state->filter = static_cast<ViewFilter>(encoded - 1);
    if (state->filter == ViewFilter::Home) {
        gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "home");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "catalogue");
        apply_filter(state);
    }
}

GtkWidget* build_sidebar(AppState* state)
{
    GtkWidget* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* list = gtk_list_box_new();
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    gtk_widget_set_size_request(sidebar, 248, -1);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(sidebar), "settings-sidebar");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(list), "nav-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);

    gtk_container_add(GTK_CONTAINER(list), make_navigation_heading("SYSTEM"));
    GtkWidget* home = make_navigation_row(
        "go-home-symbolic", "Overview", ViewFilter::Home);
    gtk_container_add(GTK_CONTAINER(list), home);
    GtkWidget* all = make_navigation_row(
        "view-grid-symbolic", "All Filesystems", ViewFilter::All);
    state->all_row = all;
    gtk_container_add(GTK_CONTAINER(list), all);

    gtk_container_add(GTK_CONTAINER(list), make_navigation_heading("PLATFORMS"));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "computer-symbolic", "Linux", ViewFilter::Linux));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "computer-symbolic", "Microsoft & DOS", ViewFilter::Microsoft));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "media-floppy-symbolic", "Amiga", ViewFilter::Amiga));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "computer-symbolic", "Apple", ViewFilter::Apple));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "utilities-terminal-symbolic", "Unix & BSD", ViewFilter::UnixBsd));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "media-optical-symbolic", "Images & FUSE", ViewFilter::ImagesFuse));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "network-wired-symbolic", "Network & Virtual", ViewFilter::NetworkVirtual));

    gtk_container_add(GTK_CONTAINER(list), make_navigation_heading("STATUS"));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "applications-engineering-symbolic", "Infiltrator Native", ViewFilter::Native));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "emblem-ok-symbolic", "Ready", ViewFilter::Installed));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "system-software-install-symbolic", "Available", ViewFilter::Available));
    gtk_container_add(GTK_CONTAINER(list), make_navigation_row(
        "dialog-warning-symbolic", "Needs Attention", ViewFilter::Attention));

    g_signal_connect(
        list, "row-selected", G_CALLBACK(navigation_selected), state);
    state->navigation = list;

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_container_add(GTK_CONTAINER(scroller), list);
    gtk_box_pack_start(GTK_BOX(sidebar), scroller, TRUE, TRUE, 0);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(footer), "sidebar-footer");
    const std::string version = std::string("Version ") + kProjectInfo.version;
    gtk_box_pack_start(
        GTK_BOX(footer), make_label(version.c_str(), "sidebar-version"),
        FALSE, FALSE, 0);
    GtkWidget* about = gtk_button_new_with_label("About");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(about), "sidebar-about");
    g_signal_connect(about, "clicked", G_CALLBACK(about_clicked), state);
    gtk_box_pack_start(GTK_BOX(footer), about, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sidebar), footer, FALSE, FALSE, 0);

    gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(home));
    return sidebar;
}

GtkWidget* make_metric_tile(const char* icon_name,
                            const char* caption,
                            const char* description,
                            GtkWidget** value_out,
                            const char* semantic_class)
{
    GtkWidget* tile = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* icon = make_icon_well(icon_name, 22, "metric-icon");
    GtkWidget* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget* value = make_label("—", "metric-value");
    gtk_widget_set_hexpand(tile, TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(tile), "metric-tile");
    if (semantic_class != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(tile), semantic_class);
    }
    gtk_box_pack_start(
        GTK_BOX(copy), make_label(caption, "metric-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(copy), value, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(copy), make_label(description, "metric-copy"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tile), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tile), copy, TRUE, TRUE, 0);
    *value_out = value;
    return tile;
}

GtkWidget* make_info_card(const char* icon_name,
                          const char* title,
                          const std::string& value,
                          const char* copy,
                          GtkWidget** value_out = nullptr)
{
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* icon = gtk_image_new_from_icon_name(
        icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
    gtk_box_pack_start(GTK_BOX(heading), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(heading), make_label(title, "card-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), heading, FALSE, FALSE, 0);
    GtkWidget* value_label = make_label(value.c_str(), "info-value");
    gtk_box_pack_start(GTK_BOX(card), value_label, FALSE, FALSE, 0);
    if (value_out != nullptr) {
        *value_out = value_label;
    }
    GtkWidget* detail = make_label(copy, "card-copy");
    gtk_label_set_line_wrap(GTK_LABEL(detail), TRUE);
    gtk_box_pack_start(GTK_BOX(card), detail, FALSE, FALSE, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(card), "content-card");
    return card;
}

GtkWidget* build_home_page(AppState* state)
{
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget* title = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget* metrics = gtk_grid_new();
    GtkWidget* info = gtk_grid_new();

    gtk_style_context_add_class(
        gtk_widget_get_style_context(page), "home-page");
    gtk_box_pack_start(
        GTK_BOX(title), make_label("FILESYSTEM SUPPORT", "page-eyebrow"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(title), make_label("System Overview", "page-title"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(title), make_label(
            "Ready support, available providers and project-native kernel modules.",
            "page-summary"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);

    gtk_grid_set_row_spacing(GTK_GRID(metrics), 10);
    gtk_grid_set_column_spacing(GTK_GRID(metrics), 10);
    gtk_grid_attach(GTK_GRID(metrics), make_metric_tile(
        "emblem-ok-symbolic", "READY", "Ready now",
        &state->ready_count, "metric-installed"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), make_metric_tile(
        "system-software-install-symbolic", "AVAILABLE", "Can be installed",
        &state->installable_count, "metric-available"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), make_metric_tile(
        "dialog-warning-symbolic", "NEEDS ATTENTION", "Unavailable or incomplete",
        &state->attention_count, "metric-warning"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), make_metric_tile(
        "applications-engineering-symbolic", "NATIVE MODULES", "Installed / managed",
        &state->native_count, "metric-native"), 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), metrics, FALSE, FALSE, 0);

    gtk_grid_set_row_spacing(GTK_GRID(info), 10);
    gtk_grid_set_column_spacing(GTK_GRID(info), 10);
    gtk_grid_attach(
        GTK_GRID(info),
        make_info_card(
            "applications-engineering-symbolic", "Running Kernel",
            running_kernel(),
            "Native modules are built and verified against the running kernel."),
        0, 0, 1, 1);
    GtkWidget* coverage = make_info_card(
        "folder-documents-symbolic", "Catalogue Coverage", "—",
        "Kernel, userspace, FUSE and tools are presented through one support catalogue.",
        &state->coverage_count);
    gtk_grid_attach(GTK_GRID(info), coverage, 1, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), info, FALSE, FALSE, 0);

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), page);
    return scroller;
}

GtkWidget* build_catalogue_page(AppState* state)
{
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* list = gtk_list_box_new();

    gtk_style_context_add_class(
        gtk_widget_get_style_context(page), "catalogue-page");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(header), "catalogue-header");
    gtk_box_pack_start(
        GTK_BOX(header), make_label("FILESYSTEM CATALOGUE", "page-eyebrow"),
        FALSE, FALSE, 0);
    state->catalogue_title = make_label("All Filesystems", "page-title");
    state->catalogue_summary = make_label("", "page-summary");
    gtk_label_set_line_wrap(GTK_LABEL(state->catalogue_summary), TRUE);
    gtk_box_pack_start(
        GTK_BOX(header), state->catalogue_title, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(header), state->catalogue_summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(list), "fs-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);

    for (const auto& descriptor : fs::catalog()) {
        auto row = std::make_unique<RowState>();
        gtk_container_add(
            GTK_CONTAINER(list), create_row(state, descriptor, row.get()));
        state->rows.push_back(std::move(row));
    }

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(scroller), "fs-scroller");
    gtk_container_add(GTK_CONTAINER(scroller), list);
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);
    return page;
}

void install_common_theme(AppState* state)
{
    if (state == nullptr) {
        return;
    }

    const InfiltratrThemePalette* palette = infiltratr_theme_resolve(
        INFILTRATR_THEME_SYSTEM, system_prefers_dark());
    const InfiltratrTypography* type = infiltratr_typography();
    const InfiltratrDesignMetrics* metrics = infiltratr_design_metrics();
    GdkScreen* screen = gdk_screen_get_default();
    if (palette == nullptr || type == nullptr || metrics == nullptr ||
        screen == nullptr || type->ui_family == nullptr ||
        type->gtk_fallback == nullptr) {
        return;
    }

    if (state->theme_provider == nullptr) {
        state->theme_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            screen,
            GTK_STYLE_PROVIDER(state->theme_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 50U);
    }

    const std::string background = rgb_hex(palette->background_rgb);
    const std::string panel = rgb_hex(palette->panel_rgb);
    const std::string card = rgb_hex(palette->card_rgb);
    const std::string surface = rgb_hex(palette->surface_rgb);
    const std::string input = rgb_hex(palette->input_rgb);
    const std::string border = rgb_hex(palette->border_rgb);
    const std::string text = rgb_hex(palette->text_rgb);
    const std::string title = rgb_hex(palette->title_rgb);
    const std::string muted = rgb_hex(palette->muted_rgb);
    const std::string subtle = rgb_hex(palette->subtle_rgb);
    const std::string titlebar = rgb_hex(palette->titlebar_rgb);
    const std::string selection = rgb_hex(palette->selection_background_rgb);
    const std::string selection_text = rgb_hex(palette->selection_foreground_rgb);
    const std::string accent = rgb_hex(palette->neutral_accent_rgb);
    const std::string success = rgb_hex(palette->success_rgb);
    const std::string warning = rgb_hex(palette->warning_rgb);
    const std::string fault = rgb_hex(palette->fault_rgb);
    const std::string button = rgb_hex(palette->button_background_rgb);
    const std::string button_text = rgb_hex(palette->button_foreground_rgb);
    const std::string hover = rgb_hex(palette->surface_hover_rgb);

    std::ostringstream css;
    css
        << "window { background: " << background << "; color: " << text
        << "; font-family: \"" << type->ui_family << "\", "
        << type->gtk_fallback << "; font-weight: "
        << type->ui_regular_weight << "; }\n"
        << ".app-shell, .fs-scroller, .fs-scroller viewport { background: "
        << background << "; }\n"
        << "#fs-shell-header { background: " << titlebar
        << "; border-bottom: 1px solid " << border
        << "; min-height: 54px; padding: " << metrics->compact_spacing
        << "px " << metrics->control_spacing << "px; }\n"
        << ".header-brand-icon { min-width: 38px; min-height: 38px; background: "
        << surface << "; border: 1px solid " << border
        << "; border-radius: " << metrics->control_radius << "px; padding: 5px; }\n"
        << ".header-brand-icon image { color: " << accent << "; }\n"
        << ".header-brand-title { color: " << title << "; font-size: 18px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".header-brand-subtitle { color: " << muted << "; font-size: 10px; }\n"
        << ".settings-search { min-width: 260px; background: " << input
        << "; color: " << text << "; border: 1px solid " << border
        << "; border-radius: " << metrics->control_radius
        << "px; padding: 7px 10px; }\n"
        << ".window-control { min-width: 30px; min-height: 30px; padding: 4px; background: transparent; "
           "border: 1px solid transparent; border-radius: "
        << metrics->small_radius << "px; }\n"
        << ".window-control:hover { background: " << hover
        << "; border-color: " << border << "; }\n"
        << ".window-control-close:hover { background: " << fault
        << "; color: " << selection_text << "; }\n"
        << ".settings-sidebar { background: " << panel
        << "; border-right: 1px solid " << border << "; padding: "
        << metrics->control_spacing << "px 8px; }\n"
        << ".nav-list, .nav-list row { background: transparent; }\n"
        << ".nav-title { color: " << subtle << "; font-size: 9px; font-weight: "
        << type->ui_bold_weight << "; letter-spacing: 0.08em; margin: 8px 6px 2px 6px; }\n"
        << ".nav-row { min-height: 40px; border: 1px solid transparent; border-radius: "
        << metrics->control_radius << "px; padding: 6px 8px; margin: 1px 2px; }\n"
        << ".nav-row:hover { background: " << hover << "; }\n"
        << ".nav-row:selected { background: " << selection
        << "; color: " << selection_text << "; }\n"
        << ".nav-primary { color: " << text << "; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".nav-row:selected .nav-primary { color: " << selection_text << "; }\n"
        << ".sidebar-footer { border-top: 1px solid " << border
        << "; padding-top: 8px; }\n"
        << ".sidebar-version { color: " << subtle << "; font-size: 9px; }\n"
        << ".sidebar-about { background: transparent; color: " << text
        << "; border: 1px solid transparent; border-radius: "
        << metrics->small_radius << "px; }\n"
        << ".sidebar-about:hover { background: " << hover
        << "; border-color: " << border << "; }\n"
        << ".home-page, .catalogue-page { padding: "
        << metrics->screen_padding << "px; }\n"
        << ".page-eyebrow { color: " << subtle << "; font-size: 9px; font-weight: "
        << type->ui_bold_weight << "; letter-spacing: 0.10em; }\n"
        << ".page-title { color: " << title << "; font-size: 26px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".page-summary { color: " << muted << "; font-size: 11px; }\n"
        << ".metric-tile, .content-card, .catalogue-header { background: " << card
        << "; border: 1px solid " << border << "; border-radius: "
        << metrics->card_radius << "px; padding: " << metrics->content_padding << "px; }\n"
        << ".metric-icon { min-width: 36px; min-height: 36px; background: " << surface
        << "; border: 1px solid " << border << "; border-radius: "
        << metrics->control_radius << "px; padding: 5px; }\n"
        << ".metric-caption { color: " << subtle << "; font-size: 9px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".metric-value, .info-value { color: " << title
        << "; font-size: 20px; font-weight: " << type->ui_bold_weight << "; }\n"
        << ".metric-copy, .card-copy { color: " << muted << "; font-size: 10px; }\n"
        << ".card-title { color: " << title << "; font-size: 14px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".metric-installed .metric-value, .metric-installed .metric-icon image { color: "
        << success << "; }\n"
        << ".metric-available .metric-value, .metric-available .metric-icon image, "
           ".metric-native .metric-value, .metric-native .metric-icon image { color: "
        << accent << "; }\n"
        << ".metric-warning .metric-value, .metric-warning .metric-icon image { color: "
        << warning << "; }\n"
        << ".fs-list, .fs-list row { background: transparent; }\n"
        << ".fs-row { background: " << card << "; border: 1px solid " << border
        << "; border-radius: " << metrics->card_radius << "px; margin-bottom: "
        << metrics->control_spacing << "px; }\n"
        << ".fs-row:hover { background: " << hover << "; }\n"
        << ".fs-icon-well { min-width: 44px; min-height: 44px; background: " << surface
        << "; border: 1px solid " << border << "; border-radius: "
        << metrics->control_radius << "px; padding: 6px; }\n"
        << ".fs-icon-well image { color: " << accent << "; }\n"
        << ".fs-row-title { color: " << title << "; font-size: 14px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".fs-family-badge, .fs-native-badge, .fs-status { background: " << surface
        << "; border: 1px solid " << border << "; border-radius: 999px; padding: 3px 7px; "
           "font-size: 9px; }\n"
        << ".fs-native-badge { color: " << accent << "; }\n"
        << ".fs-row-description { color: " << text << "; font-size: 11px; }\n"
        << ".fs-row-meta { color: " << subtle << "; font-size: 9px; }\n"
        << ".fs-row-detail { color: " << muted << "; font-size: 10px; }\n"
        << ".fs-status-ready { color: " << success << "; }\n"
        << ".fs-status-installable { color: " << accent << "; }\n"
        << ".fs-status-incomplete { color: " << warning << "; }\n"
        << ".fs-status-unavailable { color: " << fault << "; }\n"
        << "button.fs-action { background: " << button << "; color: " << button_text
        << "; border: 1px solid " << border << "; border-radius: "
        << metrics->control_radius << "px; padding: 7px 10px; }\n"
        << "button.fs-action:hover { background: " << hover << "; }\n"
        << "button.primary-action { border-color: " << accent << "; color: "
        << accent << "; }\n"
        << "button.danger-action { color: " << fault << "; }\n"
        << "button:disabled { opacity: 0.55; }\n"
        << "scrollbar { background: transparent; min-width: 8px; min-height: 8px; }\n"
        << "scrollbar slider { min-width: 6px; min-height: 24px; border-radius: 999px; background: "
        << subtle << "; }\n"
        << "scrollbar slider:hover { background: " << accent << "; }\n";

    const std::string css_text = css.str();
    GError* error = nullptr;
    if (!gtk_css_provider_load_from_data(
            state->theme_provider, css_text.c_str(),
            static_cast<gssize>(css_text.size()), &error)) {
        g_warning("Unable to apply Filesystem Support theme: %s",
                  error != nullptr && error->message != nullptr
                      ? error->message : "unknown CSS error");
    }
    g_clear_error(&error);
}

void theme_changed(GObject*, GParamSpec*, gpointer user_data)
{
    install_common_theme(static_cast<AppState*>(user_data));
}

void window_destroyed(GtkWidget*, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state != nullptr) {
        state->shutting_down = true;
        state->window = nullptr;
    }
}

gboolean begin_initial_scan(gpointer user_data)
{
    start_scan(static_cast<AppState*>(user_data));
    return G_SOURCE_REMOVE;
}

void activate(GtkApplication* application, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    if (state->window != nullptr) {
        gtk_window_present(GTK_WINDOW(state->window));
        return;
    }

    state->shutting_down = false;
    state->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state->window), kProjectInfo.program_name);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1240, 780);
    gtk_window_set_icon_name(GTK_WINDOW(state->window), kProjectInfo.icon_name);

    GdkGeometry geometry {};
    geometry.min_width = 960;
    geometry.min_height = 620;
    gtk_window_set_geometry_hints(
        GTK_WINDOW(state->window), state->window, &geometry, GDK_HINT_MIN_SIZE);
    g_signal_connect(
        state->window, "destroy", G_CALLBACK(window_destroyed), state);

    install_common_theme(state);
    GtkSettings* settings = gtk_settings_get_default();
    if (settings != nullptr) {
        g_signal_connect(
            settings, "notify::gtk-application-prefer-dark-theme",
            G_CALLBACK(theme_changed), state);
        g_signal_connect(
            settings, "notify::gtk-theme-name",
            G_CALLBACK(theme_changed), state);
    }

    GtkWidget* header = build_header(state);
    gtk_window_set_titlebar(GTK_WINDOW(state->window), header);

    GtkWidget* shell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(shell), "app-shell");
    gtk_container_add(GTK_CONTAINER(state->window), shell);

    state->stack = gtk_stack_new();
    gtk_stack_set_transition_type(
        GTK_STACK(state->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(state->stack), 120);
    gtk_widget_set_hexpand(state->stack, TRUE);
    gtk_widget_set_vexpand(state->stack, TRUE);

    GtkWidget* sidebar = build_sidebar(state);
    GtkWidget* home = build_home_page(state);
    GtkWidget* catalogue = build_catalogue_page(state);
    gtk_stack_add_named(GTK_STACK(state->stack), home, "home");
    gtk_stack_add_named(GTK_STACK(state->stack), catalogue, "catalogue");
    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "home");

    gtk_box_pack_start(GTK_BOX(shell), sidebar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(shell), state->stack, TRUE, TRUE, 0);

    update_summary(state);
    gtk_widget_show_all(state->window);

    /* First paint is intentionally complete before any package/module probe
     * starts.  Rows already contain non-blocking “Scanning…” placeholders. */
    g_idle_add(begin_initial_scan, state);
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
        kProjectInfo.application_id, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(
        application, "activate", G_CALLBACK(activate), &state);
    const int status =
        g_application_run(G_APPLICATION(application), argc, argv);

    if (state.theme_provider != nullptr) {
        g_object_unref(state.theme_provider);
    }
    g_object_unref(application);
    return status;
}
