/*
 * Rufus for Linux: control model (see winctl.h)
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
#include <windows.h>
#include <msapi_utf8_linux.h>
#include <pthread.h>

#include "rufus.h"
#include "winctl.h"

#define CTL_MAGIC 0x4C544352	/* 'RCTL' */

typedef struct _rufus_ctl {
	uint32_t magic;
	int id;
	ctl_type type;
	struct _rufus_ctl* parent;
	GtkWidget* widget;
	GtkWidget* buddy;
	DLGPROC proc;
	GPtrArray* children;
	/* Model */
	char* text;
	BOOL enabled;
	BOOL visible;
	int checked;
	int cur_sel;
	GPtrArray* items;
	GArray* item_data;
	int pos, range_min, range_max, state;
	BOOL marquee;
	char* status_text[2];
	char* tooltip;
	int max_length;
	/* Synchronization with the widget */
	BOOL text_dirty, items_dirty, sel_dirty, check_dirty, state_dirty, pos_dirty, tooltip_dirty;
	BOOL sync_pending;
	int updating;
	guint marquee_source;
	GtkWidget* status_labels[2];
} rufus_ctl;

static GRecMutex model_lock;
static GThread* main_thread = NULL;
static GtkWindow* main_window = NULL;

#define LOCK()   g_rec_mutex_lock(&model_lock)
#define UNLOCK() g_rec_mutex_unlock(&model_lock)

static rufus_ctl* to_ctl(HWND h)
{
	rufus_ctl* c = (rufus_ctl*)h;
	return (c != NULL && c != INVALID_HANDLE_VALUE && c->magic == CTL_MAGIC) ? c : NULL;
}

/* ------------------------------------------------------------------------- */
/* Threading                                                                 */
/* ------------------------------------------------------------------------- */
void ui_init_threading(void)
{
	main_thread = g_thread_self();
	g_rec_mutex_init(&model_lock);
}

BOOL ui_is_main_thread(void)
{
	return (main_thread == NULL) || (g_thread_self() == main_thread);
}

typedef struct {
	void (*fn)(void* arg);
	void* arg;
	GMutex mutex;
	GCond cond;
	BOOL done;
} sync_call;

static gboolean sync_call_cb(gpointer data)
{
	sync_call* sc = (sync_call*)data;
	sc->fn(sc->arg);
	g_mutex_lock(&sc->mutex);
	sc->done = TRUE;
	g_cond_signal(&sc->cond);
	g_mutex_unlock(&sc->mutex);
	return G_SOURCE_REMOVE;
}

void ui_run_on_main_sync(void (*fn)(void* arg), void* arg)
{
	sync_call sc;
	if (ui_is_main_thread()) {
		fn(arg);
		return;
	}
	sc.fn = fn;
	sc.arg = arg;
	sc.done = FALSE;
	g_mutex_init(&sc.mutex);
	g_cond_init(&sc.cond);
	g_main_context_invoke(NULL, sync_call_cb, &sc);
	g_mutex_lock(&sc.mutex);
	while (!sc.done)
		g_cond_wait(&sc.cond, &sc.mutex);
	g_mutex_unlock(&sc.mutex);
	g_mutex_clear(&sc.mutex);
	g_cond_clear(&sc.cond);
}

typedef struct {
	void (*fn)(void* arg);
	void* arg;
} async_call;

static gboolean async_call_cb(gpointer data)
{
	async_call* ac = (async_call*)data;
	ac->fn(ac->arg);
	g_free(ac);
	return G_SOURCE_REMOVE;
}

void ui_run_on_main_async(void (*fn)(void* arg), void* arg)
{
	async_call* ac = g_new0(async_call, 1);
	ac->fn = fn;
	ac->arg = arg;
	g_idle_add_full(G_PRIORITY_DEFAULT, async_call_cb, ac, NULL);
}

GtkWindow* ui_main_window(void)
{
	return main_window;
}

void ui_set_main_window(GtkWindow* window)
{
	main_window = window;
}

/* ------------------------------------------------------------------------- */
/* Widget synchronization (main thread only)                                 */
/* ------------------------------------------------------------------------- */
static gboolean marquee_pulse(gpointer data)
{
	rufus_ctl* c = (rufus_ctl*)data;
	if (c->widget != NULL)
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(c->widget));
	return G_SOURCE_CONTINUE;
}

static void set_style_class(GtkWidget* w, const char* name, BOOL set)
{
	GtkStyleContext* sc = gtk_widget_get_style_context(w);
	if (set)
		gtk_style_context_add_class(sc, name);
	else
		gtk_style_context_remove_class(sc, name);
}

