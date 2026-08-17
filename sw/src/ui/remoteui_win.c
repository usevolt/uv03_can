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


#include "ui/remoteui_win.h"

#if CONFIG_UI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "uv_ui_common.h"
#include "uv_ui_remote.h"

// The faces compiled into the binary, used when no font file is found. An
// installed uvcan is started from wherever the user happens to be, so the
// "fonts/" directory next to the sources is not there to open - and the whole
// mirrored view is text. The main window carries the same two faces for the
// same reason; they are a header of static data, so this is a second copy
// rather than a shared one.
#include "ui/embedded_font.h"
#include "ui/embedded_mono_font.h"

#define SANS_FONT_FILE		"fonts/LiberationSans-Regular.ttf"
#define MONO_FONT_FILE		"fonts/LiberationMono-Regular.ttf"

// PNG and JPEG in one public-domain header. The device sends the image file as
// it is stored, and it may be either.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "thirdparty/stb_image.h"


// The device draws with a Y-down coordinate system and 8 bit ARGB colours, and
// the whole command stream is relative to the display size it reported, so the
// window is created at exactly that size and the projection is set up to match
// one command unit to one pixel.

// Corner segments used to approximate a rounded rectangle, and edge segments
// for a filled circle. Enough that neither shows facets at the sizes a device
// display uses.
#define ROUND_SEGMENTS		6
#define CIRCLE_SEGMENTS		24

// Glyph atlases are built on demand, one per (font, pixel size) actually used.
// A device uses a handful of sizes, so this never grows far.
#define ATLAS_MAX			16
#define GLYPH_FIRST			32
#define GLYPH_LAST			126
#define GLYPH_ASCII_COUNT	(GLYPH_LAST - GLYPH_FIRST + 1)

// Beyond printable ASCII, the Nordic letters. The device sends strings as raw
// UTF-8, and a Finnish UI is full of these — without them "Näytön Kirkkaus"
// renders as "Nytn Kirkkaus".
static const uint32_t glyph_extra[] = {
		0x00C4,	// A umlaut
		0x00C5,	// A ring
		0x00D6,	// O umlaut
		0x00E4,	// a umlaut
		0x00E5,	// a ring
		0x00F6	// o umlaut
};
#define GLYPH_EXTRA_COUNT	((uint16_t) (sizeof(glyph_extra) / \
									sizeof(glyph_extra[0])))
#define GLYPH_COUNT			(GLYPH_ASCII_COUNT + GLYPH_EXTRA_COUNT)


/// @brief: Unicode code point held in atlas slot *gi*.
static uint32_t glyph_codepoint(uint16_t gi) {
	return (gi < GLYPH_ASCII_COUNT) ?
			(uint32_t) (GLYPH_FIRST + gi) :
			glyph_extra[gi - GLYPH_ASCII_COUNT];
}


/// @brief: Atlas slot for *cp*, or -1 when the atlas does not carry it.
static int16_t glyph_index_of(uint32_t cp) {
	int16_t ret = -1;
	if ((cp >= GLYPH_FIRST) && (cp <= GLYPH_LAST)) {
		ret = (int16_t) (cp - GLYPH_FIRST);
	}
	else {
		for (uint16_t i = 0; i < GLYPH_EXTRA_COUNT; i++) {
			if (glyph_extra[i] == cp) {
				ret = (int16_t) (GLYPH_ASCII_COUNT + i);
				break;
			}
		}
	}
	return ret;
}


/// @brief: Decodes one UTF-8 code point starting at *i, advancing it past the
/// sequence. Returns 0 for a malformed or truncated one, having advanced by a
/// single byte so the caller always makes progress.
static uint32_t utf8_next(const char *s, uint16_t len, uint16_t *i) {
	uint32_t ret = 0;
	uint8_t c = (uint8_t) s[*i];
	uint8_t extra = 0;
	if (c < 0x80) {
		ret = c;
	}
	else if ((c & 0xE0) == 0xC0) {
		ret = c & 0x1Fu;
		extra = 1;
	}
	else if ((c & 0xF0) == 0xE0) {
		ret = c & 0x0Fu;
		extra = 2;
	}
	else if ((c & 0xF8) == 0xF0) {
		ret = c & 0x07u;
		extra = 3;
	}
	else {
		// a stray continuation byte
		ret = 0;
	}

	if ((extra > 0) && ((*i + extra) < len)) {
		for (uint8_t n = 1; n <= extra; n++) {
			uint8_t cc = (uint8_t) s[*i + n];
			if ((cc & 0xC0) != 0x80) {
				ret = 0;
				extra = 0;
				break;
			}
			ret = (ret << 6) | (cc & 0x3Fu);
		}
	}
	else if (extra > 0) {
		// truncated at the end of the string
		ret = 0;
		extra = 0;
	}
	else {
	}

	*i = (uint16_t) (*i + 1 + extra);
	return ret;
}


typedef struct {
	bool used;
	bool mono;
	uint16_t px;
	GLuint tex;
	uint16_t tex_w;
	uint16_t tex_h;
	// per glyph placement inside the atlas and its metrics, in pixels
	uint16_t x[GLYPH_COUNT];
	uint16_t w[GLYPH_COUNT];
	uint16_t h[GLYPH_COUNT];
	int16_t bearing_x[GLYPH_COUNT];
	int16_t bearing_y[GLYPH_COUNT];
	uint16_t advance[GLYPH_COUNT];
	uint16_t ascender;
} atlas_st;


static GLFWwindow *win;
static uint16_t win_w;
static uint16_t win_h;
static char win_title[128];

static FT_Library ft;
static bool ft_ready;
static atlas_st atlases[ATLAS_MAX];

/// @brief: Everything drawn so far, kept as pixels.
///
/// The device redraws only what changed - a single label, most of the time -
/// and says so with FRAME_BEGIN_KEEP, meaning "put this on top of what you
/// have". Replaying the last full screen underneath each one is not enough:
/// two partial updates in a row would each be composited onto that same full
/// screen, and the second would wipe out the first. Screens that redraw a
/// widget at a time - the system configuration, the inputs - appeared not to
/// update at all, while screens that redraw wholesale were fine.
///
/// Drawing into a texture that persists between frames makes "keep" mean what
/// it says: the overlays accumulate, exactly as they do on the device's own
/// display list, and nothing has to be replayed.
static GLuint fbo;
static GLuint fbo_tex;
static uint16_t fbo_w;
static uint16_t fbo_h;


