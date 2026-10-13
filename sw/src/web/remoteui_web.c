/*
 * This file is part of the uv_hal distribution (www.usevolt.fi).
 * Copyright (c) 2026 Usevolt Oy.
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


/*
 * The remote UI window of the web browser build: the device's display mirrored
 * onto a canvas of its own, shown over the page with a title bar and a close
 * button. The desktop one is src/ui/remoteui_win.c, and this follows it
 * closely - the command stream, the device's font metrics and the asset
 * requests are handled the same way. What differs is only where it draws:
 *
 * - The canvas is the device's display at its own size, one pixel per device
 *   pixel, and the page scales it to fit. A canvas keeps what is drawn on it,
 *   which is exactly what a frame that only adds to the last one
 *   (FRAME_BEGIN_KEEP) needs; the desktop one keeps a framebuffer for that.
 * - Text is the browser's, in the same Liberation faces, laid out with the
 *   device's own glyph widths once it has sent them.
 * - "Waiting for the connection" is a line of the page over the canvas, so
 *   the picture under it is never touched.
 *
 * The page's events - a press, a drag, the wheel, the close button - are
 * queued in the page and taken in by remoteui_win_step(), on the UI task.
 */

#if CONFIG_TARGET_WEB

#include "ui/remoteui_win.h"
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uv_ui_common.h"
#include "uv_ui_remote.h"

// PNG and JPEG in one public-domain header. The device sends the image file as
// it is stored, and it may be either.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "thirdparty/stb_image.h"


// How long without a word from the device before the view says so
#define STALE_AGE_S			12

// Steps to wait for an answer about a font or an image before asking again
#define ASSET_RETRY_STEPS	150

// Quiet steps before an image still missing is asked for (see
// assets_fetch_idle())
#define ASSET_IDLE_STEPS	25

// Longest line drawn, in code points
#define STRING_MAX_CPS		512


/// @brief: What the device said about one of its fonts.
typedef struct {
	bool known;
	bool have;
	uint16_t height;
	uint8_t advance[UV_UI_REMOTE_FONT_WIDTHS];
	uint16_t retry;
} devfont_st;

#define DEVFONT_COUNT		(2 * UI_MAX_FONT_COUNT)


/// @brief: An image the device has sent, decoded into the page once and then
/// kept for as long as the view is open.
typedef struct devbitmap_st {
	struct devbitmap_st *next;
	uint32_t id;
	// the page's id for it, -1 while it has not arrived
	int32_t js_id;
	bool missing;
	bool refused;
	uint16_t retry;
} devbitmap_st;


static struct {
	bool open;
	uint16_t w;
	uint16_t h;

	uint32_t link_age_s;
	bool link_stale;
	uint32_t stale_shown_s;

	uint8_t *last_frame;
	uint32_t last_frame_len;

	devfont_st devfonts[DEVFONT_COUNT];
	devbitmap_st *devbitmaps;
	uint16_t quiet_steps;

	remoteui_asset_req_t asset_req_callb;
	void *asset_req_user;
	remoteui_input_t input_callb;
	void *input_user;

	bool mouse_down;
	int16_t last_x;
	int16_t last_y;
} rui;


// ---- the page side ----------------------------------------------------------