static void sync_widget(rufus_ctl* c)
{
	GtkWidget* w;
	guint i;

	LOCK();
	c->sync_pending = FALSE;
	w = c->widget;
	if (w == NULL) {
		UNLOCK();
		return;
	}
	c->updating++;
	if (gtk_widget_get_sensitive(w) != c->enabled)
		gtk_widget_set_sensitive(w, c->enabled);
	if (c->type != CT_DIALOG && gtk_widget_get_visible(w) != c->visible) {
		gtk_widget_set_visible(w, c->visible);
		if (c->buddy != NULL)
			gtk_widget_set_visible(c->buddy, c->visible);
	}
	if (c->tooltip_dirty) {
		gtk_widget_set_tooltip_text(w, c->tooltip);
		c->tooltip_dirty = FALSE;
	}
	switch (c->type) {
	case CT_DIALOG:
		if (c->text_dirty && GTK_IS_WINDOW(w))
			gtk_window_set_title(GTK_WINDOW(w), (c->text == NULL) ? "" : c->text);
		break;
	case CT_STATIC:
		if (c->text_dirty && GTK_IS_LABEL(w))
			gtk_label_set_text(GTK_LABEL(w), (c->text == NULL) ? "" : c->text);
		break;
	case CT_BUTTON:
		if (c->text_dirty && GTK_IS_BUTTON(w) && c->text != NULL && gtk_button_get_label(GTK_BUTTON(w)) != NULL)
			gtk_button_set_label(GTK_BUTTON(w), c->text);
		break;
	case CT_CHECK:
		if (c->text_dirty && GTK_IS_BUTTON(w))
			gtk_button_set_label(GTK_BUTTON(w), (c->text == NULL) ? "" : c->text);
		if (c->check_dirty && GTK_IS_TOGGLE_BUTTON(w))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), c->checked == BST_CHECKED);
		c->check_dirty = FALSE;
		break;
	case CT_COMBO:
		if (c->items_dirty) {
			gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(w));
			for (i = 0; i < c->items->len; i++)
				gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), (const char*)g_ptr_array_index(c->items, i));
			c->items_dirty = FALSE;
			c->sel_dirty = TRUE;
		}
		if (c->sel_dirty) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(w), c->cur_sel);
			c->sel_dirty = FALSE;
		}
		break;
	case CT_EDIT:
		if (c->max_length > 0)
			gtk_entry_set_max_length(GTK_ENTRY(w), c->max_length);
		if (c->text_dirty && strcmp(gtk_entry_get_text(GTK_ENTRY(w)), (c->text == NULL) ? "" : c->text) != 0)
			gtk_entry_set_text(GTK_ENTRY(w), (c->text == NULL) ? "" : c->text);
		break;
	case CT_PROGRESS:
		if (c->marquee && c->marquee_source == 0) {
			c->marquee_source = g_timeout_add(100, marquee_pulse, c);
		} else if (!c->marquee && c->marquee_source != 0) {
			g_source_remove(c->marquee_source);
			c->marquee_source = 0;
		}
		if (!c->marquee && c->range_max > c->range_min)
			gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w),
				CLAMP((double)(c->pos - c->range_min) / (double)(c->range_max - c->range_min), 0.0, 1.0));
		if (c->text_dirty) {
			/* The text is displayed over the bar when an overlay label is provided */
			GtkWidget* label = (GtkWidget*)g_object_get_data(G_OBJECT(w), "rufus-label");
			if (label != NULL) {
				gtk_label_set_text(GTK_LABEL(label), (c->text == NULL) ? "" : c->text);
			} else {
				gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(w), TRUE);
				gtk_progress_bar_set_text(GTK_PROGRESS_BAR(w), (c->text == NULL) ? "" : c->text);
			}
		}
		if (c->state_dirty) {
			set_style_class(w, "rufus-error", c->state == PBST_ERROR);
			set_style_class(w, "rufus-paused", c->state == PBST_PAUSED);
			c->state_dirty = FALSE;
		}
		break;
	case CT_SLIDER:
		if (c->pos_dirty) {
			gtk_range_set_range(GTK_RANGE(w), c->range_min, max(c->range_max, c->range_min + 1));
			gtk_range_set_value(GTK_RANGE(w), c->pos);
			c->pos_dirty = FALSE;
		}
		break;
	case CT_STATUS:
		for (i = 0; i < 2; i++)
			if (c->status_labels[i] != NULL)
				gtk_label_set_text(GTK_LABEL(c->status_labels[i]), (c->status_text[i] == NULL) ? "" : c->status_text[i]);
		break;
	case CT_TOOLBAR:
	default:
		break;
	}
	c->text_dirty = FALSE;
	c->updating--;
	UNLOCK();
}

static gboolean sync_idle(gpointer data)
{
	sync_widget((rufus_ctl*)data);
	return G_SOURCE_REMOVE;
}

/* Called with the lock held */
static void schedule_sync(rufus_ctl* c)
{
	if (c->widget == NULL)
		return;
	if (ui_is_main_thread()) {
		sync_widget(c);
	} else if (!c->sync_pending) {
		c->sync_pending = TRUE;
		g_idle_add(sync_idle, c);
	}
}