/// @brief: Makes sure the persistent framebuffer exists at the display's size.
/// @return: false when it could not be created, in which case drawing falls
/// back to the window itself and partial updates are all that is lost.
static bool fbo_ensure(void) {
	bool ret = true;
	if ((fbo == 0) || (fbo_w != win_w) || (fbo_h != win_h)) {
		if (fbo != 0) {
			glDeleteFramebuffers(1, &fbo);
			glDeleteTextures(1, &fbo_tex);
			fbo = 0;
			fbo_tex = 0;
		}
		glGenTextures(1, &fbo_tex);
		glBindTexture(GL_TEXTURE_2D, fbo_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, win_w, win_h, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, fbo_tex, 0);
		ret = (glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
				GL_FRAMEBUFFER_COMPLETE);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (ret) {
			fbo_w = win_w;
			fbo_h = win_h;
		}
		else {
			printf("remote UI: no framebuffer object; partial screen updates "
					"will not be shown\n");
			fflush(stdout);
			fbo = 0;
		}
	}
	else {
	}
	return ret;
}

/// @brief: How long the device may go unheard from before the view is dimmed.
/// Comfortably over the 5 s heartbeat, so a single lost one is not enough, and
/// short enough that a link that has really gone is obvious while the operator
/// is still looking at the screen.
#define STALE_AGE_S			12

/// @brief: Seconds since anything was last heard from the device, as the caller
/// last reported it, and whether that has passed STALE_AGE_S.
static uint32_t link_age_s;
static bool link_stale;
/// @brief: The age the dimmed view currently reads, so it is repainted once a
/// second rather than on every step.
static uint32_t stale_shown_s;

/// @brief: The most recent frame drawn, whatever kind it was.
///
/// The device sends a frame only when its screen changes, so an image or a font
/// that arrives afterwards would otherwise not be drawn until something else
/// moved - a still screen would sit there showing outlines where its icons
/// belong. Keeping the last frame lets it be drawn again the moment the missing
/// piece turns up.
static uint8_t *last_frame;
static uint32_t last_frame_len;

/// @brief: What the device told us about one of its fonts.
///
/// The sink draws with faces of its own, which are not the device's, so without
/// this it guessed the size from a font table compiled into uvcan and hoped the
/// two matched. They need not: the device's fonts differ between hardware
/// revisions, and two of its slots are custom fonts loaded from external flash.
/// Asking it for the height and the per-glyph advances makes the mirrored text
/// break and align where the device breaks and aligns it, whatever face draws
/// the glyphs.
typedef struct {
	// the device answered, one way or the other; stop asking
	bool known;
	// ...and it had metrics for this font (false = no such font there)
	bool have;
	uint16_t height;
	uint8_t advance[UV_UI_REMOTE_FONT_WIDTHS];
	// steps until the request is made again, 0 when not waiting for an answer
	uint16_t retry;
} devfont_st;

// font_id is an index plus a mono flag in bit 7, so both tables fit here
#define DEVFONT_COUNT		(2 * UI_MAX_FONT_COUNT)
static devfont_st devfonts[DEVFONT_COUNT];

/// @brief: How many steps to wait for an answer before asking again. The device
/// serves one asset per frame, so an answer that is coming has arrived long
/// before this; 150 steps is the three seconds after which we assume it was
/// lost.
#define DEVFONT_RETRY_STEPS	150

/// @brief: How long the link has to have been quiet before an image is asked
/// for on our own initiative, in steps of the UI cycle.
///
/// Half a second: long enough that a screen still arriving in chunks, or a view
/// being dragged about, is left alone, and short enough that the images of a
/// screen that has settled fill in while the operator is still looking at it.
#define ASSET_IDLE_STEPS	25

/// @brief: Steps since anything was last heard from the device. The idle fetch
/// below is the one thing here that speaks without being spoken to, so it waits
/// for a gap rather than adding to whatever is already in flight.
static uint16_t quiet_steps;

static remoteui_asset_req_t asset_req_callb;
static void *asset_req_user;

static remoteui_input_t input_callb;
static void *input_user;

/// @brief: Whether the mouse button is down over the mirrored view, and where
/// it was last reported to be.
///
/// The device wants raw presses and releases: its own uv_uidisplay_step turns a
/// run of them into a press, a drag or a click, exactly as it does for its own
/// touch screen. So a press is repeated as the pointer moves - that is what a
/// drag looks like from here - and released once.
static bool mouse_down;
static int16_t last_x;
static int16_t last_y;
static double last_move_t;

/// @brief: Turns a position in this window into one on the device's display.
///
/// The window opens at the device's size but the user may resize it, and the
/// device knows nothing of that: it is told where its own screen was touched.
static void win_to_device(double cx, double cy, int16_t *dx, int16_t *dy) {
	int cw = win_w;
	int ch = win_h;
	glfwGetWindowSize(win, &cw, &ch);
	if (cw <= 0) {
		cw = (int) win_w;
	}
	if (ch <= 0) {
		ch = (int) win_h;
	}
	*dx = (int16_t) ((cx * (double) win_w) / (double) cw);
	*dy = (int16_t) ((cy * (double) win_h) / (double) ch);
}


static void send_input(uint8_t action, int16_t x, int16_t y,
		int16_t scroll, char key) {
	if (input_callb != NULL) {
		input_callb(action, x, y, scroll, key, input_user);
	}
	else {
	}
}


static void mirror_mouse_button_callb(GLFWwindow *w, int button, int action,
		int mods) {
	(void) mods;
	if (button == GLFW_MOUSE_BUTTON_LEFT) {
		double cx = 0.0;
		double cy = 0.0;
		glfwGetCursorPos(w, &cx, &cy);
		win_to_device(cx, cy, &last_x, &last_y);
		mouse_down = (action == GLFW_PRESS);
		send_input(mouse_down ? (uint8_t) UV_UI_REMOTE_INPUT_PRESS :
				(uint8_t) UV_UI_REMOTE_INPUT_RELEASE, last_x, last_y, 0, '\0');
	}
	else {
	}
}


static void mirror_cursor_pos_callb(GLFWwindow *w, double cx, double cy) {
	(void) w;
	// Hovering is not touching: only a held button is going anywhere. The
	// device latches the last press position, so a moved press is a drag.
	if (mouse_down) {
		int16_t dx;
		int16_t dy;
		win_to_device(cx, cy, &dx, &dy);
		double now = glfwGetTime();
		// A mouse reports far more often than the device redraws, and every
		// report costs a message on the link. One per display step is all it
		// can act on; the release carries the exact final position.
		if (((dx != last_x) || (dy != last_y)) &&
				((now - last_move_t) >= 0.02)) {
			last_x = dx;
			last_y = dy;
			last_move_t = now;
			send_input((uint8_t) UV_UI_REMOTE_INPUT_PRESS, dx, dy, 0, '\0');
		}
		else {
		}
	}
	else {
	}
}


