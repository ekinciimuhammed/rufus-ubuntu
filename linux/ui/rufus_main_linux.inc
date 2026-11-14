/*
 * Rufus for Linux: main window, message handling and program entry point
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This file is #included at the end of src/rufus.c on Linux, in place of the
 * Win32 specific InitDialog(), MainCallback() and WinMain(), so that it shares
 * rufus.c's state. The message handling below follows MainCallback() closely:
 * the GTK controls report their events through the same WM_COMMAND/UM_xxx
 * messages as on Windows (see linux/ui/winctl.c).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <getopt.h>
#include <sys/file.h>
#include "../linux/ui/winctl.h"
#include "../linux/core/linux_drive.h"

/* Linux platform layer */
extern const char* linux_get_data_dir(void);
extern const char* linux_locale_name(void);
extern void linux_set_log_sink(void (*sink)(const char* text));
extern const char* linux_invoking_user_home(void);
extern void linux_invoking_user_ids(uid_t* uid, gid_t* gid);
/* Dialogs (stdlg_linux.c) */
extern void linux_log_dialog_create(void);
extern void linux_log_dialog_toggle(void);
extern BOOL linux_log_dialog_visible(void);
#include "automation_linux.h"
extern char* linux_log_get_text(void);
extern void linux_settings_dialog(void);
static uint32_t wue_options;
extern int unattend_xml_mask;

/* NB: hStatus is defined in src/stdio.c */
static GtkWidget* main_window = NULL;
static GtkWidget* select_dropdown = NULL;
static BOOL quit_requested = FALSE;
static int lock_fd = -1;
static char private_temp_dir[MAX_PATH] = "";

/* ------------------------------------------------------------------------- */
/* Main window construction                                                  */
/* ------------------------------------------------------------------------- */
static const char* main_css =
	".rufus-section { font-weight: bold; font-size: 115%; }\n"
	".rufus-separator { margin-top: 8px; margin-bottom: 4px; }\n"
	"progressbar progress { background-image: none; background-color: #06B025; min-height: 22px; border-radius: 0; }\n"
	"progressbar trough { min-height: 22px; border-radius: 0; }\n"
	"progressbar.rufus-error progress { background-color: #DA2626; }\n"
	"progressbar.rufus-paused progress { background-color: #DACB26; }\n"
	".rufus-progress-text { font-weight: normal; }\n"
	".rufus-status { padding: 4px 6px; border-top: 1px solid alpha(@borders, 0.6); }\n"
	".rufus-timer { opacity: 0.6; }\n"
	".rufus-link { color: @link_color; }\n"
	".rufus-main-button { min-width: 90px; font-weight: bold; }\n";

static GtkWidget* load_icon(const char* name, int size)
{
	char path[MAX_PATH];
	GdkPixbuf* pb;
	snprintf(path, sizeof(path), "%s/icons/%s-%d.png", linux_get_data_dir(), name, size);
	pb = gdk_pixbuf_new_from_file(path, NULL);
	if (pb == NULL)
		return gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_SMALL_TOOLBAR);
	{
		GtkWidget* img = gtk_image_new_from_pixbuf(pb);
		g_object_unref(pb);
		return img;
	}
}

static GtkWidget* icon_button(const char* icon, int size)
{
	GtkWidget* b = gtk_button_new();
	gtk_button_set_image(GTK_BUTTON(b), load_icon(icon, size));
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus(b, FALSE);
	return b;
}

static GtkWidget* add_label(HWND hDlg, int id, const char* text)
{
	GtkWidget* l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	gtk_label_set_line_wrap(GTK_LABEL(l), FALSE);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	ctl_create(hDlg, id, CT_STATIC, l);
	return l;
}

static GtkWidget* add_combo(HWND hDlg, int id)
{
	GtkWidget* c = gtk_combo_box_text_new();
	gtk_widget_set_hexpand(c, TRUE);
	/* Long entries (e.g. the device names) must not widen the window */
	{
		GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(c));
		if (cells != NULL) {
			g_object_set(cells->data, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
			g_list_free(cells);
		}
	}
	gtk_widget_set_size_request(c, 60, -1);
	ctl_create(hDlg, id, CT_COMBO, c);
	return c;
}

static GtkWidget* add_check(HWND hDlg, int id, const char* text)
{
	GtkWidget* c = gtk_check_button_new_with_label(text);
	ctl_create(hDlg, id, CT_CHECK, c);
	return c;
}

static GtkWidget* section_header(HWND hDlg, int id, const char* text)
{
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget* l = add_label(hDlg, id, text);
	GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_NONE);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "rufus-section");
	gtk_widget_set_valign(sep, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(sep, TRUE);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), sep, TRUE, TRUE, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(box), "rufus-separator");
	return box;
}

static GtkWidget* toggle_button(HWND hDlg, int id, const char* text)
{
	GtkWidget* b = gtk_button_new_with_label(text);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name("pan-end-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	gtk_widget_set_can_focus(b, FALSE);
	ctl_create(hDlg, id, CT_BUTTON, b);
	return b;
}

void linux_ui_set_toggle_arrow(int toggle_id, BOOL expanded)
{
	GtkWidget* b = ctl_widget(GetDlgItem(hMainDialog, toggle_id));
	if (b != NULL && ui_is_main_thread())
		gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(expanded ? "pan-down-symbolic" : "pan-end-symbolic",
			GTK_ICON_SIZE_BUTTON));
}

/* Called from a worker thread when downloading ISOs (through Fido) becomes available */
static void show_select_dropdown(void* arg)
{
	if (select_dropdown != NULL)
		gtk_widget_set_visible(select_dropdown, GPOINTER_TO_INT(arg));
}

void linux_set_iso_download_available(BOOL available)
{
	ui_run_on_main_async(show_select_dropdown, GINT_TO_POINTER(available));
}

static void on_select_menu(GtkMenuItem* item, gpointer data)
{
	(void)item;
	SendMessage(hMainDialog, WM_COMMAND, (WPARAM)GPOINTER_TO_INT(data), 0);
}

static void on_select_dropdown(GtkButton* button, gpointer data)
{
	GtkWidget *menu = gtk_menu_new(), *item;
	int i;
	(void)data;
	for (i = 0; i < 2; i++) {
		item = gtk_check_menu_item_new_with_label(uppercase_select[i]);
		gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), select_index == i);
		g_signal_connect(item, "activate", G_CALLBACK(on_select_menu), GINT_TO_POINTER(IDM_SELECT + i));
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
}

static void on_csm_help_clicked(GtkGestureMultiPress* gesture, gint n, gdouble x, gdouble y, gpointer data)
{
	GtkWidget* w = ctl_widget(GetDlgItem(hMainDialog, IDS_CSM_HELP_TXT));
	(void)gesture; (void)n; (void)x; (void)y; (void)data;
	/* Show the tooltip, like the Windows link does */
	if (w != NULL)
		gtk_widget_trigger_tooltip_query(w);
}

static INT_PTR CALLBACK MainCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
static gboolean on_main_delete(GtkWidget* widget, GdkEvent* event, gpointer data);
static gboolean on_main_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data);
static void on_drag_data_received(GtkWidget* widget, GdkDragContext* context, gint x, gint y,
	GtkSelectionData* sel, guint info, guint time, gpointer data);