/* ------------------------------------------------------------------------- */
/* Message dispatching                                                       */
/* ------------------------------------------------------------------------- */
static rufus_ctl* get_dialog(rufus_ctl* c)
{
	while (c != NULL && c->type != CT_DIALOG)
		c = c->parent;
	return c;
}

static LRESULT dispatch(rufus_ctl* dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (dlg == NULL || dlg->proc == NULL)
		return 0;
	return (LRESULT)dlg->proc((HWND)dlg, msg, wParam, lParam);
}

static void notify(rufus_ctl* c, WORD code)
{
	dispatch(get_dialog(c->parent), WM_COMMAND, MAKEWPARAM(c->id, code), (LPARAM)c);
}

typedef struct {
	rufus_ctl* dlg;
	UINT msg;
	WPARAM wParam;
	LPARAM lParam;
	LRESULT result;
} message_call;

static void send_message_cb(void* arg)
{
	message_call* mc = (message_call*)arg;
	mc->result = dispatch(mc->dlg, mc->msg, mc->wParam, mc->lParam);
}

static gboolean post_message_cb(gpointer data)
{
	message_call* mc = (message_call*)data;
	dispatch(mc->dlg, mc->msg, mc->wParam, mc->lParam);
	g_free(mc);
	return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* Signal handlers                                                           */
/* ------------------------------------------------------------------------- */
static void on_combo_changed(GtkComboBox* combo, gpointer data)
{
	rufus_ctl* c = (rufus_ctl*)data;
	(void)combo;
	if (c->updating)
		return;
	LOCK();
	c->cur_sel = gtk_combo_box_get_active(GTK_COMBO_BOX(c->widget));
	UNLOCK();
	notify(c, CBN_SELCHANGE);
}

static void on_button_clicked(GtkButton* button, gpointer data)
{
	rufus_ctl* c = (rufus_ctl*)data;
	(void)button;
	notify(c, BN_CLICKED);
}

static void on_check_toggled(GtkToggleButton* button, gpointer data)
{
	rufus_ctl* c = (rufus_ctl*)data;
	if (c->updating)
		return;
	LOCK();
	c->checked = gtk_toggle_button_get_active(button) ? BST_CHECKED : BST_UNCHECKED;
	UNLOCK();
	notify(c, BN_CLICKED);
}

static void on_entry_changed(GtkEditable* editable, gpointer data)
{
	rufus_ctl* c = (rufus_ctl*)data;
	(void)editable;
	if (c->updating)
		return;
	LOCK();
	g_free(c->text);
	c->text = g_strdup(gtk_entry_get_text(GTK_ENTRY(c->widget)));
	UNLOCK();
	notify(c, EN_CHANGE);
}

static gboolean on_entry_focus_out(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	rufus_ctl* c = (rufus_ctl*)data;
	(void)widget; (void)event;
	notify(c, EN_KILLFOCUS);
	return FALSE;
}

static void on_scale_changed(GtkRange* range, gpointer data)
{
	rufus_ctl* c = (rufus_ctl*)data;
	if (c->updating)
		return;
	LOCK();
	c->pos = (int)gtk_range_get_value(range);
	UNLOCK();
	dispatch(get_dialog(c->parent), WM_HSCROLL, 0, (LPARAM)c);
}

/* ------------------------------------------------------------------------- */
/* Control creation                                                          */
/* ------------------------------------------------------------------------- */
HWND ctl_create(HWND parent, int id, ctl_type type, GtkWidget* widget)
{
	rufus_ctl* c = g_new0(rufus_ctl, 1);
	rufus_ctl* p = to_ctl(parent);

	c->magic = CTL_MAGIC;
	c->id = id;
	c->type = type;
	c->parent = p;
	c->widget = widget;
	c->enabled = (widget == NULL) ? TRUE : gtk_widget_get_sensitive(widget);
	/* Controls are visible, unless created hidden ('no-show-all', i.e. NOT WS_VISIBLE on Windows) */
	c->visible = (widget == NULL) ? TRUE : !gtk_widget_get_no_show_all(widget);
	c->cur_sel = -1;
	c->range_max = 100;
	c->state = PBST_NORMAL;
	if (type == CT_DIALOG)
		c->children = g_ptr_array_new();
	if (type == CT_COMBO) {
		c->items = g_ptr_array_new_with_free_func(g_free);
		c->item_data = g_array_new(FALSE, TRUE, sizeof(LRESULT));
	}
	if (p != NULL && p->children != NULL) {
		LOCK();
		g_ptr_array_add(p->children, c);
		UNLOCK();
	}
	if (widget != NULL) {
		g_object_set_data(G_OBJECT(widget), "rufus-ctl", c);
		switch (type) {
		case CT_COMBO:
			g_signal_connect(widget, "changed", G_CALLBACK(on_combo_changed), c);
			break;
		case CT_BUTTON:
			g_signal_connect(widget, "clicked", G_CALLBACK(on_button_clicked), c);
			if (GTK_IS_BUTTON(widget) && gtk_button_get_label(GTK_BUTTON(widget)) != NULL)
				c->text = g_strdup(gtk_button_get_label(GTK_BUTTON(widget)));
			break;
		case CT_CHECK:
			g_signal_connect(widget, "toggled", G_CALLBACK(on_check_toggled), c);
			c->checked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) ? BST_CHECKED : BST_UNCHECKED;
			if (gtk_button_get_label(GTK_BUTTON(widget)) != NULL)
				c->text = g_strdup(gtk_button_get_label(GTK_BUTTON(widget)));
			break;
		case CT_EDIT:
			g_signal_connect(widget, "changed", G_CALLBACK(on_entry_changed), c);
			g_signal_connect(widget, "focus-out-event", G_CALLBACK(on_entry_focus_out), c);
			c->text = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
			break;
		case CT_SLIDER:
			g_signal_connect(widget, "value-changed", G_CALLBACK(on_scale_changed), c);
			break;
		case CT_STATIC:
			if (GTK_IS_LABEL(widget))
				c->text = g_strdup(gtk_label_get_text(GTK_LABEL(widget)));
			break;
		case CT_STATUS:
			if (GTK_IS_CONTAINER(widget)) {
				GList* l = gtk_container_get_children(GTK_CONTAINER(widget));
				int i = 0;
				for (GList* it = l; it != NULL && i < 2; it = it->next)
					if (GTK_IS_LABEL(it->data))
						c->status_labels[i++] = GTK_WIDGET(it->data);
				g_list_free(l);
			}
			break;
		case CT_DIALOG:
			if (GTK_IS_WINDOW(widget))
				c->text = g_strdup(gtk_window_get_title(GTK_WINDOW(widget)));
			break;
		default:
			break;
		}
	}
	return (HWND)c;
}

