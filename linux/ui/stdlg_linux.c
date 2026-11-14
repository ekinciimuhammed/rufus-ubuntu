/*
 * Rufus for Linux: standard dialog routines (GTK3 version of src/stdlg.c)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus Linux port contributors
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

/*
 * The dialogs below follow the behaviour of their Windows counterparts from
 * src/stdlg.c (texts, buttons, return values, settings), but are implemented
 * natively with GTK. They may be called from any thread: the actual dialog is
 * always run on the GTK main thread, and the caller is blocked until the user
 * has answered, exactly like Windows' modal DialogBox().
 *
 * Dialog localization goes through the regular apply_localization() calls, by
 * registering the localizable widgets with the control model (winctl.h).
 */

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <gtk/gtk.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "settings.h"
#include "license.h"
#include "winctl.h"
#include "automation_linux.h"
#include "compat.h"

extern char unattend_username[MAX_USERNAME_LENGTH];
extern int unattend_edition_index;
extern char hash_str[HASH_MAX][150];
extern BOOL enable_extra_hashes;
extern char user_dir[MAX_PATH];
extern const char* linux_get_data_dir(void);
extern void linux_set_log_sink(void (*sink)(const char* text));

/* Prototypes of the Linux specific functions from this file */
void linux_log_dialog_create(void);
void linux_log_dialog_toggle(void);
BOOL linux_log_dialog_visible(void);
void linux_log_append(const char* text);
char* linux_log_get_text(void);
void linux_settings_dialog(void);
void ShowHashDialog(void);

#define RESPONSE_MORE_INFO      1000
#define RESPONSE_LICENSE        1001
#define MAX_SELECTION_CHOICES   (IDC_SELECTION_CHOICEMAX - IDC_SELECTION_CHOICE1 + 1)
#define MAX_LIST_ITEMS          (IDC_LIST_ITEMMAX - IDC_LIST_ITEM1 + 1)

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Stack of the modal dialogs currently displayed, so that nested dialogs get the right parent */
static GPtrArray* dialog_stack = NULL;

static GtkWindow* top_window(void)
{
	if (dialog_stack != NULL && dialog_stack->len > 0)
		return GTK_WINDOW(g_ptr_array_index(dialog_stack, dialog_stack->len - 1));
	return ui_main_window();
}

static gint run_dialog(GtkWidget* dlg)
{
	gint r;
	if (dialog_stack == NULL)
		dialog_stack = g_ptr_array_new();
	g_ptr_array_add(dialog_stack, dlg);
	r = gtk_dialog_run(GTK_DIALOG(dlg));
	g_ptr_array_remove(dialog_stack, dlg);
	return r;
}

static GtkWidget* new_dialog(const char* title)
{
	GtkWidget* dlg = gtk_dialog_new();
	GtkWindow* parent = top_window();
	GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

	gtk_window_set_title(GTK_WINDOW(dlg), (title == NULL) ? APPLICATION_NAME : title);
	if (parent != NULL) {
		gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
		gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);
	} else {
		gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
	}
	gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
	gtk_window_set_icon_name(GTK_WINDOW(dlg), "rufus");
	gtk_container_set_border_width(GTK_CONTAINER(content), 8);
	gtk_box_set_spacing(GTK_BOX(content), 8);
	return dlg;
}

/* Run fn on the GTK main thread and wait for it to complete */
static void run_main_sync(void (*fn)(void* arg), void* arg)
{
	if (ui_is_main_thread())
		fn(arg);
	else
		ui_run_on_main_sync(fn, arg);
}

/* Remove the CR from CR/LF sequences, as GTK doesn't need them */
static char* strip_cr(const char* str)
{
	char *r, *d;
	const char* s;
	if (str == NULL)
		return g_strdup("");
	r = g_malloc(strlen(str) + 1);
	for (s = str, d = r; *s != 0; s++)
		if (*s != '\r')
			*d++ = *s;
	*d = 0;
	return r;
}

/* Open a URL with the desktop of the user who launched us (we may be running as root) */
static void open_url(const char* url)
{
	GError* err = NULL;
	const char* argv[16];
	char *runtime = NULL, *bus = NULL, *disp = NULL, *wl = NULL, *xauth = NULL;
	uid_t uid = 0;
	gid_t gid = 0;
	int n = 0;

	if (url == NULL || url[0] == 0)
		return;
	linux_invoking_user_ids(&uid, &gid);
	if (geteuid() == 0 && uid != 0) {
		runtime = g_strdup_printf("XDG_RUNTIME_DIR=/run/user/%u", (unsigned)uid);
		bus = g_strdup_printf("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus", (unsigned)uid);
		argv[n++] = "runuser";
		argv[n++] = "-u";
		argv[n++] = linux_invoking_user_name();
		argv[n++] = "--";
		argv[n++] = "env";
		argv[n++] = runtime;
		argv[n++] = bus;
		if (getenv("DISPLAY") != NULL) {
			disp = g_strdup_printf("DISPLAY=%s", getenv("DISPLAY"));
			argv[n++] = disp;
		}
		if (getenv("WAYLAND_DISPLAY") != NULL) {
			wl = g_strdup_printf("WAYLAND_DISPLAY=%s", getenv("WAYLAND_DISPLAY"));
			argv[n++] = wl;
		}
		if (getenv("XAUTHORITY") != NULL) {
			xauth = g_strdup_printf("XAUTHORITY=%s", getenv("XAUTHORITY"));
			argv[n++] = xauth;
		}
	}
	argv[n++] = "xdg-open";
	argv[n++] = url;
	argv[n] = NULL;
	if (!g_spawn_async(NULL, (gchar**)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
		G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, &err)) {
		uprintf("Could not open '%s': %s", url, (err == NULL) ? "Unknown error" : err->message);
		if (err != NULL)
			g_error_free(err);
	}
	g_free(runtime);
	g_free(bus);
	g_free(disp);
	g_free(wl);
	g_free(xauth);
}

static gboolean on_activate_link(GtkLabel* label, gchar* uri, gpointer data)
{
	(void)label; (void)data;
	open_url(uri);
	return TRUE;
}

/*
 * Convert the (small) subset of RTF that Rufus uses for its rich text controls
 * into plain UTF-8 text. Bold sections are delimited with \x01 (on) and \x02 (off).
 */
static char* rtf_to_text(const char* rtf)
{
	GString* out = g_string_new(NULL);
	const char* p = rtf;
	int depth = 0, skip_depth = -1, n;
	char word[32];
	long param;
	BOOL has_param, neg;

	while (p != NULL && *p != 0) {
		if (*p == '{') {
			depth++;
			if (skip_depth < 0 && strncmp(&p[1], "\\fonttbl", 8) == 0)
				skip_depth = depth;
			p++;
			continue;
		}
		if (*p == '}') {
			if (skip_depth == depth)
				skip_depth = -1;
			depth--;
			p++;
			continue;
		}
		if (*p == '\r' || *p == '\n') {
			p++;
			continue;
		}
		if (skip_depth >= 0) {
			p++;
			continue;
		}
		if (*p != '\\') {
			g_string_append_c(out, *p++);
			continue;
		}
		/* Control word or symbol */
		p++;
		if (*p == '\\' || *p == '{' || *p == '}') {
			g_string_append_c(out, *p++);
			continue;
		}
		if (*p == '\'') {
			/* \'hh: 8-bit character (Windows-1252, approximated as Latin-1) */
			if (g_ascii_isxdigit(p[1]) && g_ascii_isxdigit(p[2])) {
				g_string_append_unichar(out, (gunichar)((g_ascii_xdigit_value(p[1]) << 4) | g_ascii_xdigit_value(p[2])));
				p += 3;
			} else {
				p++;
			}
			continue;
		}
		if (*p == '\r' || *p == '\n') {
			g_string_append_c(out, '\n');
			p++;
			continue;
		}
		for (n = 0; g_ascii_isalpha(*p) && n < (int)sizeof(word) - 1; p++)
			word[n++] = *p;
		word[n] = 0;
		if (n == 0) {
			/* Unknown control symbol, e.g. "\~" */
			if (*p == '~')
				g_string_append(out, "\xc2\xa0");
			if (*p != 0)
				p++;
			continue;
		}
		has_param = FALSE;
		neg = FALSE;
		param = 0;
		if (*p == '-') {
			neg = TRUE;
			p++;
		}
		while (g_ascii_isdigit(*p)) {
			has_param = TRUE;
			param = param * 10 + (*p++ - '0');
		}
		if (neg)
			param = -param;
		if (*p == ' ')
			p++;
		if (strcmp(word, "line") == 0 || strcmp(word, "par") == 0)
			g_string_append_c(out, '\n');
		else if (strcmp(word, "tab") == 0)
			g_string_append_c(out, '\t');
		else if (strcmp(word, "b") == 0)
			g_string_append_c(out, (has_param && param == 0) ? '\x02' : '\x01');
		else if (strcmp(word, "u") == 0 && has_param) {
			g_string_append_unichar(out, (gunichar)((param < 0) ? param + 65536 : param));
			/* Skip the ANSI fallback character */
			if (*p != 0 && *p != '\\' && *p != '{' && *p != '}')
				p++;
		}
		/* Everything else (font sizes, character sets, direction...) is ignored */
	}
	return g_string_free(out, FALSE);
}