EM_JS(void, js_rui_open, (const char *title, int w, int h), {
	const old = document.getElementById('uvremote');
	if (old) {
		old.remove();
	}
	const fit = 'width:min(96vw, calc((100dvh - 64px) * ' + w + ' / ' + h + '));';
	const ov = document.createElement('div');
	ov.id = 'uvremote';
	ov.style.cssText = 'position:fixed;inset:0;z-index:10;display:flex;' +
		'flex-direction:column;align-items:center;justify-content:center;' +
		'background:rgba(0,0,0,0.85);';
	const bar = document.createElement('div');
	bar.style.cssText = 'display:flex;align-items:center;gap:12px;' +
		'color:#fff;font:15px UvSans,sans-serif;margin:0 0 8px;' + fit;
	const name = document.createElement('span');
	name.textContent = UTF8ToString(title);
	name.style.cssText = 'flex:1;overflow:hidden;text-overflow:ellipsis;' +
		'white-space:nowrap;';
	const close = document.createElement('button');
	close.textContent = 'Close';
	close.style.cssText = 'font:inherit;color:#fff;background:#945dd2;' +
		'border:0;border-radius:4px;padding:6px 14px;cursor:pointer;';
	bar.appendChild(name);
	bar.appendChild(close);
	const wrap = document.createElement('div');
	wrap.style.cssText = 'position:relative;' + fit;
	const c = document.createElement('canvas');
	c.width = w;
	c.height = h;
	c.style.cssText = 'display:block;width:100%;height:auto;' +
		'touch-action:pinch-zoom;background:#000;';
	const stale = document.createElement('div');
	stale.style.cssText = 'position:absolute;inset:0;display:none;' +
		'flex-direction:column;align-items:center;justify-content:center;' +
		'background:rgba(0,0,0,0.75);color:#fff;text-align:center;' +
		'font:18px UvSans,sans-serif;gap:6px;';
	wrap.appendChild(c);
	wrap.appendChild(stale);
	ov.appendChild(bar);
	ov.appendChild(wrap);
	document.body.appendChild(ov);

	const r = {
		ov: ov,
		canvas: c,
		ctx: c.getContext('2d'),
		stale: stale,
		w: w,
		h: h,
		events: [],
		closed: false,
		images: [],
		tinted: new Map(),
		fonts: ['UvSans', 'UvMono'],
		down: false,
		lastMove: 0
	};
	Module.uvremote = r;
	r.ctx.fillStyle = '#000';
	r.ctx.fillRect(0, 0, w, h);
	r.ctx.save();

	function pos(e) {
		const b = c.getBoundingClientRect();
		return [Math.round((e.clientX - b.left) * w / b.width),
				Math.round((e.clientY - b.top) * h / b.height)];
	}
	// [action, x, y, scroll]: action 1 is a press (or a drag), 0 a release
	c.addEventListener('pointerdown', (e) => {
		if (r.down) {
			// a second finger: a pinch is starting
			cancel(e);
			return;
		}
		c.setPointerCapture(e.pointerId);
		r.down = true;
		const p = pos(e);
		r.events.push([1, p[0], p[1], 0]);
		e.preventDefault();
	});
	c.addEventListener('pointermove', (e) => {
		// a moved press is a drag; one report per display step is all the
		// device acts on, and the release carries the final position
		const now = performance.now();
		if (r.down && (now - r.lastMove) >= 20) {
			r.lastMove = now;
			const p = pos(e);
			r.events.push([1, p[0], p[1], 0]);
		}
	});
	const up = (e) => {
		if (r.down) {
			r.down = false;
			const p = pos(e);
			r.events.push([0, p[0], p[1], 0]);
		}
	};
	// The press turned out to be the start of a pinch, which the browser took
	// over. The device clicks where a press started, whatever happens after,
	// so it is dragged far off before it is let go: a drag, not a click.
	const cancel = (e) => {
		if (r.down) {
			r.down = false;
			const p = pos(e);
			// half the screen over, and so still on it
			const x = p[0] + ((p[0] < w / 2) ? (w >> 1) : -(w >> 1));
			r.events.push([1, x, p[1], 0]);
			r.events.push([1, x, p[1], 0]);
			r.events.push([0, x, p[1], 0]);
		}
	};
	c.addEventListener('pointerup', up);
	c.addEventListener('pointercancel', cancel);
	c.addEventListener('wheel', (e) => {
		const s = (e.deltaY < 0) ? 1 : ((e.deltaY > 0) ? -1 : 0);
		if (s !== 0) {
			const p = pos(e);
			r.events.push([r.down ? 1 : 0, p[0], p[1], s]);
		}
		e.preventDefault();
	}, { passive: false });
	c.addEventListener('contextmenu', (e) => e.preventDefault());
	close.addEventListener('click', () => { r.closed = true; });
	r.onKey = (e) => {
		if (e.key === 'Escape') {
			r.closed = true;
		}
	};
	document.addEventListener('keydown', r.onKey);
});