void ctl_toolbar_add(HWND toolbar, int cmd_id, GtkWidget* button)
{
	rufus_ctl* c = to_ctl(toolbar);
	if (c == NULL || button == NULL)
		return;
	/* Toolbar buttons are addressed (and report WM_COMMAND) through their command ID */
	ctl_create((HWND)get_dialog(c), cmd_id, CT_BUTTON, button);
}

void ctl_set_dialog_proc(HWND dialog, DLGPROC proc)
{
	rufus_ctl* c = to_ctl(dialog);
	if (c != NULL)
		c->proc = proc;
}

GtkWidget* ctl_widget(HWND hCtrl)
{
	rufus_ctl* c = to_ctl(hCtrl);
	return (c == NULL) ? NULL : c->widget;
}

int ctl_id(HWND hCtrl)
{
	rufus_ctl* c = to_ctl(hCtrl);
	return (c == NULL) ? 0 : c->id;
}

HWND ctl_parent(HWND hCtrl)
{
	rufus_ctl* c = to_ctl(hCtrl);
	return (c == NULL) ? NULL : (HWND)c->parent;
}

void ctl_set_buddy(HWND hCtrl, GtkWidget* buddy)
{
	rufus_ctl* c = to_ctl(hCtrl);
	if (c != NULL)
		c->buddy = buddy;
}

void ctl_destroy_dialog(HWND dialog)
{
	rufus_ctl* d = to_ctl(dialog);
	guint i;
	if (d == NULL || d->type != CT_DIALOG)
		return;
	LOCK();
	for (i = 0; i < d->children->len; i++) {
		rufus_ctl* c = (rufus_ctl*)g_ptr_array_index(d->children, i);
		c->magic = 0;
		c->widget = NULL;
		/* NB: control objects are leaked on purpose, as other threads may still hold their HWND */
	}
	g_ptr_array_set_size(d->children, 0);
	d->magic = 0;
	d->widget = NULL;
	UNLOCK();
}

void ctl_set_tooltip(HWND hCtrl, const char* text)
{
	rufus_ctl* c = to_ctl(hCtrl);
	if (c == NULL)
		return;
	LOCK();
	g_free(c->tooltip);
	c->tooltip = g_strdup(text);
	c->tooltip_dirty = TRUE;
	schedule_sync(c);
	UNLOCK();
}

void ctl_status_set_text(HWND hStatus, int section, const char* text)
{
	rufus_ctl* c = to_ctl(hStatus);
	if (c == NULL || section < 0 || section > 1)
		return;
	LOCK();
	g_free(c->status_text[section]);
	c->status_text[section] = g_strdup(text);
	schedule_sync(c);
	UNLOCK();
}

/* ------------------------------------------------------------------------- */
/* Win32 API                                                                 */
/* ------------------------------------------------------------------------- */
HWND GetDlgItem(HWND hDlg, int nIDDlgItem)
{
	rufus_ctl* d = to_ctl(hDlg);
	HWND r = NULL;
	guint i;
	if (d == NULL || d->children == NULL)
		return NULL;
	LOCK();
	for (i = 0; i < d->children->len; i++) {
		rufus_ctl* c = (rufus_ctl*)g_ptr_array_index(d->children, i);
		if (c->id == nIDDlgItem) {
			r = (HWND)c;
			break;
		}
	}
	UNLOCK();
	return r;
}

