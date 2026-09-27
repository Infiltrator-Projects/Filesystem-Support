// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include "installer.hpp"
#include "probe.hpp"

#include <gtk/gtk.h>

#include <infiltratr/core.h>
#include <infiltratr/design.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/utsname.h>
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
    GtkWidget* row = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* detail = nullptr;
    GtkWidget* package_button = nullptr;
    GtkWidget* module_button = nullptr;
};

struct AppState {
    GtkWidget* window = nullptr;
    GtkWidget* stack = nullptr;
    GtkWidget* catalogue_title = nullptr;
    GtkWidget* catalogue_summary = nullptr;
    GtkWidget* search = nullptr;
    GtkWidget* navigation = nullptr;
    GtkWidget* all_row = nullptr;
    GtkWidget* ready_count = nullptr;
    GtkWidget* installable_count = nullptr;
    GtkWidget* unavailable_count = nullptr;
    GtkWidget* native_count = nullptr;
    GtkWidget* coverage_count = nullptr;
    ViewFilter filter = ViewFilter::Home;
    std::vector<std::unique_ptr<RowState>> rows;
};

std::string rgb_hex(const uint32_t rgb)
{
    char text[8];
    g_snprintf(
        text,
        sizeof(text),
        "#%06x",
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

std::string running_kernel()
{
    struct utsname identity {};
    if (uname(&identity) == 0) {
        return identity.release;
    }
    return "Unknown";
}

bool contains_ci(const std::string_view value, const char* needle)
{
    if (needle == nullptr || *needle == '\0') {
        return false;
    }
    const std::string copy(value);
    return infiltratr_ascii_contains_ci(copy.c_str(), needle);
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
    case ViewFilter::Home:
        return "Home";
    case ViewFilter::All:
        return "All Filesystems";
    case ViewFilter::Linux:
        return "Linux Filesystems";
    case ViewFilter::Microsoft:
        return "Microsoft & DOS";
    case ViewFilter::Amiga:
        return "Amiga Filesystems";
    case ViewFilter::Apple:
        return "Apple Filesystems";
    case ViewFilter::UnixBsd:
        return "Unix & BSD";
    case ViewFilter::ImagesFuse:
        return "Images, FUSE & Overlays";
    case ViewFilter::NetworkVirtual:
        return "Network & Virtual";
    case ViewFilter::Native:
        return "Infiltrator Native";
    case ViewFilter::Installed:
        return "Installed Support";
    case ViewFilter::Available:
        return "Available Support";
    case ViewFilter::Attention:
        return "Needs Attention";
    }
    return "Filesystems";
}

const char* filter_summary(const ViewFilter filter)
{
    switch (filter) {
    case ViewFilter::Home:
        return "Overview and quick access";
    case ViewFilter::All:
        return "Every filesystem and storage namespace known to Filesystem Support.";
    case ViewFilter::Linux:
        return "Native Linux, Linux-origin and Linux-integrated filesystem support.";
    case ViewFilter::Microsoft:
        return "Windows, DOS and Microsoft storage formats and access paths.";
    case ViewFilter::Amiga:
        return "Classic and modern Amiga filesystem implementations.";
    case ViewFilter::Apple:
        return "Classic Mac, HFS+, APFS and Apple storage access.";
    case ViewFilter::UnixBsd:
        return "Unix, BSD, workstation and historical kernel filesystems.";
    case ViewFilter::ImagesFuse:
        return "Disk images, optical media, FUSE providers, overlays and encrypted namespaces.";
    case ViewFilter::NetworkVirtual:
        return "Network, distributed, cloud, device and virtual-machine filesystems.";
    case ViewFilter::Native:
        return "Project-native Linux modules built and managed by Infiltrator.";
    case ViewFilter::Installed:
        return "Filesystem support currently ready on this system.";
    case ViewFilter::Available:
        return "Support that can be installed or enabled on this system.";
    case ViewFilter::Attention:
        return "Unavailable or incomplete support requiring review.";
    }
    return "";
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
        return row.probe.state == fs::SupportState::Ready;
    case ViewFilter::Available:
        return row.probe.state == fs::SupportState::Installable;
    case ViewFilter::Attention:
        return row.probe.state == fs::SupportState::Incomplete ||
               row.probe.state == fs::SupportState::Unavailable;
    default:
        return descriptor_matches_family(*row.descriptor, filter);
    }
}

void apply_filter(AppState* state)
{
    if (state == nullptr) {
        return;
    }

    const char* query = "";
    if (state->search != nullptr) {
        query = gtk_entry_get_text(GTK_ENTRY(state->search));
    }

    std::size_t visible = 0U;
    for (const auto& row : state->rows) {
        bool matches = row_matches_filter(*row, state->filter);
        if (matches && query != nullptr && *query != '\0') {
            const std::string haystack = searchable_text(*row->descriptor);
            matches =
                infiltratr_ascii_contains_ci(haystack.c_str(), query);
        }
        gtk_widget_set_visible(row->row, matches ? TRUE : FALSE);
        if (matches) {
            ++visible;
        }
    }

    if (state->catalogue_title != nullptr) {
        gtk_label_set_text(
            GTK_LABEL(state->catalogue_title),
            filter_title(state->filter));
    }

    if (state->catalogue_summary != nullptr) {
        std::ostringstream summary;
        summary << visible << " entries shown";
        if (query != nullptr && *query != '\0') {
            summary << " for \"" << query << "\"";
        }
        summary << "  •  " << filter_summary(state->filter);
        gtk_label_set_text(
            GTK_LABEL(state->catalogue_summary),
            summary.str().c_str());
    }
}