/* Convert text (with \x01/\x02 bold markers) to Pango markup, turning URLs into links */
static char* text_to_markup(const char* text)
{
	static const char* prefixes[] = { "https://", "http://", "mailto:" };
	GString* m = g_string_new(NULL);
	const char* p = text;
	BOOL bold = FALSE;
	size_t i, len;
	char *url, *esc;

	while (p != NULL && *p != 0) {
		if (*p == '\x01') {
			if (!bold)
				g_string_append(m, "<b>");
			bold = TRUE;
			p++;
			continue;
		}
		if (*p == '\x02') {
			if (bold)
				g_string_append(m, "</b>");
			bold = FALSE;
			p++;
			continue;
		}
		for (i = 0; i < ARRAYSIZE(prefixes); i++)
			if (strncmp(p, prefixes[i], strlen(prefixes[i])) == 0)
				break;
		if (i < ARRAYSIZE(prefixes) && (p == text || g_ascii_isspace(p[-1]) || p[-1] == '(' || p[-1] == ':')) {
			len = strcspn(p, " \t\r\n\x01\x02<>\"");
			while (len > 0 && strchr(".,;:)", p[len - 1]) != NULL)
				len--;
			url = g_strndup(p, len);
			esc = g_markup_escape_text(url, -1);
			g_string_append_printf(m, "<a href=\"%s\">%s</a>", esc, esc);
			g_free(esc);
			g_free(url);
			p += len;
			continue;
		}
		switch (*p) {
		case '&':
			g_string_append(m, "&amp;");
			break;
		case '<':
			g_string_append(m, "&lt;");
			break;
		case '>':
			g_string_append(m, "&gt;");
			break;
		default:
			g_string_append_c(m, *p);
			break;
		}
		p++;
	}
	if (bold)
		g_string_append(m, "</b>");
	return g_string_free(m, FALSE);
}

static char* rtf_to_markup(const char* rtf)
{
	char* text = rtf_to_text(rtf);
	char* markup = text_to_markup(text);
	g_free(text);
	return markup;
}

/* A word-wrapping label, with clickable links, from Rufus' RTF */
static GtkWidget* new_rtf_label(const char* rtf, int width_chars)
{
	char* markup = rtf_to_markup(rtf);
	GtkWidget* label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(label), width_chars);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
	gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	g_signal_connect(label, "activate-link", G_CALLBACK(on_activate_link), NULL);
	g_free(markup);
	return label;
}

static GtkWidget* new_message_label(const char* text)
{
	char* str = strip_cr(text);
	GtkWidget* label = gtk_label_new(str);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 64);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
	gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
	g_free(str);
	return label;
}

/* Rufus' application icon, scaled to size (falls back to the icon theme) */
static GtkWidget* new_app_icon(int size)
{
	char path[MAX_PATH];
	GdkPixbuf* pb;
	GtkWidget* img;
	static_sprintf(path, "%s/icons/rufus-128.png", linux_get_data_dir());
	pb = gdk_pixbuf_new_from_file_at_size(path, size, size, NULL);
	if (pb == NULL)
		return gtk_image_new_from_icon_name("rufus", GTK_ICON_SIZE_DIALOG);
	img = gtk_image_new_from_pixbuf(pb);
	g_object_unref(pb);
	return img;
}

static const char* message_icon_name(int type)
{
	switch (type & 0xF0) {
	case MB_ICONERROR:
		return "dialog-error";
	case MB_ICONWARNING:
		return "dialog-warning";
	case MB_ICONQUESTION:
		return "dialog-question";
	default:
		return "dialog-information";
	}
}

/* Returns a newly allocated path to the invoking user's "Downloads" directory */
static char* get_downloads_dir(void)
{
	const char* home = linux_invoking_user_home();
	char path[MAX_PATH], line[1024], *p, *e, *r = NULL;
	FILE* f;

	static_sprintf(path, "%s/.config/user-dirs.dirs", home);
	f = fopenU(path, "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strncmp(line, "XDG_DOWNLOAD_DIR=", 17) != 0)
				continue;
			p = &line[17];
			if (*p == '"')
				p++;
			e = strchr(p, '"');
			if (e != NULL)
				*e = 0;
			p[strcspn(p, "\r\n")] = 0;
			if (strncmp(p, "$HOME", 5) == 0)
				r = g_strdup_printf("%s%s", home, &p[5]);
			else if (p[0] == '/')
				r = g_strdup(p);
			break;
		}
		fclose(f);
	}
	if (r != NULL && g_file_test(r, G_FILE_TEST_IS_DIR))
		return r;
	g_free(r);
	r = g_strdup_printf("%s/Downloads", home);
	if (g_file_test(r, G_FILE_TEST_IS_DIR))
		return r;
	g_free(r);
	return g_strdup(home);
}

/*
 * Register the localizable widgets of a dialog with the control model, so that
 * apply_localization() can be used. Widget may be NULL for controls that only
 * exist in the Windows version of a dialog (prevents spurious localization warnings).
 */
static HWND loc_dialog(int dlg_id, GtkWidget* window)
{
	return ctl_create(NULL, dlg_id, CT_DIALOG, window);
}

static void loc_control(HWND hDlg, int id, ctl_type type, GtkWidget* widget)
{
	ctl_create(hDlg, id, type, widget);
}

static void loc_release(int dlg_id, HWND hDlg)
{
	reset_localization(dlg_id);
	ctl_destroy_dialog(hDlg);
}

/* ------------------------------------------------------------------------- */
/* File dialog                                                               */
/* ------------------------------------------------------------------------- */
typedef struct {
	BOOL save;
	char* path;
	const ext_t* ext;
	UINT* selected_ext;
	char* result;
	GPtrArray* filters;
} file_dialog_ctx;

/* Make a glob pattern case insensitive ("*.iso" -> "*.[iI][sS][oO]") */
static char* ci_pattern(const char* pattern)
{
	GString* s = g_string_new(NULL);
	for (; *pattern != 0; pattern++) {
		if (g_ascii_isalpha(*pattern))
			g_string_append_printf(s, "[%c%c]", g_ascii_tolower(*pattern), g_ascii_toupper(*pattern));
		else
			g_string_append_c(s, *pattern);
	}
	return g_string_free(s, FALSE);
}

/* Extension (".iso") of the first pattern of a filter, or NULL */
static char* filter_extension(const file_dialog_ctx* ctx, int index)
{
	const char* spec;
	char* ext;
	size_t len;
	if (index < 0 || (size_t)index >= ctx->ext->count)
		return NULL;
	spec = ctx->ext->extension[index];
	if (spec == NULL || spec[0] != '*' || spec[1] != '.')
		return NULL;
	len = strcspn(&spec[1], ";, ");
	if (len <= 1)
		return NULL;
	ext = g_strndup(&spec[1], len);
	/* Wildcard extensions (e.g. "*.*") can't be used as a default */
	if (strchr(ext, '*') != NULL || strchr(ext, '?') != NULL) {
		g_free(ext);
		return NULL;
	}
	return ext;
}

static int get_filter_index(GtkFileChooser* chooser, file_dialog_ctx* ctx)
{
	GtkFileFilter* f = gtk_file_chooser_get_filter(chooser);
	guint i;
	for (i = 0; i < ctx->filters->len; i++)
		if (g_ptr_array_index(ctx->filters, i) == f)
			return (int)i;
	return 0;
}

/* Like Windows' IFileSaveDialog, update the extension of the file name when the filter changes */
static void on_filter_changed(GObject* obj, GParamSpec* pspec, gpointer data)
{
	GtkFileChooser* chooser = GTK_FILE_CHOOSER(obj);
	file_dialog_ctx* ctx = (file_dialog_ctx*)data;
	char *name, *ext, *dot, *new_name;
	(void)pspec;

	ext = filter_extension(ctx, get_filter_index(chooser, ctx));
	name = gtk_file_chooser_get_current_name(chooser);
	if (ext != NULL && name != NULL && name[0] != 0) {
		dot = strrchr(name, '.');
		if (dot != NULL)
			*dot = 0;
		new_name = g_strdup_printf("%s%s", name, ext);
		gtk_file_chooser_set_current_name(chooser, new_name);
		g_free(new_name);
	}
	g_free(name);
	g_free(ext);
}