BOOL IsWindow(HWND hWnd)
{
	return (to_ctl(hWnd) != NULL);
}

BOOL EnableWindow(HWND hWnd, BOOL bEnable)
{
	rufus_ctl* c = to_ctl(hWnd);
	BOOL was_disabled;
	if (c == NULL)
		return FALSE;
	LOCK();
	was_disabled = !c->enabled;
	c->enabled = (bEnable != FALSE);
	schedule_sync(c);
	UNLOCK();
	return was_disabled;
}

BOOL IsWindowEnabled(HWND hWnd)
{
	rufus_ctl* c = to_ctl(hWnd);
	return (c != NULL) && c->enabled;
}

BOOL ShowWindow(HWND hWnd, int nCmdShow)
{
	rufus_ctl* c = to_ctl(hWnd);
	BOOL was_visible;
	if (c == NULL)
		return FALSE;
	LOCK();
	was_visible = c->visible;
	c->visible = (nCmdShow != SW_HIDE);
	if (c->type == CT_DIALOG && c->widget != NULL) {
		/* Top level windows are shown/hidden directly */
		UNLOCK();
		if (ui_is_main_thread())
			gtk_widget_set_visible(c->widget, nCmdShow != SW_HIDE);
		return was_visible;
	}
	schedule_sync(c);
	UNLOCK();
	return was_visible;
}

BOOL IsWindowVisible(HWND hWnd)
{
	rufus_ctl* c = to_ctl(hWnd);
	return (c != NULL) && c->visible;
}

BOOL SetWindowTextA(HWND hWnd, LPCSTR lpString)
{
	rufus_ctl* c = to_ctl(hWnd);
	if (c == NULL)
		return FALSE;
	LOCK();
	if (c->type == CT_STATUS) {
		g_free(c->status_text[0]);
		c->status_text[0] = g_strdup(lpString);
	} else {
		g_free(c->text);
		c->text = g_strdup((lpString == NULL) ? "" : lpString);
		c->text_dirty = TRUE;
	}
	schedule_sync(c);
	UNLOCK();
	return TRUE;
}

BOOL SetDlgItemTextU(HWND hDlg, int nIDDlgItem, const char* lpString)
{
	return SetWindowTextA(GetDlgItem(hDlg, nIDDlgItem), lpString);
}

static const char* current_text(rufus_ctl* c)
{
	if (c->type == CT_COMBO)
		return (c->cur_sel >= 0 && (guint)c->cur_sel < c->items->len) ? (const char*)g_ptr_array_index(c->items, c->cur_sel) : "";
	return (c->text == NULL) ? "" : c->text;
}

int GetWindowTextA(HWND hWnd, LPSTR lpString, int nMaxCount)
{
	rufus_ctl* c = to_ctl(hWnd);
	int len;
	if (lpString == NULL || nMaxCount <= 0)
		return 0;
	lpString[0] = 0;
	if (c == NULL)
		return 0;
	LOCK();
	len = snprintf(lpString, nMaxCount, "%s", current_text(c));
	UNLOCK();
	return min(len, nMaxCount - 1);
}

int GetWindowTextLengthA(HWND hWnd)
{
	rufus_ctl* c = to_ctl(hWnd);
	int len;
	if (c == NULL)
		return 0;
	LOCK();
	len = (int)strlen(current_text(c));
	UNLOCK();
	return len;
}

int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount)
{
	char buf[1024];
	if (lpString == NULL || nMaxCount <= 0)
		return 0;
	GetWindowTextA(hWnd, buf, sizeof(buf));
	if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, lpString, nMaxCount) == 0)
		lpString[0] = 0;
	return (int)wcslen(lpString);
}

UINT IsDlgButtonChecked(HWND hDlg, int nIDButton)
{
	return (UINT)Button_GetCheck(GetDlgItem(hDlg, nIDButton));
}

BOOL CheckDlgButton(HWND hDlg, int nIDButton, UINT uCheck)
{
	HWND h = GetDlgItem(hDlg, nIDButton);
	if (h == NULL)
		return FALSE;
	Button_SetCheck(h, (int)uCheck);
	return TRUE;
}

int Button_GetCheck(HWND hCtrl)
{
	rufus_ctl* c = to_ctl(hCtrl);
	return (c == NULL) ? BST_UNCHECKED : c->checked;
}

void Button_SetCheck(HWND hCtrl, int check)
{
	rufus_ctl* c = to_ctl(hCtrl);
	if (c == NULL)
		return;
	LOCK();
	c->checked = check;
	c->check_dirty = TRUE;
	schedule_sync(c);
	UNLOCK();
}

