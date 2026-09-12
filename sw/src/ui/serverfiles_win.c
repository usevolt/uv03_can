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
#include "ui/uv_uitabwindow.h"
#include "ui/uv_uiacceptdialog.h"
#include "remotefiles.h"
#include "credentials.h"
#include "system.h"
#include <stdio.h>
#include <string.h>


#define SFW_MARGIN		10
#define SFW_TITLE_H		30
#define SFW_BTN_H		44
// Height of one version row (metadata label + Download button) inside a
// product, and of the button on it. One line of text and a button, with just
// enough around them to separate the rows: the rows used to be tall enough for
// a second line of release notes that the server never supplies, which left a
// blank line's worth of air under every one of them and pushed the name off the
// centre of its row, where the tree's guide line points.
#define SFW_VROW_H		46
#define SFW_ROW_BTN_H	36
#define SFW_DL_W		130
// The tree rows read better a size up from the style's own font, and the whole
// panel is a list of names to pick from.
#define SFW_FONT		(&font20)
// One wheel notch scrolls the product list by this much
#define SFW_SCROLL_STEP	(CONFIG_UI_TREEVIEW_ITEM_HEIGHT)
// Where a downloaded package is put: a temporary directory of this run's own,
// created on the first download. A package is downloaded to be used now - it
// becomes the device's configuration file, and is read from there for as long as
// uvcan runs - so it is tracked with the rest of this run's temporary
// directories and removed when uvcan exits, on Ctrl-C as well, and swept out of
// /tmp by a later run if this one is killed outright. Keeping the downloads
// instead filled the user's config directory with a copy of every version they
// ever looked at, none of which they had asked to keep.
#define SFW_PKG_DIR_PREFIX	"uvcan_pkg"
static char sfw_pkg_dir[1024];


// The dialog and its persistent widgets. Kept file-scope (static) so they outlive
// the modal's own step loop; the window is one-at-a-time so a single set suffices.
static uv_uidialog_st dialog;
static uv_uiobject_st *dialog_buf[6];
static uv_uilabel_st title_label;
static char title_str[160];
static uv_uilabel_st empty_label;
static uv_uibutton_st close_btn;

// Length of one version row's label text, of a row title, and of a directory
// path relative to its fleet.
#define SFW_VSTR_LEN	512
#define SFW_NAME_LEN	176
#define SFW_PATH_LEN	256

// One tab per fleet, holding the tree of that fleet's products. An account may
// hold several fleets and they are separate collections of machines; showing
// them in one list only made the user read the fleet off every row.
static uv_uitabwindow_st fleet_tabs;
// the tab window holds exactly one child: the tree of the active fleet, or the
// label saying that fleet has no files
static uv_uiobject_st *fleet_tabs_buf[2];
static char *fleet_names[REMOTEFILES_MAX_FLEETS];
static uint8_t fleet_count;
static uv_uilabel_st fleet_empty_label;

// The tree of the fleet currently on show. One tree serves every tab: switching
// tabs rebuilds it from the products of the fleet that was picked.
static uv_uitreeview_st tree;
// Whether the tree is what the active tab is showing. A fleet with no files
// shows a label instead, and the tree is then left holding the previous fleet's
// rows -- and its parent pointer, so it still reports a position on screen.
// Scrolling it there would swallow the wheel for a window nobody can see.
static bool tree_shown;