static HWND CreateMainDialog(void)
{
	HWND hDlg;
	GtkWidget *win, *vbox, *grid, *row, *w, *box, *status_box;
	GtkCssProvider* css;
	char path[MAX_PATH];
	int r = 0;
	static GtkTargetEntry drop_targets[] = { { "text/uri-list", 0, 0 } };

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	main_window = win;
	ui_set_main_window(GTK_WINDOW(win));
	gtk_window_set_title(GTK_WINDOW(win), APPLICATION_NAME);
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
	snprintf(path, sizeof(path), "%s/icons/rufus-128.png", linux_get_data_dir());
	gtk_window_set_icon_from_file(GTK_WINDOW(win), path, NULL);
	gtk_window_set_default_icon_name("rufus");
	css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css, main_css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);

	hDlg = ctl_create(NULL, IDD_DIALOG, CT_DIALOG, win);
	hMainDialog = hDlg;
	ctl_set_dialog_proc(hDlg, MainCallback);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(win), vbox);
	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
	g_object_set(grid, "margin", 12, "margin-bottom", 6, NULL);
	gtk_widget_set_size_request(grid, 440, -1);
	gtk_box_pack_start(GTK_BOX(vbox), grid, TRUE, TRUE, 0);

	/* Drive Properties */
	gtk_grid_attach(GTK_GRID(grid), section_header(hDlg, IDS_DRIVE_PROPERTIES_TXT, "Drive Properties"), 0, r++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), add_label(hDlg, IDS_DEVICE_TXT, "Device"), 0, r++, 2, 1);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(row), add_combo(hDlg, IDC_DEVICE), TRUE, TRUE, 0);
	hSaveToolbar = ctl_create(hDlg, IDC_SAVE_TOOLBAR, CT_TOOLBAR, NULL);
	w = icon_button("save", 16);
	gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
	ctl_toolbar_add(hSaveToolbar, IDC_SAVE, w);
	gtk_grid_attach(GTK_GRID(grid), row, 0, r++, 2, 1);

	gtk_grid_attach(GTK_GRID(grid), add_label(hDlg, IDS_BOOT_SELECTION_TXT, "Boot selection"), 0, r++, 2, 1);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(row), add_combo(hDlg, IDC_BOOT_SELECTION), TRUE, TRUE, 0);
	hHashToolbar = ctl_create(hDlg, IDC_HASH_TOOLBAR, CT_TOOLBAR, NULL);
	w = icon_button("hash", 16);
	gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
	ctl_toolbar_add(hHashToolbar, IDC_HASH, w);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(box), "linked");
	w = gtk_button_new_with_label("SELECT");
	gtk_widget_set_size_request(w, 90, -1);
	ctl_create(hDlg, IDC_SELECT, CT_BUTTON, w);
	gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
	select_dropdown = gtk_button_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_no_show_all(select_dropdown, TRUE);
	g_signal_connect(select_dropdown, "clicked", G_CALLBACK(on_select_dropdown), NULL);
	gtk_box_pack_start(GTK_BOX(box), select_dropdown, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), box, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), row, 0, r++, 2, 1);

	/* Image option / persistence (shown as needed) */
	gtk_grid_attach(GTK_GRID(grid), add_label(hDlg, IDS_IMAGE_OPTION_TXT, "Image option"), 0, r++, 2, 1);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(row), add_combo(hDlg, IDC_IMAGE_OPTION), TRUE, TRUE, 0);
	w = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(w), FALSE);
	gtk_widget_set_hexpand(w, TRUE);
	ctl_create(hDlg, IDC_PERSISTENCE_SLIDER, CT_SLIDER, w);
	gtk_box_pack_start(GTK_BOX(row), w, TRUE, TRUE, 0);
	w = gtk_entry_new();
	gtk_entry_set_width_chars(GTK_ENTRY(w), 14);
	gtk_entry_set_alignment(GTK_ENTRY(w), 1.0f);
	gtk_entry_set_input_purpose(GTK_ENTRY(w), GTK_INPUT_PURPOSE_DIGITS);
	ctl_create(hDlg, IDC_PERSISTENCE_SIZE, CT_EDIT, w);
	gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
	w = gtk_combo_box_text_new();
	ctl_create(hDlg, IDC_PERSISTENCE_UNITS, CT_COMBO, w);
	gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), row, 0, r++, 2, 1);

	/* Partition scheme and target system */
	gtk_grid_attach(GTK_GRID(grid), add_label(hDlg, IDS_PARTITION_TYPE_TXT, "Partition scheme"), 0, r, 1, 1);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(box), add_label(hDlg, IDS_TARGET_SYSTEM_TXT, "Target system"), TRUE, TRUE, 0);
	gtk_grid_attach(GTK_GRID(grid), box, 1, r++, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), add_combo(hDlg, IDC_PARTITION_TYPE), 0, r, 1, 1);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(box), add_combo(hDlg, IDC_TARGET_SYSTEM), TRUE, TRUE, 0);
	{
		GtkWidget* ev = gtk_event_box_new();
		GtkWidget* l = gtk_label_new("?");
		GtkGesture* g;
		gtk_style_context_add_class(gtk_widget_get_style_context(l), "rufus-link");
		gtk_container_add(GTK_CONTAINER(ev), l);
		gtk_widget_set_no_show_all(ev, TRUE);
		gtk_widget_show(l);
		ctl_create(hDlg, IDS_CSM_HELP_TXT, CT_STATIC, ev);
		g = gtk_gesture_multi_press_new(ev);
		g_signal_connect(g, "pressed", G_CALLBACK(on_csm_help_clicked), NULL);
		g_object_set_data_full(G_OBJECT(ev), "gesture", g, g_object_unref);
		gtk_box_pack_start(GTK_BOX(box), ev, FALSE, FALSE, 2);
	}
	gtk_grid_attach(GTK_GRID(grid), box, 1, r++, 1, 1);

	/* Advanced drive properties */
	hAdvancedDeviceToolbar = ctl_create(hDlg, IDC_ADVANCED_DEVICE_TOOLBAR, CT_TOOLBAR, NULL);
	gtk_grid_attach(GTK_GRID(grid), toggle_button(hDlg, IDC_ADVANCED_DEVICE_TOOLBAR, "Show advanced drive properties"), 0, r++, 2, 1);
	ctl_create(hDlg, IDC_ADVANCED_DRIVE_PROPERTIES, CT_CHECK, NULL);
	gtk_grid_attach(GTK_GRID(grid), add_check(hDlg, IDC_LIST_USB_HDD, "List USB Hard Drives"), 0, r++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), add_check(hDlg, IDC_OLD_BIOS_FIXES, "Add fixes for old BIOSes (extra partition, align, etc.)"), 0, r++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), add_check(hDlg, IDC_UEFI_MEDIA_VALIDATION, "Enable runtime UEFI media validation"), 0, r++, 2, 1);

	/* Format Options */
	gtk_grid_attach(GTK_GRID(grid), section_header(hDlg, IDS_FORMAT_OPTIONS_TXT, "Format Options"), 0, r++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), add_label(hDlg, IDS_LABEL_TXT, "Volume label"), 0, r++, 2, 1);
	w = gtk_entry_new();
	ctl_create(hDlg, IDC_LABEL, CT_EDIT, w);
	gtk_grid_attach(GTK_GRID(grid), w, 0, r++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), add_label(hDlg, IDS_FILE_SYSTEM_TXT, "File system"), 0, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), add_label(hDlg, IDS_CLUSTER_SIZE_TXT, "Cluster size"), 1, r++, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), add_combo(hDlg, IDC_FILE_SYSTEM), 0, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), add_combo(hDlg, IDC_CLUSTER_SIZE), 1, r++, 1, 1);

	/* Advanced format options */
	hAdvancedFormatToolbar = ctl_create(hDlg, IDC_ADVANCED_FORMAT_TOOLBAR, CT_TOOLBAR, NULL);
	gtk_grid_attach(GTK_GRID(grid), toggle_button(hDlg, IDC_ADVANCED_FORMAT_TOOLBAR, "Show advanced format options"), 0, r++, 2, 1);
	ctl_create(hDlg, IDC_ADVANCED_FORMAT_OPTIONS, CT_CHECK, NULL);
	gtk_grid_attach(GTK_GRID(grid), add_check(hDlg, IDC_QUICK_FORMAT, "Quick format"), 0, r++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), add_check(hDlg, IDC_EXTENDED_LABEL, "Create extended label and icon files"), 0, r++, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), add_check(hDlg, IDC_BAD_BLOCKS, "Check device for bad blocks"), 0, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), add_combo(hDlg, IDC_NB_PASSES), 1, r++, 1, 1);

	/* Status */
	gtk_grid_attach(GTK_GRID(grid), section_header(hDlg, IDS_STATUS_TXT, "Status"), 0, r++, 2, 1);
	{
		/* Like on Windows, the status text is displayed over the progress bar */
		GtkWidget* overlay = gtk_overlay_new();
		GtkWidget* label = gtk_label_new("");
		w = gtk_progress_bar_new();
		gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(w), FALSE);
		gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(w), 0.05);
		gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
		gtk_container_add(GTK_CONTAINER(overlay), w);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_style_context_add_class(gtk_widget_get_style_context(label), "rufus-progress-text");
		gtk_overlay_add_overlay(GTK_OVERLAY(overlay), label);
		gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), label, TRUE);
		g_object_set_data(G_OBJECT(w), "rufus-label", label);
		ctl_create(hDlg, IDC_PROGRESS, CT_PROGRESS, w);
		gtk_grid_attach(GTK_GRID(grid), overlay, 0, r++, 2, 1);
	}

	/* Bottom toolbar and main buttons */
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	g_object_set(row, "margin-top", 10, NULL);
	hMultiToolbar = ctl_create(hDlg, IDC_MULTI_TOOLBAR, CT_TOOLBAR, NULL);
	{
		static const struct { const char* icon; int id; } tb[] = {
			{ "lang", IDC_LANG }, { "info", IDC_ABOUT }, { "settings", IDC_SETTINGS }, { "log", IDC_LOG } };
		size_t i;
		for (i = 0; i < ARRAYSIZE(tb); i++) {
			w = icon_button(tb[i].icon, 24);
			gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
			ctl_toolbar_add(hMultiToolbar, tb[i].id, w);
		}
	}
	w = gtk_button_new_with_label("CLOSE");
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "rufus-main-button");
	ctl_create(hDlg, IDCANCEL, CT_BUTTON, w);
	gtk_box_pack_end(GTK_BOX(row), w, FALSE, FALSE, 0);
	w = gtk_button_new_with_label("START");
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "rufus-main-button");
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "suggested-action");
	gtk_widget_set_can_default(w, TRUE);
	ctl_create(hDlg, IDC_START, CT_BUTTON, w);
	gtk_box_pack_end(GTK_BOX(row), w, FALSE, FALSE, 6);
	gtk_grid_attach(GTK_GRID(grid), row, 0, r++, 2, 1);

	/* Status bar */
	status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_style_context_add_class(gtk_widget_get_style_context(status_box), "rufus-status");
	w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(w), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_box_pack_start(GTK_BOX(status_box), w, TRUE, TRUE, 0);
	w = gtk_label_new("00:00:00");
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "rufus-timer");
	gtk_box_pack_end(GTK_BOX(status_box), w, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(vbox), status_box, FALSE, FALSE, 0);
	hStatus = ctl_create(hDlg, IDC_STATUS, CT_STATUS, status_box);

	/* Invisible controls that exist in the Windows dialog */
	ctl_create(hDlg, IDC_ABOUT, CT_BUTTON, NULL);

	g_signal_connect(win, "delete-event", G_CALLBACK(on_main_delete), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_main_key_press), NULL);
	gtk_drag_dest_set(win, GTK_DEST_DEFAULT_ALL, drop_targets, 1, GDK_ACTION_COPY);
	g_signal_connect(win, "drag-data-received", G_CALLBACK(on_drag_data_received), NULL);

	gtk_widget_show_all(win);
	gtk_window_set_default(GTK_WINDOW(win), ctl_widget(GetDlgItem(hDlg, IDC_START)));
	return hDlg;
}

/* ------------------------------------------------------------------------- */
/* Dialog initialization (InitDialog() in rufus.c)                           */
/* ------------------------------------------------------------------------- */
static void uppercase_utf8(char* dst, size_t size, const char* src)
{
	char* u = g_utf8_strup(src, -1);
	snprintf(dst, size, "%s", u);
	g_free(u);
}