static void file_dialog_main(void* arg)
{
	file_dialog_ctx* ctx = (file_dialog_ctx*)arg;
	GtkWidget* dlg;
	GtkFileChooser* chooser;
	GtkFileFilter* filter;
	char **patterns, *pat, *folder = NULL, *filename, *ext;
	int i, j, sel;

	ctx->filters = g_ptr_array_new();
	dlg = gtk_file_chooser_dialog_new(g_dgettext("gtk30", ctx->save ? "Save File" : "Open File"), top_window(),
		ctx->save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
		g_dgettext("gtk30", "_Cancel"), GTK_RESPONSE_CANCEL,
		g_dgettext("gtk30", ctx->save ? "_Save" : "_Open"), GTK_RESPONSE_ACCEPT, NULL);
	chooser = GTK_FILE_CHOOSER(dlg);
	gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
	gtk_file_chooser_set_local_only(chooser, TRUE);
	if (ctx->save)
		gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);

	/* Set up the file extension filters (plus "All files") */
	for (i = 0; i < (int)ctx->ext->count; i++) {
		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, ctx->ext->description[i]);
		patterns = g_strsplit_set(ctx->ext->extension[i], ";,", -1);
		for (j = 0; patterns[j] != NULL; j++) {
			g_strstrip(patterns[j]);
			if (patterns[j][0] == 0)
				continue;
			pat = ci_pattern(patterns[j]);
			gtk_file_filter_add_pattern(filter, pat);
			g_free(pat);
		}
		g_strfreev(patterns);
		gtk_file_chooser_add_filter(chooser, filter);
		g_ptr_array_add(ctx->filters, filter);
	}
	filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, lmprintf(MSG_107));
	gtk_file_filter_add_pattern(filter, "*");
	gtk_file_chooser_add_filter(chooser, filter);
	g_ptr_array_add(ctx->filters, filter);

	/* The selected extension index is 1 based, with 0 also meaning the first entry */
	sel = (ctx->selected_ext == NULL || *ctx->selected_ext == 0) ? 0 : (int)*ctx->selected_ext - 1;
	if (sel < 0 || sel >= (int)ctx->filters->len)
		sel = 0;
	gtk_file_chooser_set_filter(chooser, GTK_FILE_FILTER(g_ptr_array_index(ctx->filters, sel)));

	if (ctx->path == NULL || ctx->path[0] == 0) {
		folder = get_downloads_dir();
		gtk_file_chooser_set_current_folder(chooser, folder);
		g_free(folder);
	} else {
		folder = linux_path(ctx->path);
		if (folder != NULL)
			gtk_file_chooser_set_current_folder(chooser, folder);
		free(folder);
	}
	if (ctx->save) {
		gtk_file_chooser_set_current_name(chooser, (ctx->ext->filename == NULL) ? "" : ctx->ext->filename);
		g_signal_connect(dlg, "notify::filter", G_CALLBACK(on_filter_changed), ctx);
	}

	if (run_dialog(dlg) == GTK_RESPONSE_ACCEPT) {
		filename = gtk_file_chooser_get_filename(chooser);
		if (filename != NULL) {
			/* Windows appends the default extension when none was provided */
			char* basename = g_path_get_basename(filename);
			BOOL has_ext = (strchr(basename, '.') != NULL);
			g_free(basename);
			if (ctx->save && !has_ext) {
				ext = filter_extension(ctx, get_filter_index(chooser, ctx));
				if (ext != NULL) {
					char* tmp = g_strdup_printf("%s%s", filename, ext);
					g_free(filename);
					filename = tmp;
					g_free(ext);
				}
			}
			ctx->result = strdup(filename);
			g_free(filename);
		}
	}
	if (ctx->selected_ext != NULL)
		*ctx->selected_ext = (UINT)get_filter_index(chooser, ctx) + 1;
	gtk_widget_destroy(dlg);
	g_ptr_array_free(ctx->filters, TRUE);
}

char* FileDialog(BOOL save, char* path, const ext_t* ext, UINT* selected_ext)
{
	file_dialog_ctx ctx = { 0 };

	if ((ext == NULL) || (ext->count == 0) || (ext->extension == NULL) || (ext->description == NULL))
		return NULL;
	if (linux_automation_active())
		return linux_automation_file_dialog(save, ext, selected_ext);
	dialog_showing++;
	ctx.save = save;
	ctx.path = path;
	ctx.ext = ext;
	ctx.selected_ext = selected_ext;
	run_main_sync(file_dialog_main, &ctx);
	dialog_showing--;
	return ctx.result;
}

/* ------------------------------------------------------------------------- */
/* License and About dialogs                                                 */
/* ------------------------------------------------------------------------- */
static GtkWidget* new_text_view(const char* text, BOOL monospace)
{
	GtkWidget* view = gtk_text_view_new();
	char* str = strip_cr(text);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), monospace);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 4);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 4);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), str, -1);
	g_free(str);
	return view;
}

static void license_dialog(void)
{
	GtkWidget *dlg, *sw, *view, *close;
	HWND hDlg;

	dlg = new_dialog("Rufus License");
	gtk_window_set_default_size(GTK_WINDOW(dlg), 640, 480);
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_widget_set_hexpand(sw, TRUE);
	view = new_text_view(gplv3, TRUE);
	/* Like on Windows, the license is always displayed left to right */
	gtk_widget_set_direction(view, GTK_TEXT_DIR_LTR);
	gtk_container_add(GTK_CONTAINER(sw), view);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), sw, TRUE, TRUE, 0);
	close = gtk_dialog_add_button(GTK_DIALOG(dlg), "Close", GTK_RESPONSE_CLOSE);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);

	hDlg = loc_dialog(IDD_LICENSE, dlg);
	loc_control(hDlg, IDCANCEL, CT_BUTTON, close);
	apply_localization(IDD_LICENSE, hDlg);

	gtk_widget_show_all(dlg);
	run_dialog(dlg);
	loc_release(IDD_LICENSE, hDlg);
	gtk_widget_destroy(dlg);
}

static void about_main(void* arg)
{
	INT_PTR* ret = (INT_PTR*)arg;
	GtkWidget *dlg, *content, *hbox, *vbox, *icon, *blurb, *sw, *copyrights, *license, *ok, *bbox;
	char about_blurb[2048], *copyrights_rtf;
	HWND hDlg;
	gint r;

	dlg = new_dialog("About Rufus");
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
	content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	icon = new_app_icon(48);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

	static_sprintf(about_blurb, about_blurb_format, lmprintf(MSG_174|MSG_RTF),
		lmprintf(MSG_175|MSG_RTF, rufus_version[0], rufus_version[1], rufus_version[2]),
		"Copyright © 2011-2026 Pete Batard",
		lmprintf(MSG_176|MSG_RTF), lmprintf(MSG_177|MSG_RTF), lmprintf(MSG_178|MSG_RTF));
	blurb = new_rtf_label(about_blurb, 64);
	gtk_box_pack_start(GTK_BOX(vbox), blurb, FALSE, FALSE, 0);

	/* The Linux port credits are listed first in the additional copyrights */
	copyrights_rtf = g_strdup_printf("{\\rtf1\\ansi\n"
		"Linux port: GTK3 user interface and Linux platform layer\\line\n"
		"Copyright © 2026 Rufus Linux port contributors\\line\n"
		"\\line\n}%s", additional_copyrights);
	copyrights = new_rtf_label(copyrights_rtf, 64);
	g_free(copyrights_rtf);
	gtk_widget_set_margin_start(copyrights, 4);
	gtk_widget_set_margin_end(copyrights, 4);
	gtk_widget_set_margin_top(copyrights, 4);
	gtk_widget_set_margin_bottom(copyrights, 4);
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(sw, -1, 160);
	gtk_container_add(GTK_CONTAINER(sw), copyrights);
	gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);

	license = gtk_dialog_add_button(GTK_DIALOG(dlg), "License", RESPONSE_LICENSE);
	ok = gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
	bbox = gtk_widget_get_parent(license);
	if (GTK_IS_BUTTON_BOX(bbox))
		gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(bbox), license, TRUE);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

	hDlg = loc_dialog(IDD_ABOUTBOX, dlg);
	loc_control(hDlg, IDC_ABOUT_LICENSE, CT_BUTTON, license);
	loc_control(hDlg, IDOK, CT_BUTTON, ok);
	apply_localization(IDD_ABOUTBOX, hDlg);

	gtk_widget_show_all(dlg);
	gtk_widget_grab_focus(ok);
	do {
		r = run_dialog(dlg);
		if (r == RESPONSE_LICENSE)
			license_dialog();
	} while (r == RESPONSE_LICENSE);
	loc_release(IDD_ABOUTBOX, hDlg);
	gtk_widget_destroy(dlg);
	*ret = IDOK;
}