// One node of the directory tree: a directory on the server, shown as one row
// of the tree with its subdirectories cascading under it.
//
// A product from the listing is one directory's worth of files, named by its
// path relative to the fleet ("uv0d/rev2"). Hanging those on the tree as they
// come gives one flat row per directory with the path written out on it, which
// says nothing about what sits inside what. So every segment of every path
// becomes a node here, whether the listing held a product for it or not: a
// directory that holds nothing but subdirectories has no files of its own to
// show, but it is still the thing they are inside.
//
// Every block here is owned by this file and released by sfw_free_ui(). The
// widgets are handed to the dialog by pointer, so nothing may be freed until
// uv_uidialog_exec() has returned - see the ordering note in
// serverfiles_win_exec().
typedef struct {
	// the treeview row itself; its address identifies the node (sfw_index_of)
	uv_uitreeobject_st obj;
	// child-object array the tree object keeps: this node's subdirectory nodes
	// plus 2 widgets per version (label + Download button), as uv_uiwindow
	// requires the array to outlive the window
	uv_uiobject_st **child_buf;
	uv_uilabel_st *ver_labels;
	uv_uibutton_st *dl_btns;
	// version_count * SFW_VSTR_LEN, one row's text per version. Contiguous
	// rather than a string per row: one allocation instead of dozens, and the
	// labels keep pointers into it for as long as they live.
	char *ver_strs;
	// The row title: the directory's own name, i.e. the last segment of its
	// path, and the file count. Held here rather than pointing at the product's
	// name, because uv_uitreeobject_init() keeps the name BY POINTER - it has to
	// stay put for the dialog's lifetime.
	char name[SFW_NAME_LEN];
	// this directory's path relative to its fleet; "" is the fleet's own folder
	char path[SFW_PATH_LEN];
	// the node this one sits inside, or -1 at the top of its fleet's tab
	int parent;
	// which product of the listing holds this directory's files, or -1 for a
	// directory that holds nothing but other directories
	int product;
	uint16_t versions;
	// how many nodes name this one as their parent
	uint16_t children;
	// which fleet's tab this node belongs on
	uint8_t fleet;
} sfw_node_st;

static sfw_node_st *nodes;
// the pointer array uv_uitreeview_init() keeps. One entry per node: a single
// fleet can hold all of them, and the tree only ever shows one fleet's worth.
static uv_uitreeobject_st **tree_buf;
static uint16_t node_n;
// how many the arrays were allocated for; see sfw_node_bound()
static uint16_t node_cap;

// What the user clicked "Download" on, as product / version index, or -1 when
// the window was closed without downloading anything. The click closes the
// window and the transfer runs after it is gone, so the choice has to outlive
// the dialog's step loop.
static int sel_prod;
static int sel_ver;

static const uv_uistyle_st *win_style;


// Releases the whole tree UI. Idempotent, and safe to call when nothing was
// ever allocated.
//
// Must run only once the dialog is gone: every widget below was handed to the
// dialog (or to a tree object) by pointer, so freeing while it is on screen
// would leave the UI walking freed memory.
static void sfw_free_ui(void) {
	if (nodes != NULL) {
		for (uint16_t i = 0; i < node_n; i++) {
			free(nodes[i].child_buf);
			free(nodes[i].ver_labels);
			free(nodes[i].dl_btns);
			free(nodes[i].ver_strs);
		}
		free(nodes);
		nodes = NULL;
	}
	free(tree_buf);
	tree_buf = NULL;
	node_n = 0;
	node_cap = 0;
}


// A product's path relative to its fleet, "" for the fleet's own folder.
//
// Taken from the id rather than from the name, because the id is the one that
// is unambiguous: it is "<fleet>" for the fleet's own folder and
// "<fleet>/<path>" for everything else, while the name of the fleet's own
// folder is the fleet itself and would read as a directory of that name.
static void sfw_product_path(const remotefiles_product_st *prod,
		char *out, size_t out_len) {
	const char *fleet = remotefiles_get_fleet(prod->fleet);
	size_t fl = (fleet != NULL) ? strlen(fleet) : 0;
	if ((fleet != NULL) && (strncmp(prod->id, fleet, fl) == 0) &&
			(prod->id[fl] == '/')) {
		snprintf(out, out_len, "%s", &prod->id[fl + 1]);
	}
	else {
		// the fleet's own folder: no path under it
		out[0] = '\0';
	}
}


