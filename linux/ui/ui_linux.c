/*
 * Rufus for Linux: user interface functions (counterpart of src/ui.c)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus Linux port contributors
 *
 * The progress, persistence and combo box logic below is taken verbatim from
 * src/ui.c. The functions that deal with the layout of the Windows dialog are
 * replaced by their GTK equivalents at the end of this file.
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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "rufus.h"
#include "drive.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "ui.h"
#include "winctl.h"

UINT_PTR UM_LANGUAGE_MENU_MAX = UM_LANGUAGE_MENU;
extern BOOL use_vds, appstore_version, app_changed_size;
extern int imop_win_sel, persistence_unit_selection;
extern char *unattend_xml_path, *archive_path, image_option_txt[128];
int update_progress_type = UPT_PERCENT;
int advanced_device_section_height, advanced_format_section_height;
int cbw, ddw, ddbh = 0, bh = 0;

/*
 * The following is used to allocate slots within the progress bar
 * 0 means unused (no operation or no progress allocated to it)
 * +n means allocate exactly n bars (n percent of the progress bar)
 * -n means allocate a weighted slot of n from all remaining
 *    bars. E.g. if 80 slots remain and the sum of all negative entries
 *    is 10, -4 will allocate 4/10*80 = 32 bars (32%) for OP progress
 */
static int nb_slots[OP_MAX];
static float slot_end[OP_MAX+1];	// shifted +1 so that we can subtract 1 to OP indexes
static float previous_end;

// Set the combo selection according to the data
void SetComboEntry(HWND hDlg, int data)
{
	int i, nb_entries = ComboBox_GetCount(hDlg);

	if (nb_entries <= 0)
		return;
	for (i = 0; i < nb_entries; i++) {
		if (ComboBox_GetItemData(hDlg, i) == data) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hDlg, i));
			return;
		}
	}
	if (i == nb_entries)
		IGNORE_RETVAL(ComboBox_SetCurSel(hDlg, 0));
}

// Set up progress bar real estate allocation
void InitProgress(BOOL bOnlyFormat)
{
	int i;
	float last_end = 0.0f, slots_discrete = 0.0f, slots_analog = 0.0f;

	memset(nb_slots, 0, sizeof(nb_slots));
	memset(slot_end, 0, sizeof(slot_end));
	previous_end = 0.0f;

	if (bOnlyFormat) {
		nb_slots[OP_FORMAT] = -1;
	} else {
		nb_slots[OP_ANALYZE_MBR] = 1;
		if (IsChecked(IDC_BAD_BLOCKS)) {
			nb_slots[OP_BADBLOCKS] = -1;
		}
		if (boot_type != BT_NON_BOOTABLE) {
			// 1 extra slot for PBR writing
			switch (selection_default) {
			case BT_MSDOS:
				nb_slots[OP_FILE_COPY] = 3 + 1;
				break;
			case BT_FREEDOS:
				nb_slots[OP_FILE_COPY] = 5 + 1;
				break;
			case BT_IMAGE:
				nb_slots[OP_FILE_COPY] = (img_report.is_iso || img_report.is_windows_img) ? -1 : 0;
				if (HAS_WINDOWS(img_report) && (unattend_xml_path != NULL) &&
					(ComboBox_GetCurItemData(hImageOption) != IMOP_WIN_TO_GO))
					nb_slots[OP_PATCH] = -1;
				break;
			default:
				nb_slots[OP_FILE_COPY] = 2 + 1;
				break;
			}
		}
		if (selection_default == BT_IMAGE && !(img_report.is_iso || img_report.is_windows_img)) {
			nb_slots[OP_FORMAT] = -1;
		} else {
			nb_slots[OP_ZERO_MBR] = 1;
			nb_slots[OP_PARTITION] = 1;
			nb_slots[OP_FIX_MBR] = 1;
			nb_slots[OP_CREATE_FS] = (use_vds) ? 2 :
				nb_steps[ComboBox_GetCurItemData(hFileSystem)];
			// So, yeah, if you're doing slow format, or using Large FAT32, and have persistence, you'll see
			// the progress bar revert during format on account that we reuse the same operation for both
			// partitions. Maybe one day I'll be bothered to handle two separate OP_FORMAT ops...
			if ((!IsChecked(IDC_QUICK_FORMAT)) || (persistence_size != 0) || IS_EXT(fs_type) ||
				((fs_type == FS_FAT32) && ((SelectedDrive.DiskSize >= LARGE_FAT32_SIZE) || (force_large_fat32)))) {
				nb_slots[OP_FORMAT] = -1;
				nb_slots[OP_CREATE_FS] = 0;
			}
			nb_slots[OP_FINALIZE] = ((selection_default == BT_IMAGE) && (fs_type == FS_NTFS)) ? 3 : 2;
		}
	}
	if (archive_path != NULL) {
		nb_slots[OP_EXTRACT_ZIP] = -1;
	}

	for (i = 0; i < OP_MAX; i++) {
		if (nb_slots[i] > 0) {
			slots_discrete += nb_slots[i] * 1.0f;
		}
		if (nb_slots[i] < 0) {
			slots_analog += nb_slots[i] * 1.0f;
		}
	}

	for (i = 0; i < OP_MAX; i++) {
		if (nb_slots[i] == 0) {
			slot_end[i + 1] = last_end;
		} else if (nb_slots[i] > 0) {
			slot_end[i + 1] = last_end + (1.0f * nb_slots[i]);
		} else if (nb_slots[i] < 0) {
			slot_end[i + 1] = last_end + (((100.0f - slots_discrete) * nb_slots[i]) / slots_analog);
		}
		last_end = slot_end[i + 1];
	}

	// If there's no analog, adjust our discrete ends to fill the whole bar
	if (slots_analog == 0.0f) {
		for (i = 0; i < OP_MAX; i++) {
			slot_end[i + 1] *= 100.0f / slots_discrete;
		}
	}
}