static void InitDialog(HWND hDlg)
{
	DWORD len;
	int i;
	char tmp[128], *buf, *ext, *msg;
	static char* resource[2] = { MAKEINTRESOURCEA(IDR_SL_LDLINUX_V4_SYS), MAKEINTRESOURCEA(IDR_SL_LDLINUX_V6_SYS) };
	const char* version = RUFUS_VERSION_STR;

	hMainDialog = hDlg;
	MainThreadId = GetCurrentThreadId();
	hDeviceList = GetDlgItem(hDlg, IDC_DEVICE);
	hPartitionScheme = GetDlgItem(hDlg, IDC_PARTITION_TYPE);
	hTargetSystem = GetDlgItem(hDlg, IDC_TARGET_SYSTEM);
	hFileSystem = GetDlgItem(hDlg, IDC_FILE_SYSTEM);
	hClusterSize = GetDlgItem(hDlg, IDC_CLUSTER_SIZE);
	hLabel = GetDlgItem(hDlg, IDC_LABEL);
	hProgress = GetDlgItem(hDlg, IDC_PROGRESS);
	hBootType = GetDlgItem(hDlg, IDC_BOOT_SELECTION);
	hImageOption = GetDlgItem(hDlg, IDC_IMAGE_OPTION);
	hSelectImage = GetDlgItem(hDlg, IDC_SELECT);
	hNBPasses = GetDlgItem(hDlg, IDC_NB_PASSES);
	hStart = GetDlgItem(hDlg, IDC_START);

	// Convert the main button labels to uppercase
	GetWindowTextU(hStart, tmp, sizeof(tmp));
	uppercase_utf8(uppercase_start, sizeof(uppercase_start), tmp);
	SetWindowTextU(hStart, uppercase_start);
	GetWindowTextU(GetDlgItem(hDlg, IDCANCEL), tmp, sizeof(tmp));
	uppercase_utf8(uppercase_close, sizeof(uppercase_close), tmp);
	// Hardcoded exception for German
	if (strcmp("SCHLIEßEN", uppercase_close) == 0)
		strcpy(uppercase_close, "SCHLIESSEN");
	SetWindowTextU(GetDlgItem(hDlg, IDCANCEL), uppercase_close);
	GetWindowTextU(GetDlgItem(hDlg, IDC_SELECT), tmp, sizeof(tmp));
	uppercase_utf8(uppercase_select[0], sizeof(uppercase_select[0]), tmp);
	uppercase_utf8(uppercase_select[1], sizeof(uppercase_select[1]), lmprintf(MSG_040));
	SetWindowTextU(GetDlgItem(hDlg, IDC_SELECT), uppercase_select[0]);
	uppercase_utf8(uppercase_cancel, sizeof(uppercase_cancel), lmprintf(MSG_007));

	SetAccessibleName(hNBPasses, lmprintf(MSG_316));

	// Read the version from RUFUS_VERSION_STR ("x.y.build")
	for (i = 0; i < 3; i++) {
		rufus_version[i] = (uint16_t)atoi(version);
		version = strchr(version, '.');
		if (version == NULL)
			break;
		version++;
	}
	static_sprintf(tmp, APPLICATION_NAME " %d.%d.%d%s%s", rufus_version[0], rufus_version[1], rufus_version[2],
		IsAlphaOrBeta(), (ini_file != NULL) ? "" : "");
	SetWindowTextU(hDlg, tmp);

	if (persistent_log) {
		time_t ltime = time(NULL);
		char timestamp[32] = "[";
		if (ctime_r(&ltime, &timestamp[1]) != NULL) {
			timestamp[strlen(timestamp) - 1] = ']';
			uprintf(timestamp);
		}
	}
	uprintf(APPLICATION_NAME " " APPLICATION_ARCH " v%d.%d.%d%s (Linux)", rufus_version[0], rufus_version[1], rufus_version[2],
		IsAlphaOrBeta());
	for (i = 0; i < ARRAYSIZE(resource); i++) {
		len = 0;
		buf = (char*)GetResource(hMainInstance, resource[i], _RT_RCDATA, "ldlinux_sys", &len, TRUE);
		if (buf == NULL) {
			uprintf("WARNING: could not read embedded Syslinux v%d version", i + 4);
		} else {
			embedded_sl_version[i] = GetSyslinuxVersion(buf, len, &ext);
			static_sprintf(embedded_sl_version_str[i], "%d.%02d", SL_MAJOR(embedded_sl_version[i]), SL_MINOR(embedded_sl_version[i]));
			static_strcpy(embedded_sl_version_ext[i], ext);
			free(buf);
		}
	}
	uprintf("Host: %s", WindowsVersion.VersionStr);
	uprintf("Syslinux versions: %s%s, %s%s", embedded_sl_version_str[0], embedded_sl_version_ext[0],
		embedded_sl_version_str[1], embedded_sl_version_ext[1]);
	uprintf("Grub versions: %s, %s", GRUB4DOS_VERSION, GRUB2_PACKAGE_VERSION);
	uprintf("System locale ID: 0x%04X (%s)", GetUserDefaultUILanguage(), ToLocaleName(GetUserDefaultUILanguage()));
	ubflush();
	if (selected_locale->ctrl_id & LOC_NEEDS_UPDATE) {
		uprintf("NOTE: The %s translation requires an update, but the current translator hasn't submitted "
			"one. Because of this, some messages will only be displayed in English.", selected_locale->txt[1]);
		uprintf("If you think you can help update this translation, please e-mail the author of this application");
	}

	SendMessage(hProgress, PBM_SETRANGE, 0, (MAX_PROGRESS << 16) & 0xFFFF0000);

	// Fill up the passes
	for (i = 1; i <= 5; i++) {
		msg = (i == 1) ? lmprintf(MSG_034, 1) : lmprintf(MSG_035, (i == 2) ? 2 : 4,
			(i == 2) ? "" : lmprintf(MSG_087, flash_type[i - 3]));
		IGNORE_RETVAL(ComboBox_AddStringU(hNBPasses, msg));
	}
	IGNORE_RETVAL(ComboBox_SetCurSel(hNBPasses, 0));
	SetPassesTooltip();

	// Fill up the boot type dropdown
	SetBootOptions();

	// Create the string arrays
	StrArrayCreate(&BlockingProcessList, 16);
	StrArrayCreate(&ImageList, 16);
	StrArrayCreate(&modified_files, 8);
	// Set various checkboxes
	CheckDlgButton(hDlg, IDC_QUICK_FORMAT, BST_CHECKED);
	CheckDlgButton(hDlg, IDC_EXTENDED_LABEL, BST_CHECKED);

	SendMessage(GetDlgItem(hDlg, IDC_PERSISTENCE_SIZE), EM_LIMITTEXT, 7, 0);

	// Create the various tooltips
	CreateTooltip(hFileSystem, lmprintf(MSG_157), -1);
	CreateTooltip(hClusterSize, lmprintf(MSG_158), -1);
	CreateTooltip(hLabel, lmprintf(MSG_159), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_ADVANCED_DEVICE_TOOLBAR), lmprintf(MSG_160), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_ADVANCED_FORMAT_TOOLBAR), lmprintf(MSG_160), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_BAD_BLOCKS), lmprintf(MSG_161), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_QUICK_FORMAT), lmprintf(MSG_162), -1);
	CreateTooltip(hBootType, lmprintf(MSG_164), -1);
	CreateTooltip(hSelectImage, lmprintf(MSG_165), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_EXTENDED_LABEL), lmprintf(MSG_166), 10000);
	CreateTooltip(GetDlgItem(hDlg, IDC_UEFI_MEDIA_VALIDATION), lmprintf(MSG_167), 10000);
	CreateTooltip(GetDlgItem(hDlg, IDC_OLD_BIOS_FIXES), lmprintf(MSG_169), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_LIST_USB_HDD), lmprintf(MSG_170), -1);
	CreateTooltip(hStart, lmprintf(MSG_171), -1);
	CreateTooltip(hPartitionScheme, lmprintf(MSG_163), -1);
	CreateTooltip(hTargetSystem, lmprintf(MSG_150), 30000);
	CreateTooltip(GetDlgItem(hDlg, IDS_CSM_HELP_TXT), lmprintf(MSG_151), 30000);
	CreateTooltip(hImageOption, lmprintf(MSG_305), 30000);
	CreateTooltip(GetDlgItem(hDlg, IDC_PERSISTENCE_SLIDER), lmprintf(MSG_125), 30000);
	CreateTooltip(GetDlgItem(hDlg, IDC_PERSISTENCE_SIZE), lmprintf(MSG_125), 30000);
	CreateTooltip(GetDlgItem(hDlg, IDC_PERSISTENCE_UNITS), lmprintf(MSG_126), 30000);
	CreateTooltip(GetDlgItem(hDlg, IDC_LANG), lmprintf(MSG_273), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_ABOUT), lmprintf(MSG_302), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_SETTINGS), lmprintf(MSG_301), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_LOG), lmprintf(MSG_303), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_SAVE), lmprintf(MSG_304), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_HASH), lmprintf(MSG_272), -1);

	ToggleAdvancedDeviceOptions(advanced_mode_device);
	ToggleAdvancedFormatOptions(advanced_mode_format);
	ToggleImageOptions();

	// Process the image provided on the command line, if any
	if (img_provided)
		PostMessage(hDlg, WM_COMMAND, IDC_SELECT, 0);

	SetBootTypeDropdownWidth();
	CheckDlgButton(hMainDialog, IDC_LIST_USB_HDD, enable_HDDs ? BST_CHECKED : BST_UNCHECKED);
	PrintInfo(0, MSG_210);
}

static void PrintStatusTimeout(const char* str, BOOL val)
{
	PrintStatus(STATUS_MSG_TIMEOUT, (val) ? MSG_250 : MSG_251, str);
}

/* Save the log to <app_data_dir>/Rufus/rufus.log, like Windows does on exit */
static void SaveLogOnExit(void)
{
	char* log_buffer;
	DWORD log_size;
	char path[MAX_PATH];

	if (user_deleted_rufus_dir)
		return;
	log_buffer = linux_log_get_text();
	if (log_buffer == NULL)
		return;
	log_size = (DWORD)strlen(log_buffer);
	if (log_size > 1) {
		if (persistent_log) {
			char* p = realloc(log_buffer, log_size + 2);
			if (p != NULL) {
				log_buffer = p;
				log_buffer[log_size++] = '\n';
				log_buffer[log_size] = 0;
			}
		}
		static_sprintf(path, "%s/%s", app_data_dir, FILES_DIR);
		_mkdirExU(path);
		static_strcat(path, "/rufus.log");
		FileIO(persistent_log ? FILE_IO_APPEND : FILE_IO_WRITE, path, &log_buffer, &log_size);
	}
	free(log_buffer);
}

static BOOL ControlPressed(void)
{
	GdkKeymap* km = gdk_keymap_get_for_display(gdk_display_get_default());
	return (gdk_keymap_get_modifier_state(km) & GDK_CONTROL_MASK) != 0;
}

