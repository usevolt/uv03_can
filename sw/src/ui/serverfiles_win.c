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

// Length of one version row's label text, and of a product's row title.
#define SFW_VSTR_LEN	512
#define SFW_NAME_LEN	176

static uv_uitreeview_st tree;

// The per-product UI, allocated for exactly as many products as the listing
// holds rather than a fixed maximum, so no product goes unshown.
//
// Every block here is owned by this file and released by sfw_free_ui(). The
// widgets are handed to the dialog by pointer, so nothing may be freed until
// uv_uidialog_exec() has returned - see the ordering note in
// serverfiles_win_exec().
typedef struct {
	// the treeview row itself; its address identifies the product (sfw_index_of)
	uv_uitreeobject_st obj;
	// child-object array the tree object keeps: 2 widgets per version (label +
	// Download button) plus slack, as uv_uiwindow requires the array to outlive
	// the window
	uv_uiobject_st **child_buf;
	uv_uilabel_st *ver_labels;
	uv_uibutton_st *dl_btns;
	// version_count * SFW_VSTR_LEN, one row's text per version. Contiguous
	// rather than a string per row: one allocation instead of dozens, and the
	// labels keep pointers into it for as long as they live.
	char *ver_strs;
	// The row title. Held here rather than pointing at the product's own name,
	// because the title adds the file count and because uv_uitreeobject_init()
	// keeps the name BY POINTER - it has to stay put for the dialog's lifetime.
	char name[SFW_NAME_LEN];
	uint16_t versions;
} sfw_product_ui_st;

static sfw_product_ui_st *prods;
// the pointer array uv_uitreeview_init() keeps; one entry per product
static uv_uitreeobject_st **tree_buf;
static uint16_t prod_n;

static const uv_uistyle_st *win_style;

// All-files filter for the "save as" picker.
static const uv_uifileedit_filter_st SFW_ALL_FILES[] = { { "All files", "*" } };


// Releases the whole per-product UI. Idempotent, and safe to call when nothing
// was ever allocated.
//
// Must run only once the dialog is gone: every widget below was handed to the
// dialog (or to a tree object) by pointer, so freeing while it is on screen
// would leave the UI walking freed memory.
static void sfw_free_ui(void) {
	if (prods != NULL) {
		for (uint16_t p = 0; p < prod_n; p++) {
			free(prods[p].child_buf);
			free(prods[p].ver_labels);
			free(prods[p].dl_btns);
			free(prods[p].ver_strs);
		}
		free(prods);
		prods = NULL;
	}
	free(tree_buf);
	tree_buf = NULL;
	prod_n = 0;
}


// Allocates the UI for *n* products, sizing each product's arrays to its own
// version count. Returns false (having freed whatever it had taken) when any
// allocation fails, so there is no half-built UI to render.
static bool sfw_alloc_ui(uint16_t n) {
	sfw_free_ui();
	if (n == 0) {
		return true;
	}
	prods = calloc(n, sizeof(*prods));
	tree_buf = calloc(n, sizeof(*tree_buf));
	if ((prods == NULL) || (tree_buf == NULL)) {
		sfw_free_ui();
		return false;
	}
	// prod_n is raised as each product succeeds, so a failure part-way leaves
	// sfw_free_ui() with an accurate count of what to release.
	for (uint16_t p = 0; p < n; p++) {
		const remotefiles_product_st *prod = remotefiles_get_product(p);
		uint16_t v = (prod != NULL) ? prod->version_count : 0;
		prods[p].versions = v;
		prod_n = (uint16_t) (p + 1);
		// +2 of slack matches what the fixed array carried; uv_uiwindow wants
		// room for the children a show callback adds.
		prods[p].child_buf = calloc((size_t) (2 * v) + 2,
				sizeof(*prods[p].child_buf));
		if (prods[p].child_buf == NULL) {
			sfw_free_ui();
			return false;
		}
		if (v > 0) {
			prods[p].ver_labels = calloc(v, sizeof(*prods[p].ver_labels));
			prods[p].dl_btns = calloc(v, sizeof(*prods[p].dl_btns));
			prods[p].ver_strs = calloc((size_t) v, SFW_VSTR_LEN);
			if ((prods[p].ver_labels == NULL) || (prods[p].dl_btns == NULL) ||
					(prods[p].ver_strs == NULL)) {
				sfw_free_ui();
				return false;
			}
		}
	}
	return true;
}