EM_JS(void, js_rui_close, (void), {
	const r = Module.uvremote;
	if (r) {
		document.removeEventListener('keydown', r.onKey);
		r.ov.remove();
		Module.uvremote = null;
	}
	// the keyboard goes back to the main view
	const main = document.getElementById('uvui');
	if (main) {
		main.focus();
	}
});

EM_JS(bool, js_rui_take_closed, (void), {
	const r = Module.uvremote;
	return !!(r && r.closed);
});

// Takes the oldest queued event into ev[4]; false when there is none
EM_JS(bool, js_rui_take_event, (int16_t *ev), {
	const r = Module.uvremote;
	if (!r || r.events.length === 0) {
		return false;
	}
	const e = r.events.shift();
	for (let i = 0; i < 4; i++) {
		HEAP16[(ev >> 1) + i] = e[i];
	}
	return true;
});

EM_JS(void, js_rui_stale, (int show, const char *msg, const char *age), {
	const r = Module.uvremote;
	if (r) {
		r.stale.style.display = show ? 'flex' : 'none';
		r.stale.replaceChildren();
		if (show) {
			const a = document.createElement('div');
			a.textContent = UTF8ToString(msg);
			const b = document.createElement('div');
			b.textContent = UTF8ToString(age);
			b.style.color = '#b0b0b0';
			r.stale.appendChild(a);
			r.stale.appendChild(b);
		}
	}
});

// Colours are 0xAARRGGBB
EM_JS(void, js_rui_style, (uint32_t c), {
	const r = Module.uvremote;
	const s = 'rgba(' + ((c >>> 16) & 255) + ',' + ((c >>> 8) & 255) + ',' +
			(c & 255) + ',' + (((c >>> 24) & 255) / 255) + ')';
	r.ctx.fillStyle = s;
	r.ctx.strokeStyle = s;
});

EM_JS(void, js_rui_clear, (uint32_t c), {
	const r = Module.uvremote;
	r.ctx.restore();
	r.ctx.save();
	r.ctx.fillStyle = 'rgb(' + ((c >>> 16) & 255) + ',' + ((c >>> 8) & 255) +
			',' + (c & 255) + ')';
	r.ctx.fillRect(0, 0, r.w, r.h);
});

EM_JS(void, js_rui_mask, (int x, int y, int w, int h), {
	const ctx = Module.uvremote.ctx;
	ctx.restore();
	ctx.save();
	ctx.beginPath();
	ctx.rect(x, y, w, h);
	ctx.clip();
});

EM_JS(void, js_rui_unmask, (void), {
	const ctx = Module.uvremote.ctx;
	ctx.restore();
	ctx.save();
});

EM_JS(void, js_rui_rrect, (int x, int y, int w, int h, int rad), {
	const ctx = Module.uvremote.ctx;
	const r = Math.min(rad, w / 2, h / 2);
	if (r <= 0.5) {
		ctx.fillRect(x, y, w, h);
	}
	else {
		ctx.beginPath();
		ctx.roundRect(x, y, w, h, r);
		ctx.fill();
	}
});

EM_JS(void, js_rui_circle, (double x, double y, double rad), {
	const ctx = Module.uvremote.ctx;
	ctx.beginPath();
	ctx.arc(x, y, rad, 0, 2 * Math.PI);
	ctx.fill();
});