/* ------------------------------------------------------------------------- */
/* Main dialog callback (MainCallback() in rufus.c)                          */
/* ------------------------------------------------------------------------- */
static INT_PTR CALLBACK MainCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	static DWORD DeviceNum = 0;
	static uint64_t LastRefresh = 0;
	static BOOL isMarquee = FALSE, queued_hotplug_event = FALSE;
	static DWORD_PTR thread_affinity[HASH_MAX + 1];
	static BOOL automount_inhibited = FALSE;
	LONG lPos;
	BOOL set_selected_fs;
	int nDeviceIndex, i, nb_devices, selected_language, tb_state, tb_flags;
	char tmp[MAX_PATH];
	loc_cmd* lcmd = NULL;
	RECT rc = { 0 };

	switch (message) {

	case WM_COMMAND:
		if ((LOWORD(wParam) >= UM_LANGUAGE_MENU) && (LOWORD(wParam) < UM_LANGUAGE_MENU_MAX)) {
			selected_language = LOWORD(wParam) - UM_LANGUAGE_MENU;
			i = 0;
			list_for_each_entry(lcmd, &locale_list, loc_cmd, list) {
				if (i++ == selected_language) {
					if (selected_locale != lcmd) {
						selected_locale = lcmd;
						selected_langid = get_language_id(lcmd);
						// Avoid the FS being reset on language change
						selected_fs = (int)ComboBox_GetCurItemData(hFileSystem);
						relaunch = TRUE;
						PostMessage(hDlg, WM_COMMAND, IDCANCEL, 0);
					}
					break;
				}
			}
		}
		switch (LOWORD(wParam)) {
		case IDOK:			// close application
		case IDCANCEL:
			EnableWindow(GetDlgItem(hDlg, IDCANCEL), FALSE);
			if (format_thread != NULL) {
				if ((no_confirmation_on_cancel) || (Notification(MB_YESNO | MB_ICONWARNING,
					lmprintf(MSG_049), lmprintf(MSG_105)) == IDYES)) {
					// Operation may have completed in the meantime
					if (format_thread != NULL) {
						ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
						PrintInfo(0, MSG_201);
						uprintf("Cancelling");
						//  Start a timer to detect blocking operations during ISO file extraction
						if (iso_blocking_status >= 0) {
							last_iso_blocking_status = iso_blocking_status;
							SetTimer(hMainDialog, TID_BLOCKING_TIMER, 3000, BlockingTimer);
						}
					}
				} else {
					EnableWindow(GetDlgItem(hDlg, IDCANCEL), TRUE);
				}
				no_confirmation_on_cancel = FALSE;
				return (INT_PTR)TRUE;
			} else if (op_in_progress) {
				// User might be trying to cancel during preliminary checks
				ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
				PrintInfo(0, MSG_201);
				EnableWindow(GetDlgItem(hDlg, IDCANCEL), TRUE);
				return (INT_PTR)TRUE;
			}
			SaveLogOnExit();
			linux_stop_device_monitor();
			ClearDrives();
			StopProcessSearch();
			StrArrayDestroy(&BlockingProcessList);
			StrArrayDestroy(&ImageList);
			StrArrayDestroy(&modified_files);
			DestroyAllTooltips();
			quit_requested = TRUE;
			gtk_main_quit();
			break;
		case IDC_ABOUT:
			CreateAboutBox();
			break;
		case IDC_LOG:
			linux_log_dialog_toggle();
			log_displayed = linux_log_dialog_visible();
			break;
		case IDC_ADVANCED_DEVICE_TOOLBAR:
		case IDC_ADVANCED_DRIVE_PROPERTIES:
			advanced_mode_device = !advanced_mode_device;
			WriteSettingBool(SETTING_ADVANCED_MODE_DEVICE, advanced_mode_device);
			ToggleAdvancedDeviceOptions(advanced_mode_device);
			SetBootOptions();
			boot_type = (int)ComboBox_GetCurItemData(hBootType);
			EnableControls(TRUE, FALSE);
			SetFileSystemAndClusterSize(NULL);
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL << 16) | IDC_FILE_SYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			break;
		case IDC_ADVANCED_FORMAT_TOOLBAR:
		case IDC_ADVANCED_FORMAT_OPTIONS:
			advanced_mode_format = !advanced_mode_format;
			WriteSettingBool(SETTING_ADVANCED_MODE_FORMAT, advanced_mode_format);
			ToggleAdvancedFormatOptions(advanced_mode_format);
			if (selected_fs == FS_UNKNOWN)
				selected_fs = (int)ComboBox_GetCurItemData(hFileSystem);
			SetFileSystemAndClusterSize(NULL);
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL << 16) | IDC_FILE_SYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			break;
		case IDC_LABEL:
			if (HIWORD(wParam) == EN_CHANGE) {
				// We will get EN_CHANGE when we change the label automatically, so we need to detect that
				if (!app_changed_label)
					user_changed_label = TRUE;
				app_changed_label = FALSE;
			}
			break;
		case IDC_DEVICE:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			nb_devices = ComboBox_GetCount(hDeviceList);
			PrintStatusDebug(0, (nb_devices == 1) ? MSG_208 : MSG_209, nb_devices);
			PopulateProperties();
			nDeviceIndex = ComboBox_GetCurSel(hDeviceList);
			DeviceNum = (nDeviceIndex == CB_ERR) ? 0 : (DWORD)ComboBox_GetItemData(hDeviceList, nDeviceIndex);
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE_INTERNAL << 16) | IDC_FILE_SYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			if (nb_devices == 0) {
				StopProcessSearch();
			} else if (!StartProcessSearch() || !SetProcessSearch(DeviceNum)) {
				uprintf("Failed to start conflicting process search");
				StopProcessSearch();
			}
			break;
		case IDC_IMAGE_OPTION:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			SetFileSystemAndClusterSize(NULL);
			imop_win_sel = ComboBox_GetCurSel(hImageOption);
			EnableUefiValidation((imop_win_sel == 0), TRUE);
			break;
		case IDC_PERSISTENCE_SIZE:
			if (HIWORD(wParam) == EN_CHANGE) {
				uint64_t pos;
				// We get EN_CHANGE when we change the size automatically, so we need to detect that
				if (app_changed_size) {
					app_changed_size = FALSE;
					break;
				}
				GetWindowTextA(GetDlgItem(hDlg, IDC_PERSISTENCE_SIZE), tmp, sizeof(tmp));
				lPos = strtol(tmp, NULL, 10);
				persistence_unit_selection = ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_PERSISTENCE_UNITS));
				persistence_size = lPos * MB;
				for (i = 0; i < persistence_unit_selection; i++)
					persistence_size *= 1024;
				if (persistence_size > SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size))
					persistence_size = SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size);
				pos = persistence_size / MB;
				for (i = 0; i < persistence_unit_selection; i++)
					pos /= 1024;
				lPos = (LONG)pos;
				SendMessage(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER), TBM_SETPOS, TRUE, lPos);
				if (persistence_size >= (SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size))) {
					static_sprintf(tmp, "%ld", (long)lPos);
					app_changed_size = TRUE;
					SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
				}
			} else if (HIWORD(wParam) == EN_KILLFOCUS) {
				if (persistence_size == 0) {
					TogglePersistenceControls(FALSE);
					static_sprintf(tmp, "0 (%s)", lmprintf(MSG_124));
					app_changed_size = TRUE;
					SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
				} else if (persistence_size < MIN_EXT_SIZE) {
					uint64_t pos;
					persistence_size = MIN_EXT_SIZE;
					pos = persistence_size / MB;
					for (i = 0; i < persistence_unit_selection; i++)
						pos /= 1024;
					lPos = (LONG)pos;
					SendMessage(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER), TBM_SETPOS, TRUE, lPos);
					static_sprintf(tmp, "%ld", (long)lPos);
					app_changed_size = TRUE;
					SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
				}
			}
			break;
		case IDC_PERSISTENCE_UNITS:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			if (ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_PERSISTENCE_UNITS)) == persistence_unit_selection)
				break;
			GetWindowTextA(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp, sizeof(tmp));
			persistence_size = strtol(tmp, NULL, 10) * MB;
			for (i = 0; i < persistence_unit_selection; i++)
				persistence_size *= 1024;
			persistence_unit_selection = ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_PERSISTENCE_UNITS));
			SetPersistenceSize();
			break;
		case IDC_NB_PASSES:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			SetPassesTooltip();
			break;
		case IDC_TARGET_SYSTEM:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			target_type = (int)ComboBox_GetCurItemData(hTargetSystem);
			SendMessage(hMainDialog, UM_UPDATE_CSM_TOOLTIP, 0, 0);
			SetFileSystemAndClusterSize(NULL);
			EnableUefiValidation(TRUE, TRUE);
			break;
		case IDC_PARTITION_TYPE:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			partition_type = (int)ComboBox_GetCurItemData(hPartitionScheme);
			SetPartitionSchemeAndTargetSystem(TRUE);
			SetFileSystemAndClusterSize(NULL);
			EnableOldBiosFixes(TRUE, TRUE);
			EnableUefiValidation(TRUE, TRUE);
			selected_pt = partition_type;
			break;
		case IDC_FILE_SYSTEM:
			if ((HIWORD(wParam) != CBN_SELCHANGE) && (HIWORD(wParam) != CBN_SELCHANGE_INTERNAL))
				break;
			if (!IsWindowEnabled(hFileSystem))
				break;
			set_selected_fs = (HIWORD(wParam) == CBN_SELCHANGE);
			fs_type = (int)ComboBox_GetCurItemData(hFileSystem);
			SetClusterSizes(fs_type);
			if (set_selected_fs && (fs_type > 0))
				selected_fs = fs_type;
			// Some FS's (such as ReFS or Large FAT32) only have QuickFormat so make sure we reflect that
			EnableQuickFormat(TRUE, TRUE);
			EnableOldBiosFixes(TRUE, TRUE);
			EnableExtendedLabel(TRUE, TRUE);
			break;
		case IDC_BOOT_SELECTION:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			boot_type = (int)ComboBox_GetCurItemData(hBootType);
			if ((HIWORD(wParam) != CBN_SELCHANGE) || (boot_type == selection_default))
				break;
			selection_default = boot_type;
			SetPartitionSchemeAndTargetSystem(FALSE);
			// Try to reselect current FS from the drive for non-bootable
			tmp[0] = 0;
			if ((selected_fs == FS_UNKNOWN) && (SelectedDrive.DeviceNumber != 0))
				GetDrivePartitionData(SelectedDrive.DeviceNumber, tmp, sizeof(tmp), !usb_debug);
			SetFileSystemAndClusterSize(tmp);
			ToggleImageOptions();
			SetProposedLabel(ComboBox_GetCurSel(hDeviceList));
			EnableControls(TRUE, TRUE);
			// Same as upstream (where these two are swapped)
			target_type = (int)ComboBox_GetCurItemData(hPartitionScheme);
			partition_type = (int)ComboBox_GetCurItemData(hTargetSystem);
			return (INT_PTR)TRUE;
		case IDC_SELECT:
			// Ctrl-SELECT is used to select an additional archive of files to extract
			if (lParam != 0 && ControlPressed()) {
				EXT_DECL(arch_ext, NULL, __VA_GROUP__("*.zip"), __VA_GROUP__(lmprintf(MSG_309)));
				archive_path = FileDialog(FALSE, NULL, &arch_ext, NULL);
				if (archive_path != NULL) {
					uprintf("Using archive: %s (%s)", PathFindFileNameU(archive_path),
						SizeToHumanReadable(_filesizeU(archive_path), FALSE, FALSE));
				}
				break;
			}
			if (select_index == 1) {
				EnableControls(FALSE, FALSE);
				DownloadISO();
			} else {
				if (img_provided) {
					uprintf("\nImage provided: '%s'", image_path);
					img_provided = FALSE;	// One off thing...
				} else {
					char* old_image_path = image_path;
					char extensions[160] = "*.iso;*.img;*.vhd;*.vhdx;*.usb;*.bz2;*.bzip2;*.gz;*.lzma;*.xz;*.Z;*.zip;*.zst;*.wic;*.wim;*.esd;*.vtsi";
					if (has_ffu_support)
						strcat(extensions, ";*.ffu");
					EXT_DECL(img_ext, NULL, __VA_GROUP__(extensions), __VA_GROUP__(lmprintf(MSG_280)));
					image_path = FileDialog(FALSE, NULL, &img_ext, NULL);
					if (image_path == NULL) {
						if (old_image_path != NULL) {
							// Reselect previous image
							image_path = old_image_path;
						} else {
							CreateTooltip(hSelectImage, lmprintf(MSG_173), -1);
							PrintStatus(0, MSG_086);
						}
						break;
					} else {
						safe_free(archive_path);
						free(old_image_path);
					}
				}
				// Release any mount of the previous image
				linux_release_image_mounts();
				ErrorStatus = 0;
				if (CreateThread(NULL, 0, ImageScanThread, NULL, 0, NULL) == NULL) {
					uprintf("Unable to start ISO scanning thread");
					ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
				}
			}
			break;
		case IDC_UEFI_MEDIA_VALIDATION:
			if ((HIWORD(wParam)) == BN_CLICKED) {
				validate_md5sum = IsChecked(IDC_UEFI_MEDIA_VALIDATION);
				WriteSettingBool(SETTING_ENABLE_RUNTIME_VALIDATION, validate_md5sum);
			}
			break;
		case IDC_LIST_USB_HDD:
			if ((HIWORD(wParam)) == BN_CLICKED) {
				enable_HDDs = !enable_HDDs;
				PrintStatusTimeout(lmprintf(MSG_253), enable_HDDs);
				GetDevices(0);
			}
			break;
		case IDC_START:
			if (format_thread != NULL)
				return (INT_PTR)TRUE;
			// Just in case
			boot_type = (int)ComboBox_GetCurItemData(hBootType);
			partition_type = (int)ComboBox_GetCurItemData(hPartitionScheme);
			target_type = (int)ComboBox_GetCurItemData(hTargetSystem);
			fs_type = (int)ComboBox_GetCurItemData(hFileSystem);
			write_as_image = FALSE;
			write_as_esp = FALSE;
			unattend_xml_flags = 0;
			// Disable all controls except Cancel
			EnableControls(FALSE, FALSE);
			ErrorStatus = 0;
			LastWriteError = 0;
			StrArrayClear(&BlockingProcessList);
			no_confirmation_on_cancel = FALSE;
			SendMessage(hMainDialog, UM_PROGRESS_INIT, 0, 0);
			selection_default = (int)ComboBox_GetCurItemData(hBootType);
			// Create a thread to validate options and download files as needed (so that we can update the UI).
			// On exit, this thread sends message UM_FORMAT_START back to this dialog.
			if (CreateThread(NULL, 0, BootCheckThread, NULL, 0, NULL) == NULL) {
				uprintf("Unable to start boot check thread");
				ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
				PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)FALSE, 0);
			}
			break;
		case IDC_LANG:
			ShowLanguageMenu(rc);
			break;
		case IDC_SETTINGS:
			linux_settings_dialog();
			break;
		case IDC_HASH:
			if ((format_thread == NULL) && (image_path != NULL)) {
				ErrorStatus = 0;
				no_confirmation_on_cancel = TRUE;
				SendMessage(hMainDialog, UM_PROGRESS_INIT, 0, 0);
				// Disable all controls except cancel
				EnableControls(FALSE, FALSE);
				InitProgress(FALSE);
				SetThreadAffinity(thread_affinity, HASH_MAX + 1);
				format_thread = CreateThread(NULL, 0, HashThread, (LPVOID)thread_affinity, 0, NULL);
				if (format_thread != NULL) {
					SetThreadPriority(format_thread, default_thread_priority);
					PrintInfo(0, -1);
					SendMessage(hMainDialog, UM_TIMER_START, 0, 0);
				} else {
					uprintf("Unable to start hash thread");
					ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
					PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)FALSE, 0);
				}
			}
			break;
		case IDC_SAVE:
			if (format_thread == NULL && (ComboBox_GetCurSel(hDeviceList) >= 0)) {
				save_image = SaveImage();
				if (!save_image) {
					uprintf("Unable to start image save thread");
					if (!IS_ERROR(ErrorStatus))
						ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
					PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)FALSE, 0);
				}
			}
			break;
		case IDM_SELECT:
		case IDM_DOWNLOAD:
			select_index = LOWORD(wParam) - IDM_SELECT;
			SetWindowTextU(GetDlgItem(hDlg, IDC_SELECT), uppercase_select[select_index]);
			break;
		default:
			return (INT_PTR)FALSE;
		}
		return (INT_PTR)TRUE;

	case UM_UPDATE_CSM_TOOLTIP:
		ShowWindow(GetDlgItem(hMainDialog, IDS_CSM_HELP_TXT), ((target_type == TT_UEFI) || has_uefi_csm) ? SW_SHOW : SW_HIDE);
		CreateTooltip(GetDlgItem(hMainDialog, IDS_CSM_HELP_TXT), lmprintf((target_type == TT_UEFI) ? MSG_152 : MSG_151), 30000);
		break;

	case UM_ENABLE_CONTROLS:
		KillTimer(hMainDialog, TID_APP_TIMER);
		if (!IS_ERROR(ErrorStatus))
			PrintInfo(0, MSG_210);
		else switch (SCODE_CODE(ErrorStatus)) {
		case ERROR_CANCELLED:
			PrintInfo(0, MSG_211);
			break;
		case ERROR_BAD_SIGNATURE:
			PrintInfo(0, MSG_283);
			break;
		default:
			PrintInfo(0, MSG_212);
			break;
		}
		EnableControls(TRUE, FALSE);
		break;

	case UM_TIMER_START:
		PrintInfo(0, -1);
		timer = 0;
		static_sprintf(szTimer, "00:00:00");
		SendMessageA(hStatus, SB_SETTEXTA, SBT_OWNERDRAW | SB_SECTION_RIGHT, (LPARAM)szTimer);
		SetTimer(hMainDialog, TID_APP_TIMER, 1000, ClockTimer);
		break;

	case UM_SELECT_ISO:
		select_index = 0;
		img_provided = TRUE;
		SetWindowTextU(GetDlgItem(hDlg, IDC_SELECT), uppercase_select[0]);
		SendMessage(hDlg, WM_COMMAND, IDC_SELECT, 0);
		break;

	case UM_MEDIA_CHANGE:
		// Sent by our hotplug monitor (dev_linux.c), which already debounces events
		if (format_thread == NULL) {
			LastRefresh = GetTickCount64();
			KillTimer(hMainDialog, TID_REFRESH_TIMER);
			if (!op_in_progress) {
				queued_hotplug_event = FALSE;
				GetDevices((DWORD)ComboBox_GetCurItemData(hDeviceList));
				EnableControls(TRUE, FALSE);
				if (ComboBox_GetCurSel(hDeviceList) < 0) {
					SetPartitionSchemeAndTargetSystem(FALSE);
					SetFileSystemAndClusterSize(NULL);
					ShowWindow(GetDlgItem(hDlg, IDS_CSM_HELP_TXT), SW_HIDE);
					persistence_unit_selection = -1;
				}
			} else {
				queued_hotplug_event = TRUE;
			}
			return (INT_PTR)TRUE;
		}
		(void)LastRefresh;
		break;

	case WM_HSCROLL:
		lPos = (LONG)SendMessage(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER), TBM_GETPOS, 0, 0);
		SetPersistencePos(lPos);
		persistence_size = lPos * MB;
		for (i = 0; i < persistence_unit_selection; i++)
			persistence_size *= 1024;
		break;

	case WM_CLOSE:
		if (op_in_progress)
			return (INT_PTR)TRUE;
		SendMessage(hDlg, WM_COMMAND, (WPARAM)IDCANCEL, (LPARAM)0);
		break;

	case UM_PROGRESS_INIT:
		isMarquee = (wParam == PBS_MARQUEE);
		SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_NORMAL, 0);
		if (isMarquee) {
			SendMessage(hProgress, PBM_SETMARQUEE, TRUE, 0);
			SetTaskbarProgressState(TASKBAR_INDETERMINATE);
		} else {
			SendMessage(hProgress, PBM_SETPOS, 0, 0);
			SetTaskbarProgressState(TASKBAR_NORMAL);
			SetTaskbarProgressValue(0, MAX_PROGRESS);
		}
		break;

	case UM_PROGRESS_EXIT:
		tb_state = PBST_NORMAL;
		tb_flags = TASKBAR_NORMAL;
		if (isMarquee) {
			// Remove marquee style if previously set
			SendMessage(hProgress, PBM_SETMARQUEE, FALSE, 0);
			SetTaskbarProgressValue(0, MAX_PROGRESS);
		} else if (!IS_ERROR(ErrorStatus)) {
			SetTaskbarProgressValue(MAX_PROGRESS, MAX_PROGRESS);
		} else if (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED) {
			tb_state = PBST_PAUSED;
			tb_flags = TASKBAR_PAUSED;
		} else {
			tb_state = PBST_ERROR;
			tb_flags = TASKBAR_ERROR;
			MessageBeep(MB_ICONERROR);
			FlashTaskbar(dialog_handle);
		}
		SendMessage(hProgress, PBM_SETSTATE, (WPARAM)tb_state, 0);
		SetTaskbarProgressState(tb_flags);
		break;

	case UM_NO_UPDATE:
		Notification(MB_ICONINFORMATION | MB_CLOSE, lmprintf(MSG_243), lmprintf(MSG_247));
		break;

	case UM_FORMAT_START:
		if (wParam != BOOTCHECK_PROCEED)
			goto aborted_start;
		// All subsequent aborts below translate to a user cancellation
		wParam = BOOTCHECK_CANCEL;
		save_image = FALSE;

		if ((partition_type == PARTITION_STYLE_MBR) && (SelectedDrive.DiskSize > 2 * TB)) {
			if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_128, "MBR"),
				lmprintf(MSG_134, SizeToHumanReadable(SelectedDrive.DiskSize - 2 * TB, FALSE, FALSE))) != IDYES)
				goto aborted_start;
		}

		if (!zero_drive && (fs_type == FS_UDF)) {
			dur_secs = (uint32_t)(((double)SelectedDrive.DiskSize) / 1073741824.0f / UDF_FORMAT_SPEED);
			if (dur_secs > UDF_FORMAT_WARN) {
				dur_mins = dur_secs / 60;
				dur_secs -= dur_mins * 60;
				Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_113), lmprintf(MSG_112, dur_mins, dur_secs));
			} else {
				dur_secs = 0;
				dur_mins = 0;
			}
		}

		// Ask users how they want to proceed if processes are accessing the drive
		PrintStatus(0, MSG_278);
		if (GetProcessSearch(SEARCH_PROCESS_TIMEOUT, 0x06, TRUE)) {
			char title[128];
			ComboBox_GetTextU(hDeviceList, title, sizeof(title));
			if (Notification(MB_ICONWARNING | MB_YESNO, title, lmprintf(MSG_132)) != IDYES)
				goto aborted_start;
		}

		PrintStatus(0, MSG_142);
		GetWindowTextU(hDeviceList, tmp, ARRAYSIZE(tmp));
		if (Notification(MB_OKCANCEL | MB_ICONWARNING, APPLICATION_NAME, lmprintf(MSG_003, tmp)) != IDOK)
			goto aborted_start;
		if ((SelectedDrive.nPartitions > 1) && (Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_094), lmprintf(MSG_093)) != IDOK))
			goto aborted_start;
		if ((!zero_drive) && (boot_type != BT_NON_BOOTABLE) && (SelectedDrive.SectorSize != 512) &&
			(Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_197), lmprintf(MSG_196, SelectedDrive.SectorSize)) != IDOK))
			goto aborted_start;

		nDeviceIndex = ComboBox_GetCurSel(hDeviceList);
		DeviceNum = (DWORD)ComboBox_GetItemData(hDeviceList, nDeviceIndex);
		// Automated tests may only ever write to the loop device they were given
		if (!linux_automation_check_drive(DeviceNum))
			goto aborted_start;
		InitProgress(zero_drive || write_as_image);
		// Prevent the desktop from automounting the drive while we work on it
		automount_inhibited = linux_inhibit_automount(DeviceNum, TRUE);
		format_thread = CreateThread(NULL, 0, FormatThread, (LPVOID)(uintptr_t)DeviceNum, 0, NULL);
		if (format_thread == NULL) {
			uprintf("Unable to start formatting thread");
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
			PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)FALSE, 0);
		} else {
			SetThreadPriority(format_thread, default_thread_priority);
			uprintf("\nFormat operation started");
			SendMessage(hMainDialog, UM_TIMER_START, 0, 0);
			SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hMainDialog, IDCANCEL), TRUE);
		}
		if (format_thread != NULL)
			break;
	aborted_start:
		zero_drive = FALSE;
		if (queued_hotplug_event)
			SendMessage(hDlg, UM_MEDIA_CHANGE, 0, 0);
		if (wParam == BOOTCHECK_CANCEL) {
			nb_devices = ComboBox_GetCount(hDeviceList);
			PrintStatus(0, (nb_devices == 1) ? MSG_208 : MSG_209, nb_devices);
			PrintStatus(5000, MSG_041);
			if (unattend_xml_path != NULL) {
				DeleteFileU(unattend_xml_path);
				unattend_xml_path = NULL;
			}
			EnableControls(TRUE, FALSE);
			linux_automation_completed(RUFUS_ERROR(ERROR_CANCELLED));
			break;
		}
		ErrorStatus = RUFUS_ERROR((wParam == BOOTCHECK_DOWNLOAD_ERROR) ? APPERR(ERROR_CANT_DOWNLOAD) : ERROR_GEN_FAILURE);
		// Fall through

	case UM_FORMAT_COMPLETED:
		linux_automation_completed(ErrorStatus);
		zero_drive = FALSE;
		format_thread = NULL;
		// Hand the drive back to the desktop
		linux_release_all_mounts();
		if (automount_inhibited) {
			linux_inhibit_automount(DeviceNum, FALSE);
			automount_inhibited = FALSE;
		}
		if (unattend_xml_path != NULL) {
			DeleteFileU(unattend_xml_path);
			unattend_xml_path = NULL;
		}
		KillTimer(hMainDialog, TID_APP_TIMER);
		EnableWindow(GetDlgItem(hMainDialog, IDCANCEL), TRUE);
		EnableControls(TRUE, FALSE);
		if (wParam && !save_image) {
			uprintf("\n");
			GetDevices(DeviceNum);
		}
		save_image = FALSE;
		if (!IS_ERROR(ErrorStatus)) {
			SendMessage(hProgress, PBM_SETPOS, MAX_PROGRESS, 0);
			SetTaskbarProgressState(TASKBAR_NOPROGRESS);
			PrintInfo(0, MSG_210);
			MessageBeep(MB_OK);
			FlashTaskbar(dialog_handle);
		} else if (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED) {
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_PAUSED, 0);
			SetTaskbarProgressState(TASKBAR_PAUSED);
			PrintInfo(0, MSG_211);
			Notification(MB_ICONINFORMATION | MB_CLOSE, lmprintf(MSG_211), lmprintf(MSG_041));
		} else {
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_ERROR, 0);
			SetTaskbarProgressState(TASKBAR_ERROR);
			PrintInfo(0, MSG_212);
			MessageBeep(MB_ICONERROR);
			FlashTaskbar(dialog_handle);
			// Find out if we have any blocking processes
			GetProcessSearch(0, 0x07, TRUE);
			if (BlockingProcessList.Index > 0) {
				ListDialog(lmprintf(MSG_042), lmprintf(MSG_055), BlockingProcessList.String, BlockingProcessList.Index);
			} else {
				if (SCODE_CODE(ErrorStatus) == ERROR_NOT_READY) {
					// Try to cycle the port if the device is not ready
					int index = ComboBox_GetCurSel(hDeviceList);
					if (index >= 0) {
						uprintf("Device not ready → Trying to cycle port...");
						CyclePort(index);
					}
				}
				Notification(MB_ICONERROR | MB_CLOSE, lmprintf(MSG_042), lmprintf(MSG_043, StrError(ErrorStatus, FALSE)));
			}
		}
		ErrorStatus = 0;
		LastWriteError = 0;
		return (INT_PTR)TRUE;
	}
	return (INT_PTR)FALSE;
}