static void mirror_scroll_callb(GLFWwindow *w, double xoffset, double yoffset) {
	(void) w;
	(void) xoffset;
	if ((int16_t) yoffset != 0) {
		// keep whatever press state is current: a scroll must not be read as a
		// release and let go of a drag in progress
		send_input(mouse_down ? (uint8_t) UV_UI_REMOTE_INPUT_PRESS :
				(uint8_t) UV_UI_REMOTE_INPUT_RELEASE,
				last_x, last_y, (int16_t) yoffset, '\0');
	}
	else {
	}
}

static void redraw_last(void);

/// @brief: An image the device has sent, decoded and uploaded once and then
/// kept for as long as the window is open.
///
/// Held in a list allocated as images turn up rather than in a fixed table:
/// what a device has on screen is its business, and a cap here would mean
/// deciding which of its images not to show.
typedef struct devbitmap_st {
	struct devbitmap_st *next;
	uint32_t id;
	GLuint tex;
	uint16_t w;
	uint16_t h;
	// asked for and not drawable yet: no answer so far, or one that carried
	// nothing we could turn into a texture
	bool missing;
	// ...and the device has answered, so there is nothing more to wait for:
	// either it has no such image or it is in a format this cannot read. Asking
	// again would get the same answer, so the idle fetch leaves it alone.
	bool refused;
	uint16_t retry;
} devbitmap_st;

static devbitmap_st *devbitmaps;


/// @brief: Finds a device image by id, or NULL.
static devbitmap_st *devbitmap_find(uint32_t id) {
	devbitmap_st *ret = devbitmaps;
	while ((ret != NULL) && (ret->id != id)) {
		ret = ret->next;
	}
	return ret;
}


/// @brief: Drops every cached image. The textures belong to the window's
/// context, so this must run while that context is current.
static void devbitmaps_free(void) {
	while (devbitmaps != NULL) {
		devbitmap_st *next = devbitmaps->next;
		if (devbitmaps->tex != 0) {
			glDeleteTextures(1, &devbitmaps->tex);
		}
		free(devbitmaps);
		devbitmaps = next;
	}
}

/// @brief: Maps a wire font_id onto a devfonts[] slot, or -1 when it is not a
/// font this build can hold metrics for.
static int16_t devfont_slot(uint8_t font_id) {
	int16_t ret = -1;
	uint8_t idx = (uint8_t) (font_id & 0x7Fu);
	if ((font_id != UV_UI_REMOTE_FONT_UNKNOWN) && (idx < UI_MAX_FONT_COUNT)) {
		ret = (int16_t) (((font_id & 0x80u) != 0u) ?
				(UI_MAX_FONT_COUNT + idx) : idx);
	}
	return ret;
}


// --- colours ----------------------------------------------------------------

/// @brief: Applies a colour_t (0xAARRGGBB) as the current GL colour.
static void set_color(uint32_t c) {
	glColor4ub((GLubyte) ((c >> 16) & 0xFF),
			(GLubyte) ((c >> 8) & 0xFF),
			(GLubyte) (c & 0xFF),
			(GLubyte) ((c >> 24) & 0xFF));
}


// --- little-endian readers over the command stream --------------------------

static uint16_t rd16(const uint8_t *p) {
	return (uint16_t) (p[0] | (p[1] << 8));
}