// Position the progress bar within each operation range
void UpdateProgress(int op, float percent)
{
	int pos;
	static uint64_t LastRefresh = 0;

	if ((op < 0) || (op >= OP_MAX)) {
		duprintf("UpdateProgress: invalid op %d\n", op);
		return;
	}
	if (percent > 100.1f) {
		// duprintf("UpdateProgress(%d): invalid percentage %0.2f\n", op, percent);
		return;
	}
	if ((percent < 0.0f) && (nb_slots[op] <= 0)) {
		duprintf("UpdateProgress(%d): error negative percentage sent for negative slot value\n", op);
		return;
	}
	if (nb_slots[op] == 0)
		return;
	if (previous_end < slot_end[op]) {
		previous_end = slot_end[op];
	}

	if (percent < 0.0f) {
		// Negative means advance one slot (1.0%) - requires a positive slot allocation
		previous_end += (slot_end[op + 1] - slot_end[op]) / (1.0f * nb_slots[op]);
		pos = (int)(previous_end / 100.0f * MAX_PROGRESS);
	} else {
		pos = (int)((previous_end + ((slot_end[op + 1] - previous_end) * (percent / 100.0f))) / 100.0f * MAX_PROGRESS);
	}
	if (pos > MAX_PROGRESS) {
		duprintf("UpdateProgress(%d): rounding error - pos %d is greater than %d", op, pos, MAX_PROGRESS);
		pos = MAX_PROGRESS;
	}

	// Reduce the refresh rate, to avoid weird effects on the sliding part of progress bar
	if (GetTickCount64() > LastRefresh + (2 * MAX_REFRESH)) {
		LastRefresh = GetTickCount64();
		SendMessage(hProgress, PBM_SETPOS, (WPARAM)pos, 0);
		SetTaskbarProgressValue(pos, MAX_PROGRESS);
	}
}

