#include "ui.h"

#include "backup.h"
#include "restore.h"
#include "bootloader.h"
#include "device.h"
#include "log.h"
#include "model.h"
#include "storage.h"
#include "style.h"
#include "update.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Below this, and not charging, an update is refused: running out of battery
// halfway through leaves the system half written.
#define MIN_BATTERY_PCT 20

static lv_group_t *group;
static lv_obj_t *scr;
static lv_timer_t *screen_timer; // the current screen's refresh, if any

// main screen
static lv_obj_t *lbl_boot, *lbl_card, *lbl_note;

// progress screen
static lv_obj_t *lbl_warn, *lbl_phase, *bar, *lbl_msg, *btn_row;
// bootloader install screen
static lv_obj_t *bl_phase, *bl_msg, *bl_buttons;

static void show_main(void);
static void show_advanced(void);
static void show_browser(void);
static void confirm_override(void);
static bool battery_ok(void);

// ---------------------------------------------------------------------------
// Building blocks

static void screen_reset(void) {
	if (screen_timer) {
		lv_timer_delete(screen_timer);
		screen_timer = NULL;
	}
	lv_group_remove_all_objs(group);
	lv_obj_clean(scr);
	lbl_boot = lbl_card = lbl_note = NULL;
	lbl_warn = lbl_phase = bar = lbl_msg = btn_row = NULL;
	bl_phase = bl_msg = bl_buttons = NULL;
}

static lv_obj_t *column(lv_obj_t *parent) {
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_remove_style_all(c);
	lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 12, 0);
	return c;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, text);
	lv_obj_set_width(l, LV_PCT(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, color, 0);
	return l;
}

typedef void (*action_fn)(void);

// Button actions run from lv_async_call, after the click event is over: most
// of them replace the screen, and deleting the button whose event is being
// delivered is not something to do from inside that event.
static void async_run(void *fn) { ((action_fn)fn)(); }

static void click_cb(lv_event_t *e) { lv_async_call(async_run, lv_event_get_user_data(e)); }

// Where Back, Cancel and Power (LV_KEY_ESC) lead: the menu the current
// screen was reached from, set by that menu (show_main, show_advanced).
// Power does nothing on the main menu and during an update, reset, backup
// or bootloader install (back_allowed); on the Advanced menu it is its Back.
static bool back_allowed;
static bool on_advanced;
static bool on_browser; // the file browser: Power goes up a folder
static void browser_up(void);
static void go_back(void);

static void back_to_main(void *unused) {
	(void)unused;
	if (back_allowed) {
		if (on_advanced) {
			show_main();
		} else if (on_browser) {
			browser_up();
		} else {
			go_back();
		}
	}
}

static action_fn back_to;
static void go_back(void) { (back_to ? back_to : show_main)(); }

static void key_cb(lv_event_t *e) {
	if (lv_event_get_key(e) == LV_KEY_ESC && back_allowed) {
		lv_async_call(back_to_main, NULL);
	}
}

// Rows and buttons in the player's style (settingsrow.c, confirm.c): cards
// in the surface colour with 12 px corners, no shadow, flat when pressed;
// the main action accent-filled, a destructive one red. The side keys move a
// focus that has to be seen from arm's length: an accent ring.
#define CARD_RADIUS 12
#define BTN_H 72
static int32_t btn_height = BTN_H; // the main menu's rows are taller; page() resets it