void update_summary(AppState* state)
{
    std::size_t ready = 0U;
    std::size_t installable = 0U;
    std::size_t unavailable = 0U;
    std::size_t native_total = 0U;
    std::size_t native_installed = 0U;

    for (const auto& row : state->rows) {
        if (row->probe.state == fs::SupportState::Ready) {
            ++ready;
        } else if (row->probe.state == fs::SupportState::Installable) {
            ++installable;
        } else {
            ++unavailable;
        }

        if (row->descriptor->project_native_linux) {
            ++native_total;
            if (row->probe.project_native_installed) {
                ++native_installed;
            }
        }
    }

    auto set_count = [](GtkWidget* widget, const std::size_t value) {
        if (widget == nullptr) {
            return;
        }
        const std::string text = std::to_string(value);
        gtk_label_set_text(GTK_LABEL(widget), text.c_str());
    };

    set_count(state->ready_count, ready);
    set_count(state->installable_count, installable);
    set_count(state->unavailable_count, unavailable);

    if (state->native_count != nullptr) {
        const std::string text =
            std::to_string(native_installed) + " / " +
            std::to_string(native_total);
        gtk_label_set_text(GTK_LABEL(state->native_count), text.c_str());
    }

    if (state->coverage_count != nullptr) {
        const std::string text = std::to_string(state->rows.size());
        gtk_label_set_text(GTK_LABEL(state->coverage_count), text.c_str());
    }

    apply_filter(state);
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

GtkWidget* make_text_label(const char* text,
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

GtkWidget* create_row(AppState* state,
                      const fs::FilesystemDescriptor& descriptor,
                      RowState* row)
{
    row->app = state;
    row->descriptor = &descriptor;

    GtkWidget* list_row = gtk_list_box_row_new();
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget* icon = make_icon_well(
        "drive-harddisk-symbolic", 26, "fs-icon-well");
    GtkWidget* middle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(list_row), "fs-row");
    gtk_widget_set_size_request(right, 176, -1);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 14);
    gtk_container_add(GTK_CONTAINER(list_row), outer);

    GtkWidget* title =
        make_text_label(std::string(descriptor.name).c_str(),
                        "fs-row-title");

    GtkWidget* family =
        make_text_label(std::string(descriptor.family).c_str(),
                        "fs-family-badge");

    gtk_box_pack_start(GTK_BOX(heading), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), family, FALSE, FALSE, 0);

    if (descriptor.project_native_linux) {
        GtkWidget* native =
            make_text_label("INFILTRATOR NATIVE", "fs-native-badge");
        gtk_box_pack_start(GTK_BOX(heading), native, FALSE, FALSE, 0);
    }

    GtkWidget* description =
        make_text_label(std::string(descriptor.description).c_str(),
                        "fs-row-description");
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);

    const std::string meta =
        std::string("Capability: ") +
        fs::access_mode_label(descriptor.access) +
        "   •   Provider: " +
        fs::support_provider_label(descriptor.provider);

    GtkWidget* metadata =
        make_text_label(meta.c_str(), "fs-row-meta");
    gtk_label_set_line_wrap(GTK_LABEL(metadata), TRUE);

    row->detail = make_text_label("", "fs-row-detail");
    gtk_label_set_line_wrap(GTK_LABEL(row->detail), TRUE);

    gtk_box_pack_start(GTK_BOX(middle), heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(middle), description, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(middle), metadata, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(middle), row->detail, FALSE, FALSE, 0);

    row->status = make_text_label("", nullptr, 1.0F);
    gtk_widget_set_halign(row->status, GTK_ALIGN_END);

    row->package_button = gtk_button_new_with_label("Checking…");
    gtk_widget_set_no_show_all(row->package_button, TRUE);
    set_button_semantics(row->package_button, false, false);
    g_signal_connect(
        row->package_button,
        "clicked",
        G_CALLBACK(package_clicked),
        row);

    row->module_button = gtk_button_new_with_label("Checking kernel…");
    gtk_widget_set_no_show_all(row->module_button, TRUE);
    set_button_semantics(row->module_button, false, false);
    g_signal_connect(
        row->module_button,
        "clicked",
        G_CALLBACK(module_clicked),
        row);

    gtk_box_pack_start(GTK_BOX(right), row->status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(right), row->module_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(right), row->package_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), middle, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(outer), right, FALSE, FALSE, 0);

    if (!descriptor.note.empty()) {
        gtk_widget_set_tooltip_text(
            list_row, std::string(descriptor.note).c_str());
    }

    row->row = list_row;
    refresh_row(row);
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

    const char* query =
        gtk_entry_get_text(GTK_ENTRY(state->search));
    if (state->filter == ViewFilter::Home &&
        query != nullptr && *query != '\0' &&
        state->all_row != nullptr) {
        gtk_list_box_select_row(
            GTK_LIST_BOX(state->navigation),
            GTK_LIST_BOX_ROW(state->all_row));
        return;
    }

    apply_filter(state);
}