// *pts* is x, y int16 pairs, little-endian as on the wire. A polyline of
// *width* when *width* > 0, else a filled polygon.
EM_JS(void, js_rui_path, (const uint8_t *pts, int count, int width), {
	const ctx = Module.uvremote.ctx;
	const dv = new DataView(HEAPU8.buffer, pts, count * 4);
	ctx.beginPath();
	for (let i = 0; i < count; i++) {
		const x = dv.getInt16(i * 4, true);
		const y = dv.getInt16(i * 4 + 2, true);
		if (i === 0) {
			ctx.moveTo(x, y);
		}
		else {
			ctx.lineTo(x, y);
		}
	}
	if (width > 0) {
		ctx.lineWidth = width;
		ctx.lineCap = (width > 2) ? 'round' : 'butt';
		ctx.lineJoin = 'round';
		ctx.stroke();
	}
	else {
		ctx.closePath();
		ctx.fill();
	}
});

// One line of text. *cps* are its code points and *adv* the device's width of
// each, 0 where the device has not said; *px* is the font's line height, which
// is how both ends give a font's size.
EM_JS(void, js_rui_text, (const uint32_t *cps, const uint8_t *adv, int n,
		int mono, int px, int x, int y, int halign), {
	const r = Module.uvremote;
	const ctx = r.ctx;
	const family = r.fonts[mono];
	// a face asked for N pixels lays its lines out somewhat taller than N
	ctx.font = '100px ' + family;
	const m = ctx.measureText('Hgjy');
	const lh100 = (m.fontBoundingBoxAscent + m.fontBoundingBoxDescent) || 115;
	const size = px * 100 / lh100;
	ctx.font = size + 'px ' + family;
	const asc = ctx.measureText('Hgjy').fontBoundingBoxAscent || (size * 0.9);
	const chars = [];
	const widths = [];
	let total = 0;
	for (let i = 0; i < n; i++) {
		const ch = String.fromCodePoint(HEAPU32[(cps >> 2) + i]);
		const a = HEAPU8[adv + i];
		const wdt = (a > 0) ? a : ctx.measureText(ch).width;
		chars.push(ch);
		widths.push(wdt);
		total += wdt;
	}
	let pen = x;
	if (halign === 1) {
		pen = x - Math.floor(total / 2);
	}
	else if (halign === 2) {
		pen = x - total;
	}
	ctx.textAlign = 'left';
	ctx.textBaseline = 'alphabetic';
	for (let i = 0; i < n; i++) {
		ctx.fillText(chars[i], pen, y + asc);
		pen += widths[i];
	}
});

EM_JS(int, js_rui_image_new, (const uint8_t *rgba, int w, int h), {
	const r = Module.uvremote;
	const c = document.createElement('canvas');
	c.width = w;
	c.height = h;
	c.getContext('2d').putImageData(new ImageData(
			new Uint8ClampedArray(HEAPU8.subarray(rgba, rgba + w * h * 4)), w, h), 0, 0);
	r.images.push(c);
	return r.images.length - 1;
});

// Draws image *id* at x, y, w, h, multiplied by colour *c* as the device does
EM_JS(void, js_rui_image, (int id, int x, int y, int w, int h, uint32_t c), {
	const r = Module.uvremote;
	let src = r.images[id];
	if (src) {
		const cr = (c >>> 16) & 255;
		const cg = (c >>> 8) & 255;
		const cb = c & 255;
		if (cr !== 255 || cg !== 255 || cb !== 255) {
			const key = id + ':' + cr + ',' + cg + ',' + cb;
			let t = r.tinted.get(key);
			if (!t) {
				t = document.createElement('canvas');
				t.width = src.width;
				t.height = src.height;
				const tc = t.getContext('2d');
				tc.drawImage(src, 0, 0);
				tc.globalCompositeOperation = 'multiply';
				tc.fillStyle = 'rgb(' + cr + ',' + cg + ',' + cb + ')';
				tc.fillRect(0, 0, t.width, t.height);
				tc.globalCompositeOperation = 'destination-in';
				tc.drawImage(src, 0, 0);
				r.tinted.set(key, t);
			}
			src = t;
		}
		r.ctx.globalAlpha = ((c >>> 24) & 255) / 255;
		r.ctx.drawImage(src, x, y, w, h);
		r.ctx.globalAlpha = 1;
	}
});