INT_PTR CreateAboutBox(void)
{
	INT_PTR r = IDOK;
	dialog_showing++;
	run_main_sync(about_main, &r);
	dialog_showing--;
	return r;
}

/* ------------------------------------------------------------------------- */
/* Notification dialog                                                       */
/* ------------------------------------------------------------------------- */
typedef struct {
	int type;
	const char* dont_display_setting;
	const notification_info* more_info;
	const char* title;
	const char* text;
	int ret;
} notification_ctx;

static void notification_main(void* arg)
{
	notification_ctx* ctx = (notification_ctx*)arg;
	GtkWidget *dlg, *content, *hbox, *image, *label, *check = NULL, *more = NULL, *bbox;
	HWND hDlg;
	int r;

	dlg = new_dialog(ctx->title);
	/* Prevent resizing and closing other than through the buttons, like on Windows */
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
	gtk_window_set_deletable(GTK_WINDOW(dlg), FALSE);
	content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	image = gtk_image_new_from_icon_name(message_icon_name(ctx->type), GTK_ICON_SIZE_DIALOG);
	gtk_widget_set_valign(image, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);
	label = new_message_label(ctx->text);
	gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);
	if (ctx->dont_display_setting != NULL) {
		check = gtk_check_button_new_with_label(lmprintf(MSG_127));
		gtk_box_pack_start(GTK_BOX(content), check, FALSE, FALSE, 0);
	}

	hDlg = loc_dialog(IDD_NOTIFICATION, dlg);
	if ((ctx->more_info != NULL) && (ctx->more_info->callback != NULL)) {
		more = gtk_dialog_add_button(GTK_DIALOG(dlg), "More information", RESPONSE_MORE_INFO);
		bbox = gtk_widget_get_parent(more);
		if (GTK_IS_BUTTON_BOX(bbox))
			gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(bbox), more, TRUE);
		loc_control(hDlg, IDC_MORE_INFO, CT_BUTTON, more);
	} else {
		loc_control(hDlg, IDC_MORE_INFO, CT_BUTTON, NULL);
	}
	/* The button texts are set according to the type, after the localization of the dialog */
	loc_control(hDlg, IDYES, CT_BUTTON, NULL);
	loc_control(hDlg, IDNO, CT_BUTTON, NULL);
	apply_localization(IDD_NOTIFICATION, hDlg);

	/* Same layout and texts as the Windows dialog (IDABORT | IDYES | IDNO) */
	switch (ctx->type & 0x0F) {
	case MB_OKCANCEL:
		gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", IDYES);
		gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_007), IDNO);
		break;
	case MB_YESNO:
		gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_008), IDYES);
		gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_009), IDNO);
		break;
	case MB_YESNOCANCEL:
		gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_008), IDABORT);
		gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_009), IDYES);
		gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_007), IDNO);
		break;
	case MB_OK:
		gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", IDNO);
		break;
	case MB_ABORTRETRYIGNORE:
		/* Windows takes these from user32.dll.mui, which has no Linux equivalent */
		gtk_dialog_add_button(GTK_DIALOG(dlg), "Abort", IDABORT);
		gtk_dialog_add_button(GTK_DIALOG(dlg), "Retry", IDYES);
		gtk_dialog_add_button(GTK_DIALOG(dlg), "Ignore", IDNO);
		break;
	default:	// One single 'Close' button
		gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_006), IDNO);
		break;
	}
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), IDNO);

	gtk_widget_show_all(dlg);
	for (;;) {
		r = run_dialog(dlg);
		if (r == RESPONSE_MORE_INFO) {
			if (ctx->more_info->id == MORE_INFO_URL)
				open_url(ctx->more_info->url);
			else if (ctx->more_info->id == IDD_UPDATE_POLICY)
				linux_settings_dialog();
			else
				uprintf("No Linux equivalent for dialog %d", ctx->more_info->id);
			continue;
		}
		/* Escape key: the Windows dialog doesn't process IDCANCEL either */
		if (r == GTK_RESPONSE_DELETE_EVENT)
			continue;
		if (r == GTK_RESPONSE_NONE)
			r = IDNO;
		break;
	}
	// NB: Upstream always records the Secure Boot notice setting, whatever the setting provided
	if (check != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)))
		WriteSettingBool(SETTING_DISABLE_SECURE_BOOT_NOTICE, TRUE);
	switch (r) {
	case IDYES:
		// Return IDOK/IDRETRY for calls that expect it
		switch (ctx->type & 0x0F) {
		case MB_OKCANCEL:
			r = IDOK;
			break;
		case MB_ABORTRETRYIGNORE:
			r = IDRETRY;
			break;
		case MB_YESNOCANCEL:
			r = IDNO;
			break;
		}
		break;
	case IDNO:
		// Return IDCANCEL/IDOK/IDIGNORE for calls that expect it
		switch (ctx->type & 0x0F) {
		case MB_OKCANCEL:
			r = IDCANCEL;
			break;
		case MB_OK:
			r = IDOK;
			break;
		case MB_ABORTRETRYIGNORE:
			r = IDIGNORE;
			break;
		case MB_YESNOCANCEL:
			r = IDCANCEL;
			break;
		}
		break;
	case IDABORT:
		if ((ctx->type & 0x0F) == MB_YESNOCANCEL)
			r = IDYES;
		break;
	}
	ctx->ret = r;
	loc_release(IDD_NOTIFICATION, hDlg);
	gtk_widget_destroy(dlg);
}

/*
 * Display a custom notification
 */
int NotificationEx(int type, const char* dont_display_setting, const notification_info* more_info, const char* title, const char* format, ...)
{
	notification_ctx ctx = { 0 };
	char* text;
	va_list args;

	dialog_showing++;
	text = (char*)malloc(LOC_MESSAGE_SIZE);
	if (text == NULL) {
		dialog_showing--;
		return FALSE;
	}
	va_start(args, format);
	safe_vsnprintf(text, LOC_MESSAGE_SIZE - 1, format, args);
	va_end(args);
	text[LOC_MESSAGE_SIZE - 1] = 0;
	/* Automated tests (see automation_linux.c) answer dialogs without user interaction */
	if (linux_automation_active()) {
		int r = linux_automation_notification(type, title, text);
		free(text);
		dialog_showing--;
		return r;
	}
	ctx.type = type;
	ctx.dont_display_setting = dont_display_setting;
	ctx.more_info = more_info;
	ctx.title = title;
	ctx.text = text;
	run_main_sync(notification_main, &ctx);
	free(text);
	dialog_showing--;
	return ctx.ret;
}

/* ------------------------------------------------------------------------- */
/* Selection dialog                                                          */
/* ------------------------------------------------------------------------- */
typedef struct {
	char* title;
	char* message;
	selection_dialog_options_t* options;
	int ret;
} selection_ctx;

typedef struct {
	selection_dialog_options_t* options;
	int n;
	GtkWidget* choice[MAX_SELECTION_CHOICES];
	GtkWidget* row[MAX_SELECTION_CHOICES];
	GtkWidget* username;
	GtkWidget* edition;
	GArray* edition_data;
	GtkWidget* ok;
} selection_state;

static BOOL selection_checked(selection_state* st, int index)
{
	if (index < 0 || index >= st->n)
		return FALSE;
	return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->choice[index]));
}

static void on_selection_toggled(GtkToggleButton* button, gpointer data)
{
	selection_state* st = (selection_state*)data;
	selection_dialog_options_t* o = st->options;
	BOOL enable = TRUE;
	int i, idx = -1;

	for (i = 0; i < st->n; i++)
		if (GTK_WIDGET(button) == st->choice[i])
			idx = i;
	if (idx < 0)
		return;
	if (o->flags & SELECTION_NEEDS_ALL_TO_PROCEED) {
		// Check if all the currently displayed checkboxes are checked to enable/disable the OK button
		for (i = 0; i < st->n; i++)
			enable = enable && selection_checked(st, i);
		gtk_widget_set_sensitive(st->ok, enable);
	} else if (o->edition_index > 0 && o->username_index > 0 && o->regional_index > 0 &&
		(idx == o->username_index - 1 || idx == o->regional_index - 1 || idx == o->privacy_index - 1)) {
		// Show the silent install option only if local account + regional settings + data collection are checked
		enable = selection_checked(st, o->username_index - 1) && selection_checked(st, o->regional_index - 1) &&
			selection_checked(st, o->privacy_index - 1);
		if (o->edition_index - 1 < st->n)
			gtk_widget_set_visible(st->row[o->edition_index - 1], enable);
	}
}