/* ------------------------------------------------------------------------- */
/* GTK event handlers                                                        */
/* ------------------------------------------------------------------------- */
static gboolean on_main_delete(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	(void)widget; (void)event; (void)data;
	if (op_in_progress)
		return TRUE;
	SendMessage(hMainDialog, WM_COMMAND, (WPARAM)IDCANCEL, 0);
	return TRUE;
}

static void on_drag_data_received(GtkWidget* widget, GdkDragContext* context, gint x, gint y,
	GtkSelectionData* sel, guint info, guint time, gpointer data)
{
	gchar** uris;
	(void)widget; (void)x; (void)y; (void)info; (void)data;
	if (format_thread == NULL && !op_in_progress) {
		uris = gtk_selection_data_get_uris(sel);
		if (uris != NULL && uris[0] != NULL) {
			gchar* path = g_filename_from_uri(uris[0], NULL, NULL);
			if (path != NULL) {
				safe_free(image_path);
				image_path = strdup(path);
				g_free(path);
				img_provided = TRUE;
				SendMessage(hMainDialog, WM_COMMAND, IDC_SELECT, 0);
			}
		}
		g_strfreev(uris);
	}
	gtk_drag_finish(context, TRUE, FALSE, time);
}

/*
 * Keyboard shortcuts ("cheat modes"), same as the ones from WinMain()'s message loop.
 */