EM_JS(void, js_rui_outline, (int x, int y, int w, int h), {
	const ctx = Module.uvremote.ctx;
	ctx.lineWidth = 1;
	ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);
});


// ---- drawing -----------------------------------------------------------------

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


/// @brief: Draws one line, placed vertically by the caller. The widths are the
/// device's where it has sent them, so the text takes the room it takes there.
static void draw_line(const devfont_st *df, bool mono, uint16_t px,
		int16_t x, int16_t y, uint16_t align, const char *str, uint16_t len) {
	static uint32_t cps[STRING_MAX_CPS];
	static uint8_t adv[STRING_MAX_CPS];
	uint16_t n = 0;
	uint16_t i = 0;
	while ((i < len) && (n < STRING_MAX_CPS)) {
		uint32_t cp = utf8_next(str, len, &i);
		if (cp >= 0x20u) {
			uint8_t a = 0;
			if (df != NULL) {
				// the device's table is indexed by its glyph slot, which for
				// ASCII is the code point itself
				uint8_t slot = uv_ui_codepoint_glyph(cp, true);
				if (slot < UV_UI_REMOTE_FONT_WIDTHS) {
					a = df->advance[slot];
				}
			}
			cps[n] = cp;
			adv[n] = a;
			n++;
		}
	}
	int halign = 0;
	if ((align & UI_HALIGN_MASK) == UI_HALIGN_CENTER) {
		halign = 1;
	}
	else if ((align & UI_HALIGN_MASK) == UI_HALIGN_RIGHT) {
		halign = 2;
	}
	else {
	}
	if (n > 0) {
		js_rui_text(cps, adv, n, mono ? 1 : 0, px, x, y, halign);
	}
}


/// @brief: Draws a string that may hold line breaks, by the device's rules:
/// each line aligned on its own width, a drawn line advancing the baseline by
/// the font's height, an empty one advancing nothing, and a vertically centred
/// string lifted by half the height of all its lines.
static void draw_string(const devfont_st *df, bool mono, uint16_t px,
		int16_t x, int16_t y, uint16_t align, const char *str, uint16_t len) {
	uint16_t lines = 1;
	for (uint16_t i = 0; i < len; i++) {
		if (str[i] == '\n') {
			lines++;
		}
	}
	if ((align & UI_VALIGN_MASK) == UI_VALIGN_CENTER) {
		y = (int16_t) (y - ((int16_t) lines * (int16_t) px) / 2);
	}
	uint16_t start = 0;
	for (uint16_t i = 0; i <= len; i++) {
		if ((i == len) || (str[i] == '\n') || (str[i] == '\r')) {
			uint16_t line_len = (uint16_t) (i - start);
			if (line_len > 0) {
				draw_line(df, mono, px, x, y, align, &str[start], line_len);
				y = (int16_t) (y + (int16_t) px);
			}
			start = (uint16_t) (i + 1);
		}
	}
}


static devbitmap_st *devbitmap_find(uint32_t id) {
	devbitmap_st *ret = rui.devbitmaps;
	while ((ret != NULL) && (ret->id != id)) {
		ret = ret->next;
	}
	return ret;
}


static devbitmap_st *devbitmap_add(uint32_t id) {
	devbitmap_st *b = calloc(1, sizeof(*b));
	if (b != NULL) {
		b->id = id;
		b->js_id = -1;
		b->missing = true;
		b->next = rui.devbitmaps;
		rui.devbitmaps = b;
	}
	return b;
}