/* Combo boxes */
int ComboBox_GetCurSel(HWND hCtrl)
{
	rufus_ctl* c = to_ctl(hCtrl);
	int r;
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	r = (c->cur_sel >= 0 && (guint)c->cur_sel < c->items->len) ? c->cur_sel : CB_ERR;
	UNLOCK();
	return r;
}

int ComboBox_SetCurSel(HWND hCtrl, int index)
{
	rufus_ctl* c = to_ctl(hCtrl);
	int r;
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	if (index < 0 || (guint)index >= c->items->len) {
		/* Like Windows, an invalid index clears the selection */
		c->cur_sel = -1;
		r = CB_ERR;
	} else {
		c->cur_sel = index;
		r = index;
	}
	c->sel_dirty = TRUE;
	schedule_sync(c);
	UNLOCK();
	return r;
}

LRESULT ComboBox_GetItemData(HWND hCtrl, int index)
{
	rufus_ctl* c = to_ctl(hCtrl);
	LRESULT r = CB_ERR;
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	if (index >= 0 && (guint)index < c->item_data->len)
		r = g_array_index(c->item_data, LRESULT, index);
	UNLOCK();
	return r;
}

int ComboBox_SetItemData(HWND hCtrl, int index, LRESULT data)
{
	rufus_ctl* c = to_ctl(hCtrl);
	int r = CB_ERR;
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	if (index >= 0 && (guint)index < c->item_data->len) {
		g_array_index(c->item_data, LRESULT, index) = data;
		r = 0;
	}
	UNLOCK();
	return r;
}

int ComboBox_InsertStringU(HWND hCtrl, int index, const char* str)
{
	rufus_ctl* c = to_ctl(hCtrl);
	LRESULT zero = 0;
	int r;
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	if (index < 0 || (guint)index > c->items->len)
		index = (int)c->items->len;
	g_ptr_array_insert(c->items, index, g_strdup((str == NULL) ? "" : str));
	g_array_insert_val(c->item_data, index, zero);
	if (c->cur_sel >= index)
		c->cur_sel++;
	c->items_dirty = TRUE;
	r = index;
	schedule_sync(c);
	UNLOCK();
	return r;
}

int ComboBox_AddString(HWND hCtrl, const char* str)
{
	return ComboBox_InsertStringU(hCtrl, -1, str);
}

int ComboBox_DeleteString(HWND hCtrl, int index)
{
	rufus_ctl* c = to_ctl(hCtrl);
	int r = CB_ERR;
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	if (index >= 0 && (guint)index < c->items->len) {
		g_ptr_array_remove_index(c->items, index);
		g_array_remove_index(c->item_data, index);
		if (c->cur_sel == index)
			c->cur_sel = -1;
		else if (c->cur_sel > index)
			c->cur_sel--;
		c->items_dirty = TRUE;
		r = (int)c->items->len;
		schedule_sync(c);
	}
	UNLOCK();
	return r;
}

int ComboBox_ResetContent(HWND hCtrl)
{
	rufus_ctl* c = to_ctl(hCtrl);
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	g_ptr_array_set_size(c->items, 0);
	g_array_set_size(c->item_data, 0);
	c->cur_sel = -1;
	c->items_dirty = TRUE;
	schedule_sync(c);
	UNLOCK();
	return 0;
}

int ComboBox_GetCount(HWND hCtrl)
{
	rufus_ctl* c = to_ctl(hCtrl);
	int r;
	if (c == NULL || c->type != CT_COMBO)
		return CB_ERR;
	LOCK();
	r = (int)c->items->len;
	UNLOCK();
	return r;
}

int ComboBox_GetLBTextU(HWND hCtrl, int index, char* buf, size_t buf_size)
{
	rufus_ctl* c = to_ctl(hCtrl);
	int r = CB_ERR;
	if (c == NULL || c->type != CT_COMBO || buf == NULL || buf_size == 0)
		return CB_ERR;
	LOCK();
	if (index >= 0 && (guint)index < c->items->len)
		r = snprintf(buf, buf_size, "%s", (const char*)g_ptr_array_index(c->items, index));
	UNLOCK();
	return r;
}

int ComboBox_GetTextU(HWND hCtrl, char* str, int max_str)
{
	return GetWindowTextA(hCtrl, str, max_str);
}

int ComboBox_FindStringExact(HWND hCtrl, int start, const char* str)
{
	rufus_ctl* c = to_ctl(hCtrl);
	int r = CB_ERR;
	guint i;
	if (c == NULL || c->type != CT_COMBO || str == NULL)
		return CB_ERR;
	LOCK();
	for (i = (start < 0) ? 0 : (guint)start + 1; i < c->items->len; i++) {
		if (strcasecmp((const char*)g_ptr_array_index(c->items, i), str) == 0) {
			r = (int)i;
			break;
		}
	}
	UNLOCK();
	return r;
}