static void selection_main(void* arg)
{
	selection_ctx* ctx = (selection_ctx*)arg;
	selection_dialog_options_t* o = ctx->options;
	selection_state st = { 0 };
	StrArray edition_name = { 0 }, edition_index = { 0 };
	GtkWidget *dlg, *content, *hbox, *vbox, *icon, *label;
	GSList* group = NULL;
	char username[128] = { 0 };
	DWORD size = sizeof(username);
	int i, m, r, mask;

	st.options = o;
	st.n = (int)o->choices.Index;
	// Don't overflow our max radio button
	if (st.n > MAX_SELECTION_CHOICES) {
		uprintf("WARNING: Too many options requested for Selection (%d vs %d)",
			st.n, MAX_SELECTION_CHOICES - 1);
		st.n = MAX_SELECTION_CHOICES - 1;
	}
	StrArrayCreate(&edition_name, 16);
	StrArrayCreate(&edition_index, 16);
	// NB: GetEditions() may cause SelectionDialog() to be re-entered!
	if (o->edition_index > 0)
		IGNORE_RETVAL(GetEditions(&edition_name, &edition_index));

	dlg = new_dialog(ctx->title);
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
	content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	icon = gtk_image_new_from_icon_name((o->flags & SELECTION_USE_WARNING_ICON) ?
		"dialog-warning" : "dialog-question", GTK_ICON_SIZE_DIALOG);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	label = new_message_label(ctx->message);
	gtk_widget_set_margin_bottom(label, 4);
	gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

	for (i = 0; i < st.n; i++) {
		st.row[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		if (o->style == BS_AUTOCHECKBOX) {
			st.choice[i] = gtk_check_button_new_with_label(o->choices.String[i]);
		} else {
			st.choice[i] = gtk_radio_button_new_with_label(group, o->choices.String[i]);
			group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(st.choice[i]));
		}
		gtk_box_pack_start(GTK_BOX(st.row[i]), st.choice[i], FALSE, FALSE, 0);
		// Set tooltips, if any
		if (i < (int)o->tooltips.Index && o->tooltips.String[i] != NULL)
			gtk_widget_set_tooltip_text(st.choice[i], o->tooltips.String[i]);
		// Set up the username edit box
		if (i == o->username_index - 1) {
			unattend_username[0] = 0;
			st.username = gtk_entry_new();
			gtk_entry_set_max_length(GTK_ENTRY(st.username), MAX_USERNAME_LENGTH - 1);
			gtk_entry_set_width_chars(GTK_ENTRY(st.username), 16);
			if (!GetUserNameU(username, &size) || username[0] == 0)
				static_strcpy(username, "User");
			gtk_entry_set_text(GTK_ENTRY(st.username), username);
			gtk_box_pack_start(GTK_BOX(st.row[i]), st.username, FALSE, FALSE, 0);
		}
		// Set up the edition combo box
		if (i == o->edition_index - 1 && edition_name.String != NULL && edition_name.Index > 0) {
			st.edition = gtk_combo_box_text_new();
			st.edition_data = g_array_new(FALSE, TRUE, sizeof(int));
			for (m = 0; m < (int)edition_name.Index; m++) {
				int data = atoi(edition_index.String[m]);
				gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st.edition), edition_name.String[m]);
				g_array_append_val(st.edition_data, data);
			}
			if (unattend_edition_index >= 0 && unattend_edition_index < (int)edition_name.Index)
				gtk_combo_box_set_active(GTK_COMBO_BOX(st.edition), unattend_edition_index);
			gtk_box_pack_start(GTK_BOX(st.row[i]), st.edition, FALSE, FALSE, 0);
		}
		gtk_box_pack_start(GTK_BOX(vbox), st.row[i], FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);

	st.ok = gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
	gtk_dialog_add_button(GTK_DIALOG(dlg), lmprintf(MSG_007), GTK_RESPONSE_CANCEL);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
	if (o->flags & SELECTION_NEEDS_ALL_TO_PROCEED)
		gtk_widget_set_sensitive(st.ok, FALSE);

	// Set the default selection
	for (i = 0, m = 1; i < st.n; i++, m <<= 1)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st.choice[i]),
			(o->style == BS_AUTORADIOBUTTON && o->mask == 0 && i == 0) ||
			(o->mask != 0 && (m & o->mask)));
	for (i = 0; i < st.n; i++)
		g_signal_connect(st.choice[i], "toggled", G_CALLBACK(on_selection_toggled), &st);

	gtk_widget_show_all(dlg);
	// Hide the silent option if any of the username/regional/privacy checkboxes are unchecked
	if (o->edition_index > 0 && o->edition_index - 1 < st.n) {
		if (!selection_checked(&st, o->username_index - 1) ||
			!selection_checked(&st, o->regional_index - 1) ||
			!selection_checked(&st, o->privacy_index - 1))
			gtk_widget_hide(st.row[o->edition_index - 1]);
	}

	ctx->ret = -1;
	for (;;) {
		r = run_dialog(dlg);
		if (r != GTK_RESPONSE_OK)
			break;
		// Produce a big scary warning if the silent install option was selected
		if (o->edition_index > 0 && o->edition_index - 1 < st.n &&
			gtk_widget_get_visible(st.row[o->edition_index - 1]) && selection_checked(&st, o->edition_index - 1)) {
			selection_dialog_options_t selection = { 0 };
			selection.style = BS_AUTOCHECKBOX;
			selection.flags = SELECTION_NEEDS_ALL_TO_PROCEED | SELECTION_USE_WARNING_ICON;
			StrArrayCreate(&selection.choices, 4);
			StrArrayAdd(&selection.choices, lmprintf(MSG_372), FALSE);
			StrArrayAdd(&selection.choices, lmprintf(MSG_373), FALSE);
			StrArrayAdd(&selection.choices, lmprintf(MSG_374), FALSE);
			i = SelectionDialog(APPLICATION_NAME, lmprintf(MSG_356), &selection);
			safe_free(selection.choices.String);
			if (i != 7)
				continue;
		}
		for (mask = 0, i = 0, m = 1; i < st.n; i++, m <<= 1)
			if (selection_checked(&st, i) && gtk_widget_get_visible(st.row[i]))
				mask += m;
		if (o->username_index > 0 && st.username != NULL)
			static_strcpy(unattend_username, gtk_entry_get_text(GTK_ENTRY(st.username)));
		if (o->edition_index > 0) {
			int sel = (st.edition == NULL) ? -1 : gtk_combo_box_get_active(GTK_COMBO_BOX(st.edition));
			// Same as ComboBox_GetCurItemData() returning CB_ERR on no selection
			unattend_edition_index = (sel < 0) ? -1 : g_array_index(st.edition_data, int, sel);
		}
		ctx->ret = mask;
		break;
	}
	gtk_widget_destroy(dlg);
	if (st.edition_data != NULL)
		g_array_free(st.edition_data, TRUE);
	StrArrayDestroy(&edition_name);
	StrArrayDestroy(&edition_index);
}

/*
 * Display an item selection dialog
 */
int SelectionDialog(char* title, char* message, selection_dialog_options_t* options)
{
	selection_ctx ctx = { 0 };

	assert(options != NULL);
	if (options->style == 0)
		options->style = BS_AUTORADIOBUTTON;
	if (linux_automation_active())
		return linux_automation_selection(title, message, options);
	dialog_showing++;
	assert((options->style == BS_AUTORADIOBUTTON || options->style == BS_AUTOCHECKBOX));
	ctx.title = title;
	ctx.message = message;
	ctx.options = options;
	run_main_sync(selection_main, &ctx);
	dialog_showing--;
	return ctx.ret;
}

/* ------------------------------------------------------------------------- */
/* List dialog                                                               */
/* ------------------------------------------------------------------------- */
typedef struct {
	char* title;
	char* message;
	char** items;
	int size;
} list_ctx;