static void asset_request(uint8_t kind, uint32_t id) {
	if (rui.asset_req_callb != NULL) {
		rui.asset_req_callb(kind, id, rui.asset_req_user);
	}
}


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
			js_rui_clear(rd32(&p[i + 1]));
			i += 5;
			break;

		case UV_UI_REMOTE_OP_FRAME_BEGIN_KEEP:
			// drawn over what the canvas already holds
			js_rui_unmask();
			i += 1;
			break;

		case UV_UI_REMOTE_OP_POINT:
			if (left < 11) { ok = false; break; }
			js_rui_style(rd32(&p[i + 7]));
			js_rui_circle(rds16(&p[i + 1]), rds16(&p[i + 3]),
					rd16(&p[i + 5]) / 2.0);
			i += 11;
			break;

		case UV_UI_REMOTE_OP_RRECT:
			if (left < 15) { ok = false; break; }
			js_rui_style(rd32(&p[i + 11]));
			js_rui_rrect(rds16(&p[i + 1]), rds16(&p[i + 3]),
					rd16(&p[i + 5]), rd16(&p[i + 7]), rd16(&p[i + 9]));
			i += 15;
			break;

		case UV_UI_REMOTE_OP_LINE:
			if (left < 15) { ok = false; break; }
			{
				// the two end points lie back to back on the wire, which is
				// the layout the path takes
				uint16_t width = rd16(&p[i + 9]);
				js_rui_style(rd32(&p[i + 11]));
				js_rui_path(&p[i + 1], 2, (width > 0) ? width : 1);
			}
			i += 15;
			break;

		case UV_UI_REMOTE_OP_LINESTRIP:
		{
			if (left < 10) { ok = false; break; }
			uint16_t width = rd16(&p[i + 2]);
			uint32_t c = rd32(&p[i + 4]);
			uint16_t count = rd16(&p[i + 8]);
			if (left < (uint32_t) (10 + 4 * count)) { ok = false; break; }
			if (count > 1) {
				js_rui_style(c);
				js_rui_path(&p[i + 10], count, (width > 0) ? width : 1);
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
			if (count > 2) {
				js_rui_style(c);
				js_rui_path(&p[i + 7], count, 0);
			}
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
				devfont_st *df = (slot >= 0) ? &rui.devfonts[slot] : NULL;

				// Ask the device about a font it has not described yet. It is
				// drawing with it right now, so it can answer; until it does,
				// the sizes compiled in here stand in for it.
				if ((df != NULL) && !df->known && (df->retry == 0)) {
					asset_request(UV_UI_REMOTE_ASSET_KIND_FONT, font_id);
					df->retry = ASSET_RETRY_STEPS;
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
				js_rui_style(c);
				draw_string(((df != NULL) && df->have) ? df : NULL, mono, px,
						x, y, align, (const char *) &p[i + 14], slen);
			}
			i += 14u + slen;
			break;
		}

		case UV_UI_REMOTE_OP_MASK:
			if (left < 9) { ok = false; break; }
			js_rui_mask(rds16(&p[i + 1]), rds16(&p[i + 3]),
					rd16(&p[i + 5]), rd16(&p[i + 7]));
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

				// Ask for an image not seen yet while the device has it in
				// hand; what this screen does not get, assets_fetch_idle()
				// carries on asking for
				if (b == NULL) {
					b = devbitmap_add(bid);
					if (b != NULL) {
						asset_request(UV_UI_REMOTE_ASSET_KIND_BITMAP, bid);
						b->retry = ASSET_RETRY_STEPS;
					}
				}
				else if (b->missing && !b->refused && (b->retry == 0)) {
					asset_request(UV_UI_REMOTE_ASSET_KIND_BITMAP, bid);
					b->retry = ASSET_RETRY_STEPS;
				}
				else {
				}

				if ((b != NULL) && (b->js_id >= 0)) {
					js_rui_image(b->js_id, bx, by, bw, bh, bc);
				}
				else {
					// outline where it belongs while it is on its way
					js_rui_style(bc);
					js_rui_outline(bx, by, bw, bh);
				}
			}
			i += 21;
			break;

		case UV_UI_REMOTE_OP_FRAME_END:
			js_rui_unmask();
			i += 1;
			break;

		default:
			ok = false;
			break;
		}
	}

	if (!ok) {
		// a desynchronised frame; the next one starts from a clean FRAME_BEGIN
		js_rui_unmask();
	}
}