/* Messages */
LRESULT SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	rufus_ctl* c = to_ctl(hWnd);
	LRESULT r = 0;

	if (c == NULL)
		return 0;
	if (c->type == CT_DIALOG) {
		message_call mc = { c, Msg, wParam, lParam, 0 };
		if (Msg == WM_NEXTDLGCTL) {
			/* Focus a control */
			rufus_ctl* f = (lParam != 0) ? to_ctl((HWND)wParam) : NULL;
			if (f != NULL && f->widget != NULL && ui_is_main_thread())
				gtk_widget_grab_focus(f->widget);
			return 0;
		}
		ui_run_on_main_sync(send_message_cb, &mc);
		return mc.result;
	}

	/* Generic messages */
	if (Msg == WM_SETTEXT)
		return SetWindowTextA(hWnd, (LPCSTR)lParam);
	if (Msg == WM_SETFONT)
		return 0;

	/* Like on Windows, the meaning of a message depends on the control's class */
	LOCK();
	switch (c->type) {
	case CT_BUTTON:
	case CT_CHECK:
		if (Msg == BM_SETCHECK) {
			c->checked = (int)wParam;
			c->check_dirty = TRUE;
			schedule_sync(c);
		} else if (Msg == BM_GETCHECK) {
			r = c->checked;
		}
		break;
	case CT_PROGRESS:
		switch (Msg) {
		case PBM_SETRANGE:
			c->range_min = LOWORD(lParam);
			c->range_max = HIWORD(lParam);
			schedule_sync(c);
			break;
		case PBM_SETPOS:
			r = c->pos;
			c->pos = (int)wParam;
			schedule_sync(c);
			break;
		case PBM_SETSTATE:
			r = c->state;
			c->state = (int)wParam;
			c->state_dirty = TRUE;
			schedule_sync(c);
			break;
		case PBM_SETMARQUEE:
			c->marquee = (wParam != 0);
			schedule_sync(c);
			break;
		default:
			break;
		}
		break;
	case CT_STATUS:
		if (Msg == SB_SETTEXTA || Msg == SB_SETTEXTW) {
			int section = (int)(wParam & 0xFF);
			char* text = (Msg == SB_SETTEXTW) ? wchar_to_utf8((const wchar_t*)lParam) : strdup((lParam == 0) ? "" : (const char*)lParam);
			if (section >= 0 && section < 2) {
				g_free(c->status_text[section]);
				c->status_text[section] = g_strdup((text == NULL) ? "" : text);
				schedule_sync(c);
			}
			free(text);
		}
		break;
	case CT_TOOLBAR:
		if (Msg == TB_ENABLEBUTTON) {
			rufus_ctl* d = get_dialog(c);
			UNLOCK();
			if (d != NULL)
				EnableWindow(GetDlgItem((HWND)d, (int)wParam), LOWORD(lParam));
			return TRUE;
		}
		/* TB_GETRECT and others: not supported */
		break;
	case CT_SLIDER:
		switch (Msg) {
		case TBM_SETPOS:
			c->pos = (int)lParam;
			c->pos_dirty = TRUE;
			schedule_sync(c);
			break;
		case TBM_GETPOS:
			r = c->pos;
			break;
		case TBM_SETRANGEMIN:
			c->range_min = (int)lParam;
			c->pos_dirty = TRUE;
			schedule_sync(c);
			break;
		case TBM_SETRANGEMAX:
			c->range_max = (int)lParam;
			c->pos_dirty = TRUE;
			schedule_sync(c);
			break;
		default:
			break;
		}
		break;
	case CT_EDIT:
		if (Msg == EM_LIMITTEXT) {
			c->max_length = (int)wParam;
			schedule_sync(c);
		}
		break;
	default:
		break;
	}
	UNLOCK();
	return r;
}

BOOL PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	rufus_ctl* c = to_ctl(hWnd);
	message_call* mc;
	if (c == NULL)
		return FALSE;
	if (c->type != CT_DIALOG) {
		SendMessageA(hWnd, Msg, wParam, lParam);
		return TRUE;
	}
	mc = g_new0(message_call, 1);
	mc->dlg = c;
	mc->msg = Msg;
	mc->wParam = wParam;
	mc->lParam = lParam;
	g_idle_add_full(G_PRIORITY_DEFAULT, post_message_cb, mc, NULL);
	return TRUE;
}

LRESULT SendDlgItemMessageA(HWND hDlg, int nIDDlgItem, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	return SendMessageA(GetDlgItem(hDlg, nIDDlgItem), Msg, wParam, lParam);
}

/* Timers: the callback (or WM_TIMER) is invoked on the main thread */
typedef struct {
	HWND hWnd;
	UINT_PTR id;
	TIMERPROC proc;
	guint source;
} ui_timer;

static GSList* timers = NULL;