// The node for (*fleet*, *path*), created along with every one of its ancestors
// if it is not there yet. Returns -1 when the array is full, which cannot
// happen for a bound that counted the segments (sfw_node_bound).
//
// Ancestors first, so that a directory always sits after the one that holds it
// and the parent index of a node is always lower than its own.
static int sfw_node_for(uint8_t fleet, const char *path) {
	int ret = -1;
	for (uint16_t i = 0; (i < node_n) && (ret < 0); i++) {
		if ((nodes[i].fleet == fleet) && (strcmp(nodes[i].path, path) == 0)) {
			ret = (int) i;
		}
		else {
		}
	}
	if (ret < 0) {
		// the path of the directory holding this one, i.e. everything before the
		// last separator. "" for a directory at the top of the fleet.
		char up[SFW_PATH_LEN];
		snprintf(up, sizeof(up), "%s", path);
		char *sep = strrchr(up, '/');
		int parent = -1;
		if (sep != NULL) {
			*sep = '\0';
			parent = sfw_node_for(fleet, up);
		}
		else {
			// at the top of the fleet already; the fleet's own folder ("") lands
			// here too and is a top-level node like any other
		}
		// checked here rather than on the way in, because the ancestors created
		// above have taken their own slots. Cannot happen for a bound that
		// counted the path segments, and is not worth running off the end of the
		// array to find out.
		if (node_n >= node_cap) {
			ret = -1;
		}
		else {
			sfw_node_st *n = &nodes[node_n];
			memset(n, 0, sizeof(*n));
			n->fleet = fleet;
			n->parent = parent;
			n->product = -1;
			snprintf(n->path, sizeof(n->path), "%s", path);
			if (parent >= 0) {
				nodes[parent].children++;
			}
			else {
			}
			ret = (int) node_n;
			node_n++;
		}
	}
	else {
	}
	return ret;
}


// An upper bound on how many nodes the listing can come to: every product is
// one directory plus one for each of its ancestors, and ancestors shared
// between products are counted more than once. Bounding it rather than
// counting exactly keeps this to one pass; the slack is a handful of structs.
static uint16_t sfw_node_bound(void) {
	uint16_t ret = 0;
	uint16_t n = remotefiles_get_product_count();
	for (uint16_t p = 0; p < n; p++) {
		const remotefiles_product_st *prod = remotefiles_get_product(p);
		ret++;
		if (prod != NULL) {
			char path[SFW_PATH_LEN];
			sfw_product_path(prod, path, sizeof(path));
			for (const char *c = path; *c != '\0'; c++) {
				if (*c == '/') {
					ret++;
				}
				else {
				}
			}
		}
		else {
		}
	}
	return ret;
}