static void show_stale(void) {
	char age[64];
	snprintf(age, sizeof(age), "nothing heard from the device for %u s",
			(unsigned int) rui.link_age_s);
	js_rui_stale(rui.link_stale ? 1 : 0,
			"Waiting for the connection to come back online...", age);
}


/// @brief: Draws the last frame again, for when something it needed has just
/// arrived.
static void redraw_last(void) {
	if (rui.open && (rui.last_frame != NULL)) {
		render(rui.last_frame, rui.last_frame_len);
	}
}


// ---- interface ---------------------------------------------------------------

bool remoteui_win_open(const char *title, uint16_t width, uint16_t height) {
	bool ret = false;
	remoteui_win_close();
	if ((width != 0) && (height != 0)) {
		js_rui_open(title, width, height);
		rui.open = true;
		rui.w = width;
		rui.h = height;
		rui.link_stale = false;
		rui.link_age_s = 0;
		rui.stale_shown_s = 0;
		rui.quiet_steps = 0;
		rui.mouse_down = false;
		printf("remote UI: mirroring '%s' at %ux%u\n", title,
				(unsigned int) width, (unsigned int) height);
		fflush(stdout);
		ret = true;
	}
	return ret;
}


void remoteui_win_close(void) {
	if (rui.open) {
		if (rui.mouse_down && (rui.input_callb != NULL)) {
			// closing mid-press would leave the device holding a touch nobody
			// is making any more
			rui.input_callb((uint8_t) UV_UI_REMOTE_INPUT_RELEASE, rui.last_x,
					rui.last_y, 0, '\0', rui.input_user);
		}
		js_rui_close();
		while (rui.devbitmaps != NULL) {
			devbitmap_st *next = rui.devbitmaps->next;
			free(rui.devbitmaps);
			rui.devbitmaps = next;
		}
		free(rui.last_frame);
		rui.last_frame = NULL;
		rui.last_frame_len = 0;
		// a new session may be another device: nothing about this one's fonts
		// carries over
		memset(rui.devfonts, 0, sizeof(rui.devfonts));
		rui.open = false;
		rui.mouse_down = false;
		rui.link_stale = false;
	}
}


bool remoteui_win_is_open(void) {
	return rui.open;
}


void remoteui_win_draw_frame(const uint8_t *cmds, uint32_t len) {
	if (rui.open && (cmds != NULL) && (len > 0)) {
		// a frame is the device speaking: the view is live again
		if (rui.link_stale) {
			rui.link_stale = false;
			show_stale();
		}
		rui.link_age_s = 0;
		rui.stale_shown_s = 0;
		rui.quiet_steps = 0;
		render(cmds, len);
		uint8_t *copy = realloc(rui.last_frame, len);
		if (copy != NULL) {
			memcpy(copy, cmds, len);
			rui.last_frame = copy;
			rui.last_frame_len = len;
		}
		else {
			free(rui.last_frame);
			rui.last_frame = NULL;
			rui.last_frame_len = 0;
		}
	}
}


void remoteui_win_set_input_callb(remoteui_input_t callb, void *user) {
	rui.input_callb = callb;
	rui.input_user = user;
}


void remoteui_win_set_asset_request_callb(remoteui_asset_req_t callb,
		void *user) {
	rui.asset_req_callb = callb;
	rui.asset_req_user = user;
}