static gboolean timer_cb(gpointer data)
{
	ui_timer* t = (ui_timer*)data;
	HWND hWnd = t->hWnd;
	UINT_PTR id = t->id;
	TIMERPROC proc = t->proc;
	if (proc != NULL)
		proc(hWnd, WM_TIMER, id, GetTickCount());
	else
		dispatch(to_ctl(hWnd), WM_TIMER, id, 0);
	return G_SOURCE_CONTINUE;
}

typedef struct {
	HWND hWnd;
	UINT_PTR id;
	UINT elapse;
	TIMERPROC proc;
	BOOL kill;
} timer_req;

static void timer_req_cb(void* arg)
{
	timer_req* r = (timer_req*)arg;
	GSList* it;
	for (it = timers; it != NULL; it = it->next) {
		ui_timer* t = (ui_timer*)it->data;
		if (t->hWnd == r->hWnd && t->id == r->id) {
			g_source_remove(t->source);
			timers = g_slist_remove(timers, t);
			g_free(t);
			break;
		}
	}
	if (!r->kill) {
		ui_timer* t = g_new0(ui_timer, 1);
		t->hWnd = r->hWnd;
		t->id = r->id;
		t->proc = r->proc;
		t->source = g_timeout_add(max(r->elapse, 1u), timer_cb, t);
		timers = g_slist_prepend(timers, t);
	}
}

UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc)
{
	timer_req r = { hWnd, nIDEvent, uElapse, lpTimerFunc, FALSE };
	ui_run_on_main_sync(timer_req_cb, &r);
	return nIDEvent;
}

BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent)
{
	timer_req r = { hWnd, uIDEvent, 0, NULL, TRUE };
	ui_run_on_main_sync(timer_req_cb, &r);
	return TRUE;
}

BOOL InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase)
{
	rufus_ctl* c = to_ctl(hWnd);
	(void)lpRect; (void)bErase;
	if (c == NULL)
		return FALSE;
	LOCK();
	schedule_sync(c);
	UNLOCK();
	return TRUE;
}

BOOL UpdateWindow(HWND hWnd)
{
	return InvalidateRect(hWnd, NULL, FALSE);
}

BOOL RedrawWindow(HWND hWnd, const RECT* lprcUpdate, void* hrgnUpdate, UINT flags)
{
	(void)lprcUpdate; (void)hrgnUpdate; (void)flags;
	return InvalidateRect(hWnd, NULL, FALSE);
}

BOOL SetFocus(HWND hWnd)
{
	rufus_ctl* c = to_ctl(hWnd);
	if (c != NULL && c->widget != NULL && ui_is_main_thread())
		gtk_widget_grab_focus(c->widget);
	return TRUE;
}

BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
	(void)hWndInsertAfter; (void)X; (void)Y; (void)cx; (void)cy;
	if (uFlags & SWP_SHOWWINDOW)
		ShowWindow(hWnd, SW_SHOW);
	else if (uFlags & SWP_HIDEWINDOW)
		ShowWindow(hWnd, SW_HIDE);
	return TRUE;
}

BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint)
{
	(void)hWnd; (void)X; (void)Y; (void)nWidth; (void)nHeight; (void)bRepaint;
	return TRUE;
}

BOOL GetWindowRect(HWND hWnd, LPRECT lpRect)
{
	rufus_ctl* c = to_ctl(hWnd);
	GtkAllocation a = { 0, 0, 0, 0 };
	if (lpRect == NULL)
		return FALSE;
	if (c != NULL && c->widget != NULL && ui_is_main_thread())
		gtk_widget_get_allocation(c->widget, &a);
	lpRect->left = a.x;
	lpRect->top = a.y;
	lpRect->right = a.x + a.width;
	lpRect->bottom = a.y + a.height;
	return TRUE;
}

BOOL GetClientRect(HWND hWnd, LPRECT lpRect)
{
	BOOL r = GetWindowRect(hWnd, lpRect);
	if (r) {
		lpRect->right -= lpRect->left;
		lpRect->bottom -= lpRect->top;
		lpRect->left = lpRect->top = 0;
	}
	return r;
}

int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
	(void)hWnd;
	return NotificationEx((int)uType, NULL, NULL, (lpCaption == NULL) ? APPLICATION_NAME : lpCaption, "%s", (lpText == NULL) ? "" : lpText);
}

BOOL EndDialog(HWND hDlg, INT_PTR nResult)
{
	rufus_ctl* c = to_ctl(hDlg);
	(void)nResult;
	if (c != NULL && c->widget != NULL && ui_is_main_thread())
		gtk_widget_hide(c->widget);
	return TRUE;
}

BOOL DestroyWindow(HWND hWnd)
{
	rufus_ctl* c = to_ctl(hWnd);
	if (c != NULL && c->widget != NULL && ui_is_main_thread())
		gtk_widget_destroy(c->widget);
	return TRUE;
}

BOOL MessageBeep(UINT uType)
{
	(void)uType;
	if (ui_is_main_thread())
		gdk_display_beep(gdk_display_get_default());
	return TRUE;
}