// Builds the directory tree out of the listing and allocates each node's
// widgets. Returns false (having freed whatever it had taken) when any
// allocation fails, so there is no half-built UI to render.
//
// The listing arrives newest first, and the nodes are created in the order the
// products are walked, so a directory takes the place of the newest thing
// inside it and the tree reads newest first at every level.
static bool sfw_alloc_ui(void) {
	sfw_free_ui();
	uint16_t bound = sfw_node_bound();
	if (bound == 0) {
		return true;
	}
	nodes = calloc(bound, sizeof(*nodes));
	tree_buf = calloc(bound, sizeof(*tree_buf));
	if ((nodes == NULL) || (tree_buf == NULL)) {
		sfw_free_ui();
		return false;
	}
	node_cap = bound;

	// 1. a node for every directory, and for every directory above it
	uint16_t prod_count = remotefiles_get_product_count();
	for (uint16_t p = 0; p < prod_count; p++) {
		const remotefiles_product_st *prod = remotefiles_get_product(p);
		if (prod == NULL) {
			continue;
		}
		char path[SFW_PATH_LEN];
		sfw_product_path(prod, path, sizeof(path));
		int i = sfw_node_for(prod->fleet, path);
		if (i < 0) {
			continue;
		}
		nodes[i].product = (int) p;
		nodes[i].versions = prod->version_count;
	}

	// 2. the row title and the widgets, now that every node's child count is
	// known. The child array holds this node's subdirectories and two widgets
	// per file, plus the slack uv_uiwindow wants for anything added later.
	for (uint16_t i = 0; i < node_n; i++) {
		sfw_node_st *n = &nodes[i];
		const char *own = strrchr(n->path, '/');
		own = (own != NULL) ? (own + 1) : n->path;
		if (own[0] == '\0') {
			// the fleet's own folder is the fleet
			own = remotefiles_get_fleet(n->fleet);
		}
		else {
		}
		// The file count is in the row itself: a directory with no files of its
		// own then reads as what it is rather than as a row that refuses to
		// open, which is indistinguishable from something being broken.
		// the name is cut to leave room for what follows it; a directory name
		// that long is unreadable on a row either way
		if (n->versions > 0) {
			snprintf(n->name, SFW_NAME_LEN, "%.150s  (%u)",
					(own != NULL) ? own : "?", (unsigned int) n->versions);
		}
		else if (n->children > 0) {
			snprintf(n->name, SFW_NAME_LEN, "%.150s", (own != NULL) ? own : "?");
		}
		else {
			snprintf(n->name, SFW_NAME_LEN, "%.150s  (no files)",
					(own != NULL) ? own : "?");
		}

		n->child_buf = calloc((size_t) (2 * n->versions) + n->children + 2,
				sizeof(*n->child_buf));
		if (n->child_buf == NULL) {
			sfw_free_ui();
			return false;
		}
		if (n->versions > 0) {
			n->ver_labels = calloc(n->versions, sizeof(*n->ver_labels));
			n->dl_btns = calloc(n->versions, sizeof(*n->dl_btns));
			n->ver_strs = calloc((size_t) n->versions, SFW_VSTR_LEN);
			if ((n->ver_labels == NULL) || (n->dl_btns == NULL) ||
					(n->ver_strs == NULL)) {
				sfw_free_ui();
				return false;
			}
		}
		else {
		}
	}
	return true;
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


// The directory this run downloads into, created on the first call. Returns NULL
// if it cannot be created.
static const char *sfw_pkg_dir_get(void) {
	if (strlen(sfw_pkg_dir) == 0) {
		if (!system_mktempdir(SFW_PKG_DIR_PREFIX,
				sfw_pkg_dir, sizeof(sfw_pkg_dir))) {
			sfw_pkg_dir[0] = '\0';
		}
		else {
		}
	}
	else {
	}
	return (strlen(sfw_pkg_dir) != 0) ? sfw_pkg_dir : NULL;
}


// Starts the download of version *j* of product *p* into this run's download
// directory. Returns true when the transfer was started; the caller waits for it
// with remotefiles_download_is_finished().
//
// Started once the window is closed, and on a task of its own: the transfer takes
// as long as it takes and reports how far it has got on stdout, which the UI's
// log view shows only while the UI keeps running.
static bool sfw_start_fetch(uint16_t p, uint16_t j) {
	bool ret = false;
	const remotefiles_product_st *prod = remotefiles_get_product(p);
	if ((prod != NULL) && (j < prod->version_count)) {
		const remotefiles_version_st *v = &prod->versions[j];
		// the file keeps the name it has on the server; that name carries the
		// product and the version, so two downloads only collide when they are
		// the same package
		const char *base = strrchr(v->path, '/');
		base = (base != NULL) ? (base + 1) : v->path;
		const char *dir = sfw_pkg_dir_get();
		if (dir == NULL) {
			sfw_message("There is nowhere to download to: a temporary directory "
					"could not be created.");
		}
		else {
			// room for the download directory and the longest name a version's
			// path (remotefiles_version_st.path) can end with
			char path[sizeof(sfw_pkg_dir) + 520];
			snprintf(path, sizeof(path), "%s/%s", dir, base);
			remotefiles_download_async(v->path, path, v->size);
			ret = true;
		}
	}
	return ret;
}


// Fills a node's content with one row per file it holds (a metadata label plus
// a Download button).
//
// Called once, while the tree is built, rather than from a show callback when
// the node is opened. A node holds its subdirectories as well as its rows, and
// rebuilding the rows on every open would mean clearing the node - taking the
// subdirectories with them.
static void sfw_node_add_rows(uint16_t i) {
	sfw_node_st *n = &nodes[i];
	if (n->product < 0) {
		return;
	}
	uv_uitreeobject_st *obj = &n->obj;
	const remotefiles_product_st *prod =
			remotefiles_get_product((uint16_t) n->product);
	if (prod == NULL) {
		return;
	}

	// The content's own coordinate space: it starts below the header row and
	// indented under the header's name, and is that much narrower than the
	// object itself. Taking the object's width instead would run every row
	// past the right edge by the indent.
	int16_t w = uv_uitreeobject_get_content_bb(obj).width;
	int16_t label_w = w - SFW_DL_W - SFW_MARGIN;

	for (uint16_t j = 0; j < n->versions; j++) {
		const remotefiles_version_st *v = &prod->versions[j];
		char sz[24];
		sfw_fmt_size(v->size, sz, sizeof(sz));
		char *str = &n->ver_strs[(size_t) j * SFW_VSTR_LEN];
		// File name, release date, size, and the release notes after them when
		// there are any. All on one line: the server reports only what a file
		// system knows, so the notes are almost always empty, and a second line
		// kept for them left every row with a blank line in it -- which also lifted
		// the name off the row's centre, because a label centres the whole text
		// block it is given, empty last line included.
		// No "v" prefix: a version here is the package's file name, not a
		// number, so it read "vuv0d_jhc_uv0d1_1028-g5346.uvdev".
		snprintf(str, SFW_VSTR_LEN, "%s   %s   %s%s%s",
				(strlen(v->version) > 0) ? v->version : "?",
				(strlen(v->released) > 0) ? v->released : "-",
				sz,
				(strlen(v->notes) > 0) ? "   " : "", v->notes);

		// NOT offset by CONFIG_UI_TREEVIEW_ITEM_HEIGHT. uv_uitreeobject_init()
		// already calls uv_uiwindow_set_content_bb_default_pos(), so this
		// coordinate space starts below the header row; adding it again
		// pushed every row down by a header's height and ran the last row past
		// the object's own height, which clipped it to a sliver.
		int16_t y = (int16_t) j * SFW_VROW_H;
		uv_uilabel_init(&n->ver_labels[j], SFW_FONT,
				ALIGN_CENTER_LEFT, win_style->text_color, str);
		uv_uitreeobject_addxy(obj, &n->ver_labels[j],
				0, y, label_w, SFW_VROW_H);

		uv_uibutton_init(&n->dl_btns[j], "Download", win_style);
		uv_uitreeobject_addxy(obj, &n->dl_btns[j],
				w - SFW_DL_W, y + (SFW_VROW_H - SFW_ROW_BTN_H) / 2,
				SFW_DL_W, SFW_ROW_BTN_H);
	}
}


// Adds node *i* to *container* -- the tree view for a directory at the top of
// the fleet, the parent node for one nested inside another -- then its own
// file rows and then, recursively, the directories inside it.
//
// The rows go in before the subdirectories: the layout stacks a container's
// child nodes below whatever plain widgets it holds, so this is what puts a
// directory's own files above the directories inside it. The recursion runs in
// node order, which is the order the listing was walked in, so each level keeps
// the newest-first order the listing arrived in.
static void sfw_build_node(void *container, uint16_t i) {
	sfw_node_st *n = &nodes[i];
	uv_uitreeobject_init(&n->obj, n->child_buf, n->name, NULL, win_style);
	uv_uitreeobject_set_font(&n->obj, SFW_FONT);
	// Every node starts closed: a tab opens on the list of what the fleet
	// holds, which is what the user picks from, rather than on one directory's
	// files with the rest of the list pushed down the screen.
	uv_uitreeview_add(container, &n->obj,
			(int16_t) n->versions * SFW_VROW_H, false);
	sfw_node_add_rows(i);
	for (uint16_t c = 0; c < node_n; c++) {
		if (nodes[c].parent == (int) i) {
			sfw_build_node(&n->obj, c);
		}
		else {
		}
	}
}


// Fills the fleet tab window with the products of fleet *f*: a tree with one row
// per product, or a label when that fleet holds no files at all.
//
// Called every time a tab is picked. The tree objects are re-initialised rather
// than kept, because a tree object belongs to the tree it was added to - the
// previous fleet's tree is exactly what this replaces.
static void sfw_show_fleet(uint8_t f) {
	uv_uitabwindow_clear(&fleet_tabs);
	uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(&fleet_tabs);

	uint16_t count = 0;
	for (uint16_t i = 0; i < node_n; i++) {
		if (nodes[i].fleet == f) {
			count++;
		}
		else {
		}
	}

	tree_shown = (count != 0);

	if (count == 0) {
		uv_uilabel_init(&fleet_empty_label, win_style->font, ALIGN_CENTER,
				C(0xFFFFFFFF), "No files in this fleet.");
		uv_uitabwindow_addxy(&fleet_tabs, &fleet_empty_label,
				0, 0, cbb.width, cbb.height);
	}
	else {
		uv_uitreeview_init(&tree, tree_buf, win_style);
		uv_uitabwindow_addxy(&fleet_tabs, &tree, 0, 0, cbb.width, cbb.height);
		// the directories at the top of this fleet; each of them brings the
		// whole branch below it
		for (uint16_t i = 0; i < node_n; i++) {
			if ((nodes[i].fleet == f) && (nodes[i].parent < 0)) {
				sfw_build_node(&tree, i);
			}
			else {
			}
		}
	}
	uv_ui_refresh(&fleet_tabs);
}


// Scrolls the product tree with the mouse wheel while the pointer is over it.
//
// A ui window scrolls itself by dragging only; the wheel arrives as a global
// notch counter which whoever the pointer is over has to drain (the running
// simulator list and the log view do the same). Hence the hit test against the
// tree's global bounding box: it is what keeps a notch meant for this list from
// being taken by whatever else is reading the wheel.
static void sfw_wheel_step(void) {
	if (tree_shown) {
		int16_t x = 0;
		int16_t y = 0;
		// the position is reported whether or not a button is held down
		uv_ui_get_touch(&x, &y);
		int16_t gx = uv_ui_get_xglobal(&tree);
		int16_t gy = uv_ui_get_yglobal(&tree);
		uv_bounding_box_st *bb = uv_uibb(&tree);
		if ((x >= gx) && (x < (gx + bb->width)) &&
				(y >= gy) && (y < (gy + bb->height))) {
			int16_t scroll = uv_ui_get_scroll();
			if (scroll != 0) {
				// positive (wheel up) moves the content down. content_move clamps
				// to the content box, so a list that fits stays put
				uv_uiwindow_content_move(&tree, 0, scroll * SFW_SCROLL_STEP);
				uv_ui_refresh(&tree);
			}
			else {
			}
		}
		else {
		}
	}
	else {
	}
}


static uv_uiobject_ret_e sfw_step(void *user_ptr, uint16_t step_ms) {
	(void) user_ptr;
	(void) step_ms;
	uv_uiobject_ret_e ret = UIOBJECT_RETURN_ALIVE;

	sfw_wheel_step();

	if (uv_uibutton_clicked(&close_btn)) {
		ret = UIOBJECT_RETURN_KILLED;
	}
	else if ((fleet_count > 0) && uv_uitabwindow_tab_changed(&fleet_tabs)) {
		sfw_show_fleet((uint8_t) uv_uitabwindow_get_tab(&fleet_tabs));
	}
	else {
		// poll every version's Download button (products not on the active tab,
		// and un-opened ones, were never added to a window, so they simply never
		// report a click)
		bool handled = false;
		for (uint16_t i = 0; (i < node_n) && !handled; i++) {
			for (uint16_t j = 0; j < nodes[i].versions; j++) {
				if (uv_uibutton_clicked(&nodes[i].dl_btns[j])) {
					// The window closes and the download runs after it: it is
					// the caller that does something with the file, and a modal
					// dialog frozen for the length of a transfer shows nothing
					// the log does not show better.
					sel_prod = nodes[i].product;
					sel_ver = (int) j;
					ret = UIOBJECT_RETURN_KILLED;
					handled = true;
					break;
				}
			}
		}
	}
	return ret;
}


bool serverfiles_win_exec(const uv_uistyle_st *style) {
	bool ret = false;
	win_style = style;
	sel_prod = -1;
	sel_ver = -1;
	tree_shown = false;

	// 1. log in and fetch the file list (blocks; failures are reported and abort)
	char err[256] = "";
	if (!remotefiles_login(credentials_get_url(), credentials_get_username(),
			credentials_get_password(), err, sizeof(err))) {
		sfw_message(err);
		return false;
	}
	if (!remotefiles_list(err, sizeof(err))) {
		sfw_message(err);
		return false;
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

	int16_t tabs_y = SFW_MARGIN + SFW_TITLE_H + SFW_MARGIN;
	int16_t tabs_h = h - tabs_y - (SFW_BTN_H + 2 * SFW_MARGIN);

	if (!sfw_alloc_ui()) {
		sfw_message("Not enough memory to show the file list.");
		return false;
	}

	// 3. one tab per fleet. The tab names point straight at the fleet names
	// remotefiles keeps, which outlive this window.
	fleet_count = remotefiles_get_fleet_count();
	if (fleet_count > REMOTEFILES_MAX_FLEETS) {
		fleet_count = REMOTEFILES_MAX_FLEETS;
	}
	for (uint8_t f = 0; f < fleet_count; f++) {
		fleet_names[f] = (char*) remotefiles_get_fleet(f);
	}
	if (fleet_count == 0) {
		uv_uilabel_init(&empty_label, style->font, ALIGN_CENTER,
				C(0xFFFFFFFF), "No files are available for this account.");
		uv_uidialog_addxy(&dialog, &empty_label,
				SFW_MARGIN, tabs_y, w - 2 * SFW_MARGIN, tabs_h);
	}
	else {
		uv_uitabwindow_init(&fleet_tabs, fleet_count, style,
				fleet_tabs_buf, fleet_names);
		uv_uidialog_addxy(&dialog, &fleet_tabs,
				SFW_MARGIN, tabs_y, w - 2 * SFW_MARGIN, tabs_h);
		sfw_show_fleet(0);
	}

	uv_uibutton_init(&close_btn, "Close", style);
	uv_uidialog_addxy(&dialog, &close_btn,
			w - SFW_DL_W - SFW_MARGIN, h - SFW_BTN_H - SFW_MARGIN,
			SFW_DL_W, SFW_BTN_H);

	uv_uidialog_exec(&dialog);

	// Only now: uv_uidialog_exec() runs the modal's own loop and returns when
	// the dialog is closed, so up to this point the dialog still holds pointers
	// to everything allocated above.
	int p = sel_prod;
	int j = sel_ver;
	sfw_free_ui();

	// The listing itself is not freed with the UI, so the chosen file is still
	// known here - and the window is gone while the transfer runs.
	if ((p >= 0) && (j >= 0)) {
		ret = sfw_start_fetch((uint16_t) p, (uint16_t) j);
	}
	return ret;
}