static int16_t rds16(const uint8_t *p) {
	return (int16_t) rd16(p);
}

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
			((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}


// --- text -------------------------------------------------------------------

/// @brief: Returns the atlas for *px* pixels of the sans or mono face, building
/// it the first time it is asked for. NULL when the font cannot be loaded.
static atlas_st *atlas_get(bool mono, uint16_t px) {
	atlas_st *ret = NULL;
	for (uint8_t i = 0; i < ATLAS_MAX; i++) {
		if (atlases[i].used &&
				(atlases[i].mono == mono) &&
				(atlases[i].px == px)) {
			ret = &atlases[i];
			break;
		}
	}
	if (ret == NULL) {
		for (uint8_t i = 0; i < ATLAS_MAX; i++) {
			if (!atlases[i].used) {
				ret = &atlases[i];
				break;
			}
		}
	}
	else {
		// already built
		return ret;
	}
	if (ret == NULL) {
		return NULL;
	}

	if (!ft_ready) {
		if (FT_Init_FreeType(&ft) != 0) {
			printf("remote UI: FreeType could not be initialised, "
					"the mirrored view will have no text\n");
			fflush(stdout);
			return NULL;
		}
		ft_ready = true;
	}

	FT_Face face;
	const char *path = mono ? MONO_FONT_FILE : SANS_FONT_FILE;
	if (FT_New_Face(ft, path, 0, &face) != 0) {
		// no font file where we were started from; the compiled-in face draws
		// the same glyphs
		const unsigned char *mem = mono ?
				embedded_mono_font_ttf : embedded_font_ttf;
		unsigned int mem_len = mono ?
				embedded_mono_font_ttf_len : embedded_font_ttf_len;
		if (FT_New_Memory_Face(ft, mem, mem_len, 0, &face) != 0) {
			printf("remote UI: neither '%s' nor the embedded font could be "
					"loaded, the mirrored view will have no text\n", path);
			fflush(stdout);
			return NULL;
		}
	}
	// *px* is a line height - what both ends call a font's char_height - and not
	// the pixel size a face is set to. A face asked for N pixels lays its lines
	// out somewhat taller than N, so setting the size to the line height draws
	// visibly larger text than the device does. Ask for it, see what the face
	// actually gives, and scale the request by the miss.
	FT_Set_Pixel_Sizes(face, 0, px);
	uint16_t line_h = (uint16_t) (face->size->metrics.height / 64);
	if ((line_h > 0) && (line_h != px)) {
		uint32_t want = ((uint32_t) px * px) / line_h;
		if (want < 1u) {
			want = 1u;
		}
		FT_Set_Pixel_Sizes(face, 0, (FT_UInt) want);
	}
	else {
	}

	// lay the printable ASCII range out in one row
	uint16_t total_w = 0;
	uint16_t max_h = 0;
	for (uint16_t gi = 0; gi < GLYPH_COUNT; gi++) {
		if (FT_Load_Char(face, glyph_codepoint(gi), FT_LOAD_RENDER) != 0) {
			continue;
		}
		total_w = (uint16_t) (total_w + face->glyph->bitmap.width + 1);
		if (face->glyph->bitmap.rows > max_h) {
			max_h = (uint16_t) face->glyph->bitmap.rows;
		}
	}
	if ((total_w == 0) || (max_h == 0)) {
		FT_Done_Face(face);
		return NULL;
	}

	uint8_t *pixels = calloc((size_t) total_w * max_h, 1);
	if (pixels == NULL) {
		FT_Done_Face(face);
		return NULL;
	}

	uint16_t pen = 0;
	for (uint16_t gi = 0; gi < GLYPH_COUNT; gi++) {
		if (FT_Load_Char(face, glyph_codepoint(gi), FT_LOAD_RENDER) != 0) {
			continue;
		}
		FT_GlyphSlot g = face->glyph;
		for (uint16_t row = 0; row < g->bitmap.rows; row++) {
			memcpy(&pixels[(size_t) row * total_w + pen],
					&g->bitmap.buffer[(size_t) row * g->bitmap.pitch],
					g->bitmap.width);
		}
		ret->x[gi] = pen;
		ret->w[gi] = (uint16_t) g->bitmap.width;
		ret->h[gi] = (uint16_t) g->bitmap.rows;
		ret->bearing_x[gi] = (int16_t) g->bitmap_left;
		ret->bearing_y[gi] = (int16_t) g->bitmap_top;
		ret->advance[gi] = (uint16_t) (g->advance.x >> 6);
		pen = (uint16_t) (pen + g->bitmap.width + 1);
	}
	ret->ascender = (uint16_t) (face->size->metrics.ascender >> 6);

	glGenTextures(1, &ret->tex);
	glBindTexture(GL_TEXTURE_2D, ret->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	// single channel coverage, expanded to white with per-pixel alpha so the
	// glyph can be tinted by the current colour
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, total_w, max_h, 0,
			GL_ALPHA, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	free(pixels);
	FT_Done_Face(face);

	ret->used = true;
	ret->mono = mono;
	ret->px = px;
	ret->tex_w = total_w;
	ret->tex_h = max_h;
	return ret;
}


/// @brief: The advance to move on by after drawing *cp*: the device's, when it
/// has told us one.
///
/// The two ends number glyphs differently. This atlas is packed from 32
/// upwards, because it holds no control characters; the device's table is
/// indexed by uv_ui_codepoint_glyph(), which for ASCII is the code point
/// itself. Reading one with the other's index is off by 32 and gives every
/// letter somebody else's width.
static uint16_t glyph_advance(atlas_st *a, const devfont_st *df,
		uint32_t cp, int16_t gi) {
	uint16_t ret = a->advance[gi];
	if (df != NULL) {
		uint8_t slot = uv_ui_codepoint_glyph(cp, true);
		if (df->advance[slot] > 0) {
			ret = df->advance[slot];
		}
	}
	return ret;
}


static uint16_t text_width(atlas_st *a, const devfont_st *df,
		const char *str, uint16_t len) {
	uint16_t ret = 0;
	uint16_t i = 0;
	while (i < len) {
		uint32_t cp = utf8_next(str, len, &i);
		int16_t gi = glyph_index_of(cp);
		if (gi >= 0) {
			ret = (uint16_t) (ret + glyph_advance(a, df, cp, gi));
		}
		else {
			// a code point the atlas does not carry contributes nothing rather
			// than a wrong glyph
		}
	}
	return ret;
}


/// @brief: Draws one line. The caller has already placed it vertically.
static void draw_text(atlas_st *a, const devfont_st *df,
		int16_t x, int16_t y, uint16_t align,
		uint32_t color, const char *str, uint16_t len) {
	uint16_t w = text_width(a, df, str, len);

	// the device's alignment is about the anchor point, not a box
	if ((align & UI_HALIGN_MASK) == UI_HALIGN_CENTER) {
		x = (int16_t) (x - w / 2);
	}
	else if ((align & UI_HALIGN_MASK) == UI_HALIGN_RIGHT) {
		x = (int16_t) (x - w);
	}
	else {
	}

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, a->tex);
	set_color(color);
	glBegin(GL_QUADS);
	int16_t pen = x;
	uint16_t i = 0;
	while (i < len) {
		uint32_t cp = utf8_next(str, len, &i);
		int16_t g = glyph_index_of(cp);
		if (g < 0) {
			continue;
		}
		uint16_t gi = (uint16_t) g;
		float gx = (float) pen + a->bearing_x[gi];
		float gy = (float) y + (float) a->ascender - a->bearing_y[gi];
		float gw = a->w[gi];
		float gh = a->h[gi];
		float u0 = (float) a->x[gi] / a->tex_w;
		float u1 = (float) (a->x[gi] + a->w[gi]) / a->tex_w;
		float v1 = (float) a->h[gi] / a->tex_h;
		glTexCoord2f(u0, 0.0f);  glVertex2f(gx, gy);
		glTexCoord2f(u1, 0.0f);  glVertex2f(gx + gw, gy);
		glTexCoord2f(u1, v1);    glVertex2f(gx + gw, gy + gh);
		glTexCoord2f(u0, v1);    glVertex2f(gx, gy + gh);
		pen = (int16_t) (pen + glyph_advance(a, df, cp, (int16_t) gi));
	}
	glEnd();
	glDisable(GL_TEXTURE_2D);
}


// --- geometry ---------------------------------------------------------------

static void fill_rrect(float x, float y, float w, float h, float r) {
	if (r > w / 2.0f) {
		r = w / 2.0f;
	}
	if (r > h / 2.0f) {
		r = h / 2.0f;
	}
	if (r <= 0.5f) {
		glBegin(GL_QUADS);
		glVertex2f(x, y);
		glVertex2f(x + w, y);
		glVertex2f(x + w, y + h);
		glVertex2f(x, y + h);
		glEnd();
	}
	else {
		// centre block plus the two side bars, then a fan per corner
		glBegin(GL_QUADS);
		glVertex2f(x + r, y);         glVertex2f(x + w - r, y);
		glVertex2f(x + w - r, y + h); glVertex2f(x + r, y + h);
		glVertex2f(x, y + r);         glVertex2f(x + r, y + r);
		glVertex2f(x + r, y + h - r); glVertex2f(x, y + h - r);
		glVertex2f(x + w - r, y + r); glVertex2f(x + w, y + r);
		glVertex2f(x + w, y + h - r); glVertex2f(x + w - r, y + h - r);
		glEnd();

		const float cx[4] = { x + r, x + w - r, x + w - r, x + r };
		const float cy[4] = { y + r, y + r, y + h - r, y + h - r };
		const float a0[4] = { (float) M_PI, (float) (1.5 * M_PI),
				0.0f, (float) (0.5 * M_PI) };
		for (uint8_t c = 0; c < 4; c++) {
			glBegin(GL_TRIANGLE_FAN);
			glVertex2f(cx[c], cy[c]);
			for (uint8_t i = 0; i <= ROUND_SEGMENTS; i++) {
				float a = a0[c] + ((float) M_PI / 2.0f) *
						((float) i / ROUND_SEGMENTS);
				glVertex2f(cx[c] + cosf(a) * r, cy[c] + sinf(a) * r);
			}
			glEnd();
		}
	}
}


static void fill_circle(float cx, float cy, float r) {
	glBegin(GL_TRIANGLE_FAN);
	glVertex2f(cx, cy);
	for (uint8_t i = 0; i <= CIRCLE_SEGMENTS; i++) {
		float a = 2.0f * (float) M_PI * ((float) i / CIRCLE_SEGMENTS);
		glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
	}
	glEnd();
}


/// @brief: A line of *width* pixels, drawn as a quad plus round caps so joins
/// in a strip do not show notches.
static void fill_line(float x0, float y0, float x1, float y1, float width) {
	float dx = x1 - x0;
	float dy = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	float half = (width < 1.0f) ? 0.5f : (width / 2.0f);
	if (len < 0.0001f) {
		fill_circle(x0, y0, half);
	}
	else {
		float nx = -dy / len * half;
		float ny = dx / len * half;
		glBegin(GL_QUADS);
		glVertex2f(x0 + nx, y0 + ny);
		glVertex2f(x1 + nx, y1 + ny);
		glVertex2f(x1 - nx, y1 - ny);
		glVertex2f(x0 - nx, y0 - ny);
		glEnd();
		if (width > 2.0f) {
			fill_circle(x0, y0, half);
			fill_circle(x1, y1, half);
		}
	}
}


/// @brief: Draws a string that may hold line breaks, the way the device does.
///
/// A string arrives as the device stores it, newlines and all - the wire never
/// splits it - and every one of them is a line the device drew separately. Left
/// as they were, the lines ran together into one and its width was the sum of
/// them all, so a centred block of text sat far off to one side.
///
/// The rules are the device's, quirks included: each line is aligned
/// horizontally on its own width, a drawn line advances the baseline by the
/// font's height, an empty line advances nothing at all, and a vertically
/// centred string is lifted by half the height of all its lines together.
static void draw_string(atlas_st *a, const devfont_st *df,
		int16_t x, int16_t y, uint16_t align,
		uint32_t color, const char *str, uint16_t len) {
	uint16_t lines = 1;
	for (uint16_t i = 0; i < len; i++) {
		if (str[i] == '\n') {
			lines++;
		}
		else {
		}
	}

	int16_t line_h = (int16_t) a->px;
	if ((align & UI_VALIGN_MASK) == UI_VALIGN_CENTER) {
		y = (int16_t) (y - ((int16_t) lines * line_h) / 2);
	}
	else {
	}

	uint16_t start = 0;
	for (uint16_t i = 0; i <= len; i++) {
		if ((i == len) || (str[i] == '\n') || (str[i] == '\r')) {
			uint16_t line_len = (uint16_t) (i - start);
			if (line_len > 0) {
				draw_text(a, df, x, y, align, color, &str[start], line_len);
				y = (int16_t) (y + line_h);
			}
			else {
				// an empty line draws nothing and moves nothing
			}
			start = (uint16_t) (i + 1);
		}
		else {
		}
	}
}


// --- the command stream -----------------------------------------------------

/// @brief: Decodes and draws one frame. Any opcode that does not decode cleanly
/// ends the frame: the stream is sequential, so once the offset is wrong
/// nothing after it can be trusted.
static void render(const uint8_t *p, uint32_t len) {
	uint32_t i = 0;
	bool ok = true;

	while ((i < len) && ok) {
		uint8_t op = p[i];
		uint32_t left = len - i;

		switch (op) {
		case UV_UI_REMOTE_OP_FRAME_BEGIN:
			if (left < 5) { ok = false; break; }
			{
				uint32_t c = rd32(&p[i + 1]);
				glClearColor(((c >> 16) & 0xFF) / 255.0f,
						((c >> 8) & 0xFF) / 255.0f,
						(c & 0xFF) / 255.0f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
			i += 5;
			break;

		case UV_UI_REMOTE_OP_FRAME_BEGIN_KEEP:
			// deliberately no clear: the caller has already replayed the frame
			// this one is drawn over
			i += 1;
			break;

		case UV_UI_REMOTE_OP_POINT:
			if (left < 11) { ok = false; break; }
			set_color(rd32(&p[i + 7]));
			fill_circle(rds16(&p[i + 1]), rds16(&p[i + 3]),
					rd16(&p[i + 5]) / 2.0f);
			i += 11;
			break;

		case UV_UI_REMOTE_OP_RRECT:
			if (left < 15) { ok = false; break; }
			set_color(rd32(&p[i + 11]));
			fill_rrect(rds16(&p[i + 1]), rds16(&p[i + 3]),
					rd16(&p[i + 5]), rd16(&p[i + 7]), rd16(&p[i + 9]));
			i += 15;
			break;

		case UV_UI_REMOTE_OP_LINE:
			if (left < 15) { ok = false; break; }
			set_color(rd32(&p[i + 11]));
			fill_line(rds16(&p[i + 1]), rds16(&p[i + 3]),
					rds16(&p[i + 5]), rds16(&p[i + 7]), rd16(&p[i + 9]));
			i += 15;
			break;

		case UV_UI_REMOTE_OP_LINESTRIP:
		{
			if (left < 10) { ok = false; break; }
			uint16_t width = rd16(&p[i + 2]);
			uint32_t c = rd32(&p[i + 4]);
			uint16_t count = rd16(&p[i + 8]);
			if (left < (uint32_t) (10 + 4 * count)) { ok = false; break; }
			set_color(c);
			const uint8_t *pts = &p[i + 10];
			for (uint16_t n = 1; n < count; n++) {
				fill_line(rds16(&pts[(n - 1) * 4]), rds16(&pts[(n - 1) * 4 + 2]),
						rds16(&pts[n * 4]), rds16(&pts[n * 4 + 2]), width);
			}
			i += 10u + 4u * count;
			break;
		}

		case UV_UI_REMOTE_OP_POLYGON:
		{
			if (left < 7) { ok = false; break; }
			uint32_t c = rd32(&p[i + 1]);
			uint16_t count = rd16(&p[i + 5]);
			if (left < (uint32_t) (7 + 4 * count)) { ok = false; break; }
			set_color(c);
			const uint8_t *pts = &p[i + 7];
			glBegin(GL_TRIANGLE_FAN);
			for (uint16_t n = 0; n < count; n++) {
				glVertex2f(rds16(&pts[n * 4]), rds16(&pts[n * 4 + 2]));
			}
			glEnd();
			i += 7u + 4u * count;
			break;
		}

		case UV_UI_REMOTE_OP_STRING:
		{
			if (left < 14) { ok = false; break; }
			uint8_t font_id = p[i + 1];
			int16_t x = rds16(&p[i + 2]);
			int16_t y = rds16(&p[i + 4]);
			uint16_t align = rd16(&p[i + 6]);
			uint32_t c = rd32(&p[i + 8]);
			uint16_t slen = rd16(&p[i + 12]);
			if (left < (uint32_t) (14 + slen)) { ok = false; break; }
			if (font_id != UV_UI_REMOTE_FONT_UNKNOWN) {
				bool mono = ((font_id & 0x80) != 0);
				uint8_t idx = (uint8_t) (font_id & 0x7F);
				int16_t slot = devfont_slot(font_id);
				devfont_st *df = (slot >= 0) ? &devfonts[slot] : NULL;

				// Ask the device about a font we have not been told about. It
				// is drawing with it right now, so it can answer; until it
				// does, fall back to the font table compiled in here, which is
				// a guess that happens to be right when both ends were built
				// for the same hardware.
				if ((df != NULL) && !df->known && (df->retry == 0) &&
						(asset_req_callb != NULL)) {
					asset_req_callb(UV_UI_REMOTE_ASSET_KIND_FONT,
							font_id, asset_req_user);
					df->retry = DEVFONT_RETRY_STEPS;
				}
				else {
				}

				uint16_t px = 0;
				if ((df != NULL) && df->have) {
					px = df->height;
				}
				else if (idx < UI_MAX_FONT_COUNT) {
					px = mono ? ui_mono_fonts[idx].char_height :
							ui_fonts[idx].char_height;
				}
				else {
				}
				if (px == 0) {
					px = 16;
				}
				atlas_st *a = atlas_get(mono, px);
				if (a != NULL) {
					draw_string(a, ((df != NULL) && df->have) ? df : NULL,
							x, y, align, c, (const char *) &p[i + 14], slen);
				}
			}
			i += 14u + slen;
			break;
		}

		case UV_UI_REMOTE_OP_MASK:
			if (left < 9) { ok = false; break; }
			{
				int16_t mx = rds16(&p[i + 1]);
				int16_t my = rds16(&p[i + 3]);
				uint16_t mw = rd16(&p[i + 5]);
				uint16_t mh = rd16(&p[i + 7]);
				// GL scissors from the bottom left, the device masks from the
				// top left
				glEnable(GL_SCISSOR_TEST);
				int32_t sy = (int32_t) win_h - (int32_t) my - (int32_t) mh;
				glScissor(mx, (sy < 0) ? 0 : sy, mw, mh);
			}
			i += 9;
			break;

		case UV_UI_REMOTE_OP_BITMAP:
			if (left < 21) { ok = false; break; }
			{
				uint32_t bid = rd32(&p[i + 1]);
				int16_t bx = rds16(&p[i + 5]);
				int16_t by = rds16(&p[i + 7]);
				uint16_t bw = rd16(&p[i + 9]);
				uint16_t bh = rd16(&p[i + 11]);
				uint32_t bc = rd32(&p[i + 17]);
				devbitmap_st *b = devbitmap_find(bid);

				// Ask for an image we have not seen. Asking while it is being
				// drawn is asking at the best moment there is: the device can
				// only turn the id back into a file name while it has the
				// bitmap in hand. What comes of a request made at any other
				// time is up to the device - it redraws to serve it - so the
				// idle fetch in remoteui_win_step() carries on from here for
				// whatever this screen did not get.
				if ((b == NULL) && (asset_req_callb != NULL)) {
					b = calloc(1, sizeof(*b));
					if (b != NULL) {
						b->id = bid;
						b->missing = true;
						b->next = devbitmaps;
						devbitmaps = b;
						asset_req_callb(UV_UI_REMOTE_ASSET_KIND_BITMAP, bid,
								asset_req_user);
						b->retry = DEVFONT_RETRY_STEPS;
					}
				}
				else if ((b != NULL) && b->missing && !b->refused &&
						(b->retry == 0) && (asset_req_callb != NULL)) {
					asset_req_callb(UV_UI_REMOTE_ASSET_KIND_BITMAP, bid,
							asset_req_user);
					b->retry = DEVFONT_RETRY_STEPS;
				}
				else {
				}

				if ((b != NULL) && (b->tex != 0)) {
					// the device blends the image with a colour, so the same
					// icon can be drawn in several tints
					set_color(bc);
					glEnable(GL_TEXTURE_2D);
					glBindTexture(GL_TEXTURE_2D, b->tex);
					glBegin(GL_QUADS);
					glTexCoord2f(0.0f, 0.0f); glVertex2f(bx, by);
					glTexCoord2f(1.0f, 0.0f); glVertex2f(bx + bw, by);
					glTexCoord2f(1.0f, 1.0f); glVertex2f(bx + bw, by + bh);
					glTexCoord2f(0.0f, 1.0f); glVertex2f(bx, by + bh);
					glEnd();
					glDisable(GL_TEXTURE_2D);
				}
				else {
					// nothing to draw yet: outline where it belongs so the
					// layout still reads while the image is on its way
					set_color(bc);
					glBegin(GL_LINE_LOOP);
					glVertex2f(bx, by);
					glVertex2f(bx + bw, by);
					glVertex2f(bx + bw, by + bh);
					glVertex2f(bx, by + bh);
					glEnd();
				}
			}
			i += 21;
			break;

		case UV_UI_REMOTE_OP_FRAME_END:
			glDisable(GL_SCISSOR_TEST);
			i += 1;
			break;

		default:
			ok = false;
			break;
		}
	}

	if (!ok) {
		// a desynchronised frame; the next one starts from a clean FRAME_BEGIN
		glDisable(GL_SCISSOR_TEST);
	}
}


// --- window -----------------------------------------------------------------

bool remoteui_win_open(const char *title, uint16_t width, uint16_t height) {
	remoteui_win_close();

	if ((width == 0) || (height == 0)) {
		return false;
	}

	// The main UI already called glfwInit(); doing it again is refcounted and
	// harmless, and keeps this module usable on its own.
	if (!glfwInit()) {
		printf("remote UI: GLFW is not available\n");
		fflush(stdout);
		return false;
	}

	GLFWwindow *prev = glfwGetCurrentContext();

	snprintf(win_title, sizeof(win_title), "%s", title);
	// Shares the main window's context so the GL objects this module creates
	// live in one place, and so a driver that dislikes several independent
	// contexts in one process does not have to deal with them.
	win = glfwCreateWindow(width, height, win_title, NULL, prev);
	if (win == NULL) {
		printf("remote UI: could not create the mirror window\n");
		fflush(stdout);
		glfwMakeContextCurrent(prev);
		return false;
	}
	win_w = width;
	win_h = height;
	// a fresh session starts live; the caller reports the link age from here on
	link_stale = false;
	link_age_s = 0;
	stale_shown_s = 0;
	// the first screen is on its way: nothing to fetch behind its back yet
	quiet_steps = 0;

	// the device is driven from this window: a press here is a touch there
	glfwSetMouseButtonCallback(win, &mirror_mouse_button_callb);
	glfwSetCursorPosCallback(win, &mirror_cursor_pos_callb);
	glfwSetScrollCallback(win, &mirror_scroll_callb);
	mouse_down = false;

	glfwMakeContextCurrent(win);
	glewInit();
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glfwSwapBuffers(win);

	glfwMakeContextCurrent(prev);

	printf("remote UI: mirroring '%s' at %ux%u\n", win_title,
			(unsigned int) width, (unsigned int) height);
	fflush(stdout);
	return true;
}


void remoteui_win_set_input_callb(remoteui_input_t callb, void *user) {
	input_callb = callb;
	input_user = user;
}


void remoteui_win_set_asset_request_callb(remoteui_asset_req_t callb,
		void *user) {
	asset_req_callb = callb;
	asset_req_user = user;
}


void remoteui_win_asset_received(uint8_t kind, uint32_t id,
		const uint8_t *data, uint32_t len) {
	// an asset is the link working; leave it to finish before asking for more
	quiet_steps = 0;
	if (kind == UV_UI_REMOTE_ASSET_KIND_FONT) {
		int16_t slot = devfont_slot((uint8_t) id);
		if (slot >= 0) {
			devfont_st *df = &devfonts[slot];
			// answered either way: an empty answer means the device has no such
			// font, and asking again would only get the same reply
			df->known = true;
			df->retry = 0;
			if ((data != NULL) && (len >= UV_UI_REMOTE_FONT_BODY_HDR_LEN)) {
				df->height = (uint16_t) (data[0] | (data[1] << 8));
				memcpy(df->advance, &data[7], UV_UI_REMOTE_FONT_WIDTHS);
				df->have = (df->height > 0);
				printf("remote UI: font %u is %u px on the device\n",
						(unsigned int) id, (unsigned int) df->height);
				fflush(stdout);
				// the screen may not change again for a while, so draw it now
				// with the metrics it was waiting for
				redraw_last();
			}
			else {
				df->have = false;
			}
		}
	}
	else if (kind == UV_UI_REMOTE_ASSET_KIND_BITMAP) {
		devbitmap_st *b = devbitmap_find(id);
		if (b == NULL) {
			b = calloc(1, sizeof(*b));
			if (b != NULL) {
				b->id = id;
				b->next = devbitmaps;
				devbitmaps = b;
			}
		}
		if (b != NULL) {
			b->retry = 0;
			int w = 0;
			int h = 0;
			int comp = 0;
			unsigned char *px = ((data != NULL) && (len > 0)) ?
					stbi_load_from_memory(data, (int) len, &w, &h, &comp, 4) :
					NULL;
			if (px == NULL) {
				// either the device has no such image, or it is in a format
				// this cannot read; either way asking again would not help
				b->missing = true;
				b->refused = true;
				printf("remote UI: image %u could not be decoded (%u bytes)\n",
						(unsigned int) id, (unsigned int) len);
				fflush(stdout);
			}
			else {
				GLFWwindow *prev = glfwGetCurrentContext();
				glfwMakeContextCurrent(win);
				if (b->tex == 0) {
					glGenTextures(1, &b->tex);
				}
				glBindTexture(GL_TEXTURE_2D, b->tex);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
						GL_RGBA, GL_UNSIGNED_BYTE, px);
				glfwMakeContextCurrent(prev);
				stbi_image_free(px);
				b->w = (uint16_t) w;
				b->h = (uint16_t) h;
				b->missing = false;
				printf("remote UI: image %u is %dx%d\n",
						(unsigned int) id, w, h);
				fflush(stdout);
				redraw_last();
			}
		}
	}
	else {
	}
}


void remoteui_win_close(void) {
	if (win != NULL) {
		GLFWwindow *prev = glfwGetCurrentContext();
		// the atlases live in the shared context, but their textures were
		// created while this window was current; drop them so a reopen at a
		// different size rebuilds cleanly
		glfwMakeContextCurrent(win);
		for (uint8_t i = 0; i < ATLAS_MAX; i++) {
			if (atlases[i].used) {
				glDeleteTextures(1, &atlases[i].tex);
				memset(&atlases[i], 0, sizeof(atlases[i]));
			}
		}
		// the device's images were uploaded into this context too, so they go
		// while it is still current and still exists
		devbitmaps_free();
		if (fbo != 0) {
			glDeleteFramebuffers(1, &fbo);
			glDeleteTextures(1, &fbo_tex);
			fbo = 0;
			fbo_tex = 0;
			fbo_w = 0;
			fbo_h = 0;
		}
		else {
		}
		if (mouse_down) {
			// closing mid-press would leave the device holding a touch nobody
			// is making any more
			send_input((uint8_t) UV_UI_REMOTE_INPUT_RELEASE, last_x, last_y,
					0, '\0');
			mouse_down = false;
		}
		else {
		}
		glfwMakeContextCurrent((prev == win) ? NULL : prev);
		glfwDestroyWindow(win);
		win = NULL;
		win_w = 0;
		win_h = 0;
		link_stale = false;
		link_age_s = 0;
		stale_shown_s = 0;
		free(last_frame);
		last_frame = NULL;
		last_frame_len = 0;
		// a new session may be a different device, so nothing learned about the
		// last one's fonts carries over
		memset(devfonts, 0, sizeof(devfonts));
	}
}


bool remoteui_win_is_open(void) {
	return (win != NULL);
}


/// @brief: Darkens the view and says the device has gone quiet.
///
/// Drawn onto the window and never into the persistent framebuffer, so the
/// frame underneath is left intact and comes back undimmed the moment the
/// device is heard from again.
static void draw_stale_overlay(void) {
	glDisable(GL_SCISSOR_TEST);
	set_color(0xC0000000);
	glBegin(GL_QUADS);
	glVertex2f(0.0f, 0.0f);
	glVertex2f(win_w, 0.0f);
	glVertex2f(win_w, win_h);
	glVertex2f(0.0f, win_h);
	glEnd();

	// sized off the display rather than fixed: a device's screen may be a
	// quarter of the size of another's
	uint16_t px = (uint16_t) (win_h / 16u);
	if (px < 14u) {
		px = 14u;
	}
	else if (px > 40u) {
		px = 40u;
	}
	else {
	}

	atlas_st *a = atlas_get(false, px);
	if (a != NULL) {
		const char *msg = "Waiting for the connection to come back online...";
		char age[64];
		snprintf(age, sizeof(age), "nothing heard from the device for %u s",
				(unsigned int) link_age_s);
		int16_t cx = (int16_t) (win_w / 2);
		int16_t cy = (int16_t) (win_h / 2);
		draw_text(a, NULL, cx, (int16_t) (cy - px), UI_HALIGN_CENTER,
				0xFFFFFFFF, msg, (uint16_t) strlen(msg));
		draw_text(a, NULL, cx, (int16_t) (cy + px / 4), UI_HALIGN_CENTER,
				0xFFB0B0B0, age, (uint16_t) strlen(age));
	}
	else {
		// no text to be had; the dimming alone still says the view is not live
	}
}


static void draw_frame_impl(const uint8_t *cmds, uint32_t len, bool replay) {
	// A NULL frame is a repaint of what is already there - what the dimming
	// needs, and it may be asked for before the device has sent anything at all.
	bool have_cmds = ((cmds != NULL) && (len > 0));
	if (win == NULL) {
		return;
	}
	GLFWwindow *prev = glfwGetCurrentContext();
	glfwMakeContextCurrent(win);

	bool have_fbo = fbo_ensure();
	if (have_fbo) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, win_w, win_h);
	}
	else {
		int fb_w;
		int fb_h;
		glfwGetFramebufferSize(win, &fb_w, &fb_h);
		glViewport(0, 0, fb_w, fb_h);
	}
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	// y grows downwards, one unit per device pixel, so the device's coordinates
	// are used verbatim however the window has been resized
	glOrtho(0.0, win_w, win_h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// A frame that keeps what is there draws straight on top; only a frame that
	// begins with a clear starts afresh, and render() does that clearing itself.
	if (have_cmds) {
		render(cmds, len);
	}
	else if (!have_fbo) {
		// nothing is being kept between frames, so there is nothing to repaint
		// under the dimming: start from black rather than from an undefined
		// back buffer
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else {
	}

	if (have_fbo) {
		// put the accumulated picture on the window, scaled to whatever size it
		// has been dragged to
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		int fb_w;
		int fb_h;
		glfwGetFramebufferSize(win, &fb_w, &fb_h);
		glViewport(0, 0, fb_w, fb_h);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0.0, win_w, win_h, 0.0, -1.0, 1.0);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glDisable(GL_BLEND);
		glColor4ub(255, 255, 255, 255);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, fbo_tex);
		glBegin(GL_QUADS);
		// the texture's origin is bottom left, the device's is top left
		glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 0.0f);
		glTexCoord2f(1.0f, 1.0f); glVertex2f(win_w, 0.0f);
		glTexCoord2f(1.0f, 0.0f); glVertex2f(win_w, win_h);
		glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, win_h);
		glEnd();
		glDisable(GL_TEXTURE_2D);
		glEnable(GL_BLEND);
	}
	else {
	}

	if (link_stale) {
		draw_stale_overlay();
	}
	else {
	}

	glfwSwapBuffers(win);
	glfwMakeContextCurrent(prev);

	if (!replay && have_cmds) {
		uint8_t *copy = realloc(last_frame, len);
		if (copy != NULL) {
			memcpy(copy, cmds, len);
			last_frame = copy;
			last_frame_len = len;
		}
		else {
			free(last_frame);
			last_frame = NULL;
			last_frame_len = 0;
		}
	}
	else {
	}
}