GtkWidget* build_header(AppState* state)
{
    GtkWidget* header = gtk_header_bar_new();
    GtkWidget* brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* icon =
        make_icon_well(kProjectInfo.icon_name, 28, "header-brand-icon");
    GtkWidget* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* header_end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* search = gtk_search_entry_new();
    GtkWidget* minimize =
        make_window_control("window-minimize-symbolic", "Minimize", nullptr);
    GtkWidget* maximize =
        make_window_control("window-maximize-symbolic", "Maximize / Restore", nullptr);
    GtkWidget* close =
        make_window_control("window-close-symbolic", "Close", "window-control-close");

    gtk_style_context_add_class(
        gtk_widget_get_style_context(header), "shell-header");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), FALSE);
    gtk_header_bar_set_custom_title(
        GTK_HEADER_BAR(header), gtk_label_new(""));

    gtk_style_context_add_class(
        gtk_widget_get_style_context(brand), "header-brand");
    gtk_box_pack_start(GTK_BOX(brand), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(copy),
        make_text_label("Filesystem Support", "header-brand-title"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(copy),
        make_text_label("Infiltrator OS", "header-brand-subtitle"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand), copy, FALSE, FALSE, 0);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), brand);

    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search), "Search filesystems…");
    gtk_widget_set_size_request(search, 310, -1);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(search), "settings-search");
    state->search = search;
    g_signal_connect(
        search, "search-changed", G_CALLBACK(search_changed), state);

    g_signal_connect(
        minimize, "clicked", G_CALLBACK(minimize_window), state->window);
    g_signal_connect(
        maximize, "clicked", G_CALLBACK(toggle_maximize_window), state->window);
    g_signal_connect(
        close, "clicked", G_CALLBACK(close_window), state->window);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(header_end), "header-end");
    gtk_box_pack_start(GTK_BOX(header_end), search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), minimize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), maximize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_end), close, FALSE, FALSE, 0);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), header_end);

    return header;
}

GtkWidget* make_navigation_heading(const char* text)
{
    GtkWidget* row = gtk_list_box_row_new();
    GtkWidget* label = make_text_label(text, "nav-title");
    gtk_container_set_border_width(GTK_CONTAINER(row), 5);
    gtk_container_add(GTK_CONTAINER(row), label);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(row), "nav-heading-row");
    return row;
}

GtkWidget* make_navigation_row(const char* icon_name,
                               const char* title,
                               const char* subtitle,
                               const ViewFilter filter,
                               const char* accent_class)
{
    GtkWidget* row = gtk_list_box_row_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* icon = make_icon_well(icon_name, 24, "nav-icon-well");
    GtkWidget* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);

    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(copy),
        make_text_label(title, "nav-primary"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(copy),
        make_text_label(subtitle, "nav-secondary"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), copy, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(row), "nav-row");
    if (accent_class != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(row), accent_class);
    }

    g_object_set_data(
        G_OBJECT(row),
        "view-filter",
        GINT_TO_POINTER(static_cast<int>(filter) + 1));
    return row;
}

void navigation_selected(GtkListBox*,
                         GtkListBoxRow* row,
                         gpointer user_data)
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
    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_widget_set_size_request(sidebar, 300, -1);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(sidebar), "settings-sidebar");

    gtk_list_box_set_selection_mode(
        GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(list), "nav-list");

    gtk_container_add(
        GTK_CONTAINER(list), make_navigation_heading("SYSTEM"));

    GtkWidget* home = make_navigation_row(
        "go-home-symbolic",
        "Home",
        "Overview & quick access",
        ViewFilter::Home,
        "nav-gold");
    gtk_container_add(GTK_CONTAINER(list), home);

    GtkWidget* all = make_navigation_row(
        "view-grid-symbolic",
        "All Filesystems",
        "Complete support catalogue",
        ViewFilter::All,
        "nav-cyan");
    state->all_row = all;
    gtk_container_add(GTK_CONTAINER(list), all);

    gtk_container_add(
        GTK_CONTAINER(list), make_navigation_heading("PLATFORMS"));

    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "computer-symbolic", "Linux", "Native & Linux-integrated",
            ViewFilter::Linux, "nav-cyan"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "computer-symbolic", "Microsoft & DOS", "Windows storage formats",
            ViewFilter::Microsoft, "nav-gold"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "media-floppy-symbolic", "Amiga", "OFS, FFS, SFS & more",
            ViewFilter::Amiga, "nav-gold"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "computer-symbolic", "Apple", "HFS, APFS & Apple access",
            ViewFilter::Apple, "nav-cyan"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "utilities-terminal-symbolic", "Unix & BSD", "Unix workstation formats",
            ViewFilter::UnixBsd, "nav-gold"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "media-optical-symbolic", "Images & FUSE", "Images, overlays & FUSE",
            ViewFilter::ImagesFuse, "nav-cyan"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "network-wired-symbolic", "Network & Virtual", "Remote, cloud & VM storage",
            ViewFilter::NetworkVirtual, "nav-cyan"));

    gtk_container_add(
        GTK_CONTAINER(list), make_navigation_heading("STATUS"));

    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "applications-engineering-symbolic",
            "Infiltrator Native",
            "Project-native kernel modules",
            ViewFilter::Native,
            "nav-cyan"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "emblem-ok-symbolic", "Installed", "Ready on this system",
            ViewFilter::Installed, "nav-green"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "system-software-install-symbolic", "Available", "Ready to install",
            ViewFilter::Available, "nav-cyan"));
    gtk_container_add(
        GTK_CONTAINER(list),
        make_navigation_row(
            "dialog-warning-symbolic", "Needs Attention", "Unavailable or incomplete",
            ViewFilter::Attention, "nav-gold"));

    g_signal_connect(
        list,
        "row-selected",
        G_CALLBACK(navigation_selected),
        state);

    state->navigation = list;
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_container_add(GTK_CONTAINER(scroller), list);
    gtk_box_pack_start(GTK_BOX(sidebar), scroller, TRUE, TRUE, 0);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(footer), "sidebar-footer");

    const std::string version =
        std::string("Version ") + kProjectInfo.version;
    gtk_box_pack_start(
        GTK_BOX(footer),
        make_text_label(version.c_str(), "sidebar-version"),
        FALSE, FALSE, 0);

    GtkWidget* about = gtk_button_new_with_label("About Filesystem Support");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(about), "sidebar-about");
    g_signal_connect(
        about, "clicked", G_CALLBACK(about_clicked), state);
    gtk_box_pack_start(GTK_BOX(footer), about, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sidebar), footer, FALSE, FALSE, 0);

    gtk_list_box_select_row(
        GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(home));
    return sidebar;
}