static gboolean on_main_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data)
{
	static BOOL previous_enable_HDDs = FALSE;
	char tmp_path[MAX_PATH];
	BOOL ctrl = (event->state & GDK_CONTROL_MASK) != 0;
	BOOL alt = (event->state & GDK_MOD1_MASK) != 0;
	guint key = gdk_keyval_to_upper(event->keyval);
	(void)widget; (void)data;

	if (ctrl && !alt && key == GDK_KEY_L) {
		SendMessage(hMainDialog, WM_COMMAND, IDC_LOG, 0);
		return TRUE;
	}
	if (ctrl && !alt && key == GDK_KEY_P) {
		persistent_log = !persistent_log;
		WriteSettingBool(SETTING_PERSISTENT_LOG, persistent_log);
		PrintStatusTimeout(lmprintf(MSG_336), persistent_log);
		return TRUE;
	}
	// Alt +/- changes the thread priority
	if (alt && !ctrl && (key == GDK_KEY_plus || key == GDK_KEY_KP_Add || key == GDK_KEY_equal ||
		key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract)) {
		int delta = (key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract) ? -1 : +1;
		if (((delta == +1) && (default_thread_priority < THREAD_PRIORITY_HIGHEST)) ||
			((delta == -1) && (default_thread_priority > THREAD_PRIORITY_LOWEST))) {
			default_thread_priority += delta;
			WriteSetting32(SETTING_DEFAULT_THREAD_PRIORITY, default_thread_priority - THREAD_PRIORITY_ABOVE_NORMAL);
			if (format_thread != NULL)
				SetThreadPriority(format_thread, default_thread_priority);
		}
		PrintStatus(STATUS_MSG_TIMEOUT, MSG_318, default_thread_priority);
		return TRUE;
	}
	if (op_in_progress || !alt)
		return FALSE;

	if (ctrl) {
		switch (key) {
		case GDK_KEY_Z:		// Fast zeroing of the drive
			zero_drive = TRUE;
			fast_zeroing = TRUE;
			PostMessage(hMainDialog, WM_COMMAND, (WPARAM)IDC_START, 0);
			return TRUE;
		case GDK_KEY_E:
			expert_mode = !expert_mode;
			WriteSettingBool(SETTING_EXPERT_MODE, expert_mode);
			PrintStatusTimeout(lmprintf(MSG_347), expert_mode);
			return TRUE;
		case GDK_KEY_F:
			list_non_usb_removable_drives = !list_non_usb_removable_drives;
			if (list_non_usb_removable_drives) {
				previous_enable_HDDs = enable_HDDs;
				enable_HDDs = TRUE;
			} else {
				enable_HDDs = previous_enable_HDDs;
			}
			CheckDlgButton(hMainDialog, IDC_LIST_USB_HDD, enable_HDDs ? BST_CHECKED : BST_UNCHECKED);
			PrintStatusTimeout(lmprintf(MSG_287), list_non_usb_removable_drives);
			uprintf("%sListing of non-USB removable drives %s",
				(list_non_usb_removable_drives) ? "CAUTION: " : "", (list_non_usb_removable_drives) ? "enabled" : "disabled");
			if (list_non_usb_removable_drives)
				uprintf("By using this unofficial cheat mode you forfeit ANY RIGHT to complain if you lose valuable data!");
			GetDevices(0);
			return TRUE;
		case GDK_KEY_Y:
			force_update = (force_update > 0) ? 0 : 2;
			PrintStatusTimeout(lmprintf(MSG_259), force_update);
			return TRUE;
		default:
			return FALSE;
		}
	}

	switch (key) {
	case GDK_KEY_period:	// Toggle USB debug
		usb_debug = !usb_debug;
		cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
		WriteSettingBool(SETTING_ENABLE_USB_DEBUG, usb_debug);
		PrintStatusTimeout(lmprintf(MSG_270), usb_debug);
		GetDevices(0);
		return TRUE;
	case GDK_KEY_comma:		// Toggle drive locking
		lock_drive = !lock_drive;
		PrintStatusTimeout(lmprintf(MSG_282), lock_drive);
		return TRUE;
	case GDK_KEY_A:			// Toggle the Rufus MBR
		use_rufus_mbr = !use_rufus_mbr;
		WriteSettingBool(SETTING_DISABLE_RUFUS_MBR, !use_rufus_mbr);
		PrintStatusTimeout(lmprintf(MSG_349), use_rufus_mbr);
		return TRUE;
	case GDK_KEY_B:			// Toggle fake drive detection during bad blocks check
		detect_fakes = !detect_fakes;
		WriteSettingBool(SETTING_DISABLE_FAKE_DRIVES_CHECK, !detect_fakes);
		PrintStatusTimeout(lmprintf(MSG_256), detect_fakes);
		return TRUE;
	case GDK_KEY_C:			// Cycle the USB port of the selected drive
		{
			int index = ComboBox_GetCurSel(hDeviceList);
			if (index >= 0)
				CyclePort(index);
		}
		return TRUE;
	case GDK_KEY_D:			// Delete the Rufus download directory
		static_sprintf(tmp_path, "%s/%s", app_data_dir, FILES_DIR);
		PrintStatusDebug(STATUS_MSG_TIMEOUT, MSG_264, tmp_path);
		SHDeleteDirectoryExU(NULL, tmp_path, FOF_NO_UI);
		user_deleted_rufus_dir = TRUE;
		return TRUE;
	case GDK_KEY_E:			// Dual BIOS+UEFI for Windows media
		allow_dual_uefi_bios = !allow_dual_uefi_bios;
		WriteSettingBool(SETTING_ENABLE_WIN_DUAL_EFI_BIOS, allow_dual_uefi_bios);
		PrintStatusTimeout(lmprintf(MSG_266), allow_dual_uefi_bios);
		SetPartitionSchemeAndTargetSystem(FALSE);
		SetFileSystemAndClusterSize(NULL);
		return TRUE;
	case GDK_KEY_F:			// List USB hard drives
		enable_HDDs = !enable_HDDs;
		PrintStatusTimeout(lmprintf(MSG_253), enable_HDDs);
		GetDevices(0);
		CheckDlgButton(hMainDialog, IDC_LIST_USB_HDD, enable_HDDs ? BST_CHECKED : BST_UNCHECKED);
		return TRUE;
	case GDK_KEY_G:			// Virtual drives
		enable_VHDs = !enable_VHDs;
		WriteSettingBool(SETTING_DISABLE_VHDS, !enable_VHDs);
		PrintStatusTimeout(lmprintf(MSG_308), enable_VHDs);
		GetDevices(0);
		return TRUE;
	case GDK_KEY_H:			// SHA-512
		enable_extra_hashes = !enable_extra_hashes;
		WriteSettingBool(SETTING_ENABLE_EXTRA_HASHES, enable_extra_hashes);
		PrintStatusTimeout(lmprintf(MSG_312), enable_extra_hashes);
		return TRUE;
	case GDK_KEY_I:			// ISO support
		enable_iso = !enable_iso;
		PrintStatusTimeout(lmprintf(MSG_262), enable_iso);
		if (image_path != NULL) {
			img_provided = TRUE;
			dont_display_image_name = TRUE;
			SendMessage(hMainDialog, WM_COMMAND, IDC_SELECT, 0);
		}
		return TRUE;
	case GDK_KEY_J:			// Joliet
		enable_joliet = !enable_joliet;
		PrintStatusTimeout(lmprintf(MSG_257), enable_joliet);
		return TRUE;
	case GDK_KEY_K:			// Rock Ridge
		enable_rockridge = !enable_rockridge;
		PrintStatusTimeout(lmprintf(MSG_258), enable_rockridge);
		return TRUE;
	case GDK_KEY_L:			// Large FAT32 formatting
		force_large_fat32 = !force_large_fat32;
		WriteSettingBool(SETTING_FORCE_LARGE_FAT32_FORMAT, force_large_fat32);
		PrintStatusTimeout(lmprintf(MSG_254), force_large_fat32);
		GetDevices(0);
		return TRUE;
	case GDK_KEY_M:			// Ignore the boot marker of DD images
		ignore_boot_marker = !ignore_boot_marker;
		WriteSettingBool(SETTING_IGNORE_BOOT_MARKER, ignore_boot_marker);
		PrintStatusTimeout(lmprintf(MSG_319), ignore_boot_marker);
		return TRUE;
	case GDK_KEY_N:			// NTFS compression
		enable_ntfs_compression = !enable_ntfs_compression;
		PrintStatusTimeout(lmprintf(MSG_260), enable_ntfs_compression);
		return TRUE;
	case GDK_KEY_O:			// Save an optical disc to ISO
		OpticalDiscSaveImage();
		return TRUE;
	case GDK_KEY_P:			// Toggle ESP <-> Basic Data partition
		{
			int index = ComboBox_GetCurSel(hDeviceList);
			DWORD DeviceNum = (DWORD)ComboBox_GetItemData(hDeviceList, index);
			if (ToggleEsp(DeviceNum, 0))
				CyclePort(index);
		}
		return TRUE;
	case GDK_KEY_Q:			// File indexing
		enable_file_indexing = !enable_file_indexing;
		WriteSettingBool(SETTING_ENABLE_FILE_INDEXING, enable_file_indexing);
		PrintStatusTimeout(lmprintf(MSG_290), enable_file_indexing);
		return TRUE;
	case GDK_KEY_R:			// Reset the settings
		PrintStatus(STATUS_MSG_TIMEOUT, (ini_file != NULL && truncate(ini_file, 0) == 0) ? MSG_248 : MSG_249);
		return TRUE;
	case GDK_KEY_S:			// Image size check
		size_check = !size_check;
		PrintStatusTimeout(lmprintf(MSG_252), size_check);
		GetDevices(0);
		return TRUE;
	case GDK_KEY_T:			// Preserve timestamps
		preserve_timestamps = !preserve_timestamps;
		WriteSettingBool(SETTING_PRESERVE_TIMESTAMPS, preserve_timestamps);
		PrintStatusTimeout(lmprintf(MSG_269), preserve_timestamps);
		return TRUE;
	case GDK_KEY_U:			// Proper size units
		use_fake_units = !use_fake_units;
		WriteSettingBool(SETTING_USE_PROPER_SIZE_UNITS, !use_fake_units);
		PrintStatusTimeout(lmprintf(MSG_263), !use_fake_units);
		GetDevices(0);
		return TRUE;
	case GDK_KEY_W:			// VMWare disk detection
		enable_vmdk = !enable_vmdk;
		WriteSettingBool(SETTING_ENABLE_VMDK_DETECTION, enable_vmdk);
		PrintStatusTimeout(lmprintf(MSG_265), enable_vmdk);
		GetDevices(0);
		return TRUE;
	case GDK_KEY_Y:			// Force the update check
		force_update = (force_update > 0) ? 0 : 1;
		PrintStatusTimeout(lmprintf(MSG_259), force_update);
		return TRUE;
	case GDK_KEY_Z:			// Zero the drive
		zero_drive = TRUE;
		fast_zeroing = FALSE;
		PostMessage(hMainDialog, WM_COMMAND, (WPARAM)IDC_START, 0);
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Program entry point (WinMain() in rufus.c)                                */
/* ------------------------------------------------------------------------- */
static void PrintUsage(const char* appname)
{
	printf("\nUsage: %s [-x] [-h] [-f FILESYSTEM] [-i PATH] [-l LOCALE] [-w TIMEOUT]\n", appname);
	printf("  -x, --extra-devs\n");
	printf("     List extra devices, such as USB HDDs\n");
	printf("  -i PATH, --iso=PATH\n");
	printf("     Select the ISO image pointed by PATH to be used on startup\n");
	printf("  -l LOCALE, --locale=LOCALE\n");
	printf("     Select the locale to be used on startup\n");
	printf("  -f FILESYSTEM, --filesystem=FILESYSTEM\n");
	printf("     Preselect the file system to be preferred when formatting\n");
	printf("  -w TIMEOUT, --wait=TIMEOUT\n");
	printf("     Wait TIMEOUT tens of seconds for the global application mutex to be released.\n");
	printf("     Used when launching a newer version of " APPLICATION_NAME " from a running application.\n");
	printf("  -h, --help\n");
	printf("     This usage guide.\n");
}

/* Settings are stored in ~/.config/rufus/rufus.ini (same format as Rufus' portable mode) */
static void SetupDirectories(void)
{
	// Automated tests use their own (empty) settings, from $HOME
	const char* home = (linux_automation_active() && getenv("HOME") != NULL) ? getenv("HOME") : linux_invoking_user_home();
	char path[MAX_PATH];
	uid_t uid;
	gid_t gid;
	FILE* fd;
	static char ini_path[MAX_PATH];

	linux_invoking_user_ids(&uid, &gid);
	GetModuleFileNameU(NULL, app_dir, sizeof(app_dir));
	if (strrchr(app_dir, '/') != NULL)
		*(strrchr(app_dir, '/') + 1) = 0;
	static_strcpy(system_dir, "/usr/bin");
	static_strcpy(sysnative_dir, system_dir);
	// Downloaded bootloader files (signature checked) are cached by root
	static_strcpy(app_data_dir, "/var/cache/rufus");
	_mkdirExU(app_data_dir);
	chmod(app_data_dir, 0755);
	static_strcpy(user_dir, home);
	// A private temporary directory
	static_sprintf(private_temp_dir, "%s/rufus.XXXXXX", (getenv("TMPDIR") != NULL) ? getenv("TMPDIR") : "/tmp");
	if (mkdtemp(private_temp_dir) == NULL)
		static_strcpy(private_temp_dir, "/tmp");
	static_sprintf(temp_dir, "%s/", private_temp_dir);
	// The bad blocks log is created in %USERPROFILE%
	setenv("USERPROFILE", home, 1);

	static_sprintf(path, "%s/.config", home);
	if (mkdir(path, 0755) == 0 && chown(path, uid, gid) != 0)
		uprintf("Could not set the ownership of '%s'", path);
	static_sprintf(path, "%s/.config/rufus", home);
	if (mkdir(path, 0755) == 0 && chown(path, uid, gid) != 0)
		uprintf("Could not set the ownership of '%s'", path);
	static_sprintf(ini_path, "%s/.config/rufus/rufus.ini", home);
	fd = fopenU(ini_path, "a");
	if (fd != NULL) {
		fclose(fd);
		if (chown(ini_path, uid, gid) != 0)
			uprintf("Could not set the ownership of '%s'", ini_path);
		ini_file = ini_path;
	}
}

/*
 * Developer helper: save a screenshot of the visible Rufus windows to $RUFUS_SCREENSHOT
 * (requires the X11 GDK backend) and exit.
 */
static gboolean screenshot_cb(gpointer data)
{
	GList *list = gtk_window_list_toplevels(), *it;
	int n = 0;
	(void)data;
	for (it = list; it != NULL; it = it->next) {
		GtkWidget* w = GTK_WIDGET(it->data);
		GdkWindow* gw = gtk_widget_get_window(w);
		GdkPixbuf* pb;
		char path[MAX_PATH];
		if (gw == NULL || !gtk_widget_get_visible(w) || !GTK_IS_WINDOW(w) ||
			gtk_window_get_window_type(GTK_WINDOW(w)) != GTK_WINDOW_TOPLEVEL)
			continue;
		pb = gdk_pixbuf_get_from_window(gw, 0, 0, gdk_window_get_width(gw), gdk_window_get_height(gw));
		if (pb == NULL)
			continue;
		if (n == 0)
			static_strcpy(path, getenv("RUFUS_SCREENSHOT"));
		else
			static_sprintf(path, "%s.%d.png", getenv("RUFUS_SCREENSHOT"), n);
		gdk_pixbuf_save(pb, path, "png", NULL, NULL);
		g_object_unref(pb);
		n++;
	}
	g_list_free(list);
	if (getenv("RUFUS_SCREENSHOT_KEEP") == NULL) {
		// A modal dialog (nested main loop) may have been opened in the meantime
		if (gtk_main_level() > 1)
			exit(0);
		quit_requested = TRUE;
		gtk_main_quit();
	}
	return G_SOURCE_REMOVE;
}

/* Single instance check, like Windows' "Global/Rufus" mutex */
static BOOL AcquireInstanceLock(int wait)
{
	lock_fd = open("/run/rufus.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock_fd < 0)
		return TRUE;
	for (;;) {
		if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
			return TRUE;
		if (wait-- <= 0)
			return FALSE;
		Sleep(100);
	}
}

static void Cleanup(void)
{
	linux_release_all_mounts();
	linux_inhibit_automount(0, FALSE);
	if (private_temp_dir[0] != 0 && strcmp(private_temp_dir, "/tmp") != 0)
		SHDeleteDirectoryExU(NULL, private_temp_dir, FOF_NO_UI);
	if (lock_fd >= 0)
		close(lock_fd);
}

int main(int argc, char* argv[])
{
	const char* rufus_loc = "loc/rufus.loc";
	int i, opt, option_index = 0, wait_for_mutex = 0;
	char loc_file[MAX_PATH], *tmp, *locale_name = NULL, **argv_utf8 = argv;
	BOOL list_extra = FALSE;
	uint16_t lcid = GetUserDefaultUILanguage();
	struct option long_options[] = {
		{"extra-devs",  no_argument,       NULL, 'x'},
		{"gui",         no_argument,       NULL, 'g'},
		{"help",        no_argument,       NULL, 'h'},
		{"iso",         required_argument, NULL, 'i'},
		{"locale",      required_argument, NULL, 'l'},
		{"filesystem",  required_argument, NULL, 'f'},
		{"wait",        required_argument, NULL, 'w'},
		{0, 0, NULL, 0}
	};

	ui_init_threading();
	if (!gtk_init_check(&argc, &argv)) {
		fprintf(stderr, "%s: Could not initialize the user interface (no display?)\n", APPLICATION_NAME);
		return 1;
	}
	// Capture the log from the start (the log dialog itself is created with the main window)
	{
		extern void linux_log_append(const char* text);
		linux_set_log_sink(linux_log_append);
	}
	SetupDirectories();
	uprintf("*** " APPLICATION_NAME " init ***");

	while ((opt = getopt_long(argc, argv_utf8, "xghi:w:l:f:", long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'x':
			list_extra = TRUE;
			break;
		case 'g':
			break;
		case 'i':
			if (_access(optarg, 0) != -1) {
				safe_free(image_path);
				image_path = realpath(optarg, NULL);
				if (image_path == NULL)
					image_path = strdup(optarg);
				img_provided = TRUE;
			} else {
				printf("Could not find ISO image '%s'\n", optarg);
			}
			break;
		case 'l':
			if (isdigitU(optarg[0])) {
				lcid = (uint16_t)strtol(optarg, NULL, 0);
			} else {
				safe_free(locale_name);
				locale_name = strdup(optarg);
			}
			break;
		case 'f':
			if (isdigitU(optarg[0])) {
				preselected_fs = (int)strtol(optarg, NULL, 0);
			} else {
				for (i = 0; i < FS_MAX; i++) {
					if (safe_stricmp(optarg, FileSystemLabel[i]) == 0) {
						preselected_fs = i;
						break;
					}
				}
			}
			if ((preselected_fs < FS_UNKNOWN) || (preselected_fs >= FS_MAX))
				preselected_fs = FS_UNKNOWN;
			selected_fs = preselected_fs;
			break;
		case 'w':
			wait_for_mutex = atoi(optarg);
			break;
		case 'h':
		default:
			PrintUsage(argv[0]);
			return 0;
		}
	}

	uprintf("Will use settings from %s", (ini_file != NULL) ? ini_file : "defaults");
	tmp = ReadSettingStr(SETTING_LOCALE);
	if (tmp != NULL && tmp[0] != 0 && locale_name == NULL) {
		locale_name = safe_strdup(tmp);
		uprintf("found locale '%s'", locale_name);
	}
	if (locale_name == NULL)
		locale_name = strdup(linux_locale_name());

	// Restore user application settings
	advanced_mode_device = ReadSettingBool(SETTING_ADVANCED_MODE_DEVICE);
	advanced_mode_format = ReadSettingBool(SETTING_ADVANCED_MODE_FORMAT);
	preserve_timestamps = ReadSettingBool(SETTING_PRESERVE_TIMESTAMPS);
	use_fake_units = !ReadSettingBool(SETTING_USE_PROPER_SIZE_UNITS);
	is_vds_available = IsVdsAvailable(FALSE);
	use_vds = FALSE;	// The VDS formatting path is Windows only
	usb_debug = ReadSettingBool(SETTING_ENABLE_USB_DEBUG);
	cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
	use_rufus_mbr = !ReadSettingBool(SETTING_DISABLE_RUFUS_MBR);
	validate_md5sum = ReadSettingBool(SETTING_ENABLE_RUNTIME_VALIDATION);
	detect_fakes = !ReadSettingBool(SETTING_DISABLE_FAKE_DRIVES_CHECK);
	allow_dual_uefi_bios = ReadSettingBool(SETTING_ENABLE_WIN_DUAL_EFI_BIOS);
	force_large_fat32 = ReadSettingBool(SETTING_FORCE_LARGE_FAT32_FORMAT);
	enable_vmdk = ReadSettingBool(SETTING_ENABLE_VMDK_DETECTION);
	enable_file_indexing = ReadSettingBool(SETTING_ENABLE_FILE_INDEXING);
	enable_VHDs = !ReadSettingBool(SETTING_DISABLE_VHDS);
	enable_extra_hashes = ReadSettingBool(SETTING_ENABLE_EXTRA_HASHES);
	expert_mode = ReadSettingBool(SETTING_EXPERT_MODE);
	ignore_boot_marker = ReadSettingBool(SETTING_IGNORE_BOOT_MARKER);
	persistent_log = ReadSettingBool(SETTING_PERSISTENT_LOG);
	save_image_type = ReadSettingStr(SETTING_PREFERRED_SAVE_IMAGE_TYPE);
	// Restore the WUE options, if any
	wue_options = ReadSetting32(SETTING_WUE_OPTIONS);
	if ((wue_options >> 16) != 0) {
		uint32_t mask = wue_options >> 16;
		unattend_xml_mask &= ~mask;
		unattend_xml_mask |= (wue_options & mask);
	}
	default_thread_priority = ReadSetting32(SETTING_DEFAULT_THREAD_PRIORITY) + THREAD_PRIORITY_ABOVE_NORMAL;
	if (list_extra)
		enable_HDDs = TRUE;
	// libcdio log handler
	{
		extern void libcdio_log_handler(cdio_log_level_t level, const char* message);
		cdio_log_set_handler(libcdio_log_handler);
	}

	// Init localization
	init_localization();
	static_sprintf(loc_file, "%s/%s", linux_get_data_dir(), rufus_loc);
	ubprintf("Using loc file '%s'", loc_file);
	if ((!get_supported_locales(loc_file))
		|| ((selected_locale = get_locale_from_name(locale_name, FALSE)) == NULL
			&& (selected_locale = get_locale_from_lcid(lcid, TRUE)) == NULL)) {
		uprintf("FATAL: Could not access locale!");
		MessageBoxA(NULL, "The locale data is missing or invalid. This application will now exit.",
			"Fatal error", MB_ICONERROR | MB_SYSTEMMODAL);
		goto out;
	}
	selected_langid = get_language_id(selected_locale);

	GetWindowsVersion(&WindowsVersion);

	if (!IsCurrentProcessElevated() && getenv("RUFUS_DEVELOPER_NONROOT") == NULL) {
		uprintf("FATAL: No administrative privileges!");
		get_loc_data_file(loc_file, selected_locale);
		right_to_left_mode = ((selected_locale->ctrl_id) & LOC_RIGHT_TO_LEFT);
		MessageBoxA(NULL, lmprintf(MSG_289), lmprintf(MSG_288), MB_ICONERROR | MB_SYSTEMMODAL);
		goto out;
	}

	// Prevent 2 applications from running at the same time
	if (!AcquireInstanceLock(wait_for_mutex)) {
		get_loc_data_file(loc_file, selected_locale);
		right_to_left_mode = ((selected_locale->ctrl_id) & LOC_RIGHT_TO_LEFT);
		MessageBoxA(NULL, lmprintf(MSG_002), lmprintf(MSG_001), MB_ICONERROR | MB_SYSTEMMODAL);
		goto out;
	}
	// We're the only instance: release anything a previous (killed) instance may have left mounted
	if (IsCurrentProcessElevated())
		linux_cleanup_stale_mounts();

	EnablePrivileges();
	cpu_has_sha1_accel = DetectSHA1Acceleration();
	cpu_has_sha256_accel = DetectSHA256Acceleration();
	has_ffu_support = FALSE;
	bcdboot_supports_ex = FALSE;
	appstore_version = FALSE;

relaunch:
	ubprintf("Localization set to '%s'", selected_locale->txt[0]);
	right_to_left_mode = ((selected_locale->ctrl_id) & LOC_RIGHT_TO_LEFT);
	gtk_widget_set_default_direction(right_to_left_mode ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
	image_options = IMOP_WINTOGO;
	image_option_txt[0] = 0;
	select_index = 0;
	safe_free(fido_url);
	if (get_loc_data_file(loc_file, selected_locale))
		WriteSettingStr(SETTING_LOCALE, selected_locale->txt[0]);

	// Create the main window (the equivalent of WM_INITDIALOG)
	relaunch = FALSE;
	quit_requested = FALSE;
	hMainDialog = CreateMainDialog();
	// Automated tests run unattended: don't take the keyboard focus away from the user
	if (linux_automation_active()) {
		gtk_window_set_accept_focus(GTK_WINDOW(main_window), FALSE);
		gtk_window_set_focus_on_map(GTK_WINDOW(main_window), FALSE);
	}
	apply_localization(IDD_DIALOG, hMainDialog);
	if (!linux_automation_active())
		SetUpdateCheck();
	log_displayed = FALSE;
	linux_log_dialog_create();
	InitDialog(hMainDialog);
	GetDevices(0);
	EnableControls(TRUE, FALSE);
	UpdateImage(FALSE);
	if (!linux_automation_active())
		CheckForUpdates(FALSE);
	if (!linux_start_device_monitor())
		uprintf("WARNING: Could not start the device hotplug monitor");
	SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)(IsWindowEnabled(hStart) ? hStart : hSelectImage), TRUE);

	if (getenv("RUFUS_SCREENSHOT") != NULL)
		g_timeout_add(2500, screenshot_cb, NULL);
	linux_automation_start();

	gtk_main();

	if (relaunch) {
		HWND old = hMainDialog;
		GtkWidget* w = main_window;
		reset_localization(IDD_DIALOG);
		ctl_destroy_dialog(old);
		gtk_widget_destroy(w);
		main_window = NULL;
		goto relaunch;
	}

out:
	Cleanup();
	safe_free(locale_name);
	safe_free(image_path);
	exit_localization();
	return linux_automation_active() ? linux_automation_exit_code() : 0;
}