/// @brief: Draws the last frame again, for when something it needed has just
/// arrived.
void remoteui_win_draw_frame(const uint8_t *cmds, uint32_t len) {
	// a frame is the device speaking: the view is live again whatever the link
	// age last reported said, and the frame must not be drawn dimmed
	link_stale = false;
	link_age_s = 0;
	stale_shown_s = 0;
	quiet_steps = 0;
	draw_frame_impl(cmds, len, false);
}


static void redraw_last(void) {
	if ((win != NULL) && (last_frame != NULL)) {
		draw_frame_impl(last_frame, last_frame_len, true);
	}
	else {
	}
}


void remoteui_win_set_link_age(uint32_t seconds) {
	link_age_s = seconds;
	if (win == NULL) {
		link_stale = false;
	}
	else {
		bool stale = (seconds >= STALE_AGE_S);
		// The seconds are part of what is drawn, so a repaint is due whenever
		// they move on as well as when the view goes stale or comes back.
		if ((stale != link_stale) ||
				(stale && (seconds != stale_shown_s))) {
			link_stale = stale;
			stale_shown_s = seconds;
			// Nothing else is going to repaint this: a device that has gone
			// quiet is exactly one that sends no frames, which is the case
			// being drawn for.
			if (last_frame != NULL) {
				redraw_last();
			}
			else {
				// nothing has ever been drawn; the dimming and its message are
				// the whole picture
				draw_frame_impl(NULL, 0, true);
			}
		}
		else {
		}
	}
}