GtkWidget* make_feature(const char* icon_name,
                        const char* title,
                        const char* copy)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget* icon = gtk_image_new_from_icon_name(
        icon_name, GTK_ICON_SIZE_BUTTON);
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(text),
        make_text_label(title, "home-feature-title"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(text),
        make_text_label(copy, "home-feature-copy"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(box), "home-feature");
    return box;
}

GtkWidget* make_metric_tile(const char* icon_name,
                            const char* caption,
                            const char* copy,
                            GtkWidget** value_out,
                            const char* semantic_class)
{
    GtkWidget* tile = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 11);
    GtkWidget* icon = make_icon_well(icon_name, 22, "metric-icon");
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget* value = make_text_label("—", "metric-value");

    gtk_widget_set_hexpand(tile, TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(tile), "metric-tile");
    if (semantic_class != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(tile), semantic_class);
    }

    gtk_box_pack_start(GTK_BOX(text), make_text_label(caption, "metric-caption"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), make_text_label(copy, "metric-copy"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tile), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tile), text, TRUE, TRUE, 0);

    *value_out = value;
    return tile;
}

void quick_nav_clicked(GtkButton* button, gpointer user_data)
{
    auto* state = static_cast<AppState*>(user_data);
    const int encoded = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "view-filter"));
    if (state == nullptr || encoded == 0) {
        return;
    }

    const ViewFilter target =
        static_cast<ViewFilter>(encoded - 1);
    GList* children =
        gtk_container_get_children(GTK_CONTAINER(state->navigation));
    for (GList* item = children; item != nullptr; item = item->next) {
        GtkWidget* row = GTK_WIDGET(item->data);
        const int row_filter = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(row), "view-filter"));
        if (row_filter == encoded) {
            gtk_list_box_select_row(
                GTK_LIST_BOX(state->navigation),
                GTK_LIST_BOX_ROW(row));
            break;
        }
    }
    g_list_free(children);

    if (state->filter != target) {
        state->filter = target;
        gtk_stack_set_visible_child_name(
            GTK_STACK(state->stack), "catalogue");
        apply_filter(state);
    }
}

GtkWidget* make_quick_action(AppState* state,
                             const char* icon_name,
                             const char* title,
                             const char* copy,
                             const ViewFilter filter,
                             const char* accent_class)
{
    GtkWidget* button = gtk_button_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* icon = make_icon_well(icon_name, 24, "quick-action-icon");
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget* arrow = gtk_image_new_from_icon_name(
        "go-next-symbolic", GTK_ICON_SIZE_BUTTON);

    gtk_box_pack_start(GTK_BOX(text), make_text_label(title, "quick-action-title"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), make_text_label(copy, "quick-action-copy"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(box), arrow, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(button), box);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(button), "quick-action");
    if (accent_class != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(button), accent_class);
    }

    g_object_set_data(
        G_OBJECT(button),
        "view-filter",
        GINT_TO_POINTER(static_cast<int>(filter) + 1));
    g_signal_connect(
        button, "clicked", G_CALLBACK(quick_nav_clicked), state);
    return button;
}