static void list_main(void* arg)
{
	list_ctx* ctx = (list_ctx*)arg;
	GtkWidget *dlg, *content, *hbox, *vbox, *icon, *label, *item;
	int i, n = ctx->size;

	// Don't overflow our max items
	if (n > MAX_LIST_ITEMS) {
		uprintf("WARNING: Too many items requested for List (%d vs %d)", n, MAX_LIST_ITEMS - 1);
		n = MAX_LIST_ITEMS - 1;
	}
	dlg = new_dialog(ctx->title);
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
	content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	icon = gtk_image_new_from_icon_name("dialog-warning", GTK_ICON_SIZE_DIALOG);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	label = new_message_label(ctx->message);
	gtk_widget_set_margin_bottom(label, 4);
	gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
	for (i = 0; i < n; i++) {
		item = gtk_label_new((ctx->items[i] == NULL) ? "" : ctx->items[i]);
		// Same as the SS_PATHELLIPSIS style
		gtk_label_set_ellipsize(GTK_LABEL(item), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars(GTK_LABEL(item), 64);
		gtk_label_set_xalign(GTK_LABEL(item), 0.0f);
		gtk_widget_set_tooltip_text(item, ctx->items[i]);
		gtk_box_pack_start(GTK_BOX(vbox), item, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);
	gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
	gtk_widget_show_all(dlg);
	run_dialog(dlg);
	gtk_widget_destroy(dlg);
}

/*
 * Display a dialog with a list of items
 */
void ListDialog(char* title, char* message, char** items, int size)
{
	list_ctx ctx = { title, message, items, size };
	dialog_showing++;
	run_main_sync(list_main, &ctx);
	dialog_showing--;
}

/* ------------------------------------------------------------------------- */
/* Hash dialog                                                               */
/* ------------------------------------------------------------------------- */
typedef struct {
	char hash[HASH_MAX][150];
	BOOL extra;
	char* title;
} hash_ctx;

static void on_hash_response(GtkDialog* dlg, gint response, gpointer data)
{
	(void)response; (void)data;
	gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void hash_main(void* arg)
{
	hash_ctx* ctx = (hash_ctx*)arg;
	static const char* hash_label[HASH_MAX] = { "MD5:", "SHA1:", "SHA256:", "SHA512:" };
	GtkWidget *dlg, *grid, *label, *field;
	PangoAttrList* attrs;
	int i;

	dlg = new_dialog((ctx->title != NULL) ? ctx->title : "Checksums");
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
	for (i = 0; i < HASH_MAX; i++) {
		label = gtk_label_new(hash_label[i]);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
		gtk_widget_set_valign(label, GTK_ALIGN_START);
		gtk_grid_attach(GTK_GRID(grid), label, 0, i, 1, 1);
		if (i == HASH_MAX - 1 && !ctx->extra) {
			field = gtk_label_new(lmprintf(MSG_311, "<Alt>-<H>"));
			gtk_label_set_xalign(GTK_LABEL(field), 0.0f);
		} else {
			/* Selectable labels, so that the values can be copied */
			field = gtk_label_new(ctx->hash[i]);
			gtk_label_set_attributes(GTK_LABEL(field), attrs);
			gtk_label_set_selectable(GTK_LABEL(field), TRUE);
			gtk_label_set_xalign(GTK_LABEL(field), 0.0f);
			gtk_label_set_line_wrap(GTK_LABEL(field), TRUE);
			gtk_label_set_line_wrap_mode(GTK_LABEL(field), PANGO_WRAP_CHAR);
			gtk_label_set_max_width_chars(GTK_LABEL(field), 64);
		}
		gtk_grid_attach(GTK_GRID(grid), field, 1, i, 1, 1);
	}
	pango_attr_list_unref(attrs);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid, TRUE, TRUE, 0);
	gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
	g_signal_connect(dlg, "response", G_CALLBACK(on_hash_response), NULL);
	gtk_widget_show_all(dlg);
	g_free(ctx->title);
	g_free(ctx);
}

/* Display the result of the hash computation (called from the hash thread) */
void ShowHashDialog(void)
{
	hash_ctx* ctx;
	const char* p;
	int i;

	/* Automated tests only need the hashes, which are in the log */
	if (linux_automation_active())
		return;
	ctx = g_new0(hash_ctx, 1);

	for (i = 0; i < HASH_MAX; i++)
		static_strcpy(ctx->hash[i], hash_str[i]);
	ctx->extra = enable_extra_hashes;
	if (image_path != NULL) {
		for (p = &image_path[strlen(image_path)]; (p > image_path) && (p[-1] != '/') && (p[-1] != '\\'); p--);
		ctx->title = g_strdup(p);
	}
	ui_run_on_main_async(hash_main, ctx);
}

/* ------------------------------------------------------------------------- */
/* Log dialog                                                                */
/* ------------------------------------------------------------------------- */
static GtkWidget *log_window = NULL, *log_view = NULL;
static GtkTextBuffer* log_buffer = NULL;
static GtkTextMark* log_end_mark = NULL;
static GMutex log_mutex;
static GString *log_pending = NULL, *log_all = NULL;
static BOOL log_flush_scheduled = FALSE, log_positioned = FALSE;

static gboolean log_flush(gpointer data)
{
	GString* pending;
	GtkTextIter end;
	(void)data;

	g_mutex_lock(&log_mutex);
	pending = log_pending;
	log_pending = g_string_new(NULL);
	log_flush_scheduled = FALSE;
	g_mutex_unlock(&log_mutex);
	if (log_buffer != NULL && pending->len > 0) {
		gtk_text_buffer_get_end_iter(log_buffer, &end);
		gtk_text_buffer_insert(log_buffer, &end, pending->str, (gint)pending->len);
		gtk_text_buffer_get_end_iter(log_buffer, &end);
		gtk_text_buffer_move_mark(log_buffer, log_end_mark, &end);
		gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_view), log_end_mark);
	}
	g_string_free(pending, TRUE);
	return G_SOURCE_REMOVE;
}

/* Log sink: may be called from any thread, at a high rate */
void linux_log_append(const char* text)
{
	const char* p;
	if (text == NULL)
		return;
	g_mutex_lock(&log_mutex);
	if (log_all == NULL) {
		log_all = g_string_new(NULL);
		log_pending = g_string_new(NULL);
	}
	for (p = text; *p != 0; p++) {
		if (*p == '\r')
			continue;
		g_string_append_c(log_all, *p);
		g_string_append_c(log_pending, *p);
	}
	if (log_all->len > MAX_LOG_SIZE)
		g_string_erase(log_all, 0, log_all->len - MAX_LOG_SIZE);
	if (!log_flush_scheduled && log_buffer != NULL) {
		log_flush_scheduled = TRUE;
		g_timeout_add(50, log_flush, NULL);
	}
	g_mutex_unlock(&log_mutex);
}

char* linux_log_get_text(void)
{
	char* r;
	g_mutex_lock(&log_mutex);
	r = strdup((log_all == NULL) ? "" : log_all->str);
	g_mutex_unlock(&log_mutex);
	return r;
}

static void log_clear(void)
{
	g_mutex_lock(&log_mutex);
	if (log_all != NULL)
		g_string_truncate(log_all, 0);
	if (log_pending != NULL)
		g_string_truncate(log_pending, 0);
	g_mutex_unlock(&log_mutex);
	if (log_buffer != NULL)
		gtk_text_buffer_set_text(log_buffer, "", 0);
}

static void log_save(void)
{
	EXT_DECL(log_ext, "rufus.log", __VA_GROUP__("*.log"), __VA_GROUP__("Rufus log"));
	char *filepath, *log_buffer_text;
	DWORD log_size;

	log_buffer_text = linux_log_get_text();
	log_size = (DWORD)strlen(log_buffer_text);
	if (log_size > 0) {
		filepath = FileDialog(TRUE, user_dir, &log_ext, NULL);
		if (filepath != NULL)
			FileIO(FILE_IO_WRITE, filepath, &log_buffer_text, &log_size);
		safe_free(filepath);
	}
	free(log_buffer_text);
}

static void on_log_response(GtkDialog* dlg, gint response, gpointer data)
{
	(void)data;
	switch (response) {
	case IDC_LOG_CLEAR:
		log_clear();
		break;
	case IDC_LOG_SAVE:
		log_save();
		break;
	default:
		gtk_widget_hide(GTK_WIDGET(dlg));
		break;
	}
}

static gboolean on_log_delete(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	(void)event; (void)data;
	gtk_widget_hide(widget);
	return TRUE;
}