// This code attempts to maintain the notion of a "current" download speed, over the course
// of no less than 3s. (Shorter intervals produce very erratic results.)
//
// To do so, it samples the speed in 150ms intervals and stores the recorded samples in a
// FIFO history ring. The ring stores no more than 20 intervals, hence the history covers
// the period of at least three seconds and at most 20 reads into the past. This method
// should produce reasonable results for downloads ranging from very slow to very fast.
//
// The idea is that for fast downloads, we get the speed over exactly the last three seconds.
// For slow downloads (where a network read takes more than 150ms to complete), we get the
// speed over a larger time period, as large as it takes to complete twenty reads. This is
// good because slow downloads tend to fluctuate more and a 3-second average would be too
// erratic.
/*
 * The following is taken from GNU wget (progress.c)
 */
struct bar_progress {
	uint64_t total_length;          // expected total byte count when the download finishes
	uint64_t count;                 // bytes downloaded so far
	uint64_t last_screen_update;    // time of the last screen update, measured since the beginning of download.
	uint64_t dltime;                // download time so far
	// Keep track of recent download speeds.
	struct bar_progress_hist {
		uint64_t pos;
		uint64_t times[SPEED_HISTORY_SIZE];
		uint64_t bytes[SPEED_HISTORY_SIZE];
		// The sum of times and bytes respectively, maintained for efficiency.
		uint64_t total_time;
		uint64_t total_bytes;
	} hist;
	uint64_t recent_start;          // timestamp of beginning of current position.
	uint64_t recent_bytes;          // bytes downloaded so far.
	BOOL stalled;                   // set when no data arrives for longer than STALL_START_TIME, then reset when new data arrives.

	// The following are used to make sure that ETA information doesn't flicker.
	uint64_t last_eta_time;         // time of the last update to download speed and ETA, measured since the beginning of download.
	int last_eta_value;
};

static void bar_update(struct bar_progress* bp, uint64_t howmuch, uint64_t dltime)
{
	struct bar_progress_hist* hist = &bp->hist;
	uint64_t recent_age = dltime - bp->recent_start;

	// Update the download count.
	bp->recent_bytes += howmuch;

	// For very small time intervals, we return after having updated the
	// "recent" download count. When its age reaches or exceeds minimum
	// sample time, it will be recorded in the history ring.
	if (recent_age < SPEED_SAMPLE_MIN)
		return;

	if (howmuch == 0) {
		// If we're not downloading anything, we might be stalling,
		// i.e. not downloading anything for an extended period of time.
		// Since 0-reads do not enter the history ring, recent_age
		// effectively measures the time since last read.
		if (recent_age >= STALL_START_TIME) {
			// If we're stalling, reset the ring contents because it's
			// stale and because it will make bar_update stop printing
			// the (bogus) current bandwidth.
			bp->stalled = TRUE;
			memset(hist, 0, sizeof(struct bar_progress_hist));
			bp->recent_bytes = 0;
		}
		return;
	}

	// We now have a non-zero amount of to store to the speed ring.

	// If the stall status was acquired, reset it.
	if (bp->stalled) {
		bp->stalled = FALSE;
		// "recent_age" includes the entire stalled period, which
		// could be very long. Don't update the speed ring with that
		// value because the current bandwidth would start too small.
		// Start with an arbitrary (but more reasonable) time value and
		// let it level out.
		recent_age = 1000;
	}

	// Store "recent" bytes and download time to history ring at the position POS.

	// To correctly maintain the totals, first invalidate existing data
	// (least recent in time) at this position. */
	hist->total_time -= hist->times[hist->pos];
	hist->total_bytes -= hist->bytes[hist->pos];

	// Now store the new data and update the totals.
	hist->times[hist->pos] = recent_age;
	hist->bytes[hist->pos] = bp->recent_bytes;
	hist->total_time += recent_age;
	hist->total_bytes += bp->recent_bytes;

	// Start a new "recent" period.
	bp->recent_start = dltime;
	bp->recent_bytes = 0;

	// Advance the current ring position.
	if (++hist->pos == SPEED_HISTORY_SIZE)
		hist->pos = 0;
}