static void card_style(lv_obj_t *b, lv_color_t bg, lv_color_t fg) {
	lv_obj_set_style_bg_color(b, bg, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(b, fg, 0);
	lv_obj_set_style_radius(b, CARD_RADIUS, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_bg_color(b, S.card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_transform_width(b, 0, LV_STATE_PRESSED);
	lv_obj_set_style_transform_height(b, 0, LV_STATE_PRESSED);
	lv_obj_set_style_border_color(b, S.accent, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_border_width(b, 3, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_outline_width(b, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_pad_hor(b, 20, 0);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, action_fn fn, bool danger) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, LV_PCT(100), btn_height);
	card_style(b, danger ? S.destructive : S.card, danger ? S.on_accent : S.text);
	if (danger) {
		lv_obj_set_style_bg_color(b, lv_color_darken(S.destructive, LV_OPA_20), LV_STATE_PRESSED);
		lv_obj_set_style_border_color(b, S.text, LV_STATE_FOCUS_KEY); // the accent may be red-ish too
	}
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, F24, 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, click_cb, LV_EVENT_CLICKED, (void *)fn);
	lv_obj_add_event_cb(b, key_cb, LV_EVENT_KEY, NULL);
	lv_group_add_obj(group, b);
	return b;
}

// The one action a screen is there for: accent-filled.
static lv_obj_t *button_primary(lv_obj_t *parent, const char *text, action_fn fn) {
	lv_obj_t *b = button(parent, text, fn, false);
	lv_obj_set_style_bg_color(b, S.accent, 0);
	lv_obj_set_style_text_color(b, S.on_accent, 0);
	lv_obj_set_style_bg_color(b, lv_color_darken(S.accent, LV_OPA_20), LV_STATE_PRESSED);
	lv_obj_set_style_border_color(b, S.text, LV_STATE_FOCUS_KEY);
	return b;
}

// A menu row: the name on the left, and a chevron when it opens a page.
static lv_obj_t *row(lv_obj_t *parent, const char *text, action_fn fn, bool chevron) {
	lv_obj_t *b = button(parent, text, fn, false);
	lv_obj_t *l = lv_obj_get_child(b, 0);
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	if (chevron) {
		lv_obj_t *c = lv_label_create(b);
		lv_label_set_text(c, LV_SYMBOL_RIGHT);
		lv_obj_set_style_text_font(c, F20, 0);
		lv_obj_set_style_text_color(c, S.dim, 0);
		lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	return b;
}

// A progress bar: the accent on a card-coloured track.
static lv_obj_t *progress_bar(lv_obj_t *parent) {
	lv_obj_t *b = lv_bar_create(parent);
	lv_obj_set_size(b, LV_PCT(100), 12);
	lv_bar_set_range(b, 0, 100);
	lv_obj_set_style_bg_color(b, S.card, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, S.accent, LV_PART_INDICATOR);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	return b;
}

static lv_obj_t *page(const char *title) {
	screen_reset();
	back_allowed = true;
	on_advanced = false;
	on_browser = false;
	btn_height = BTN_H;
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(scr, 24, 0);
	lv_obj_set_style_pad_row(scr, 16, 0);
	label(scr, title, F32, S.text);
	return scr;
}

// ---------------------------------------------------------------------------
// Messages and confirmations

static action_fn pending_action;

static void show_message(const char *title, const char *text) {
	page(title);
	label(scr, text, F24, S.text);
	lv_group_focus_obj(button(scr, "Back", go_back, false));
}

static void confirm_yes(void) {
	action_fn fn = pending_action;
	pending_action = NULL;
	if (fn) {
		fn();
	}
}

static void confirm(const char *title, const char *text, const char *yes, action_fn fn) {
	pending_action = fn;
	page(title);
	label(scr, text, F24, S.text);
	button(scr, yes, confirm_yes, true);
	// Focus starts on Cancel: a stray Play press does nothing harmful.
	lv_group_focus_obj(button(scr, "Cancel", go_back, false));
}

// ---------------------------------------------------------------------------
// Actions

static void reboot_system(void) {
	if (bootflag_read() != BOOT_NORMAL && bootflag_write(BOOT_NORMAL) != 0) {
		show_message("Error", "The boot record could not be set to the system.");
		return;
	}
	device_reboot();
}

static void reboot_fastboot(void) {
	if (bootflag_request_fastboot() != 0) {
		show_message("Error", "The fastboot request could not be written.");
		return;
	}
	device_reboot();
}

static void poweroff(void) { device_poweroff(); }

// Update progress

// Set by the automatic update: on success it restarts into the system by
// itself, like the stock recovery, unless Back is pressed first.
#define AUTO_REBOOT_DELAY_S 5
static bool update_is_auto;
static int reboot_left;

static void reboot_tick(lv_timer_t *t) {
	(void)t;
	if (--reboot_left > 0) {
		lv_label_set_text_fmt(lbl_phase, "Done. Restarting in %d s", reboot_left);
		return;
	}
	lv_timer_delete(screen_timer);
	screen_timer = NULL;
	reboot_system();
}

static void update_poll(lv_timer_t *t) {
	(void)t;
	update_status_t s;
	update_get(&s);
	if (!lbl_phase) {
		return;
	}
	lv_label_set_text(lbl_phase, s.phase[0] ? s.phase : "Starting");
	if (s.percent >= 0) {
		lv_bar_set_value(bar, s.percent, LV_ANIM_OFF);
	}
	if (s.state == UPDATE_RUNNING) {
		return;
	}

	lv_timer_delete(screen_timer);
	screen_timer = NULL;
	back_allowed = true;
	lv_obj_set_hidden(lbl_warn, true);
	lv_label_set_text(lbl_phase, s.state == UPDATE_DONE ? "Done" : "Failed");
	lv_obj_set_style_text_color(lbl_msg, s.state == UPDATE_DONE ? S.ok
																 : S.err, 0);
	lv_label_set_text(lbl_msg, s.message);
	if (s.state == UPDATE_DONE) {
		lv_bar_set_value(bar, 100, LV_ANIM_OFF);
		lv_group_focus_obj(button_primary(btn_row, "Reboot to system", reboot_system));
		if (update_is_auto) {
			reboot_left = AUTO_REBOOT_DELAY_S;
			lv_label_set_text_fmt(lbl_phase, "Done. Restarting in %d s", reboot_left);
			screen_timer = lv_timer_create(reboot_tick, 1000, NULL);
		}
	}
	if (s.state == UPDATE_FAILED && s.compat_overridable) {
		button(btn_row, "Install anyway", confirm_override, true);
	}
	lv_obj_t *b = button(btn_row, "Back", go_back, false);
	if (s.state != UPDATE_DONE) {
		lv_group_focus_obj(b);
	}
}

// The file picked in the browser; empty for <stem>.upt.
static char chosen_path[512];
// Set only by "Install anyway", for the next start_update().
static bool override_compat;

static void start_update(void) {
	bool ovr = override_compat;
	override_compat = false;
	if (update_start(chosen_path[0] ? chosen_path : NULL, ovr) != 0) {
		show_message("Error", "An update is already running.");
		return;
	}
	page("Installing update");
	back_allowed = false;
	lbl_warn = label(scr, "Do not turn the player off or remove the card.", F24,
		  S.warn);
	lbl_phase = label(scr, "Starting", F24, S.text);
	bar = progress_bar(scr);
	lbl_msg = label(scr, "", F24, S.text);
	btn_row = column(scr);
	screen_timer = lv_timer_create(update_poll, 100, NULL);
}

// "Install anyway": the same file again, the compatibility record ignored.
// Offered only when the record was the one thing wrong (update.c).
static void start_update_override(void) {
	override_compat = true;
	start_update();
}

static void confirm_override(void) {
	char msg[320];
	snprintf(msg, sizeof(msg),
			 "The package's compatibility record does not match the %s, but its system image is one. Install "
			 "it anyway? Everything else is still checked.",
			 g_model.name);
	confirm("Install anyway", msg, "Install anyway", start_update_override);
}

// Installing the Sonix bootloader (bootloader.h)

static void confirm_bootloader(void);

static void bl_poll(lv_timer_t *t) {
	(void)t;
	bl_status_t s;
	bootloader_get(&s);
	if (!bl_phase) {
		return;
	}
	lv_label_set_text(bl_phase, s.phase[0] ? s.phase : "Starting");
	if (s.state == BL_RUNNING) {
		return;
	}
	lv_timer_delete(screen_timer);
	screen_timer = NULL;
	lv_obj_set_hidden(lbl_warn, s.state != BL_FAILED_BAD);
	lv_label_set_text(bl_phase, s.state == BL_DONE ? "Done" : "Failed");
	lv_obj_set_style_text_color(bl_msg, s.state == BL_DONE ? S.ok
														   : S.err, 0);
	lv_label_set_text(bl_msg, s.message);
	if (s.state == BL_FAILED_BAD) {
		// The partition matches neither image: no Back, only another go.
		lv_group_focus_obj(button(bl_buttons, "Try again", confirm_bootloader, true));
		return;
	}
	back_allowed = true;
	lv_group_focus_obj(button(bl_buttons, "Back", go_back, false));
}

static void start_bootloader(void) {
	if (bootloader_install_start() != 0) {
		show_message("Error", "An install is already running.");
		return;
	}
	page("Installing bootloader");
	back_allowed = false;
	lbl_warn = label(scr, "Do not turn the player off.", F24, S.warn);
	bl_phase = label(scr, "Starting", F24, S.text);
	bl_msg = label(scr, "", F24, S.text);
	bl_buttons = column(scr);
	screen_timer = lv_timer_create(bl_poll, 200, NULL);
}

static void confirm_bootloader(void) {
	if (!battery_ok()) {
		return;
	}
	confirm("Install fastboot",
			"Replaces HiBy's bootloader with the Sonix one, which adds USB fastboot (hold Play/Pause at power-on). "
			"The player's partition table is kept and the old bootloader is saved on the card if there is one. "
			"If this write fails the player cannot start without Ingenic's USB boot tool.",
			"Install", start_bootloader);
}

// Backup of the partitions to the card (backup.h)

static void bk_poll(lv_timer_t *t) {
	(void)t;
	backup_status_t s;
	backup_get(&s);
	if (!lbl_phase) {
		return;
	}
	lv_label_set_text(lbl_phase, s.phase[0] ? s.phase : "Starting");
	lv_bar_set_value(bar, s.percent, LV_ANIM_OFF);
	if (s.state == BACKUP_RUNNING) {
		return;
	}
	lv_timer_delete(screen_timer);
	screen_timer = NULL;
	back_allowed = true;
	lv_obj_set_hidden(lbl_warn, true);
	lv_label_set_text(lbl_phase, s.state == BACKUP_DONE ? "Done" : "Failed");
	lv_color_t col = s.state != BACKUP_DONE ? S.err
					 : s.warnings			? S.warn
											: S.ok;
	lv_obj_set_style_text_color(lbl_msg, col, 0);
	lv_label_set_text(lbl_msg, s.message);
	lv_group_focus_obj(button(btn_row, "Back", go_back, false));
}

static void start_backup(bool with_userdata) {
	if (backup_start(with_userdata) != 0) {
		show_message("Error", "A backup is already running.");
		return;
	}
	page("Backing up");
	back_allowed = false;
	lbl_warn = label(scr, "Do not remove the card.", F24, S.warn);
	lbl_phase = label(scr, "Starting", F24, S.text);
	bar = progress_bar(scr);
	lbl_msg = label(scr, "", F24, S.text);
	btn_row = column(scr);
	screen_timer = lv_timer_create(bk_poll, 200, NULL);
}

static void backup_all(void) { start_backup(true); }
static void backup_no_userdata(void) { start_backup(false); }

static void show_backup(void) {
	page("Back up partitions");
	label(scr,
		  "Copies the flash partitions, raw, to a new sonix-backup folder on the card, with md5sums.txt to check "
		  "them on a PC. Nothing on the player is changed.",
		  F24, S.text);
	char a[64], b[64];
	snprintf(a, sizeof(a), "Everything (%lld MB)", backup_size(true) >> 20);
	snprintf(b, sizeof(b), "All but user data (%lld MB)", backup_size(false) >> 20);
	lv_obj_t *first = row(scr, a, backup_all, false);
	row(scr, b, backup_no_userdata, false);
	button(scr, "Cancel", go_back, false);
	lv_group_focus_obj(first);
}

// Restore a backup (restore.h)

#define MAX_BACKUPS 16
static backup_entry_t backups[MAX_BACKUPS];
static int n_backups, cur_backup;
static restore_part_t rparts[RESTORE_MAX_PARTS];
static int n_rparts;
static lv_obj_t *rchecks[RESTORE_MAX_PARTS];
static uint32_t restore_mask;
static void show_restore_list(void);
static void show_restore_parts(void);

static const char *part_title(const restore_part_t *p) {
	if (strcmp(p->name, "uboot") == 0) {
		return "Bootloader";
	}
	if (strcmp(p->name, "kernel") == 0) {
		return "Kernel";
	}
	if (strcmp(p->name, "rootfs") == 0) {
		return p->restorable || strstr(p->why_not, "recovery") == NULL ? "System" : "Recovery system";
	}
	if (strcmp(p->name, "kernel2") == 0) {
		return "Recovery kernel";
	}
	if (strcmp(p->name, "ota") == 0) {
		return "Boot record";
	}
	if (strcmp(p->name, "userdata") == 0) {
		return "User data";
	}
	return p->name;
}

static void rs_poll(lv_timer_t *t);

static void run_restore(void) {
	if (restore_start(backups[cur_backup].dir, restore_mask) != 0) {
		show_message("Error", "A restore is already running.");
		return;
	}
	page("Restoring");
	back_allowed = false;
	lbl_warn = label(scr, "Do not turn the player off or remove the card.", F24,
					 S.warn);
	lbl_phase = label(scr, "Starting", F24, S.text);
	bar = progress_bar(scr);
	lbl_msg = label(scr, "", F24, S.text);
	btn_row = column(scr);
	screen_timer = lv_timer_create(rs_poll, 200, NULL);
}

static void rs_poll(lv_timer_t *t) {
	(void)t;
	restore_status_t s;
	restore_get(&s);
	if (!lbl_phase) {
		return;
	}
	lv_label_set_text(lbl_phase, s.phase[0] ? s.phase : "Starting");
	lv_bar_set_value(bar, s.percent, LV_ANIM_OFF);
	if (s.state == RESTORE_RUNNING) {
		return;
	}
	lv_timer_delete(screen_timer);
	screen_timer = NULL;
	lv_obj_set_hidden(lbl_warn, s.state != RESTORE_FAILED_BL);
	lv_label_set_text(lbl_phase, s.state == RESTORE_DONE ? "Done" : "Failed");
	lv_obj_set_style_text_color(lbl_msg, s.state == RESTORE_DONE ? S.ok
																 : S.err, 0);
	lv_label_set_text(lbl_msg, s.message);
	if (s.state == RESTORE_FAILED_BL) {
		lv_group_focus_obj(button(btn_row, "Try again", run_restore, true));
		return;
	}
	back_allowed = true;
	if (s.wrote) {
		// what is on the flash now is not what this boot started from
		lv_group_focus_obj(button_primary(btn_row, "Reboot to system", reboot_system));
		button(btn_row, "Back", go_back, false);
	} else {
		lv_group_focus_obj(button(btn_row, "Back", go_back, false));
	}
}

static void confirm_restore(void) {
	restore_mask = 0;
	char list[256] = "";
	for (int i = 0; i < n_rparts; i++) {
		if (rchecks[i] && lv_obj_has_state(rchecks[i], LV_STATE_CHECKED)) {
			restore_mask |= 1u << rparts[i].num;
			size_t l = strlen(list);
			snprintf(list + l, sizeof(list) - l, "%s%s (mtd%d)", l ? ", " : "", part_title(&rparts[i]), rparts[i].num);
		}
	}
	if (!restore_mask) {
		show_message("Restore backup", "Nothing is ticked.");
		return;
	}
	if (!battery_ok()) {
		return;
	}
	char msg[480];
	snprintf(msg, sizeof(msg),
			 "Writes %s from %s over what the player has now. Everything is checked first; once writing starts it "
			 "cannot be undone.%s",
			 list, backups[cur_backup].name,
			 (restore_mask & 1u) ? " The bootloader too: if that write fails the player cannot start without "
								   "Ingenic's USB boot tool."
								 : "");
	confirm("Restore backup", msg, "Restore", run_restore);
}

static lv_obj_t *check(const char *text, bool on) {
	lv_obj_t *c = lv_checkbox_create(scr);
	lv_checkbox_set_text(c, text);
	lv_obj_set_width(c, LV_PCT(100));
	lv_obj_set_style_text_font(c, F24, 0);
	lv_obj_set_style_pad_ver(c, 8, 0);
	lv_obj_set_style_text_color(c, S.text, 0);
	lv_obj_set_style_bg_color(c, S.card, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(c, 20, 0);
	lv_obj_set_style_pad_ver(c, 18, 0);
	lv_obj_set_style_radius(c, CARD_RADIUS, 0);
	lv_obj_set_style_border_width(c, 0, 0);
	lv_obj_set_style_border_color(c, S.accent, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_border_width(c, 3, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_outline_width(c, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_bg_color(c, S.bg, LV_PART_INDICATOR);
	lv_obj_set_style_border_color(c, S.dim, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(c, S.accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_border_color(c, S.accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
	if (on) {
		lv_obj_add_state(c, LV_STATE_CHECKED);
	}
	lv_obj_add_event_cb(c, key_cb, LV_EVENT_KEY, NULL);
	lv_group_add_obj(group, c);
	return c;
}

static void show_restore_parts(void) {
	backup_entry_t *b = &backups[cur_backup];
	page("Restore backup");
	back_to = show_restore_list;
	label(scr, b->name, F24, S.accent);
	n_rparts = restore_scan(b->dir, rparts, RESTORE_MAX_PARTS);
	lv_obj_t *first = NULL;
	char text[128];
	for (int i = 0; i < n_rparts; i++) {
		rchecks[i] = NULL;
		if (rparts[i].size >= (1 << 20)) {
			snprintf(text, sizeof(text), "%s (mtd%d, %lld MB)", part_title(&rparts[i]), rparts[i].num,
					 rparts[i].size >> 20);
		} else {
			snprintf(text, sizeof(text), "%s (mtd%d, %lld KB)", part_title(&rparts[i]), rparts[i].num,
					 rparts[i].size >> 10);
		}
		if (rparts[i].restorable) {
			rchecks[i] = check(text, rparts[i].default_on);
			if (!first) {
				first = rchecks[i];
			}
		}
	}
	for (int i = 0; i < n_rparts; i++) {
		if (!rparts[i].restorable) {
			snprintf(text, sizeof(text), "%s (mtd%d): %s", part_title(&rparts[i]), rparts[i].num,
					 rparts[i].why_not);
			label(scr, text, F20, S.dim);
		}
	}
	lv_obj_t *go = button(scr, "Restore", confirm_restore, true);
	button(scr, "Cancel", show_restore_list, false);
	lv_group_focus_obj(first ? first : go);
}

static void pick_backup(void *idx_p) {
	int i = (int)(intptr_t)idx_p;
	if (i < 0 || i >= n_backups) {
		return;
	}
	cur_backup = i;
	if (!backups[i].same_model) {
		char msg[256];
		snprintf(msg, sizeof(msg), "%s is a backup of a %s, not of this %s.", backups[i].name,
				 backups[i].model[0] ? backups[i].model : "player of unknown model", g_model.name);
		show_message("Restore backup", msg);
		return;
	}
	show_restore_parts();
}

static void backup_click_cb(lv_event_t *e) { lv_async_call(pick_backup, lv_event_get_user_data(e)); }

static void show_restore_list(void) {
	page("Restore backup");
	back_to = show_restore_list;
	lv_obj_t *l = label(scr, "Reading the card...", F24, S.text);
	lv_refr_now(NULL);
	n_backups = restore_list_backups(backups, MAX_BACKUPS);
	back_to = show_advanced;
	if (n_backups <= 0) {
		show_message("Restore backup", n_backups < 0 ? "No microSD card could be read (FAT32 or exFAT)."
													 : "There is no sonix-backup folder in the top of the card. "
													   "Make one with \"Back up partitions\".");
		return;
	}
	lv_label_set_text(l, "Choose a backup (sonix-backup folders on the card):");
	lv_obj_t *first = NULL;
	for (int i = 0; i < n_backups; i++) {
		lv_obj_t *b = button(scr, "", NULL, false);
		lv_obj_remove_event_cb(b, click_cb);
		lv_obj_add_event_cb(b, backup_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_set_height(b, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_ver(b, 12, 0);
		if (!backups[i].same_model) {
			lv_obj_set_style_text_color(b, S.dim, 0);
		}
		lv_obj_t *t = lv_obj_get_child(b, 0);
		lv_obj_set_width(t, LV_PCT(100));
		lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
		lv_label_set_text_fmt(t, "%s\n%s, %d partitions, %lld MB", backups[i].name,
							  backups[i].model[0] ? backups[i].model : "unknown player", backups[i].n_files,
							  backups[i].bytes >> 20);
		if (!first) {
			first = b;
		}
	}
	button(scr, "Back", show_advanced, false);
	lv_group_focus_obj(first);
}

// Factory reset

static bool reset_keep_db;

static void do_reset_now(lv_timer_t *t) {
	(void)t;
	screen_timer = NULL; // one-shot: deleted by LVGL after this call
	int rc = device_factory_reset(reset_keep_db);
	show_message(rc == 0 ? "Factory reset" : "Error",
				 rc == 0 ? "The data partition was erased." : "recovery_all.sh failed; see the log.");
}

static void run_reset(void) {
	page("Factory reset");
	back_allowed = false;
	label(scr, "Erasing...", F24, S.text);
	// After this frame is drawn: the script blocks.
	screen_timer = lv_timer_create(do_reset_now, 200, NULL);
	lv_timer_set_repeat_count(screen_timer, 1);
}

static void reset_all(void) {
	reset_keep_db = false;
	run_reset();
}

static void reset_keep(void) {
	reset_keep_db = true;
	run_reset();
}

static void show_reset(void) {
	page("Factory reset");
	label(scr,
		  "Erases settings, Bluetooth pairings and everything else in the data partition. Music on the card is "
		  "not touched.",
		  F24, S.text);
	button(scr, "Erase everything", reset_all, true);
	button(scr, "Erase, keep library DB", reset_keep, true);
	lv_group_focus_obj(button(scr, "Cancel", go_back, false));
}

// ---------------------------------------------------------------------------
// Main screen

static void refresh_status(lv_timer_t *t) {
	(void)t;
	if (!lbl_boot) {
		return;
	}
	boot_target_t bt = bootflag_read();
	lv_label_set_text_fmt(lbl_boot, "Next boot: %s", boot_target_name(bt));
	battery_t b;
	battery_read(&b);
	char batt[48];
	if (b.capacity >= 0) {
		snprintf(batt, sizeof(batt), "%d%%%s", b.capacity, b.charging ? ", charging" : "");
	} else {
		snprintf(batt, sizeof(batt), "unknown");
	}
	lv_label_set_text_fmt(lbl_card, "Card: %s    Battery: %s", storage_card_present() ? "inserted" : "none", batt);
}

// False, with the reason on screen, when the battery is too low for an
// update.
static bool battery_ok(void) {
	battery_t b;
	battery_read(&b);
	if (b.capacity >= 0 && b.capacity < MIN_BATTERY_PCT && !b.charging) {
		char msg[128];
		snprintf(msg, sizeof(msg), "The battery is at %d%%. Charge it to %d%% or plug in USB first.", b.capacity,
				 MIN_BATTERY_PCT);
		show_message("Battery low", msg);
		return false;
	}
	return true;
}

static void install(void) {
	if (!battery_ok()) {
		return;
	}
	char msg[256];
	snprintf(msg, sizeof(msg),
			 "Installs %s.upt from the top folder of the microSD card over the current system. The package is "
			 "checked before anything is written.",
			 g_model.upt_stem);
	update_is_auto = false;
	chosen_path[0] = '\0';
	confirm("Install update", msg, "Install", start_update);
}

// ---------------------------------------------------------------------------
// File browser: the card one folder at a time, folders and .upt files only

#define MAX_UPT_FILES 64
static upt_entry_t files[MAX_UPT_FILES];
static int n_files;
static char browse_rel[256]; // the folder shown, "" for the top of the card

// Up one folder; from the top, out of the browser.
static void browser_up(void) {
	char *slash = strrchr(browse_rel, '/');
	if (slash) {
		*slash = '\0';
	} else if (browse_rel[0]) {
		browse_rel[0] = '\0';
	} else {
		show_main();
		return;
	}
	show_browser();
}

static void open_browser(void) {
	browse_rel[0] = '\0';
	show_browser();
}

static void pick_entry(void *idx_p) {
	int i = (int)(intptr_t)idx_p;
	if (i < 0 || i >= n_files) {
		return;
	}
	if (files[i].is_dir) {
		size_t l = strlen(browse_rel);
		if (l + 1 + strlen(files[i].name) >= sizeof(browse_rel)) {
			return;
		}
		snprintf(browse_rel + l, sizeof(browse_rel) - l, "%s%s", l ? "/" : "", files[i].name);
		show_browser();
		return;
	}
	if (!battery_ok()) {
		return;
	}
	snprintf(chosen_path, sizeof(chosen_path), "%s", files[i].path);
	update_is_auto = false;
	char msg[600];
	snprintf(msg, sizeof(msg),
			 "Installs /%s%s%s over the current system. It is checked first, including that it is for the %s; "
			 "nothing is written if it is not.",
			 browse_rel, browse_rel[0] ? "/" : "", files[i].name, g_model.name);
	confirm("Install update", msg, "Install", start_update);
}

static void entry_click_cb(lv_event_t *e) { lv_async_call(pick_entry, lv_event_get_user_data(e)); }

static lv_obj_t *entry_button(const char *icon, const char *text, const char *detail, int idx) {
	lv_obj_t *b = button(scr, "", NULL, false);
	lv_obj_remove_event_cb(b, click_cb);
	lv_obj_add_event_cb(b, entry_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
	lv_obj_set_height(b, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_ver(b, 12, 0);
	lv_obj_t *t = lv_obj_get_child(b, 0);
	lv_obj_set_width(t, LV_PCT(100));
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
	lv_label_set_text_fmt(t, "%s  %s%s%s", icon, text, detail[0] ? "\n" : "", detail);
	return b;
}

static void show_browser(void) {
	char title[300];
	snprintf(title, sizeof(title), "/%s", browse_rel);
	page("Choose a file");
	on_browser = true;
	back_to = show_browser; // a confirmation's Cancel comes back to this folder
	lv_obj_t *path = label(scr, title, F24, S.accent);
	lv_refr_now(NULL);
	n_files = update_list_dir(browse_rel, files, MAX_UPT_FILES);
	if (n_files < 0) {
		if (browse_rel[0]) { // the folder went away: back to the top
			browse_rel[0] = '\0';
			show_browser();
			return;
		}
		back_to = show_main;
		show_message("Choose a file", "No microSD card could be read (FAT32 or exFAT).");
		return;
	}
	lv_obj_t *first = NULL;
	if (browse_rel[0]) {
		lv_obj_t *b = button(scr, LV_SYMBOL_UP "  Up", browser_up, false);
		lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_LEFT, 0);
		first = b;
	}
	if (n_files == 0) {
		label(scr, "No folders or .upt files here.", F24, S.dim);
	}
	for (int i = 0; i < n_files; i++) {
		char size[32] = "";
		if (!files[i].is_dir) {
			snprintf(size, sizeof(size), "%.1f MB", (double)files[i].size / (1024.0 * 1024.0));
		}
		lv_obj_t *b = entry_button(files[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, files[i].name, size, i);
		if (i == 0) {
			first = b; // the first entry, also in a sub-folder (Up is just above)
		}
	}
	if (n_files == MAX_UPT_FILES) {
		label(scr, "(only the first ones are shown)", F20, S.dim);
	}
	lv_obj_t *back = button(scr, "Back", show_main, false);
	lv_group_focus_obj(first ? first : back);
	(void)path;
}

static void fastboot(void) {
	confirm("Fastboot", "Reboots into USB fastboot (Sonix bootloader).",
			"Reboot to fastboot", reboot_fastboot);
}

static void adb(void) {
	int rc = device_start_adb();
	if (lbl_note) {
		lv_label_set_text(lbl_note, rc == 0 ? "ADB started." : "ADB failed to start; see the log.");
		lv_obj_set_hidden(lbl_note, false);
	}
}

static void show_main(void) {
	page("Sonix Recovery");
	back_allowed = false;
	back_to = show_main;
	// Six rows as tall as the screen leaves room for: about 90 px on the R1's
	// 800 lines, 80 on the R3 Pro II's 720 (the player's rows are 100).
	int32_t vres = lv_display_get_vertical_resolution(lv_obj_get_display(scr));
	btn_height = (vres - 196) / 6 - 8;
	btn_height = btn_height > 100 ? 100 : btn_height < 64 ? 64 : btn_height;
	lv_obj_set_style_pad_row(scr, 8, 0);
	char head[96];
	snprintf(head, sizeof(head), "%s  %s", g_model.name, SONIX_RECOVERY_VERSION);
	label(scr, head, F20, S.dim);

	lv_obj_t *st = column(scr);
	lv_obj_set_style_pad_row(st, 2, 0);
	lbl_boot = label(st, "", F20, S.dim);
	lbl_card = label(st, "", F20, S.dim);

	char install_txt[64];
	snprintf(install_txt, sizeof(install_txt), "Install %s.upt", g_model.upt_stem);
	lv_obj_t *first = row(scr, install_txt, install, false);
	row(scr, "Choose file on card", open_browser, true);
	row(scr, "Reboot to system", reboot_system, false);
	row(scr, "Factory reset", show_reset, true);
	row(scr, "Advanced", show_advanced, true);
	row(scr, "Power off", poweroff, false);
	lv_group_focus_obj(first);

	refresh_status(NULL);
	screen_timer = lv_timer_create(refresh_status, 2000, NULL);
}

// The Advanced menu: what is for development or for taking the player apart,
// kept off the main menu.
static void show_advanced(void) {
	page("Advanced");
	on_advanced = true;
	back_to = show_advanced;
	// Results of the buttons that stay on this screen (Start ADB).
	lbl_note = label(scr, "", F20, S.warn);
	lv_obj_set_hidden(lbl_note, true);
	lv_obj_t *first = row(scr, "Back up partitions", show_backup, true);
	row(scr, "Restore backup", show_restore_list, true);
	// Fastboot needs the Sonix bootloader: with HiBy's the request is ignored.
	if (bootloader_is_sonix()) {
		row(scr, "Reboot to fastboot", fastboot, true);
	} else if (bootloader_can_install()) {
		row(scr, "Install fastboot", confirm_bootloader, true);
	}
	row(scr, "Start ADB", adb, false);
	button(scr, "Back", show_main, false);
	lv_group_focus_obj(first);
}

// ---------------------------------------------------------------------------
// Automatic update
//
// The record says ota:kernel2: the player asked for this boot to install its
// update (firmware.c's firmware_update_start(), or the stock player's), and
// the stock recovery would just go ahead. So does this one, after
// AUTO_UPDATE_DELAY_S seconds in which it can be stopped. Entered with
// Volume up the record still says ota:kernel, and the menu comes up instead.
//
// A failed update leaves the record on ota:kernel2, so the next start tries
// again -- what a half-written system needs. Cancelling leaves it there too;
// "Reboot to system" in the menu sets it back.

#define AUTO_UPDATE_DELAY_S 5

static int auto_left;
static lv_obj_t *lbl_count;

static void auto_install_now(void) {
	update_is_auto = true;
	if (battery_ok()) {
		start_update();
	}
}

static void auto_tick(lv_timer_t *t) {
	(void)t;
	if (--auto_left > 0) {
		lv_label_set_text_fmt(lbl_count, "Starting in %d s", auto_left);
		return;
	}
	lv_timer_delete(screen_timer);
	screen_timer = NULL;
	LOG("auto update: no one stopped it, installing");
	auto_install_now();
}

static void auto_cancel(void) {
	LOG("auto update: cancelled");
	show_main();
}

static void show_auto_update(void) {
	char msg[192];
	snprintf(msg, sizeof(msg), "The player restarted to install %s.upt from the microSD card.", g_model.upt_stem);
	page("Installing update");
	label(scr, msg, F24, S.text);
	auto_left = AUTO_UPDATE_DELAY_S;
	lbl_count = label(scr, "", F32, S.warn);
	lv_label_set_text_fmt(lbl_count, "Starting in %d s", auto_left);
	button_primary(scr, "Install now", auto_install_now);
	// Focus on Cancel, so any press of Play/Pause stops it (Power does too).
	lv_group_focus_obj(button(scr, "Cancel", auto_cancel, false));
	back_allowed = true; // Power = Cancel
	screen_timer = lv_timer_create(auto_tick, 1000, NULL);
	LOG("auto update: boot record is ota:kernel2, installing in %d s unless stopped", auto_left);
}

void ui_init(lv_display_t *disp, lv_indev_t *keys) {
	lv_theme_t *th = lv_theme_default_init(disp, S.accent, S.destructive, S.dark, F24);
	lv_display_set_theme(disp, th);
	scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, S.bg, 0);
	group = lv_group_create();
	lv_group_set_wrap(group, true);
	if (keys) {
		lv_indev_set_group(keys, group);
	}
	if (bootflag_read() == BOOT_RECOVERY) {
		show_auto_update();
	} else {
		show_main();
	}
}