static void log_create_main(void* arg)
{
	GtkWidget *sw, *clear, *save, *close;
	GtkTextIter end;
	HWND hDlg;
	(void)arg;

	if (log_window != NULL)
		return;
	log_window = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(log_window), "Log");
	gtk_window_set_icon_name(GTK_WINDOW(log_window), "rufus");
	if (ui_main_window() != NULL)
		gtk_window_set_transient_for(GTK_WINDOW(log_window), ui_main_window());
	gtk_window_set_default_size(GTK_WINDOW(log_window), 640, 560);
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
	gtk_widget_set_vexpand(sw, TRUE);
	log_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view), GTK_WRAP_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(log_view), 4);
	/* The log is always left to right, like on Windows */
	gtk_widget_set_direction(log_view, GTK_TEXT_DIR_LTR);
	gtk_container_add(GTK_CONTAINER(sw), log_view);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(log_window))), sw, TRUE, TRUE, 0);
	clear = gtk_dialog_add_button(GTK_DIALOG(log_window), "Clear", IDC_LOG_CLEAR);
	save = gtk_dialog_add_button(GTK_DIALOG(log_window), "Save", IDC_LOG_SAVE);
	close = gtk_dialog_add_button(GTK_DIALOG(log_window), "Close", GTK_RESPONSE_CLOSE);
	gtk_dialog_set_default_response(GTK_DIALOG(log_window), GTK_RESPONSE_CLOSE);
	g_signal_connect(log_window, "response", G_CALLBACK(on_log_response), NULL);
	g_signal_connect(log_window, "delete-event", G_CALLBACK(on_log_delete), NULL);

	hDlg = loc_dialog(IDD_LOG, log_window);
	loc_control(hDlg, IDC_LOG_CLEAR, CT_BUTTON, clear);
	loc_control(hDlg, IDC_LOG_SAVE, CT_BUTTON, save);
	loc_control(hDlg, IDCANCEL, CT_BUTTON, close);
	apply_localization(IDD_LOG, hDlg);
	hLogDialog = hDlg;

	/* Insert what was logged before the log window was created */
	g_mutex_lock(&log_mutex);
	if (log_all == NULL) {
		log_all = g_string_new(NULL);
		log_pending = g_string_new(NULL);
	}
	log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
	gtk_text_buffer_set_text(log_buffer, log_all->str, (gint)log_all->len);
	g_string_truncate(log_pending, 0);
	gtk_text_buffer_get_end_iter(log_buffer, &end);
	log_end_mark = gtk_text_buffer_create_mark(log_buffer, "end", &end, FALSE);
	g_mutex_unlock(&log_mutex);
	gtk_widget_show_all(gtk_dialog_get_content_area(GTK_DIALOG(log_window)));
	linux_set_log_sink(linux_log_append);
}

void linux_log_dialog_create(void)
{
	run_main_sync(log_create_main, NULL);
}

static void log_toggle_main(void* arg)
{
	GtkWindow* main_window = ui_main_window();
	gint x, y, w, h;
	(void)arg;

	if (log_window == NULL)
		log_create_main(NULL);
	if (gtk_widget_get_visible(log_window)) {
		gtk_widget_hide(log_window);
		return;
	}
	// On first display, position the log to the right of the main window
	if (!log_positioned && main_window != NULL) {
		gtk_window_get_position(main_window, &x, &y);
		gtk_window_get_size(main_window, &w, &h);
		gtk_window_move(GTK_WINDOW(log_window), x + w + 8, y);
		gtk_window_resize(GTK_WINDOW(log_window), 640, max(h, 480));
		log_positioned = TRUE;
	}
	gtk_widget_show(log_window);
	gtk_window_present(GTK_WINDOW(log_window));
	if (log_end_mark != NULL)
		gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_view), log_end_mark);
}

void linux_log_dialog_toggle(void)
{
	run_main_sync(log_toggle_main, NULL);
}

BOOL linux_log_dialog_visible(void)
{
	return (log_window != NULL) && gtk_widget_get_visible(log_window);
}

/* ------------------------------------------------------------------------- */
/* Update policy and settings dialog                                         */
/* ------------------------------------------------------------------------- */
typedef struct {
	GtkWidget* frequency;
	GArray* frequency_data;
} settings_state;

static void on_frequency_changed(GtkComboBox* combo, gpointer data)
{
	settings_state* st = (settings_state*)data;
	int sel = gtk_combo_box_get_active(combo);
	int32_t freq;
	if (sel < 0)
		return;
	freq = g_array_index(st->frequency_data, int32_t, sel);
	WriteSetting32(SETTING_UPDATE_INTERVAL, (DWORD)freq);
}

static void on_check_now(GtkButton* button, gpointer data)
{
	(void)button; (void)data;
	CheckForUpdates(TRUE);
}

static void settings_main(void* arg)
{
	GtkWidget *dlg, *content, *hbox, *vbox, *icon, *sw, *policy, *frame, *frame_label, *grid, *freq_label;
	GtkWidget *check_now, *close, *right;
	settings_state st = { 0 };
	char update_policy_text[4096], *policy_rtf;
	int32_t freq, data;
	HWND hDlg;
	(void)arg;

	dlg = new_dialog("Update policy and settings");
	content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	icon = new_app_icon(48);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

	static_sprintf(update_policy_text, update_policy, lmprintf(MSG_179|MSG_RTF),
		lmprintf(MSG_180|MSG_RTF), lmprintf(MSG_181|MSG_RTF), lmprintf(MSG_182|MSG_RTF), lmprintf(MSG_183|MSG_RTF),
		lmprintf(MSG_184|MSG_RTF), lmprintf(MSG_185|MSG_RTF), lmprintf(MSG_186|MSG_RTF));
	/* On Linux, the application itself is updated by the package manager */
	policy_rtf = g_strdup_printf("%s{\\rtf1\\ansi\\line\n\\b Linux:\\b0\\line\n"
		"Rufus is updated through your distribution's package manager. On Linux, the update check "
		"setting below controls the online refresh of the Secure Boot revocation data (DBX, SBAT) and "
		"of the Windows ISO download feature.}", update_policy_text);
	policy = new_rtf_label(policy_rtf, 60);
	g_free(policy_rtf);
	gtk_widget_set_margin_start(policy, 6);
	gtk_widget_set_margin_end(policy, 6);
	gtk_widget_set_margin_top(policy, 6);
	gtk_widget_set_margin_bottom(policy, 6);
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(sw, -1, 260);
	gtk_container_add(GTK_CONTAINER(sw), policy);
	gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

	/* Settings group */
	right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	frame = gtk_frame_new(NULL);
	frame_label = gtk_label_new("Settings");
	gtk_frame_set_label_widget(GTK_FRAME(frame), frame_label);
	grid = gtk_grid_new();
	gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	freq_label = gtk_label_new("Check for updates");
	gtk_label_set_xalign(GTK_LABEL(freq_label), 0.0f);
	gtk_grid_attach(GTK_GRID(grid), freq_label, 0, 0, 1, 1);
	st.frequency = gtk_combo_box_text_new();
	st.frequency_data = g_array_new(FALSE, TRUE, sizeof(int32_t));
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st.frequency), lmprintf(MSG_013));
	data = -1; g_array_append_val(st.frequency_data, data);
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st.frequency), lmprintf(MSG_030, lmprintf(MSG_014)));
	data = 86400; g_array_append_val(st.frequency_data, data);
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st.frequency), lmprintf(MSG_015));
	data = 604800; g_array_append_val(st.frequency_data, data);
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st.frequency), lmprintf(MSG_016));
	data = 2629800; g_array_append_val(st.frequency_data, data);
	freq = ReadSetting32(SETTING_UPDATE_INTERVAL);
	switch (freq) {
	case -1:
		gtk_combo_box_set_active(GTK_COMBO_BOX(st.frequency), 0);
		break;
	case 0:
	case 86400:
		gtk_combo_box_set_active(GTK_COMBO_BOX(st.frequency), 1);
		break;
	case 604800:
		gtk_combo_box_set_active(GTK_COMBO_BOX(st.frequency), 2);
		break;
	case 2629800:
		gtk_combo_box_set_active(GTK_COMBO_BOX(st.frequency), 3);
		break;
	default:
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st.frequency), lmprintf(MSG_017));
		g_array_append_val(st.frequency_data, freq);
		gtk_combo_box_set_active(GTK_COMBO_BOX(st.frequency), 4);
		break;
	}
	g_signal_connect(st.frequency, "changed", G_CALLBACK(on_frequency_changed), &st);
	gtk_grid_attach(GTK_GRID(grid), st.frequency, 1, 0, 1, 1);
	gtk_container_add(GTK_CONTAINER(frame), grid);
	gtk_box_pack_start(GTK_BOX(right), frame, TRUE, TRUE, 0);
	check_now = gtk_button_new_with_label("Check Now");
	gtk_widget_set_valign(check_now, GTK_ALIGN_CENTER);
	gtk_widget_set_sensitive(check_now, (freq != 0));
	g_signal_connect(check_now, "clicked", G_CALLBACK(on_check_now), NULL);
	gtk_box_pack_start(GTK_BOX(right), check_now, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), right, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);
	close = gtk_dialog_add_button(GTK_DIALOG(dlg), "Close", GTK_RESPONSE_CLOSE);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);

	hDlg = loc_dialog(IDD_UPDATE_POLICY, dlg);
	loc_control(hDlg, IDS_UPDATE_SETTINGS_GRP, CT_STATIC, frame_label);
	loc_control(hDlg, IDS_UPDATE_FREQUENCY_TXT, CT_STATIC, freq_label);
	/* No beta versions on Linux (the application is updated through apt) */
	loc_control(hDlg, IDS_INCLUDE_BETAS_TXT, CT_STATIC, NULL);
	loc_control(hDlg, IDC_CHECK_NOW, CT_BUTTON, check_now);
	loc_control(hDlg, IDCANCEL, CT_BUTTON, close);
	apply_localization(IDD_UPDATE_POLICY, hDlg);

	gtk_widget_show_all(dlg);
	run_dialog(dlg);
	loc_release(IDD_UPDATE_POLICY, hDlg);
	gtk_widget_destroy(dlg);
	g_array_free(st.frequency_data, TRUE);
}

