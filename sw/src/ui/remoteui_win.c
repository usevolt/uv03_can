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
#define GLYPH_COUNT			(GLYPH_LAST - GLYPH_FIRST + 1)


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
	const char *path = mono ? "fonts/LiberationMono-Regular.ttf" :
			"fonts/LiberationSans-Regular.ttf";
	if (FT_New_Face(ft, path, 0, &face) != 0) {
		printf("remote UI: could not open '%s', the mirrored view will have "
				"no text\n", path);
		fflush(stdout);
		return NULL;
	}
	FT_Set_Pixel_Sizes(face, 0, px);

	// lay the printable ASCII range out in one row
	uint16_t total_w = 0;
	uint16_t max_h = 0;
	for (uint16_t ch = GLYPH_FIRST; ch <= GLYPH_LAST; ch++) {
		if (FT_Load_Char(face, ch, FT_LOAD_RENDER) != 0) {
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
	for (uint16_t ch = GLYPH_FIRST; ch <= GLYPH_LAST; ch++) {
		uint16_t gi = (uint16_t) (ch - GLYPH_FIRST);
		if (FT_Load_Char(face, ch, FT_LOAD_RENDER) != 0) {
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


static uint16_t text_width(atlas_st *a, const char *str, uint16_t len) {
	uint16_t ret = 0;
	for (uint16_t i = 0; i < len; i++) {
		uint8_t ch = (uint8_t) str[i];
		if ((ch >= GLYPH_FIRST) && (ch <= GLYPH_LAST)) {
			ret = (uint16_t) (ret + a->advance[ch - GLYPH_FIRST]);
		}
		else {
			// a non-ASCII byte: the stream is UTF-8 and the atlas is not, so it
			// contributes nothing rather than a wrong glyph
		}
	}
	return ret;
}


static void draw_text(atlas_st *a, int16_t x, int16_t y, uint16_t align,
		uint32_t color, const char *str, uint16_t len) {
	uint16_t w = text_width(a, str, len);

	// the device's alignment is about the anchor point, not a box
	if ((align & UI_HALIGN_MASK) == UI_HALIGN_CENTER) {
		x = (int16_t) (x - w / 2);
	}
	else if ((align & UI_HALIGN_MASK) == UI_HALIGN_RIGHT) {
		x = (int16_t) (x - w);
	}
	else {
	}
	if ((align & UI_VALIGN_MASK) == UI_VALIGN_CENTER) {
		y = (int16_t) (y - a->px / 2);
	}
	else {
	}

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, a->tex);
	set_color(color);
	glBegin(GL_QUADS);
	int16_t pen = x;
	for (uint16_t i = 0; i < len; i++) {
		uint8_t ch = (uint8_t) str[i];
		if ((ch < GLYPH_FIRST) || (ch > GLYPH_LAST)) {
			continue;
		}
		uint16_t gi = (uint16_t) (ch - GLYPH_FIRST);
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
		pen = (int16_t) (pen + a->advance[gi]);
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
				// the font table is the same on both ends, so the index gives
				// the pixel size the device drew with
				uint16_t px = (idx < UI_MAX_FONT_COUNT) ?
						(mono ? ui_mono_fonts[idx].char_height :
								ui_fonts[idx].char_height) : 0;
				if (px == 0) {
					px = 16;
				}
				atlas_st *a = atlas_get(mono, px);
				if (a != NULL) {
					draw_text(a, x, y, align, c,
							(const char *) &p[i + 14], slen);
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
			if (left < 19) { ok = false; break; }
			{
				// Asset streaming is not implemented on the device side yet, so
				// there is no image to draw. Outline where it belongs rather
				// than leaving a hole, so the layout still reads correctly.
				int16_t bx = rds16(&p[i + 3]);
				int16_t by = rds16(&p[i + 5]);
				uint16_t bw = rd16(&p[i + 7]);
				uint16_t bh = rd16(&p[i + 9]);
				set_color(rd32(&p[i + 15]));
				glBegin(GL_LINE_LOOP);
				glVertex2f(bx, by);
				glVertex2f(bx + bw, by);
				glVertex2f(bx + bw, by + bh);
				glVertex2f(bx, by + bh);
				glEnd();
			}
			i += 19;
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
		glfwMakeContextCurrent((prev == win) ? NULL : prev);
		glfwDestroyWindow(win);
		win = NULL;
		win_w = 0;
		win_h = 0;
	}
}


bool remoteui_win_is_open(void) {
	return (win != NULL);
}


void remoteui_win_draw_frame(const uint8_t *cmds, uint32_t len) {
	if ((win == NULL) || (cmds == NULL) || (len == 0)) {
		return;
	}
	GLFWwindow *prev = glfwGetCurrentContext();
	glfwMakeContextCurrent(win);

	int fb_w;
	int fb_h;
	glfwGetFramebufferSize(win, &fb_w, &fb_h);
	glViewport(0, 0, fb_w, fb_h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	// y grows downwards, one unit per device pixel, so the device's coordinates
	// are used verbatim however the window has been resized
	glOrtho(0.0, win_w, win_h, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	render(cmds, len);

	glfwSwapBuffers(win);
	glfwMakeContextCurrent(prev);
}


void remoteui_win_step(void) {
	if (win != NULL) {
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