/// @brief: Asks the device for one image the view is still missing, when the
/// link has been quiet long enough to spare it.
///
/// Requests made while a screen is being drawn are the cheap ones - the device
/// has the bitmap in hand - but the device serves one asset at a time and
/// remembers only a handful of requests, so a screen with more images than that
/// loses the rest. Nothing then asks for them again until that screen is drawn
/// once more, which on a display that has settled may be never: the operator
/// sits looking at outlines where the icons belong.
///
/// So they are picked up here instead, one per quiet gap. Each ask sets the
/// image's retry, so the next gap moves on to another one and the screen fills
/// in a piece at a time. One at a time is what the device can serve anyway, and
/// the gap is what keeps this off the back of a link that is busy carrying the
/// screen itself.
static void assets_fetch_idle(void) {
	devbitmap_st *want = NULL;
	if (link_stale) {
		// nothing has been heard from the device for a while; asking it for
		// pictures is not what will bring it back
		return;
	}
	for (devbitmap_st *b = devbitmaps; (b != NULL) && (want == NULL);
			b = b->next) {
		if (b->missing && !b->refused && (b->retry == 0)) {
			want = b;
		}
		else {
		}
	}
	if ((want != NULL) && (asset_req_callb != NULL)) {
		asset_req_callb(UV_UI_REMOTE_ASSET_KIND_BITMAP, want->id,
				asset_req_user);
		want->retry = DEVFONT_RETRY_STEPS;
		// the request itself is traffic: wait out another gap before the next
		quiet_steps = 0;
	}
	else {
	}
}


void remoteui_win_step(void) {
	if (win != NULL) {
		// count down the wait for an answer about a font, so a request that was
		// lost is made again rather than leaving that font guessed at forever
		for (uint16_t i = 0; i < DEVFONT_COUNT; i++) {
			if (devfonts[i].retry > 0) {
				devfonts[i].retry--;
			}
			else {
			}
		}
		for (devbitmap_st *b = devbitmaps; b != NULL; b = b->next) {
			if (b->retry > 0) {
				b->retry--;
			}
			else {
			}
		}
		if (quiet_steps < ASSET_IDLE_STEPS) {
			quiet_steps++;
		}
		else {
			assets_fetch_idle();
		}
		// glfwPollEvents is global, and the main UI loop already calls it; what
		// matters here is noticing the user closing this window
		if (glfwWindowShouldClose(win)) {
			printf("remote UI: window closed\n");
			fflush(stdout);
			remoteui_win_close();
		}
	}
}


#endif
