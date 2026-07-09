/*
 * This file is part of the uv_hal distribution (www.usevolt.fi).
 * Copyright (c) 2017 Usevolt Oy.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#include "ui/serverfiles_win.h"
#include "ui/uv_uidialog.h"
#include "ui/uv_uitreeview.h"
#include "ui/uv_uiacceptdialog.h"
#include "ui/uv_uifileedit.h"
#include "remotefiles.h"
#include "credentials.h"
#include <stdio.h>
#include <string.h>


#define SFW_MARGIN		10
#define SFW_TITLE_H		30
#define SFW_BTN_H		44
// height of one version row (metadata label + download button) inside a product
#define SFW_VROW_H		66
#define SFW_DL_W		130


// The dialog and its persistent widgets. Kept file-scope (static) so they outlive
// the modal's own step loop; the window is one-at-a-time so a single set suffices.
static uv_uidialog_st dialog;
static uv_uiobject_st *dialog_buf[6];
static uv_uilabel_st title_label;
static char title_str[160];
static uv_uilabel_st empty_label;
static uv_uibutton_st close_btn;

static uv_uitreeview_st tree;
static uv_uitreeobject_st *tree_buf[REMOTEFILES_MAX_PRODUCTS];
static uv_uitreeobject_st prod_objs[REMOTEFILES_MAX_PRODUCTS];
// each product (tree object) holds up to 2 widgets per version (a metadata label
// and a Download button), so its child-object array is sized accordingly
static uv_uiobject_st *prod_child_buf[REMOTEFILES_MAX_PRODUCTS]
		[2 * REMOTEFILES_MAX_VERSIONS + 2];

// per-version widgets, indexed [product][version]
static uv_uilabel_st ver_labels[REMOTEFILES_MAX_PRODUCTS][REMOTEFILES_MAX_VERSIONS];
static char ver_strs[REMOTEFILES_MAX_PRODUCTS][REMOTEFILES_MAX_VERSIONS][512];
static uv_uibutton_st dl_btns[REMOTEFILES_MAX_PRODUCTS][REMOTEFILES_MAX_VERSIONS];

static const uv_uistyle_st *win_style;

// All-files filter for the "save as" picker.
static const uv_uifileedit_filter_st SFW_ALL_FILES[] = { { "All files", "*" } };


// Shows a simple one-button information/error dialog.
static void sfw_message(const char *msg) {
	uv_uiacceptdialog_st ad = { };
	char buf[640];
	strncpy(buf, msg, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	uv_uiacceptdialog_exec(&ad, buf, "OK", "OK", win_style);
}


// Formats a human-readable size (e.g. "184 KB") into *out*.
static void sfw_fmt_size(uint64_t bytes, char *out, size_t out_len) {
	if (bytes >= 1024u * 1024u) {
		snprintf(out, out_len, "%.1f MB", (double) bytes / (1024.0 * 1024.0));
	}
	else if (bytes >= 1024u) {
		snprintf(out, out_len, "%u KB", (unsigned int) (bytes / 1024u));
	}
	else {
		snprintf(out, out_len, "%u B", (unsigned int) bytes);
	}
}


// Runs the "save as" picker and downloads version *j* of product *p* to the chosen
// location, showing the outcome.
static void sfw_download(uint8_t p, uint8_t j) {
	const remotefiles_product_st *prod = remotefiles_get_product(p);
	if ((prod == NULL) || (j >= prod->version_count)) {
		return;
	}
	const remotefiles_version_st *v = &prod->versions[j];

	// default the save name to the file's base name
	const char *base = strrchr(v->path, '/');
	base = (base != NULL) ? (base + 1) : v->path;
	char dest[1024];
	strncpy(dest, base, sizeof(dest) - 1);
	dest[sizeof(dest) - 1] = '\0';

	if (uv_uifiledialog_exec("Save file as", SFW_ALL_FILES, 1, true,
			dest, sizeof(dest))) {
		char err[256] = "";
		bool ok = remotefiles_download(v->path, dest, err, sizeof(err));
		char msg[640];
		if (ok) {
			snprintf(msg, sizeof(msg), "Downloaded '%s'.", base);
		}
		else {
			snprintf(msg, sizeof(msg), "%s", err);
		}
		sfw_message(msg);
	}
}


// Tree-object show callback: populates a product's content with one row per version
// (a metadata label plus a Download button). Called by the treeview when the
// product is opened.
static void product_show(uv_uitreeobject_st *obj) {
	int p = (int) (obj - prod_objs);
	if ((p < 0) || (p >= (int) remotefiles_get_product_count())) {
		return;
	}
	const remotefiles_product_st *prod = remotefiles_get_product((uint8_t) p);
	if (prod == NULL) {
		return;
	}

	uv_uitreeobject_clear(obj);
	int16_t w = uv_uibb(obj)->width;
	int16_t label_w = w - SFW_DL_W - 3 * SFW_MARGIN;

	for (uint8_t j = 0; j < prod->version_count; j++) {
		const remotefiles_version_st *v = &prod->versions[j];
		char sz[24];
		sfw_fmt_size(v->size, sz, sizeof(sz));
		// line 1: version, release date, size; line 2: notes
		snprintf(ver_strs[p][j], sizeof(ver_strs[p][j]), "v%s   %s   %s\n%s",
				(strlen(v->version) > 0) ? v->version : "?",
				(strlen(v->released) > 0) ? v->released : "-",
				sz, v->notes);

		int16_t y = CONFIG_UI_TREEVIEW_ITEM_HEIGHT + (int16_t) j * SFW_VROW_H;
		uv_uilabel_init(&ver_labels[p][j], win_style->font, ALIGN_CENTER_LEFT,
				win_style->text_color, ver_strs[p][j]);
		uv_uitreeobject_addxy(obj, &ver_labels[p][j],
				SFW_MARGIN, y, label_w, SFW_VROW_H);

		uv_uibutton_init(&dl_btns[p][j], "Download", win_style);
		uv_uitreeobject_addxy(obj, &dl_btns[p][j],
				w - SFW_DL_W - SFW_MARGIN, y + (SFW_VROW_H - SFW_BTN_H) / 2,
				SFW_DL_W, SFW_BTN_H);
	}
}


static uv_uiobject_ret_e sfw_step(void *user_ptr, uint16_t step_ms) {
	(void) user_ptr;
	(void) step_ms;
	uv_uiobject_ret_e ret = UIOBJECT_RETURN_ALIVE;

	if (uv_uibutton_clicked(&close_btn)) {
		ret = UIOBJECT_RETURN_KILLED;
	}
	else {
		// poll every version's Download button (un-opened products' buttons were
		// never added to a window, so they simply never report a click)
		uint8_t n = remotefiles_get_product_count();
		bool handled = false;
		for (uint8_t p = 0; (p < n) && !handled; p++) {
			const remotefiles_product_st *prod = remotefiles_get_product(p);
			if (prod == NULL) {
				continue;
			}
			for (uint8_t j = 0; j < prod->version_count; j++) {
				if (uv_uibutton_clicked(&dl_btns[p][j])) {
					sfw_download(p, j);
					handled = true;
					break;
				}
			}
		}
	}
	return ret;
}


void serverfiles_win_exec(const uv_uistyle_st *style) {
	win_style = style;

	// 1. log in and fetch the file list (blocks; failures are reported and abort)
	char err[256] = "";
	if (!remotefiles_login(credentials_get_url(), credentials_get_username(),
			credentials_get_password(), err, sizeof(err))) {
		sfw_message(err);
		return;
	}
	if (!remotefiles_list(err, sizeof(err))) {
		sfw_message(err);
		return;
	}

	// 2. build the modal window
	uv_uidialog_init(&dialog, dialog_buf, style);
	uv_uidialog_set_stepcallback(&dialog, &sfw_step, NULL);
	int16_t w = uv_uibb(&dialog)->width;
	int16_t h = uv_uibb(&dialog)->height;

	snprintf(title_str, sizeof(title_str), "Server files for '%s'",
			credentials_get_username());
	uv_uilabel_init(&title_label, &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
			C(0xFFFFFFFF), title_str);
	uv_uidialog_addxy(&dialog, &title_label,
			SFW_MARGIN, SFW_MARGIN, w - 2 * SFW_MARGIN, SFW_TITLE_H);

	int16_t tree_y = SFW_MARGIN + SFW_TITLE_H + SFW_MARGIN;
	int16_t tree_h = h - tree_y - (SFW_BTN_H + 2 * SFW_MARGIN);

	uint8_t n = remotefiles_get_product_count();
	if (n == 0) {
		uv_uilabel_init(&empty_label, style->font, ALIGN_CENTER,
				C(0xFFFFFFFF), "No files are available for this account.");
		uv_uidialog_addxy(&dialog, &empty_label,
				SFW_MARGIN, tree_y, w - 2 * SFW_MARGIN, tree_h);
	}
	else {
		uv_uitreeview_init(&tree, tree_buf, style);
		uv_uidialog_addxy(&dialog, &tree,
				SFW_MARGIN, tree_y, w - 2 * SFW_MARGIN, tree_h);
		for (uint8_t p = 0; p < n; p++) {
			const remotefiles_product_st *prod = remotefiles_get_product(p);
			uv_uitreeobject_init(&prod_objs[p], prod_child_buf[p],
					prod->name, &product_show, style);
			// content height = one row per version; start collapsed
			uv_uitreeview_add(&tree, &prod_objs[p],
					(int16_t) prod->version_count * SFW_VROW_H, false);
		}
	}

	uv_uibutton_init(&close_btn, "Close", style);
	uv_uidialog_addxy(&dialog, &close_btn,
			w - SFW_DL_W - SFW_MARGIN, h - SFW_BTN_H - SFW_MARGIN,
			SFW_DL_W, SFW_BTN_H);

	uv_uidialog_exec(&dialog);
}