GtkWidget* make_section_heading(const char* icon_name,
                                const char* title,
                                const char* copy)
{
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* icon = make_icon_well(icon_name, 21, "section-icon");
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_box_pack_start(
        GTK_BOX(text),
        make_text_label(title, "home-card-title"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(text),
        make_text_label(copy, "home-card-copy"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(row), "section-heading");
    return row;
}

GtkWidget* build_home_page(AppState* state)
{
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget* hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget* hero_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget* features = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* hero_mark = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    GtkWidget* grid = gtk_grid_new();
    GtkWidget* overview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* metrics = gtk_grid_new();
    GtkWidget* quick = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* quick_grid = gtk_grid_new();
    GtkWidget* kernel_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget* coverage_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(page), "home-page");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(hero), "home-hero");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(hero_mark), "hero-mark");

    gtk_box_pack_start(
        GTK_BOX(hero_left),
        make_text_label("SYSTEM STORAGE", "home-hero-eyebrow"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(hero_left),
        make_text_label("Welcome to", "home-hero-title"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(hero_left),
        make_text_label("Filesystem Support", "home-hero-accent"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(hero_left),
        make_text_label(
            "Discover, install and manage filesystem support without memorising packages or kernel modules.",
            "home-hero-subtitle"),
        FALSE, FALSE, 0);

    gtk_box_pack_start(
        GTK_BOX(features),
        make_feature("system-search-symbolic", "Discover", "108 support definitions"),
        TRUE, TRUE, 0);
    gtk_box_pack_start(
        GTK_BOX(features),
        make_feature("system-software-install-symbolic", "Install", "Packages and drivers"),
        TRUE, TRUE, 0);
    gtk_box_pack_start(
        GTK_BOX(features),
        make_feature("applications-engineering-symbolic", "Native", "Project kernel modules"),
        TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hero_left), features, FALSE, FALSE, 8);

    GtkWidget* mark_icon =
        gtk_image_new_from_icon_name("drive-harddisk-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(mark_icon), 56);
    gtk_widget_set_halign(mark_icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(hero_mark), mark_icon, TRUE, TRUE, 0);
    gtk_box_pack_start(
        GTK_BOX(hero_mark),
        make_text_label("INFILTRATOR OS", "hero-mark-title", 0.5F),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(hero_mark),
        make_text_label("FILESYSTEM CONTROL", "hero-mark-copy", 0.5F),
        FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hero), hero_left, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hero), hero_mark, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), hero, FALSE, FALSE, 0);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_hexpand(grid, TRUE);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(overview), "home-card");
    gtk_box_pack_start(
        GTK_BOX(overview),
        make_section_heading(
            "view-grid-symbolic",
            "Support Overview",
            "Live state for this system"),
        FALSE, FALSE, 0);

    gtk_grid_set_row_spacing(GTK_GRID(metrics), 8);
    gtk_grid_set_column_spacing(GTK_GRID(metrics), 8);
    gtk_grid_attach(
        GTK_GRID(metrics),
        make_metric_tile(
            "emblem-ok-symbolic", "INSTALLED", "Ready now",
            &state->ready_count, "metric-installed"),
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(metrics),
        make_metric_tile(
            "system-software-install-symbolic", "AVAILABLE", "Can be installed",
            &state->installable_count, "metric-available"),
        1, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(metrics),
        make_metric_tile(
            "dialog-warning-symbolic", "NEEDS ATTENTION", "Unavailable / incomplete",
            &state->unavailable_count, "metric-warning"),
        0, 1, 1, 1);
    gtk_grid_attach(
        GTK_GRID(metrics),
        make_metric_tile(
            "applications-engineering-symbolic", "NATIVE MODULES", "Installed / managed",
            &state->native_count, "metric-native"),
        1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(overview), metrics, TRUE, TRUE, 0);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(quick), "home-card");
    gtk_box_pack_start(
        GTK_BOX(quick),
        make_section_heading(
            "system-run-symbolic",
            "Quick Actions",
            "Jump straight to the support you need"),
        FALSE, FALSE, 0);

    gtk_grid_set_row_spacing(GTK_GRID(quick_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(quick_grid), 8);
    gtk_grid_attach(
        GTK_GRID(quick_grid),
        make_quick_action(
            state, "view-grid-symbolic", "All Filesystems",
            "Browse the full catalogue", ViewFilter::All, "quick-cyan"),
        0, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(quick_grid),
        make_quick_action(
            state, "emblem-ok-symbolic", "Installed",
            "See support ready now", ViewFilter::Installed, "quick-green"),
        1, 0, 1, 1);
    gtk_grid_attach(
        GTK_GRID(quick_grid),
        make_quick_action(
            state, "applications-engineering-symbolic", "Native Modules",
            "Manage Infiltrator drivers", ViewFilter::Native, "quick-cyan"),
        0, 1, 1, 1);
    gtk_grid_attach(
        GTK_GRID(quick_grid),
        make_quick_action(
            state, "dialog-warning-symbolic", "Needs Attention",
            "Review incomplete support", ViewFilter::Attention, "quick-gold"),
        1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(quick), quick_grid, TRUE, TRUE, 0);

    gtk_grid_attach(GTK_GRID(grid), overview, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), quick, 1, 0, 1, 1);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(kernel_card), "home-card");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(kernel_card), "kernel-card");
    gtk_box_pack_start(
        GTK_BOX(kernel_card),
        make_section_heading(
            "applications-engineering-symbolic",
            "Kernel Integration",
            "Native support follows the running kernel"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(kernel_card),
        make_text_label("RUNNING KERNEL", "info-caption"),
        FALSE, FALSE, 0);
    const std::string kernel = running_kernel();
    gtk_box_pack_start(
        GTK_BOX(kernel_card),
        make_text_label(kernel.c_str(), "info-value"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(kernel_card),
        make_text_label(
            "Project-native drivers are compiled, installed and verified against this kernel.",
            "info-copy"),
        FALSE, FALSE, 0);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(coverage_card), "home-card");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(coverage_card), "coverage-card");
    gtk_box_pack_start(
        GTK_BOX(coverage_card),
        make_section_heading(
            "folder-documents-symbolic",
            "Catalogue Coverage",
            "Kernel, userspace, FUSE and tools"),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(coverage_card),
        make_text_label("FILESYSTEM DEFINITIONS", "info-caption"),
        FALSE, FALSE, 0);
    state->coverage_count = make_text_label("—", "info-value");
    gtk_box_pack_start(
        GTK_BOX(coverage_card), state->coverage_count, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(coverage_card),
        make_text_label(
            "One catalogue unifies package availability, kernel modules and project-native drivers.",
            "info-copy"),
        FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid), kernel_card, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), coverage_card, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), page);
    return scroller;
}

void refresh_clicked(GtkButton*, gpointer user_data)
{
    refresh_all(static_cast<AppState*>(user_data));
}

