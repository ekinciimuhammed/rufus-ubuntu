/*
 * Rufus for Linux: control model shared between the GTK front-end and the
 * Windows-style code (rufus.c, localization.c, ...)
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Rufus' logic manipulates its dialog through Win32 handles (HWND), control IDs
 * and window messages. On Linux, every control is a 'rufus_ctl' object that
 * holds a thread-safe model of the control's state (text, check state, combo
 * items and selection, enabled/visible state, progress position...) and the
 * GtkWidget that displays it. HWNDs are pointers to these objects.
 *
 * - Reading control state (ComboBox_GetCurSel(), IsDlgButtonChecked(), ...) is
 *   safe from any thread, like on Windows.
 * - Modifying control state updates the model immediately and the widget on the
 *   GTK main thread.
 * - User interaction is reported to the dialog procedure as WM_COMMAND messages
 *   (CBN_SELCHANGE, BN_CLICKED, EN_CHANGE...), exactly like Windows does.
 * - SendMessage() to a dialog runs its procedure synchronously on the main
 *   thread; PostMessage() queues it.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <windows.h>
#include <winctl_api.h>
#include <gtk/gtk.h>

typedef enum {
	CT_DIALOG = 1,		/* A top level window, with a dialog procedure */
	CT_STATIC,			/* GtkLabel */
	CT_BUTTON,			/* GtkButton */
	CT_CHECK,			/* GtkCheckButton */
	CT_COMBO,			/* GtkComboBoxText */
	CT_EDIT,			/* GtkEntry */
	CT_PROGRESS,		/* GtkProgressBar */
	CT_SLIDER,			/* GtkScale */
	CT_STATUS,			/* Status bar (two GtkLabels) */
	CT_TOOLBAR,			/* A group of buttons, addressed by command ID */
} ctl_type;

/* Create a control model object for a widget (the widget may be NULL for invisible controls) */
HWND ctl_create(HWND parent, int id, ctl_type type, GtkWidget* widget);
/* Same, for a toolbar (a set of buttons, each identified by a command ID) */
void ctl_toolbar_add(HWND toolbar, int cmd_id, GtkWidget* button);
/* Set the procedure that receives the messages of a dialog */
void ctl_set_dialog_proc(HWND dialog, DLGPROC proc);
/* Accessors */
GtkWidget* ctl_widget(HWND hCtrl);
int ctl_id(HWND hCtrl);
HWND ctl_parent(HWND hCtrl);
/* Additional label widget whose visibility follows the control's (e.g. a caption) */
void ctl_set_buddy(HWND hCtrl, GtkWidget* buddy);
/* Destroy a dialog's control objects (when a secondary dialog is closed) */
void ctl_destroy_dialog(HWND dialog);
/* Tooltips */
void ctl_set_tooltip(HWND hCtrl, const char* text);

/* Status bar (CT_STATUS): left and right texts */
void ctl_status_set_text(HWND hStatus, int section, const char* text);

/* Threading */
BOOL ui_is_main_thread(void);
void ui_run_on_main_sync(void (*fn)(void* arg), void* arg);
void ui_run_on_main_async(void (*fn)(void* arg), void* arg);
void ui_init_threading(void);
/* The main application window */
GtkWindow* ui_main_window(void);
void ui_set_main_window(GtkWindow* window);

/* Main window helpers (rufus_main_linux.inc) */
void linux_ui_set_toggle_arrow(int toggle_id, BOOL expanded);