// This updates the progress bar as well as the data displayed on it so that we can
// display percentage completed, rate of transfer and estimated remaining duration.
// During init (op = OP_INIT) an optional HWND can be passed on which to look for
// a progress bar. Part of the code (eta, speed) comes from GNU wget.
void _UpdateProgressWithInfo(int op, int msg, uint64_t processed, uint64_t total, BOOL force)
{
	static int last_update_progress_type = UPT_PERCENT;
	static struct bar_progress bp = { 0 };
	HWND hProgressDialog = (HWND)(uintptr_t)processed;
	static HWND hProgressBar = NULL;
	static uint64_t start_time = 0, last_refresh = 0;
	uint64_t speed = 0, current_time = GetTickCount64();
	double percent = 0.0;
	char msg_data[128];
	static BOOL bNoAltMode = FALSE;

	if (op == OP_INIT) {
		start_time = current_time - 1;
		last_refresh = 0;
		last_update_progress_type = UPT_PERCENT;
		percent = 0.0f;
		speed = 0;
		memset(&bp, 0, sizeof(bp));
		bp.total_length = total;
		hProgressBar = NULL;
		bNoAltMode = (BOOL)msg;
		if (hProgressDialog != NULL) {
			// Use the progress control provided, if any
			hProgressBar = GetDlgItem(hProgressDialog, IDC_PROGRESS);
			if (hProgressBar != NULL) {
				SendMessage(hProgressBar, PBM_SETSTATE, (WPARAM)PBST_NORMAL, 0);
				SendMessage(hProgressBar, PBM_SETMARQUEE, FALSE, 0);
				SendMessage(hProgressBar, PBM_SETPOS, 0, 0);
			}
			SendMessage(hProgressDialog, UM_PROGRESS_INIT, 0, 0);
		}
	} else if ((hProgressBar != NULL) || (op > 0)) {
		uint64_t dl_total_time = current_time - start_time;
		uint64_t howmuch = processed - bp.count;
		bp.count = processed;
		bp.total_length = total;
		if (bp.count > bp.total_length)
			bp.total_length = bp.count;
		if (bp.total_length > 0)
			percent = (100.0f * bp.count) / (1.0f * bp.total_length);
		else
			percent = 0.0f;

		if ((bp.hist.total_time > 999) && (bp.hist.total_bytes != 0)) {
			// Calculate the download speed using the history ring and
			// recent data that hasn't made it to the ring yet.
			uint64_t dlquant = bp.hist.total_bytes + bp.recent_bytes;
			uint64_t dltime = bp.hist.total_time + (dl_total_time - bp.recent_start);
			speed = (dltime == 0) ? 0 : (dlquant * 1000) / dltime;
		} else {
			speed = 0;
		}
		bar_update(&bp, howmuch, dl_total_time);

		if (bNoAltMode)
			update_progress_type = UPT_PERCENT;
		switch (update_progress_type) {
		case UPT_SPEED:
			if (speed != 0)
				static_sprintf(msg_data, "%s/s", SizeToHumanReadable(speed, FALSE, FALSE));
			else
				static_sprintf(msg_data, "---");
			break;
		case UPT_ETA:
			if ((bp.total_length > 0) && (bp.count > 0) && (dl_total_time > 3000)) {
				uint32_t eta = 0;

				// Don't change the value of ETA more than approximately once
				// per second; doing so would cause flashing without providing
				// any value to the user.
				if ((bp.total_length != processed) && (bp.last_eta_value != 0) &&
					(dl_total_time - bp.last_eta_time < ETA_REFRESH_INTERVAL)) {
					eta = bp.last_eta_value;
				} else {
					// Calculate ETA using the average download speed to predict
					// the future speed. If you want to use a speed averaged
					// over a more recent period, replace dl_total_time with
					// hist->total_time and bp->count with hist->total_bytes.
					// I found that doing that results in a very jerky and
					// ultimately unreliable ETA.
					uint64_t bytes_remaining = bp.total_length - processed;
					double d_eta = (dl_total_time / 1000.0) * (bytes_remaining * 1.0) / (bp.count * 1.0);
					if (d_eta >= INT_MAX - 1)
						goto skip_eta;
					eta = (uint32_t)(d_eta + 0.5);
					bp.last_eta_value = eta;
					bp.last_eta_time = dl_total_time;
				}
				static_sprintf(msg_data, "%d:%02d:%02d", eta / 3600, (uint16_t)((eta % 3600) / 60), (uint16_t)(eta % 60));
			} else {
			skip_eta:
				static_sprintf(msg_data, "-:--:--");
			}
			break;
		default:
			static_sprintf(msg_data, "%0.1f%%", percent);
			break;
		}
		if ((force) || (bp.count == bp.total_length) || (current_time > last_refresh + MAX_REFRESH)) {
			if (op < 0) {
				SendMessage(hProgressBar, PBM_SETPOS, (WPARAM)(MAX_PROGRESS * percent / 100.0f), 0);
				if (op == OP_NOOP_WITH_TASKBAR)
					SetTaskbarProgressValue((ULONGLONG)(MAX_PROGRESS * percent / 100.0f), MAX_PROGRESS);
			} else {
				UpdateProgress(op, (float)percent);
			}
			if ((force) || ((msg >= 0) && ((current_time > bp.last_screen_update + SCREEN_REFRESH_INTERVAL) ||
				(last_update_progress_type != update_progress_type) || (bp.count == bp.total_length)))) {
				PrintInfo(0, msg, msg_data);
				bp.last_screen_update = current_time;
			}
			last_refresh = current_time;
		}
		last_update_progress_type = update_progress_type;
	}
}