void linux_settings_dialog(void)
{
	dialog_showing++;
	run_main_sync(settings_main, NULL);
	dialog_showing--;
}

/* The Windows dialog procedure for the update policy (referenced by net_linux.c) */
INT_PTR CALLBACK UpdateCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	(void)hDlg; (void)message; (void)wParam; (void)lParam;
	return (INT_PTR)FALSE;
}

/* ------------------------------------------------------------------------- */
/* Tooltips                                                                  */
/* ------------------------------------------------------------------------- */
static HWND tooltip_ctl[MAX_TOOLTIPS] = { 0 };
static GMutex tooltip_mutex;

BOOL CreateTooltipEx(HWND hDialog, HWND hControl, const char* message, int duration)
{
	int i, slot = -1;
	(void)hDialog; (void)duration;

	if ((hControl == NULL) || (message == NULL))
		return FALSE;
	g_mutex_lock(&tooltip_mutex);
	for (i = 0; i < MAX_TOOLTIPS; i++) {
		if (tooltip_ctl[i] == hControl) {
			slot = i;
			break;
		}
		if (tooltip_ctl[i] == NULL && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		g_mutex_unlock(&tooltip_mutex);
		uprintf("Maximum number of tooltips reached (%d)\n", MAX_TOOLTIPS);
		return FALSE;
	}
	tooltip_ctl[slot] = hControl;
	g_mutex_unlock(&tooltip_mutex);
	ctl_set_tooltip(hControl, message);
	return TRUE;
}

/* GTK hides tooltips on its own when the pointer moves */
void PopTooltip(HWND hControl)
{
	(void)hControl;
}

void DestroyTooltip(HWND hControl)
{
	int i;
	if (hControl == NULL)
		return;
	g_mutex_lock(&tooltip_mutex);
	for (i = 0; i < MAX_TOOLTIPS; i++)
		if (tooltip_ctl[i] == hControl)
			tooltip_ctl[i] = NULL;
	g_mutex_unlock(&tooltip_mutex);
	ctl_set_tooltip(hControl, NULL);
}

void DestroyAllTooltips(void)
{
	HWND list[MAX_TOOLTIPS];
	int i, n = 0;
	g_mutex_lock(&tooltip_mutex);
	for (i = 0; i < MAX_TOOLTIPS; i++) {
		if (tooltip_ctl[i] != NULL)
			list[n++] = tooltip_ctl[i];
		tooltip_ctl[i] = NULL;
	}
	g_mutex_unlock(&tooltip_mutex);
	for (i = 0; i < n; i++)
		ctl_set_tooltip(list[i], NULL);
}

/* ------------------------------------------------------------------------- */
/* Window helpers                                                            */
/* ------------------------------------------------------------------------- */
BOOL IsShown(HWND hDlg)
{
	GtkWidget* w = ctl_widget(hDlg);
	return (w != NULL) && gtk_widget_get_visible(w);
}

SIZE GetTextSize(HWND hCtrl, char* txt)
{
	SIZE sz = { 0, 0 };
	char buf[1024];
	GtkWidget* w = ctl_widget(hCtrl);
	PangoLayout* layout;
	int width, height;

	if (txt == NULL) {
		buf[0] = 0;
		GetWindowTextU(hCtrl, buf, sizeof(buf));
		txt = buf;
	}
	if (w != NULL && ui_is_main_thread()) {
		layout = gtk_widget_create_pango_layout(w, txt);
		pango_layout_get_pixel_size(layout, &width, &height);
		g_object_unref(layout);
		sz.cx = width;
		sz.cy = height;
	} else {
		sz.cx = (LONG)(7 * g_utf8_strlen(txt, -1));
		sz.cy = 16;
	}
	return sz;
}

LONG GetEntryWidth(HWND hDropDown, const char* entry)
{
	return GetTextSize(hDropDown, (char*)entry).cx;
}

void CenterDialog(HWND hDlg, HWND hParent)
{
	GtkWidget* w = ctl_widget(hDlg);
	(void)hParent;
	if (w != NULL && GTK_IS_WINDOW(w) && ui_is_main_thread())
		gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER_ON_PARENT);
}

/* GTK containers size and position controls automatically */
void ResizeMoveCtrl(HWND hDlg, HWND hCtrl, int dx, int dy, int dw, int dh, float scale)
{
	(void)hDlg; (void)hCtrl; (void)dx; (void)dy; (void)dw; (void)dh; (void)scale;
}

void ResizeButtonHeight(HWND hDlg, int id)
{
	(void)hDlg; (void)id;
}

void CreateStatusBar(HFONT* hFont)
{
	(void)hFont;
}

void CreateStaticFont(HDC hDC, HFONT* hFont, BOOL underlined)
{
	(void)hDC; (void)hFont; (void)underlined;
}

void SetHyperLinkFont(HWND hWnd, HDC hDC, HFONT* hFont, BOOL underlined)
{
	(void)hWnd; (void)hDC; (void)hFont; (void)underlined;
}

void SetTitleBarIcon(HWND hDlg)
{
	GtkWidget* w = ctl_widget(hDlg);
	if (w != NULL && GTK_IS_WINDOW(w) && ui_is_main_thread())
		gtk_window_set_icon_name(GTK_WINDOW(w), "rufus");
}

/* The Windows taskbar progress has no portable equivalent: progress is shown in the window */
BOOL CreateTaskbarList(void)
{
	return TRUE;
}

BOOL SetTaskbarProgressState(TASKBAR_PROGRESS_FLAGS tbpFlags)
{
	(void)tbpFlags;
	return TRUE;
}

BOOL SetTaskbarProgressValue(ULONGLONG ullCompleted, ULONGLONG ullTotal)
{
	(void)ullCompleted; (void)ullTotal;
	return TRUE;
}

static gboolean on_main_focus_in(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	(void)event; (void)data;
	gtk_window_set_urgency_hint(GTK_WINDOW(widget), FALSE);
	return FALSE;
}

static void flash_main(void* arg)
{
	static BOOL connected = FALSE;
	GtkWindow* w = ui_main_window();
	(void)arg;
	if (w == NULL || gtk_window_is_active(w))
		return;
	if (!connected) {
		g_signal_connect(w, "focus-in-event", G_CALLBACK(on_main_focus_in), NULL);
		connected = TRUE;
	}
	gtk_window_set_urgency_hint(w, TRUE);
}

/* Draw the user's attention to the application when an operation completes */
void FlashTaskbar(HANDLE handle)
{
	(void)handle;
	ui_run_on_main_async(flash_main, NULL);
}

HICON CreateMirroredIcon(HICON hiconOrg)
{
	return hiconOrg;
}

/* The alert prompt hook replaces the texts of a Windows system dialog: not applicable */
void SetAlertPromptMessages(void)
{
}

BOOL SetAlertPromptHook(void)
{
	return TRUE;
}

void ClrAlertPromptHook(void)
{
}

/* Dialogs are created natively on Linux, not from Windows dialog templates */
HWND MyCreateDialog(HINSTANCE hInstance, int Dialog_ID, HWND hWndParent, DLGPROC lpDialogFunc)
{
	(void)hInstance; (void)hWndParent; (void)lpDialogFunc;
	uprintf("Error: Windows dialog template %d is not available on Linux", Dialog_ID);
	return NULL;
}

INT_PTR MyDialogBox(HINSTANCE hInstance, int Dialog_ID, HWND hWndParent, DLGPROC lpDialogFunc)
{
	(void)hInstance; (void)hWndParent; (void)lpDialogFunc;
	uprintf("Error: Windows dialog template %d is not available on Linux", Dialog_ID);
	return -1;
}