// Which product a tree object belongs to, or -1. A search rather than pointer
// arithmetic: it makes no assumption about the struct layout, and the counts
// here are small.
static int sfw_index_of(const uv_uitreeobject_st *obj) {
	int ret = -1;
	for (uint16_t p = 0; (p < prod_n) && (ret < 0); p++) {
		if (&prods[p].obj == obj) {
			ret = (int) p;
		}
	}
	return ret;
}


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
static void sfw_download(uint16_t p, uint16_t j) {
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
	int pi = sfw_index_of(obj);
	if (pi < 0) {
		return;
	}
	uint16_t p = (uint16_t) pi;
	const remotefiles_product_st *prod = remotefiles_get_product(p);
	if (prod == NULL) {
		return;
	}

	// The rows are rebuilt on every open. uv_uitreeobject_clear() is safe to
	// call now that it no longer replaces the tree object's draw callback with
	// the plain window one (uv_hal 18bfab8); before that fix, clearing here cost
	// the object its +/- marker, its name and its separator line the instant a
	// product was opened.
	uv_uitreeobject_clear(obj);
	int16_t w = uv_uibb(obj)->width;
	int16_t label_w = w - SFW_DL_W - 3 * SFW_MARGIN;

	for (uint16_t j = 0; j < prods[p].versions; j++) {
		const remotefiles_version_st *v = &prod->versions[j];
		char sz[24];
		sfw_fmt_size(v->size, sz, sizeof(sz));
		char *str = &prods[p].ver_strs[(size_t) j * SFW_VSTR_LEN];
		// line 1: file name, release date, size; line 2: notes.
		// No "v" prefix: a version here is the package's file name, not a
		// number, so it read "vuv0d_jhc_uv0d1_1028-g5346.uvdev".
		snprintf(str, SFW_VSTR_LEN, "%s   %s   %s\n%s",
				(strlen(v->version) > 0) ? v->version : "?",
				(strlen(v->released) > 0) ? v->released : "-",
				sz, v->notes);

		// NOT offset by CONFIG_UI_TREEVIEW_ITEM_HEIGHT. uv_uitreeobject_init()
		// already calls uv_uiwindow_set_content_bb_default_pos(0, ITEM_HEIGHT),
		// so this coordinate space starts below the header row; adding it again
		// pushed every row down by a header's height and ran the last row past
		// the object's own height, which clipped it to a sliver.
		int16_t y = (int16_t) j * SFW_VROW_H;
		uv_uilabel_init(&prods[p].ver_labels[j], win_style->font,
				ALIGN_CENTER_LEFT, win_style->text_color, str);
		uv_uitreeobject_addxy(obj, &prods[p].ver_labels[j],
				SFW_MARGIN, y, label_w, SFW_VROW_H);

		uv_uibutton_init(&prods[p].dl_btns[j], "Download", win_style);
		uv_uitreeobject_addxy(obj, &prods[p].dl_btns[j],
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
		bool handled = false;
		for (uint16_t p = 0; (p < prod_n) && !handled; p++) {
			for (uint16_t j = 0; j < prods[p].versions; j++) {
				if (uv_uibutton_clicked(&prods[p].dl_btns[j])) {
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

	uint16_t n = remotefiles_get_product_count();
	if (!sfw_alloc_ui(n)) {
		sfw_message("Not enough memory to show the file list.");
		return;
	}
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
		for (uint16_t p = 0; p < n; p++) {
			const remotefiles_product_st *prod = remotefiles_get_product(p);
			// The count is in the row itself: a product with no files then reads
			// as "(no files)" rather than as a row that refuses to open, which
			// is indistinguishable from something being broken.
			if (prods[p].versions > 0) {
				snprintf(prods[p].name, SFW_NAME_LEN, "%s  (%u)",
						prod->name, (unsigned int) prods[p].versions);
			}
			else {
				snprintf(prods[p].name, SFW_NAME_LEN, "%s  (no files)",
						prod->name);
			}
			uv_uitreeobject_init(&prods[p].obj, prods[p].child_buf,
					prods[p].name, &product_show, style);
			// Content height is one row per version. The first product opens
			// expanded, so the panel shows actual files without a click - the
			// treeview keeps one product open at a time anyway.
			uv_uitreeview_add(&tree, &prods[p].obj,
					(int16_t) prods[p].versions * SFW_VROW_H, (p == 0));
		}
	}

	uv_uibutton_init(&close_btn, "Close", style);
	uv_uidialog_addxy(&dialog, &close_btn,
			w - SFW_DL_W - SFW_MARGIN, h - SFW_BTN_H - SFW_MARGIN,
			SFW_DL_W, SFW_BTN_H);

	uv_uidialog_exec(&dialog);

	// Only now: uv_uidialog_exec() runs the modal's own loop and returns when
	// the dialog is closed, so up to this point the dialog still holds pointers
	// to everything allocated above.
	sfw_free_ui();
}