GtkWidget* build_catalogue_page(AppState* state)
{
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* identity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* refresh = gtk_button_new_with_label("Refresh support scan");
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* list = gtk_list_box_new();

    gtk_style_context_add_class(
        gtk_widget_get_style_context(page), "catalogue-page");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(header), "catalogue-header");

    gtk_box_pack_start(
        GTK_BOX(identity),
        make_text_label("FILESYSTEM CATALOGUE", "page-eyebrow"),
        FALSE, FALSE, 0);

    state->catalogue_title =
        make_text_label("All Filesystems", "page-title");
    state->catalogue_summary =
        make_text_label("", "page-summary");
    gtk_label_set_line_wrap(
        GTK_LABEL(state->catalogue_summary), TRUE);

    gtk_box_pack_start(
        GTK_BOX(identity), state->catalogue_title, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(identity), state->catalogue_summary, FALSE, FALSE, 0);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(refresh), "catalogue-refresh");
    g_signal_connect(
        refresh, "clicked", G_CALLBACK(refresh_clicked), state);

    gtk_box_pack_start(GTK_BOX(header), identity, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(header), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(list), "fs-list");
    gtk_list_box_set_selection_mode(
        GTK_LIST_BOX(list), GTK_SELECTION_NONE);

    for (const auto& descriptor : fs::catalog()) {
        auto row = std::make_unique<RowState>();
        gtk_container_add(
            GTK_CONTAINER(list),
            create_row(state, descriptor, row.get()));
        state->rows.push_back(std::move(row));
    }

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(scroller), "fs-scroller");
    gtk_container_add(GTK_CONTAINER(scroller), list);
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);
    return page;
}