void remoteui_win_asset_received(uint8_t kind, uint32_t id,
		const uint8_t *data, uint32_t len) {
	// an asset is the link working; let it finish before asking for more
	rui.quiet_steps = 0;
	if (kind == UV_UI_REMOTE_ASSET_KIND_FONT) {
		int16_t slot = devfont_slot((uint8_t) id);
		if (slot >= 0) {
			devfont_st *df = &rui.devfonts[slot];
			// answered either way: an empty answer means the device has no
			// such font, and asking again would get the same reply
			df->known = true;
			df->retry = 0;
			if ((data != NULL) && (len >= UV_UI_REMOTE_FONT_BODY_HDR_LEN)) {
				df->height = (uint16_t) (data[0] | (data[1] << 8));
				memcpy(df->advance, &data[7], UV_UI_REMOTE_FONT_WIDTHS);
				df->have = (df->height > 0);
				// the screen may sit still for a while: draw it with the
				// metrics it was waiting for now
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
			b = devbitmap_add(id);
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
				// no such image, or one this cannot read: asking again would
				// not help
				b->missing = true;
				b->refused = true;
				printf("remote UI: image %u could not be decoded (%u bytes)\n",
						(unsigned int) id, (unsigned int) len);
				fflush(stdout);
			}
			else {
				b->js_id = js_rui_image_new(px, w, h);
				stbi_image_free(px);
				b->missing = false;
				redraw_last();
			}
		}
	}
	else {
	}
}


void remoteui_win_set_link_age(uint32_t seconds) {
	rui.link_age_s = seconds;
	if (rui.open) {
		bool stale = (seconds >= STALE_AGE_S);
		// the seconds are part of what is shown, so it is redone whenever they
		// move on as well as when the view goes stale or comes back
		if ((stale != rui.link_stale) ||
				(stale && (seconds != rui.stale_shown_s))) {
			rui.link_stale = stale;
			rui.stale_shown_s = seconds;
			show_stale();
		}
	}
	else {
		rui.link_stale = false;
	}
}


/// @brief: Asks the device for one image the view is still missing, when the
/// link has been quiet long enough to spare it. The device serves one asset at
/// a time and remembers only a few requests, so a screen with many images
/// fills in one quiet gap at a time.
static void assets_fetch_idle(void) {
	if (!rui.link_stale) {
		devbitmap_st *want = NULL;
		for (devbitmap_st *b = rui.devbitmaps; (b != NULL) && (want == NULL);
				b = b->next) {
			if (b->missing && !b->refused && (b->retry == 0)) {
				want = b;
			}
		}
		if (want != NULL) {
			asset_request(UV_UI_REMOTE_ASSET_KIND_BITMAP, want->id);
			want->retry = ASSET_RETRY_STEPS;
			rui.quiet_steps = 0;
		}
	}
}


void remoteui_win_step(void) {
	if (rui.open) {
		// count down the waits for answers, so a lost request is made again
		for (uint16_t i = 0; i < DEVFONT_COUNT; i++) {
			if (rui.devfonts[i].retry > 0) {
				rui.devfonts[i].retry--;
			}
		}
		for (devbitmap_st *b = rui.devbitmaps; b != NULL; b = b->next) {
			if (b->retry > 0) {
				b->retry--;
			}
		}
		if (rui.quiet_steps < ASSET_IDLE_STEPS) {
			rui.quiet_steps++;
		}
		else {
			assets_fetch_idle();
		}

		// what the user did on the view, to the device as its own screen
		// being touched
		int16_t ev[4];
		while (js_rui_take_event(ev)) {
			rui.mouse_down = (ev[0] != 0);
			rui.last_x = ev[1];
			rui.last_y = ev[2];
			if (rui.input_callb != NULL) {
				rui.input_callb((uint8_t) ev[0], ev[1], ev[2], ev[3], '\0',
						rui.input_user);
			}
		}

		if (js_rui_take_closed()) {
			printf("remote UI: window closed\n");
			fflush(stdout);
			remoteui_win_close();
		}
	}
}


void remoteui_win_poll_events(void) {
	// the page delivers its events by itself; remoteui_win_step() takes them
}

#endif