void SetPassesTooltip(void)
{
	const unsigned int pattern[BADLOCKS_PATTERN_TYPES][BADBLOCK_PATTERN_COUNT] =
	{ BADBLOCK_PATTERN_ONE_PASS, BADBLOCK_PATTERN_TWO_PASSES, BADBLOCK_PATTERN_SLC,
	  BADCLOCK_PATTERN_MLC, BADBLOCK_PATTERN_TLC };
	int sel = ComboBox_GetCurSel(hNBPasses);
	CreateTooltip(hNBPasses, lmprintf(MSG_153 + ((sel >= 2) ? 3 : sel),
		pattern[sel][0], pattern[sel][1], pattern[sel][2], pattern[sel][3]), -1);
}

void SetPersistencePos(uint64_t pos)
{
	char tmp[64];

	if ((boot_type == BT_IMAGE) && (pos != 0)) {
		TogglePersistenceControls(TRUE);
		static_sprintf(tmp, "%d", (int)pos);
	} else {
		TogglePersistenceControls(FALSE);
		static_sprintf(tmp, "0 (%s)", lmprintf(MSG_124));
	}
	app_changed_size = TRUE;
	SetWindowTextU(GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE), tmp);
}

void SetPersistenceSize(void)
{
	int i, proposed_unit_selection = 0;
	LONGLONG base_unit = MB;
	HWND hCtrl;
	uint64_t max = 0, pos = 0;

	if (ComboBox_GetCurSel(hDeviceList) >= 0) {
		max = SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size);
		persistence_size = min(persistence_size, max);
		pos = persistence_size;

		// Reset the the Persistence Units dropdown
		hCtrl = GetDlgItem(hMainDialog, IDC_PERSISTENCE_UNITS);
		IGNORE_RETVAL(ComboBox_ResetContent(hCtrl));
		for (i = 0; i < 3; i++) {
			IGNORE_RETVAL(ComboBox_SetItemData(hCtrl, ComboBox_AddStringU(hCtrl, lmprintf(MSG_022 + i)), i));
			// If we have more than 7 discrete positions, set this unit as our base
			if (SelectedDrive.DiskSize > 7 * base_unit)
				proposed_unit_selection = i;
			base_unit *= 1024;
			// Don't allow a base unit unless the drive is at least twice the size of that unit
			if (SelectedDrive.DiskSize < 2 * base_unit)
				break;
		}
		if (persistence_unit_selection < 0)
			persistence_unit_selection = proposed_unit_selection;

		IGNORE_RETVAL(ComboBox_SetCurSel(hCtrl, persistence_unit_selection));
		if ((pos != 0) && (pos < MIN_EXT_SIZE))
			pos = MIN_EXT_SIZE;
		pos /= MB;
		max /= MB;
		for (i = 0; i < persistence_unit_selection; i++) {
			pos /= 1024;
			max /= 1024;
		}

	}

	hCtrl = GetDlgItem(hMainDialog, IDC_PERSISTENCE_SLIDER);
	// Wow! Unless you set *all* these redraw WPARAMs to true, the one from
	// TBM_SETPOS gets completely ignored if the value is zero!
	SendMessage(hCtrl, TBM_SETRANGEMIN, (WPARAM)TRUE, (LPARAM)0);
	SendMessage(hCtrl, TBM_SETRANGEMAX, (WPARAM)TRUE, (LPARAM)max);
	SendMessage(hCtrl, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)pos);

	SetPersistencePos(pos);
}