void install_common_theme()
{
    const InfiltratrThemePalette* palette =
        infiltratr_theme_resolve(
            INFILTRATR_THEME_SYSTEM, system_prefers_dark());
    const InfiltratrTypography* type = infiltratr_typography();
    GdkScreen* screen = gdk_screen_get_default();

    if (palette == nullptr || type == nullptr || screen == nullptr) {
        return;
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
    const std::string accent = rgb_hex(palette->neutral_accent_rgb);
    const std::string accent_foreground =
        rgb_hex(palette->accent_foreground_rgb);
    const std::string accent_hover = rgb_hex(palette->accent_hover_rgb);
    const std::string success = rgb_hex(palette->success_rgb);
    const std::string warning = rgb_hex(palette->warning_rgb);
    const std::string fault = rgb_hex(palette->fault_rgb);
    const std::string hover = rgb_hex(palette->card_hover_rgb);
    const std::string operation = rgb_hex(palette->operation_rgb);

    std::ostringstream css;
    css
        << "window { background: " << background << "; color: " << text
        << "; font-family: \"" << type->ui_family << "\", "
        << type->gtk_fallback << "; font-weight: "
        << type->ui_regular_weight << "; }\n"
        << ".app-shell, .fs-scroller, .fs-scroller viewport { background: "
        << background << "; }\n"
        << ".shell-header { background-image: linear-gradient(to right, #06131f, #08263a); "
           "border-bottom: 1px solid " << border << "; min-height: 58px; padding: 6px 10px; }\n"
        << ".header-brand { padding: 2px 4px; }\n"
        << ".header-brand-icon { min-width: 40px; min-height: 40px; background: " << card
        << "; border: 1px solid " << border
        << "; border-radius: 12px; padding: 6px; box-shadow: 0 0 18px rgba(0,183,255,0.18); }\n"
        << ".header-brand-icon image { color: " << accent << "; }\n"
        << ".header-brand-title { color: " << title << "; font-size: 19px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".header-brand-subtitle { color: " << muted << "; font-size: 10px; }\n"
        << ".settings-search { min-width: 300px; background: " << input
        << "; color: " << text << "; border: 1px solid " << border
        << "; border-radius: 14px; padding: 8px 12px; }\n"
        << ".settings-search:focus { border-color: " << accent << "; }\n"
        << ".header-end { margin-left: 10px; }\n"
        << ".window-control { min-width: 30px; min-height: 30px; padding: 4px; "
           "background: transparent; border: 1px solid transparent; border-radius: 8px; }\n"
        << ".window-control:hover { background: " << hover << "; border-color: " << border << "; }\n"
        << ".window-control-close:hover { background: " << fault << "; color: "
        << accent_foreground << "; }\n"

        << ".settings-sidebar { min-width: 300px; background-image: linear-gradient(to bottom, "
        << panel << ", " << background << "); border-right: 1px solid " << border
        << "; padding: 14px 10px 12px 10px; }\n"
        << ".nav-list, .nav-list row { background: transparent; }\n"
        << ".nav-heading-row { background: transparent; }\n"
        << ".nav-title { color: " << warning
        << "; font-size: 9px; font-weight: " << type->ui_bold_weight
        << "; letter-spacing: 0.10em; margin: 8px 7px 2px 7px; }\n"
        << ".nav-row { min-height: 54px; border: 1px solid transparent; border-radius: 12px; "
           "padding: 6px 8px; margin: 2px 4px; }\n"
        << ".nav-row:hover { background: " << hover << "; }\n"
        << ".nav-row:selected { background-image: linear-gradient(to right, rgba(0,174,255,0.88), "
           "rgba(0,112,194,0.72)); border-color: #47d4ff; "
           "box-shadow: inset 0 0 0 1px #47d4ff, 0 0 18px rgba(0,183,255,0.18); }\n"
        << ".nav-icon-well { min-width: 42px; min-height: 42px; background: " << surface
        << "; border: 1px solid " << border << "; border-radius: 12px; padding: 5px; }\n"
        << ".nav-icon-well image { color: " << accent << "; }\n"
        << ".nav-gold .nav-icon-well image { color: " << warning << "; }\n"
        << ".nav-green .nav-icon-well image { color: " << success << "; }\n"
        << ".nav-row:selected .nav-icon-well { background: rgba(3,18,28,0.46); "
           "border-color: rgba(255,255,255,0.28); }\n"
        << ".nav-row:selected .nav-icon-well image, .nav-row:selected .nav-primary, "
           ".nav-row:selected .nav-secondary { color: #ffffff; }\n"
        << ".nav-primary { color: " << text << "; font-size: 13px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".nav-secondary { color: " << muted << "; font-size: 9px; }\n"
        << ".sidebar-footer { border-top: 1px solid " << border
        << "; padding: 10px 8px 0 8px; margin-top: 8px; }\n"
        << ".sidebar-version { color: " << subtle << "; font-size: 9px; }\n"
        << ".sidebar-about { background: transparent; color: " << text
        << "; border: 1px solid transparent; border-radius: 8px; padding: 7px 9px; }\n"
        << ".sidebar-about:hover { background: " << hover << "; border-color: " << border << "; }\n"

        << ".home-page { padding: 18px 20px 24px 20px; }\n"
        << ".home-hero { min-height: 224px; background-image: linear-gradient(115deg, #06131f, "
           "#08263a 62%, #10171d); border: 1px solid " << border
        << "; border-radius: 20px; padding: 22px 24px; }\n"
        << ".home-hero-eyebrow { color: " << warning
        << "; font-size: 9px; font-weight: " << type->ui_bold_weight
        << "; letter-spacing: 0.12em; }\n"
        << ".home-hero-title { color: " << title << "; font-size: 34px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".home-hero-accent { color: " << warning << "; font-size: 34px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".home-hero-subtitle { color: " << muted << "; font-size: 13px; margin-top: 4px; }\n"
        << ".home-feature { min-height: 48px; background: rgba(4,15,24,0.68); "
           "border: 1px solid rgba(86,176,219,0.34); border-radius: 12px; padding: 8px 11px; }\n"
        << ".home-feature image { color: " << accent << "; }\n"
        << ".home-feature-title { color: " << title << "; font-weight: "
        << type->ui_bold_weight << "; font-size: 11px; }\n"
        << ".home-feature-copy { color: " << muted << "; font-size: 9px; }\n"
        << ".hero-mark { min-width: 260px; min-height: 170px; background: rgba(2,9,15,0.48); "
           "border: 1px solid rgba(70,210,255,0.38); border-radius: 18px; padding: 14px 18px; }\n"
        << ".hero-mark image { color: " << accent << "; }\n"
        << ".hero-mark-title { color: " << title << "; font-size: 16px; font-weight: "
        << type->ui_bold_weight << "; letter-spacing: 0.08em; }\n"
        << ".hero-mark-copy { color: " << muted << "; font-size: 9px; letter-spacing: 0.08em; }\n"

        << ".home-card { background-image: linear-gradient(145deg, " << card << ", " << panel
        << "); border: 1px solid " << border << "; border-radius: 17px; padding: 16px; }\n"
        << ".home-card:hover { border-color: " << accent << "; }\n"
        << ".home-card-title { color: " << title << "; font-size: 16px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".home-card-copy { color: " << muted << "; font-size: 10px; }\n"
        << ".section-heading { margin-bottom: 5px; }\n"
        << ".section-icon { min-width: 34px; min-height: 34px; background: " << surface
        << "; border: 1px solid " << border << "; border-radius: 10px; padding: 5px; }\n"
        << ".section-icon image { color: " << warning << "; }\n"
        << ".metric-tile { min-height: 74px; background: " << surface
        << "; border: 1px solid " << border << "; border-radius: 12px; padding: 10px; }\n"
        << ".metric-icon { min-width: 36px; min-height: 36px; background: " << card
        << "; border: 1px solid " << border << "; border-radius: 10px; padding: 5px; }\n"
        << ".metric-caption { color: " << subtle << "; font-size: 8px; font-weight: "
        << type->ui_bold_weight << "; letter-spacing: 0.08em; }\n"
        << ".metric-value { color: " << title << "; font-size: 19px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".metric-copy { color: " << muted << "; font-size: 9px; }\n"
        << ".metric-installed .metric-value, .metric-installed .metric-icon image { color: "
        << success << "; }\n"
        << ".metric-available .metric-value, .metric-available .metric-icon image, "
           ".metric-native .metric-value, .metric-native .metric-icon image { color: "
        << accent << "; }\n"
        << ".metric-warning .metric-value, .metric-warning .metric-icon image { color: "
        << warning << "; }\n"

        << ".quick-action { min-height: 70px; background: " << surface
        << "; border: 1px solid " << border << "; border-radius: 13px; padding: 8px 10px; }\n"
        << ".quick-action:hover { border-color: " << accent
        << "; box-shadow: 0 0 18px rgba(0,183,255,0.14); }\n"
        << ".quick-action-icon { min-width: 42px; min-height: 42px; background: rgba(4,17,27,0.74); "
           "border: 1px solid rgba(85,189,235,0.30); border-radius: 11px; padding: 5px; }\n"
        << ".quick-action-icon image { color: " << accent << "; }\n"
        << ".quick-gold .quick-action-icon image { color: " << warning << "; }\n"
        << ".quick-green .quick-action-icon image { color: " << success << "; }\n"
        << ".quick-action-title { color: " << title << "; font-weight: "
        << type->ui_bold_weight << "; font-size: 11px; }\n"
        << ".quick-action-copy { color: " << muted << "; font-size: 9px; }\n"
        << ".info-caption { color: " << subtle << "; font-size: 8px; font-weight: "
        << type->ui_bold_weight << "; letter-spacing: 0.08em; }\n"
        << ".info-value { color: " << title << "; font-size: 21px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".info-copy { color: " << muted << "; font-size: 10px; }\n"
        << ".kernel-card { border-top: 2px solid " << accent << "; }\n"
        << ".coverage-card { border-top: 2px solid " << warning << "; }\n"

        << ".catalogue-page { padding: 22px 24px 22px 24px; }\n"
        << ".catalogue-header { background-image: linear-gradient(145deg, " << card << ", "
        << panel << "); border: 1px solid " << border
        << "; border-radius: 17px; padding: 16px 18px; }\n"
        << ".page-eyebrow { color: " << warning << "; font-size: 9px; font-weight: "
        << type->ui_bold_weight << "; letter-spacing: 0.11em; }\n"
        << ".page-title { color: " << title << "; font-size: 28px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".page-summary { color: " << muted << "; font-size: 11px; }\n"
        << ".catalogue-refresh { background: " << operation << "; color: " << text
        << "; border: 1px solid " << border << "; border-radius: 10px; padding: 8px 12px; }\n"
        << ".catalogue-refresh:hover { background: " << hover << "; border-color: " << accent << "; }\n"

        << ".fs-list, .fs-list row { background: transparent; }\n"
        << ".fs-row { background-image: linear-gradient(145deg, " << card << ", " << panel
        << "); border: 1px solid " << border << "; border-radius: 15px; margin: 0 0 9px 0; }\n"
        << ".fs-row:hover { border-color: " << accent << "; background: " << hover << "; }\n"
        << ".fs-icon-well { min-width: 48px; min-height: 48px; background: " << surface
        << "; border: 1px solid " << border << "; border-radius: 13px; padding: 7px; }\n"
        << ".fs-icon-well image { color: " << accent << "; }\n"
        << ".fs-row-title { color: " << title << "; font-size: 15px; font-weight: "
        << type->ui_bold_weight << "; }\n"
        << ".fs-family-badge { background: rgba(9,37,51,0.76); color: " << accent
        << "; border: 1px solid rgba(70,210,255,0.28); border-radius: 999px; padding: 3px 8px; "
           "font-size: 9px; }\n"
        << ".fs-native-badge { background: rgba(79,54,10,0.72); color: " << warning
        << "; border: 1px solid rgba(218,164,58,0.40); border-radius: 999px; padding: 3px 8px; "
           "font-size: 8px; font-weight: " << type->ui_bold_weight << "; }\n"
        << ".fs-row-description { color: " << text << "; font-size: 11px; }\n"
        << ".fs-row-meta { color: " << subtle << "; font-size: 9px; }\n"
        << ".fs-row-detail { color: " << muted << "; font-size: 9px; }\n"
        << ".fs-status { background: " << surface << "; color: " << muted
        << "; border: 1px solid " << border << "; border-radius: 999px; padding: 4px 9px; "
           "font-size: 9px; font-weight: " << type->ui_bold_weight << "; }\n"
        << ".fs-status-ready { color: " << success << "; border-color: " << success << "; }\n"
        << ".fs-status-installable { color: " << accent << "; border-color: " << accent << "; }\n"
        << ".fs-status-incomplete { color: " << warning << "; border-color: " << warning << "; }\n"
        << ".fs-status-unavailable { color: " << fault << "; border-color: " << fault << "; }\n"
        << "button.fs-action { background: " << operation << "; color: " << text
        << "; border: 1px solid " << border << "; border-radius: 10px; padding: 7px 10px; }\n"
        << "button.fs-action:hover { background: " << hover << "; border-color: " << accent << "; }\n"
        << "button.primary-action { background-image: linear-gradient(to right, " << accent
        << ", " << accent_hover << "); color: " << accent_foreground
        << "; border-color: " << accent << "; font-weight: " << type->ui_bold_weight << "; }\n"
        << "button.danger-action { color: " << fault << "; }\n"
        << "button:disabled { opacity: 0.55; }\n"

        << "scrollbar { background: transparent; min-width: 10px; min-height: 10px; }\n"
        << "scrollbar slider { min-width: 8px; min-height: 28px; border-radius: 999px; background: "
        << subtle << "; }\n"
        << "scrollbar slider:hover { background: " << accent << "; }\n";

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

    state->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state->window), kProjectInfo.program_name);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1460, 900);
    gtk_window_set_icon_name(GTK_WINDOW(state->window), kProjectInfo.icon_name);

    GdkGeometry geometry {};
    geometry.min_width = 1024;
    geometry.min_height = 680;
    gtk_window_set_geometry_hints(
        GTK_WINDOW(state->window),
        state->window,
        &geometry,
        GDK_HINT_MIN_SIZE);

    GtkWidget* header = build_header(state);
    gtk_window_set_titlebar(GTK_WINDOW(state->window), header);

    GtkWidget* shell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(shell), "app-shell");
    gtk_container_add(GTK_CONTAINER(state->window), shell);

    state->stack = gtk_stack_new();
    gtk_stack_set_transition_type(
        GTK_STACK(state->stack),
        GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(state->stack), 140);
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

    /*
     * Package/module buttons use no-show-all so refresh_row() remains the sole
     * authority over whether an action exists for a catalogue entry.
     */
    for (const auto& row : state->rows) {
        refresh_row(row.get());
    }
    update_summary(state);
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