/*
 * Linux specific versions of the layout functions from ui.c
 */

void SetAccessibleName(HWND hCtrl, const char* name)
{
	GtkWidget* w = ctl_widget(hCtrl);
	if (w != NULL && name != NULL)
		atk_object_set_name(gtk_widget_get_accessible(w), name);
}

void TogglePersistenceControls(BOOL display)
{
	HWND hSize = GetDlgItem(hMainDialog, IDC_PERSISTENCE_SIZE);
	HWND hUnits = GetDlgItem(hMainDialog, IDC_PERSISTENCE_UNITS);
	EnableWindow(hSize, display ? TRUE : FALSE);
	EnableWindow(hUnits, display ? TRUE : FALSE);
	ShowWindow(hUnits, display ? SW_SHOW : SW_HIDE);
}

static int image_option_toggle_ids[][2] = {
	{ IDS_IMAGE_OPTION_TXT, 0x03 },
	{ IDC_IMAGE_OPTION, 0x01 },
	{ IDC_PERSISTENCE_SLIDER, 0x02 },
	{ IDC_PERSISTENCE_SIZE, 0x02 },
	{ IDC_PERSISTENCE_UNITS, 0x02 }
};

void ToggleImageOptions(void)
{
	BOOL has_wintogo, has_persistence;
	int i;

	/*
	 * Windows To Go requires Windows' bcdboot (and an offline registry), which
	 * have no Linux equivalent, so, much like on a host that is older than
	 * Windows 8, the option is never offered.
	 */
	has_wintogo = FALSE;
	has_persistence = ((boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.is_iso) && (HAS_PERSISTENCE(img_report)));
	assert(popcnt8(image_options) <= 1);

	// Keep a copy of the "Image Option" text (so that we don't have to duplicate its translation in the .loc)
	if (image_option_txt[0] == 0)
		GetWindowTextU(GetDlgItem(hMainDialog, IDS_IMAGE_OPTION_TXT), image_option_txt, sizeof(image_option_txt));

	if (((has_wintogo) && !(image_options & IMOP_WINTOGO)) ||
		((!has_wintogo) && (image_options & IMOP_WINTOGO))) {
		image_options ^= IMOP_WINTOGO;
		if (image_options & IMOP_WINTOGO) {
			SetWindowTextU(GetDlgItem(hMainDialog, IDS_IMAGE_OPTION_TXT), image_option_txt);
			IGNORE_RETVAL(ComboBox_SetCurSel(hImageOption, imop_win_sel));
		}
	}
	if (((has_persistence) && !(image_options & IMOP_PERSISTENCE)) ||
		((!has_persistence) && (image_options & IMOP_PERSISTENCE))) {
		image_options ^= IMOP_PERSISTENCE;
		if (image_options & IMOP_PERSISTENCE) {
			SetWindowTextU(GetDlgItem(hMainDialog, IDS_IMAGE_OPTION_TXT), lmprintf(MSG_123));
			TogglePersistenceControls(persistence_size != 0);
			SetPersistenceSize();
		}
	}
	// Hide or show the boot options (GTK handles the relayout)
	for (i = 0; i < ARRAYSIZE(image_option_toggle_ids); i++) {
		ShowWindow(GetDlgItem(hMainDialog, image_option_toggle_ids[i][0]),
			(image_options & image_option_toggle_ids[i][1]) ? SW_SHOW : SW_HIDE);
	}
	if ((image_options & IMOP_PERSISTENCE) && !(image_options & IMOP_WINTOGO))
		TogglePersistenceControls(persistence_size != 0);
	InvalidateRect(hMainDialog, NULL, TRUE);
}

static const int advanced_device_ids[] = { IDC_SAVE, IDC_LIST_USB_HDD, IDC_OLD_BIOS_FIXES, IDC_UEFI_MEDIA_VALIDATION };
static const int advanced_format_ids[] = { IDC_QUICK_FORMAT, IDC_BAD_BLOCKS, IDC_NB_PASSES, IDC_EXTENDED_LABEL };

static void set_toggle_text(int id, BOOL enable, int msg)
{
	char tmp[256];
	static_sprintf(tmp, "%s", lmprintf(enable ? MSG_122 : MSG_121, lmprintf(msg)));
	SetWindowTextU(GetDlgItem(hMainDialog, id), tmp);
}

void ToggleAdvancedDeviceOptions(BOOL enable)
{
	size_t i;
	for (i = 0; i < ARRAYSIZE(advanced_device_ids); i++)
		ShowWindow(GetDlgItem(hMainDialog, advanced_device_ids[i]), enable ? SW_SHOW : SW_HIDE);
	set_toggle_text(IDC_ADVANCED_DEVICE_TOOLBAR, enable, MSG_119);
	linux_ui_set_toggle_arrow(IDC_ADVANCED_DEVICE_TOOLBAR, enable);
}

void ToggleAdvancedFormatOptions(BOOL enable)
{
	size_t i;
	for (i = 0; i < ARRAYSIZE(advanced_format_ids); i++)
		ShowWindow(GetDlgItem(hMainDialog, advanced_format_ids[i]), enable ? SW_SHOW : SW_HIDE);
	set_toggle_text(IDC_ADVANCED_FORMAT_TOOLBAR, enable, MSG_120);
	linux_ui_set_toggle_arrow(IDC_ADVANCED_FORMAT_TOOLBAR, enable);
}

void SetBootTypeDropdownWidth(void)
{
	/* GTK sizes drop downs to their content */
}

static void on_language_selected(GtkMenuItem* item, gpointer data)
{
	(void)item;
	PostMessage(hMainDialog, WM_COMMAND, (WPARAM)(UM_LANGUAGE_MENU + GPOINTER_TO_INT(data)), 0);
}

/* Display the language selection menu next to the language button */
void ShowLanguageMenu(RECT rcExclude)
{
	GtkWidget *menu, *item, *anchor = ctl_widget(GetDlgItem(hMainDialog, IDC_LANG));
	loc_cmd* lcmd = NULL;
	char lang[256];
	int index = 0;
	(void)rcExclude;

	menu = gtk_menu_new();
	UM_LANGUAGE_MENU_MAX = UM_LANGUAGE_MENU;
	list_for_each_entry(lcmd, &locale_list, loc_cmd, list) {
		// The appearance of LTR languages must be fixed for RTL menus
		if ((right_to_left_mode) && (!(lcmd->ctrl_id & LOC_RIGHT_TO_LEFT)))  {
			static_strcpy(lang, LEFT_TO_RIGHT_EMBEDDING);
			static_strcat(lang, lcmd->txt[1]);
			static_strcat(lang, POP_DIRECTIONAL_FORMATTING);
		} else {
			static_strcpy(lang, lcmd->txt[1]);
		}
		item = gtk_check_menu_item_new_with_label(lang);
		gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), selected_locale == lcmd);
		g_signal_connect(item, "activate", G_CALLBACK(on_language_selected), GINT_TO_POINTER(index));
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
		UM_LANGUAGE_MENU_MAX++;
		index++;
	}
	gtk_widget_show_all(menu);
	if (anchor != NULL)
		gtk_menu_popup_at_widget(GTK_MENU(menu), anchor, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
	else
		gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}
