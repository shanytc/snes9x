/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The gui module: drawing commands each script records into its layer, and
// the compositor that blends the layers onto the finished frame. Coordinates
// are SNES pixels; hires and interlaced frames scale them up.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "lua/lua_internal.h"
#include "lua/lua_font_small.h"
#include "font.h"
#include "var8x10font.h"
#include "external/stb/stb_image.h"
#include "memmap.h"
#include "gfx.h"
#include "ppu.h"
#include "sgb/sgb.h"

namespace
{

const size_t	kMaxLayer = 100000;	// commands per script between clears

//------------------------------------------------------------------------------
// Fonts

enum { FONT_NORMAL, FONT_FIXED, FONT_SMALL, FONT_COUNT };

struct Glyph
{
	uint8	width;
	uint8	rows[8];	// bit x is column x
};

struct Font
{
	const char	*name;
	int			height, line_height;
	Glyph		glyphs[95];
};

Font	fonts[FONT_COUNT];
bool	fonts_built = false;

void BuildFonts (void)
{
	if (fonts_built)
		return;
	fonts_built = true;

	// The OSD fonts draw their own outline around the ink; keep only the ink.
	Font	&normal = fonts[FONT_NORMAL];
	normal = { "normal", 8, 9, {} };
	for (int c = 0; c < 95; c++)
	{
		const char	**cell = &var8x10font[(c >> 4) * 10 + 1];
		int			col = (c & 15) * 8, first = 8, last = -1;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				if (cell[y][col + x] == '#')
				{
					first = std::min(first, x);
					last = std::max(last, x);
				}

		Glyph	&g = normal.glyphs[c];
		if (last < 0)
		{
			g.width = 2;
			continue;
		}
		g.width = last - first + 1;
		for (int y = 0; y < 8; y++)
			for (int x = first; x <= last; x++)
				if (cell[y][col + x] == '#')
					g.rows[y] |= 1 << (x - first);
	}

	Font	&fixed = fonts[FONT_FIXED];
	fixed = { "fixed", 7, 8, {} };
	for (int c = 0; c < 95; c++)
	{
		const char	**cell = &font[(c >> 4) * 9 + 1];
		int			col = (c & 15) * 8 + 1;
		Glyph		&g = fixed.glyphs[c];
		g.width = 5;
		for (int y = 0; y < 7; y++)
			for (int x = 0; x < 5; x++)
				if (cell[y][col + x] == '#')
					g.rows[y] |= 1 << x;
	}

	Font	&tiny = fonts[FONT_SMALL];
	tiny = { "small", 5, 6, {} };
	for (int c = 0; c < 95; c++)
	{
		Glyph	&g = tiny.glyphs[c];
		g.width = 3;
		for (int y = 0; y < 5; y++)
			for (int x = 0; x < 3; x++)
				if (lua_font_small[c][y][x] == '#')
					g.rows[y] |= 1 << x;
	}
}

// One glyph index per character: UTF-8 is decoded, anything outside
// printable ASCII shows as '?'. -1 marks a line break.
std::vector<int> Characters (const std::string &text)
{
	std::vector<int>	out;
	for (size_t i = 0; i < text.size();)
	{
		uint8	c = text[i];
		int		length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
		i += length;
		if (c == '\n')
			out.push_back(-1);
		else if (c == '\t')
			out.push_back(0);
		else if (length == 1 && c >= 32 && c < 127)
			out.push_back(c - 32);
		else if (c != '\r')
			out.push_back('?' - 32);
	}
	return out;
}

void MeasureText (const Font &f, const std::vector<int> &chars, int &width, int &height)
{
	int	lines = 1, line_width = 0;
	width = 0;
	for (size_t i = 0; i < chars.size(); i++)
	{
		if (chars[i] < 0)
		{
			lines++;
			line_width = 0;
			continue;
		}
		if (line_width)
			line_width++;
		line_width += f.glyphs[chars[i]].width;
		width = std::max(width, line_width);
	}
	height = (lines - 1) * f.line_height + f.height;
}

//------------------------------------------------------------------------------
// Colours: 0xAARRGGBB inside; an alpha of 0 draws nothing.

const struct
{
	const char	*name;
	uint32		argb;
} named_colors[] =
{
	{ "black", 0xff000000 }, { "white", 0xffffffff }, { "red", 0xffff0000 },
	{ "green", 0xff00ff00 }, { "blue", 0xff0000ff }, { "yellow", 0xffffff00 },
	{ "cyan", 0xff00ffff }, { "magenta", 0xffff00ff }, { "orange", 0xffff8000 },
	{ "purple", 0xff8000ff }, { "pink", 0xffff80c0 }, { "brown", 0xff804000 },
	{ "gray", 0xff808080 }, { "grey", 0xff808080 }, { "lightgray", 0xffc0c0c0 },
	{ "darkgray", 0xff404040 }, { "none", 0 }, { "clear", 0 }, { "transparent", 0 }
};

int HexDigit (char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool ParseColorString (const char *s, uint32 &argb)
{
	if (s[0] != '#')
	{
		std::string	lower;
		for (const char *p = s; *p; p++)
			lower += (char) tolower((uint8) *p);
		for (auto &c : named_colors)
			if (lower == c.name)
			{
				argb = c.argb;
				return true;
			}
		return false;
	}

	size_t	n = strlen(s + 1);
	uint32	v = 0;
	for (size_t i = 1; i <= n; i++)
	{
		int	d = HexDigit(s[i]);
		if (d < 0)
			return false;
		v = (v << 4) | d;
	}

	switch (n)
	{
		case 3:
			argb = 0xff000000 | ((v & 0xf00) * 0x1100) | ((v & 0xf0) * 0x110) | ((v & 0xf) * 0x11);
			return true;
		case 6:
			argb = 0xff000000 | v;
			return true;
		case 8:
			argb = (v >> 8) | (v << 24);	// CSS order: #RRGGBBAA
			return true;
		default:
			return false;
	}
}

int Component (lua_State *L, int table, int index, const char *field, int fallback)
{
	lua_geti(L, table, index);
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		lua_getfield(L, table, field);
	}
	int	v = lua_isnil(L, -1) ? fallback : (int) luaL_checkinteger(L, -1);
	lua_pop(L, 1);
	return std::clamp(v, 0, 255);
}

uint32 Fade (lua_State *L, uint32 argb)
{
	float	opacity = LuaScriptOf(L)->opacity;
	if (opacity >= 1.0f)
		return argb;
	uint32	a = (uint32) ((argb >> 24) * opacity + 0.5f);
	return (argb & 0xffffff) | (a << 24);
}

uint32 OptColor (lua_State *L, int index, uint32 fallback)
{
	return Fade(L, LuaCheckColor(L, index, fallback));
}

// A shape's outline: left out means white, an explicit nil means none.
uint32 OutlineColor (lua_State *L, int index)
{
	return lua_type(L, index) == LUA_TNIL ? 0 : OptColor(L, index, 0xffffffff);
}

//------------------------------------------------------------------------------
// Compositing

inline void Blend (uint16 &dst, uint32 argb)
{
	int	a = argb >> 24;
	int	r = (argb >> 19) & 0x1f, g = (argb >> 11) & 0x1f, b = (argb >> 3) & 0x1f;
	if (a < 255)
	{
		uint32	dr, dg, db;
		DECOMPOSE_PIXEL(dst, dr, dg, db);
		r = dr + (r - (int) dr) * a / 255;
		g = dg + (g - (int) dg) * a / 255;
		b = db + (b - (int) db) * a / 255;
	}
	dst = (uint16) BUILD_PIXEL(r, g, b);
}

struct Target
{
	uint16	*screen;
	int		pitch;
	int		sx, sy, ox;					// physical pixels per SNES pixel; columns left of x = 0
	int		left, top, right, bottom;	// visible SNES pixels, right/bottom exclusive
	int		pw, ph;						// physical pixels drawn on

	Target (uint16 *screen_, int pitch_, int width, int height)
		: screen(screen_), pitch(pitch_)
	{
		ox = Settings.SuperGameBoy ? 0 : IPPU.WideExtent;
		int	base = Settings.SuperGameBoy ? (int) SGB_GB_SCREEN_W : SNES_WIDTH + 2 * ox;
		sx = width >= base * 2 ? 2 : 1;
		sy = height > 256 ? 2 : 1;
		left = -ox;
		top = 0;
		right = width / sx - ox;
		bottom = height / sy;
		pw = (right + ox) * sx;
		ph = bottom * sy;
	}

	void Fill (int x, int y, int w, int h, uint32 argb)
	{
		if (!(argb >> 24) || w <= 0 || h <= 0)
			return;
		int	x0 = std::max(x, left), x1 = std::min(x + w, right);
		int	y0 = std::max(y, top), y1 = std::min(y + h, bottom);
		if (x0 >= x1 || y0 >= y1)
			return;

		for (int py = y0 * sy; py < y1 * sy; py++)
		{
			uint16	*row = screen + (size_t) py * pitch;
			for (int px = (x0 + ox) * sx; px < (x1 + ox) * sx; px++)
				Blend(row[px], argb);
		}
	}

	void Plot (int x, int y, uint32 argb)
	{
		Fill(x, y, 1, 1, argb);
	}
};

// Marks pixels once so overlapping strokes don't blend twice.
struct Mask
{
	int		x0, y0, w, h;
	std::vector<uint8>	bits;

	Mask (const Target &t, int bx0, int by0, int bx1, int by1)	// inclusive box
	{
		x0 = std::max(bx0, t.left);
		y0 = std::max(by0, t.top);
		w = std::max(0, std::min(bx1 + 1, t.right) - x0);
		h = std::max(0, std::min(by1 + 1, t.bottom) - y0);
		bits.assign((size_t) w * h, 0);
	}

	void Set (int x, int y, uint8 v)
	{
		x -= x0;
		y -= y0;
		if (x >= 0 && y >= 0 && x < w && y < h)
			bits[(size_t) y * w + x] = v;
	}

	uint8 Get (int x, int y) const
	{
		x -= x0;
		y -= y0;
		return x >= 0 && y >= 0 && x < w && y < h ? bits[(size_t) y * w + x] : 0;
	}
};

template <typename F>
void Bresenham (int x1, int y1, int x2, int y2, F plot)
{
	int	dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
	int	dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
	int	err = dx + dy;
	for (int guard = 0; guard < 8192; guard++)
	{
		plot(x1, y1);
		if (x1 == x2 && y1 == y2)
			break;
		int	e2 = 2 * err;
		if (e2 >= dy)
		{
			err += dy;
			x1 += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y1 += sy;
		}
	}
}

void DrawRect (Target &t, const LuaDrawCmd &c)
{
	int	x = c.x, y = c.y, w = c.w, h = c.h;
	if (w <= 0 || h <= 0)
		return;
	bool	outline = (c.color >> 24) != 0;
	if (outline && w > 2 && h > 2)
		t.Fill(x + 1, y + 1, w - 2, h - 2, c.fill);
	else if (!outline)
		t.Fill(x, y, w, h, c.fill);

	if (!outline)
		return;
	t.Fill(x, y, w, 1, c.color);
	if (h > 1)
		t.Fill(x, y + h - 1, w, 1, c.color);
	if (h > 2)
	{
		t.Fill(x, y + 1, 1, h - 2, c.color);
		if (w > 1)
			t.Fill(x + w - 1, y + 1, 1, h - 2, c.color);
	}
}

void DrawEllipse (Target &t, const LuaDrawCmd &c)
{
	if (c.w <= 0 || c.h <= 0)
		return;
	double	rx = c.w / 2.0, ry = c.h / 2.0;
	double	cx = c.x + rx, cy = c.y + ry;
	auto	inside = [&](int x, int y) {
		if (x < c.x || y < c.y || x >= c.x + c.w || y >= c.y + c.h)
			return false;
		double	dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
		return dx * dx + dy * dy <= 1.0 + 1e-9;
	};

	bool	outline = (c.color >> 24) != 0;
	int		x0 = std::max(c.x, t.left), x1 = std::min(c.x + c.w, t.right);
	int		y0 = std::max(c.y, t.top), y1 = std::min(c.y + c.h, t.bottom);
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
		{
			if (!inside(x, y))
				continue;
			bool	edge = !inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1);
			if (outline && edge)
				t.Plot(x, y, c.color);
			else
				t.Plot(x, y, c.fill);
		}
}

void DrawPolygon (Target &t, const LuaDrawCmd &c)
{
	std::vector<int>	p(c.points.size());
	for (size_t i = 0; i < p.size(); i++)
		p[i] = (int) c.points[i];
	size_t	n = p.size() / 2;
	if (n == 0)
		return;

	int	bx0 = p[0], by0 = p[1], bx1 = p[0], by1 = p[1];
	for (size_t i = 1; i < n; i++)
	{
		bx0 = std::min(bx0, p[i * 2]);
		bx1 = std::max(bx1, p[i * 2]);
		by0 = std::min(by0, p[i * 2 + 1]);
		by1 = std::max(by1, p[i * 2 + 1]);
	}

	Mask	edge(t, bx0, by0, bx1, by1);
	if (edge.w == 0 || edge.h == 0)
		return;

	if (c.color >> 24)
		for (size_t i = 0; i < n; i++)
		{
			size_t	j = (i + 1) % n;
			Bresenham(p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1], [&](int x, int y) { edge.Set(x, y, 1); });
		}

	if ((c.fill >> 24) && n >= 3)
	{
		std::vector<double>	xs;
		for (int y = edge.y0; y < edge.y0 + edge.h; y++)
		{
			double	sy = y + 0.5;
			xs.clear();
			for (size_t i = 0; i < n; i++)
			{
				size_t	j = (i + 1) % n;
				double	ya = p[i * 2 + 1], yb = p[j * 2 + 1];
				if ((ya <= sy && yb > sy) || (yb <= sy && ya > sy))
					xs.push_back(p[i * 2] + (sy - ya) / (yb - ya) * (p[j * 2] - p[i * 2]));
			}
			std::sort(xs.begin(), xs.end());
			for (size_t k = 0; k + 1 < xs.size(); k += 2)
				for (int x = (int) std::ceil(xs[k] - 0.5); x <= (int) std::floor(xs[k + 1] - 0.5); x++)
					if (!edge.Get(x, y))
						t.Plot(x, y, c.fill);
		}
	}

	for (int y = 0; y < edge.h; y++)
		for (int x = 0; x < edge.w; x++)
			if (edge.bits[(size_t) y * edge.w + x])
				t.Plot(edge.x0 + x, edge.y0 + y, c.color);
}

void DrawText (Target &t, const LuaDrawCmd &c)
{
	const Font	&f = fonts[c.font];
	std::vector<int>	chars = Characters(c.text);
	int		w, h;
	MeasureText(f, chars, w, h);
	int		scale = c.scale;

	int	x = c.x, y = c.y;
	if (c.align == 0)
		x -= w * scale / 2;
	else if (c.align > 0)
		x -= w * scale;
	if (c.valign == 0)
		y -= h * scale / 2;
	else if (c.valign > 0)
		y -= h * scale;

	// Ink at glyph resolution with a one-pixel border for the outline.
	int		mw = w + 2, mh = h + 2;
	std::vector<uint8>	ink((size_t) mw * mh, 0);
	int		pen_x = 0, pen_y = 0;
	for (int ch : chars)
	{
		if (ch < 0)
		{
			pen_x = 0;
			pen_y += f.line_height;
			continue;
		}
		const Glyph	&g = f.glyphs[ch];
		if (pen_x)
			pen_x++;
		for (int gy = 0; gy < f.height; gy++)
			for (int gx = 0; gx < g.width; gx++)
				if (g.rows[gy] & (1 << gx))
					ink[(size_t) (pen_y + gy + 1) * mw + pen_x + gx + 1] = 1;
		pen_x += g.width;
	}

	if (c.fill >> 24)
		t.Fill(x - scale, y - scale, mw * scale, mh * scale, c.fill);

	bool	outline = (c.outline >> 24) != 0;
	for (int my = 0; my < mh; my++)
		for (int mx = 0; mx < mw; mx++)
		{
			uint32	color = 0;
			if (ink[(size_t) my * mw + mx])
				color = c.color;
			else if (outline)
			{
				bool	touching = false;
				for (int dy = -1; dy <= 1 && !touching; dy++)
					for (int dx = -1; dx <= 1 && !touching; dx++)
					{
						int	nx = mx + dx, ny = my + dy;
						touching = nx >= 0 && ny >= 0 && nx < mw && ny < mh && ink[(size_t) ny * mw + nx];
					}
				if (touching)
					color = c.outline;
			}
			if (color)
				t.Fill(x + (mx - 1) * scale, y + (my - 1) * scale, scale, scale, color);
		}
}

void DrawImage (Target &t, const LuaDrawCmd &c)
{
	const LuaImage	&img = *c.image;
	int	scale = c.scale;
	for (int y = 0; y < c.h; y++)
	{
		int	iy = c.sy + y;
		if (iy < 0 || iy >= img.height)
			continue;
		for (int x = 0; x < c.w; x++)
		{
			int	ix = c.sx + x;
			if (ix < 0 || ix >= img.width)
				continue;
			uint32	argb = img.argb[(size_t) iy * img.width + ix];
			uint32	a = (argb >> 24) * c.alpha / 255;
			if (a)
				t.Fill(c.x + x * scale, c.y + y * scale, scale, scale, (argb & 0xffffff) | (a << 24));
		}
	}
}

//------------------------------------------------------------------------------
// Anti-aliased shapes. Each pixel row is cut by kSubRows lines and each line's
// spans give exact horizontal coverage. Outlines are 1 pixel wide.

const int	kSubRows = 5;

struct Span
{
	double	a, b;
};

uint32 WithAlpha (uint32 argb, double alpha)
{
	return (argb & 0xffffff) | (uint32) std::min(alpha * 255 + 0.5, 255.0) << 24;
}

// Merges the spans and adds each pixel's share of them to cov.
void Cover (std::vector<Span> &spans, std::vector<float> &cov, double scale, double offset)
{
	std::sort(spans.begin(), spans.end(), [](const Span &p, const Span &q) { return p.a < q.a; });
	for (size_t i = 0; i < spans.size();)
	{
		double	a = spans[i].a, b = spans[i].b;
		for (i++; i < spans.size() && spans[i].a <= b; i++)
			b = std::max(b, spans[i].b);
		a = std::max(a * scale + offset, 0.0);
		b = std::min(b * scale + offset, (double) cov.size());
		if (a >= b)
			continue;
		int	ia = (int) a, ib = (int) b;
		if (ia == ib)
		{
			cov[ia] += (float) (b - a);
			continue;
		}
		cov[ia] += (float) (ia + 1 - a);
		for (int k = ia + 1; k < ib; k++)
			cov[k] += 1.0f;
		if (ib < (int) cov.size())
			cov[ib] += (float) (b - ib);
	}
}

// spans(y, stroke, area) lists what the row at y crosses: the outline, then
// the inside. The box bounds the shape.
template <typename F>
void DrawSmooth (Target &t, const LuaDrawCmd &c, double x0, double y0, double x1, double y1, F spans)
{
	int	px0 = (int) std::max(std::floor((x0 + t.ox) * t.sx), 0.0);
	int	px1 = (int) std::min(std::ceil((x1 + t.ox) * t.sx), (double) t.pw);
	int	py0 = (int) std::max(std::floor(y0 * t.sy), 0.0);
	int	py1 = (int) std::min(std::ceil(y1 * t.sy), (double) t.ph);
	if (px0 >= px1 || py0 >= py1)
		return;

	std::vector<float>	stroke_cov(px1 - px0), all_cov(px1 - px0);
	std::vector<Span>	stroke, area;
	double	ao = (c.color >> 24) / 255.0, af = (c.fill >> 24) / 255.0;
	double	offset = (double) t.ox * t.sx - px0;
	for (int py = py0; py < py1; py++)
	{
		std::fill(stroke_cov.begin(), stroke_cov.end(), 0.0f);
		std::fill(all_cov.begin(), all_cov.end(), 0.0f);
		for (int k = 0; k < kSubRows; k++)
		{
			stroke.clear();
			area.clear();
			spans((py + (k + 0.5) / kSubRows) / t.sy, stroke, area);
			area.insert(area.end(), stroke.begin(), stroke.end());
			Cover(stroke, stroke_cov, t.sx, offset);
			Cover(area, all_cov, t.sx, offset);
		}

		// The outline over the fill: fill alpha wf / (1 - wo), then outline wo.
		uint16	*row = t.screen + (size_t) py * t.pitch + px0;
		for (int i = 0; i < px1 - px0; i++)
		{
			double	co = std::min(stroke_cov[i] / kSubRows, 1.0f);
			double	cf = std::max(std::min(all_cov[i] / kSubRows, 1.0f) - co, 0.0);
			double	wo = co * ao, wf = cf * af;
			if (wf > 0 && wo < 1)
				Blend(row[i], WithAlpha(c.fill, wf / (1 - wo)));
			if (wo > 0)
				Blend(row[i], WithAlpha(c.color, wo));
		}
	}
}

// Where the row at y crosses the points within half a pixel of segment a-b.
bool SegmentSpan (double y, double ax, double ay, double bx, double by, Span &s)
{
	const double	r = 0.5;
	if (y <= std::min(ay, by) - r || y >= std::max(ay, by) + r)
		return false;

	// The round ends and the band between them are convex, so the hull of
	// their crossings is the crossing of the whole.
	double	lo = HUGE_VAL, hi = -HUGE_VAL;
	auto	take = [&](double x) {
		lo = std::min(lo, x);
		hi = std::max(hi, x);
	};
	auto	end = [&](double cx, double cy) {
		double	d = y - cy;
		if (d * d < r * r)
		{
			double	h = std::sqrt(r * r - d * d);
			take(cx - h);
			take(cx + h);
		}
	};
	end(ax, ay);
	end(bx, by);

	double	dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
	if (len > 0)
	{
		double	nx = -dy / len * r, ny = dx / len * r;
		double	qx[4] = { ax + nx, bx + nx, bx - nx, ax - nx };
		double	qy[4] = { ay + ny, by + ny, by - ny, ay - ny };
		for (int i = 0; i < 4; i++)
		{
			int	j = (i + 1) & 3;
			if ((qy[i] <= y) != (qy[j] <= y))
				take(qx[i] + (y - qy[i]) / (qy[j] - qy[i]) * (qx[j] - qx[i]));
		}
	}
	if (lo > hi)
		return false;
	s = { lo, hi };
	return true;
}

void SmoothLine (Target &t, const LuaDrawCmd &c)
{
	const double	*p = c.points.data();
	DrawSmooth(t, c, std::min(p[0], p[2]) - 1, std::min(p[1], p[3]) - 1, std::max(p[0], p[2]) + 1, std::max(p[1], p[3]) + 1,
		[&](double y, std::vector<Span> &stroke, std::vector<Span> &) {
			Span	s;
			if (SegmentSpan(y, p[0], p[1], p[2], p[3], s))
				stroke.push_back(s);
		});
}

void SmoothEllipse (Target &t, const LuaDrawCmd &c)
{
	double	cx = c.points[0], cy = c.points[1], rx = c.points[2], ry = c.points[3];
	if (rx <= 0 || ry <= 0)
		return;
	bool	outline = (c.color >> 24) != 0;
	DrawSmooth(t, c, cx - rx, cy - ry, cx + rx, cy + ry,
		[&](double y, std::vector<Span> &stroke, std::vector<Span> &area) {
			auto	chord = [&](double a, double b, Span &s) {
				if (a <= 0 || b <= 0)
					return false;
				double	d = (y - cy) / b;
				if (d * d >= 1)
					return false;
				double	h = a * std::sqrt(1 - d * d);
				s = { cx - h, cx + h };
				return true;
			};
			Span	o, i;
			if (!chord(rx, ry, o))
				return;
			area.push_back(o);
			if (!outline)
				return;
			if (chord(rx - 1, ry - 1, i))
			{
				stroke.push_back({ o.a, i.a });
				stroke.push_back({ i.b, o.b });
			}
			else
				stroke.push_back(o);
		});
}

void SmoothPolygon (Target &t, const LuaDrawCmd &c)
{
	const std::vector<double>	&p = c.points;
	size_t	n = p.size() / 2;
	if (n == 0)
		return;

	double	x0 = p[0], y0 = p[1], x1 = p[0], y1 = p[1];
	for (size_t i = 1; i < n; i++)
	{
		x0 = std::min(x0, p[i * 2]);
		x1 = std::max(x1, p[i * 2]);
		y0 = std::min(y0, p[i * 2 + 1]);
		y1 = std::max(y1, p[i * 2 + 1]);
	}

	// Rows come top to bottom, so only the edges near the row are looked at.
	struct Edge
	{
		double	ax, ay, bx, by, top, bottom;
	};
	std::vector<Edge>	edges(n);
	for (size_t i = 0; i < n; i++)
	{
		size_t	j = (i + 1) % n;
		double	ay = p[i * 2 + 1], by = p[j * 2 + 1];
		edges[i] = { p[i * 2], ay, p[j * 2], by, std::min(ay, by) - 0.5, std::max(ay, by) + 0.5 };
	}
	std::sort(edges.begin(), edges.end(), [](const Edge &e, const Edge &f) { return e.top < f.top; });

	bool	outline = (c.color >> 24) != 0;
	std::vector<double>	xs;
	std::vector<const Edge *>	active;
	size_t	next = 0;
	DrawSmooth(t, c, x0 - 1, y0 - 1, x1 + 1, y1 + 1,
		[&](double y, std::vector<Span> &stroke, std::vector<Span> &area) {
			while (next < n && edges[next].top < y)
				active.push_back(&edges[next++]);
			active.erase(std::remove_if(active.begin(), active.end(), [&](const Edge *e) { return e->bottom <= y; }), active.end());

			xs.clear();
			for (const Edge *e : active)
			{
				if (n >= 3 && ((e->ay <= y && e->by > y) || (e->by <= y && e->ay > y)))
					xs.push_back(e->ax + (y - e->ay) / (e->by - e->ay) * (e->bx - e->ax));
				Span	s;
				if (outline && SegmentSpan(y, e->ax, e->ay, e->bx, e->by, s))
					stroke.push_back(s);
			}
			std::sort(xs.begin(), xs.end());
			for (size_t k = 0; k + 1 < xs.size(); k += 2)
				area.push_back({ xs[k], xs[k + 1] });
		});
}

void Draw (Target &t, const LuaDrawCmd &c)
{
	switch (c.type)
	{
		case LuaDrawCmd::PIXEL:
			t.Plot(c.x, c.y, c.color);
			break;
		case LuaDrawCmd::LINE:
			if (c.smooth)
				SmoothLine(t, c);
			else
				Bresenham(c.x, c.y, c.w, c.h, [&](int x, int y) { t.Plot(x, y, c.color); });
			break;
		case LuaDrawCmd::RECT:
			DrawRect(t, c);
			break;
		case LuaDrawCmd::ELLIPSE:
			if (c.smooth)
				SmoothEllipse(t, c);
			else
				DrawEllipse(t, c);
			break;
		case LuaDrawCmd::POLYGON:
			if (c.smooth)
				SmoothPolygon(t, c);
			else
				DrawPolygon(t, c);
			break;
		case LuaDrawCmd::TEXT:
			DrawText(t, c);
			break;
		case LuaDrawCmd::IMAGE:
			DrawImage(t, c);
			break;
	}
}

//------------------------------------------------------------------------------
// Lua functions

LuaDrawCmd &Record (lua_State *L, LuaDrawCmd::Type type)
{
	LuaScript	*s = LuaScriptOf(L);
	if (s->layer.size() >= kMaxLayer)
		luaL_error(L, "more than %d drawings without a clear; call gui.clear()", (int) kMaxLayer);
	s->layer.emplace_back();
	s->layer.back().type = type;
	s->layer.back().smooth = s->antialias;
	LuaEng.overlay_dirty = true;
	return s->layer.back();
}

double CheckReal (lua_State *L, int arg)
{
	return std::clamp((double) luaL_checknumber(L, arg), -1e6, 1e6);
}

int CheckCoord (lua_State *L, int arg)
{
	return (int) std::floor(CheckReal(L, arg));
}

int gui_pixel (lua_State *L)
{
	int		x = CheckCoord(L, 1), y = CheckCoord(L, 2);
	uint32	color = OptColor(L, 3, 0xffffffff);
	LuaDrawCmd	&c = Record(L, LuaDrawCmd::PIXEL);
	c.x = x;
	c.y = y;
	c.color = color;
	return 0;
}

int gui_line (lua_State *L)
{
	double	x1 = CheckReal(L, 1), y1 = CheckReal(L, 2), x2 = CheckReal(L, 3), y2 = CheckReal(L, 4);
	uint32	color = OptColor(L, 5, 0xffffffff);
	LuaDrawCmd	&c = Record(L, LuaDrawCmd::LINE);
	c.x = (int) std::floor(x1);
	c.y = (int) std::floor(y1);
	c.w = (int) std::floor(x2);
	c.h = (int) std::floor(y2);
	c.color = color;
	if (c.smooth)
		c.points = { x1 + 0.5, y1 + 0.5, x2 + 0.5, y2 + 0.5 };	// pixel centres
	return 0;
}

template <LuaDrawCmd::Type type>
int gui_shape (lua_State *L)
{
	double	x = CheckReal(L, 1), y = CheckReal(L, 2), w = CheckReal(L, 3), h = CheckReal(L, 4);
	uint32	outline = OutlineColor(L, 5);
	uint32	fill = OptColor(L, 6, 0);
	LuaDrawCmd	&c = Record(L, type);
	c.x = (int) std::floor(x);
	c.y = (int) std::floor(y);
	c.w = (int) std::floor(w);
	c.h = (int) std::floor(h);
	c.color = outline;
	c.fill = fill;
	if (type == LuaDrawCmd::ELLIPSE && c.smooth)
		c.points = { x + w / 2, y + h / 2, w / 2, h / 2 };
	return 0;
}

int gui_circle (lua_State *L)
{
	double	cx = CheckReal(L, 1), cy = CheckReal(L, 2), r = CheckReal(L, 3);
	uint32	outline = OutlineColor(L, 4);
	uint32	fill = OptColor(L, 5, 0);
	LuaDrawCmd	&c = Record(L, LuaDrawCmd::ELLIPSE);
	int		ir = (int) std::floor(r);
	c.x = (int) std::floor(cx) - ir;
	c.y = (int) std::floor(cy) - ir;
	c.w = c.h = ir * 2 + 1;
	c.color = outline;
	c.fill = fill;
	if (c.smooth)
		c.points = { cx + 0.5, cy + 0.5, r + 0.5, r + 0.5 };	// the same disc, unrounded
	return 0;
}

// gui.polygon({x1, y1, x2, y2, ...} or {{x1, y1}, ...}, outline, fill)
int gui_polygon (lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	std::vector<double>	points;
	lua_Integer	n = luaL_len(L, 1);
	for (lua_Integer i = 1; i <= n; i++)
	{
		lua_geti(L, 1, i);
		if (lua_istable(L, -1))
		{
			lua_geti(L, -1, 1);
			lua_geti(L, -2, 2);
			points.push_back(CheckReal(L, -2));
			points.push_back(CheckReal(L, -1));
			lua_pop(L, 2);
		}
		else
			points.push_back(CheckReal(L, -1));
		lua_pop(L, 1);
	}
	if (points.size() % 2)
		luaL_argerror(L, 1, "expected pairs of coordinates");

	uint32	outline = OutlineColor(L, 2);
	uint32	fill = OptColor(L, 3, 0);
	LuaDrawCmd	&c = Record(L, LuaDrawCmd::POLYGON);
	for (double &v : points)
		v = c.smooth ? v + 0.5 : std::floor(v);	// smooth: pixel centres
	c.points = std::move(points);
	c.color = outline;
	c.fill = fill;
	return 0;
}

int FontIndex (lua_State *L, const char *name)
{
	for (int i = 0; i < FONT_COUNT; i++)
		if (!strcmp(fonts[i].name, name))
			return i;
	return luaL_error(L, "no font named '%s' (try normal, fixed or small)", name);
}

int8 Alignment (lua_State *L, const char *value, const char *before, const char *after)
{
	if (!strcmp(value, before))
		return -1;
	if (!strcmp(value, "center") || !strcmp(value, "middle"))
		return 0;
	if (!strcmp(value, after))
		return 1;
	return (int8) luaL_error(L, "alignment '%s': expected %s, center or %s", value, before, after);
}

void ParseStyle (lua_State *L, int index, LuaDrawCmd &c)
{
	lua_getfield(L, index, "color");
	c.color = LuaCheckColor(L, -1, c.color);
	lua_getfield(L, index, "background");
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		lua_getfield(L, index, "bg");
	}
	c.fill = LuaCheckColor(L, -1, c.fill);
	lua_getfield(L, index, "outline");
	c.outline = LuaCheckColor(L, -1, c.outline);
	lua_getfield(L, index, "font");
	if (lua_isstring(L, -1))
		c.font = (uint8) FontIndex(L, lua_tostring(L, -1));
	lua_getfield(L, index, "align");
	if (lua_isstring(L, -1))
		c.align = Alignment(L, lua_tostring(L, -1), "left", "right");
	lua_getfield(L, index, "valign");
	if (lua_isstring(L, -1))
		c.valign = Alignment(L, lua_tostring(L, -1), "top", "bottom");
	lua_getfield(L, index, "scale");
	if (!lua_isnil(L, -1))
		c.scale = (uint8) std::clamp<lua_Integer>(luaL_checkinteger(L, -1), 1, 8);
	lua_pop(L, 7);
}

LuaDrawCmd TextDefaults (void)
{
	LuaDrawCmd	c;
	c.type = LuaDrawCmd::TEXT;
	c.color = 0xffffffff;
	c.outline = 0xff000000;
	return c;
}

// gui.text(x, y, text [, color [, background]]) or gui.text(x, y, text, style)
int gui_text (lua_State *L)
{
	BuildFonts();
	LuaDrawCmd	c = TextDefaults();
	c.x = CheckCoord(L, 1);
	c.y = CheckCoord(L, 2);
	size_t		length;
	const char	*text = luaL_tolstring(L, 3, &length);
	c.text.assign(text, length);
	lua_pop(L, 1);

	if (lua_istable(L, 4))
		ParseStyle(L, 4, c);
	else
	{
		c.color = LuaCheckColor(L, 4, c.color);
		c.fill = LuaCheckColor(L, 5, 0);
	}
	c.color = Fade(L, c.color);
	c.outline = Fade(L, c.outline);
	c.fill = Fade(L, c.fill);
	Record(L, LuaDrawCmd::TEXT) = std::move(c);
	return 0;
}

// gui.textf(x, y, format, ...): string.format, then gui.text.
int gui_textf (lua_State *L)
{
	BuildFonts();
	int	top = lua_gettop(L);
	luaL_checkstring(L, 3);
	lua_getglobal(L, "string");
	lua_getfield(L, -1, "format");
	lua_remove(L, -2);
	for (int i = 3; i <= top; i++)
		lua_pushvalue(L, i);
	lua_call(L, top - 2, 1);

	LuaDrawCmd	c = TextDefaults();
	c.x = CheckCoord(L, 1);
	c.y = CheckCoord(L, 2);
	c.text = lua_tostring(L, -1);
	c.color = Fade(L, c.color);
	c.outline = Fade(L, c.outline);
	Record(L, LuaDrawCmd::TEXT) = std::move(c);
	return 0;
}

int gui_text_size (lua_State *L)
{
	BuildFonts();
	size_t		length;
	const char	*text = luaL_tolstring(L, 1, &length);
	std::string	s(text, length);
	lua_pop(L, 1);

	LuaDrawCmd	c = TextDefaults();
	if (lua_istable(L, 2))
		ParseStyle(L, 2, c);
	else if (lua_isstring(L, 2))
		c.font = (uint8) FontIndex(L, lua_tostring(L, 2));

	int	w, h;
	MeasureText(fonts[c.font], Characters(s), w, h);
	lua_pushinteger(L, w * c.scale);
	lua_pushinteger(L, h * c.scale);
	return 2;
}

//------------------------------------------------------------------------------
// Images

const char	kImageType[] = "snes9x.image";

struct ImageData
{
	std::shared_ptr<LuaImage>	image;
};

std::string ResolvePath (lua_State *L, const char *path)
{
	std::string	p = path;
	const std::string	&dir = LuaScriptOf(L)->dir;
	bool	absolute = !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
	return absolute || dir.empty() ? p : dir + p;
}

std::shared_ptr<LuaImage> LoadImage (lua_State *L, const char *path)
{
	LuaScript	*s = LuaScriptOf(L);
	std::string	full = ResolvePath(L, path);
	auto		cached = s->images.find(full);
	if (cached != s->images.end())
		return cached->second;

	int		w, h, channels;
	uint8	*rgba = stbi_load(full.c_str(), &w, &h, &channels, 4);
	if (!rgba)
		luaL_error(L, "can't load image '%s': %s", full.c_str(), stbi_failure_reason());

	auto	image = std::make_shared<LuaImage>();
	image->width = w;
	image->height = h;
	image->argb.resize((size_t) w * h);
	for (size_t i = 0; i < image->argb.size(); i++)
	{
		const uint8	*p = rgba + i * 4;
		image->argb[i] = (uint32) p[3] << 24 | p[0] << 16 | p[1] << 8 | p[2];
	}
	stbi_image_free(rgba);

	if (s->images.size() > 256)
		s->images.clear();
	s->images[full] = image;
	return image;
}

std::shared_ptr<LuaImage> CheckImage (lua_State *L, int arg)
{
	if (lua_type(L, arg) == LUA_TSTRING)
		return LoadImage(L, lua_tostring(L, arg));
	return ((ImageData *) luaL_checkudata(L, arg, kImageType))->image;
}

int gui_load_image (lua_State *L)
{
	auto	image = LoadImage(L, luaL_checkstring(L, 1));
	auto	*data = (ImageData *) lua_newuserdatauv(L, sizeof(ImageData), 0);
	new (data) ImageData { image };
	luaL_setmetatable(L, kImageType);
	return 1;
}

int image_gc (lua_State *L)
{
	((ImageData *) luaL_checkudata(L, 1, kImageType))->~ImageData();
	return 0;
}

int image_width (lua_State *L)
{
	lua_pushinteger(L, CheckImage(L, 1)->width);
	return 1;
}

int image_height (lua_State *L)
{
	lua_pushinteger(L, CheckImage(L, 1)->height);
	return 1;
}

// gui.image(x, y, image_or_path [, {sx =, sy =, w =, h =, scale =, opacity =}])
int gui_image (lua_State *L)
{
	int		x = CheckCoord(L, 1), y = CheckCoord(L, 2);
	auto	image = CheckImage(L, 3);

	int		sx = 0, sy = 0, w = image->width, h = image->height, scale = 1;
	double	opacity = 1.0;
	if (lua_istable(L, 4))
	{
		auto	field = [&](const char *name, auto fallback) {
			lua_getfield(L, 4, name);
			auto	v = lua_isnil(L, -1) ? fallback : (decltype(fallback)) luaL_checknumber(L, -1);
			lua_pop(L, 1);
			return v;
		};
		sx = field("sx", sx);
		sy = field("sy", sy);
		w = field("w", w);
		h = field("h", h);
		scale = std::clamp(field("scale", scale), 1, 16);
		opacity = std::clamp(field("opacity", opacity), 0.0, 1.0);
	}

	LuaDrawCmd	&c = Record(L, LuaDrawCmd::IMAGE);
	c.x = x;
	c.y = y;
	c.sx = sx;
	c.sy = sy;
	c.w = w;
	c.h = h;
	c.scale = (uint8) scale;
	c.alpha = (int) (opacity * LuaScriptOf(L)->opacity * 255 + 0.5);
	c.image = std::move(image);
	return 0;
}

//------------------------------------------------------------------------------
// Layer state and the screen

int gui_clear (lua_State *L)
{
	LuaScript	*s = LuaScriptOf(L);
	if (!s->layer.empty())
		LuaEng.overlay_dirty = true;
	s->layer.clear();
	return 0;
}

int gui_set_auto_clear (lua_State *L)
{
	LuaScriptOf(L)->auto_clear = lua_toboolean(L, 1);
	return 0;
}

int gui_set_opacity (lua_State *L)
{
	LuaScriptOf(L)->opacity = (float) std::clamp(luaL_checknumber(L, 1), 0.0, 1.0);
	return 0;
}

int gui_set_antialias (lua_State *L)
{
	LuaScriptOf(L)->antialias = lua_toboolean(L, 1);
	return 0;
}

int gui_width (lua_State *L)
{
	lua_pushinteger(L, Settings.SuperGameBoy ? (int) SGB_GB_SCREEN_W : SNES_WIDTH);
	return 1;
}

int ScreenHeight (void)
{
	return Settings.SuperGameBoy ? (int) SGB_GB_SCREEN_H : PPU.ScreenHeight;
}

int gui_height (lua_State *L)
{
	lua_pushinteger(L, ScreenHeight());
	return 1;
}

int gui_screen (lua_State *L)
{
	int	ext = Settings.SuperGameBoy ? 0 : IPPU.WideExtent;
	lua_createtable(L, 0, 6);
	lua_pushinteger(L, -ext);
	lua_setfield(L, -2, "left");
	lua_pushinteger(L, 0);
	lua_setfield(L, -2, "top");
	lua_pushinteger(L, (Settings.SuperGameBoy ? (int) SGB_GB_SCREEN_W : SNES_WIDTH) + 2 * ext);
	lua_setfield(L, -2, "width");
	lua_pushinteger(L, ScreenHeight());
	lua_setfield(L, -2, "height");
	lua_pushboolean(L, !Settings.SuperGameBoy && IPPU.DoubleWidthPixels);
	lua_setfield(L, -2, "hires");
	lua_pushboolean(L, !Settings.SuperGameBoy && IPPU.DoubleHeightPixels);
	lua_setfield(L, -2, "interlace");
	return 1;
}

// The game's own picture under the overlay, as 0xRRGGBB.
int gui_get_pixel (lua_State *L)
{
	int	x = CheckCoord(L, 1), y = CheckCoord(L, 2);

	const uint16	*pixels = GFX.Screen;
	int		pitch = GFX.RealPPL, width = LuaEng.present_width, height = LuaEng.present_height;
	if (!LuaEng.present_pending)
	{
		if (!LuaEng.clean_valid)
			return 0;
		pixels = LuaEng.clean.data();
		pitch = width = LuaEng.clean_width;
		height = LuaEng.clean_height;
	}

	Target	t(nullptr, pitch, width, height);
	if (x < t.left || x >= t.right || y < t.top || y >= t.bottom)
		return 0;
	uint16	p = pixels[(size_t) y * t.sy * pitch + (x + t.ox) * t.sx];
	uint32	r, g, b;
	DECOMPOSE_PIXEL(p, r, g, b);
	lua_pushinteger(L, ((r * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31));
	return 1;
}

int gui_rgb (lua_State *L)
{
	lua_Integer	r = luaL_checkinteger(L, 1), g = luaL_checkinteger(L, 2), b = luaL_checkinteger(L, 3);
	lua_Integer	a = luaL_optinteger(L, 4, 255);
	auto	clamp = [](lua_Integer v) { return (uint32) std::clamp<lua_Integer>(v, 0, 255); };
	uint32	alpha = std::max<uint32>(clamp(a), 1);	// an alpha byte of 0 would read as opaque
	lua_pushinteger(L, (alpha << 24) | (clamp(r) << 16) | (clamp(g) << 8) | clamp(b));
	return 1;
}

const luaL_Reg	gui_functions[] =
{
	{ "pixel",			gui_pixel },
	{ "line",			gui_line },
	{ "rect",			gui_shape<LuaDrawCmd::RECT> },
	{ "ellipse",		gui_shape<LuaDrawCmd::ELLIPSE> },
	{ "circle",			gui_circle },
	{ "polygon",		gui_polygon },
	{ "text",			gui_text },
	{ "textf",			gui_textf },
	{ "text_size",		gui_text_size },
	{ "image",			gui_image },
	{ "load_image",		gui_load_image },
	{ "clear",			gui_clear },
	{ "set_auto_clear",	gui_set_auto_clear },
	{ "set_opacity",	gui_set_opacity },
	{ "set_antialias",	gui_set_antialias },
	{ "width",			gui_width },
	{ "height",			gui_height },
	{ "screen",			gui_screen },
	{ "get_pixel",		gui_get_pixel },
	{ "rgb",			gui_rgb },
	{ nullptr, nullptr }
};

}	// namespace

uint32 LuaCheckColor (lua_State *L, int index, uint32 fallback)
{
	switch (lua_type(L, index))
	{
		case LUA_TNONE:
		case LUA_TNIL:
			return fallback;

		case LUA_TBOOLEAN:
			return lua_toboolean(L, index) ? fallback : 0;

		case LUA_TNUMBER:
		{
			uint32	argb = (uint32) luaL_checkinteger(L, index);
			return (argb & 0xff000000) ? argb : (argb | 0xff000000);
		}

		case LUA_TSTRING:
		{
			uint32	argb = 0;
			if (!ParseColorString(lua_tostring(L, index), argb))
				luaL_argerror(L, index, lua_pushfstring(L, "'%s' isn't a color", lua_tostring(L, index)));
			return argb;
		}

		case LUA_TTABLE:
		{
			int	t = lua_absindex(L, index);
			return (uint32) Component(L, t, 4, "a", 255) << 24 | Component(L, t, 1, "r", 0) << 16 |
			       Component(L, t, 2, "g", 0) << 8 | Component(L, t, 3, "b", 0);
		}

		default:
			luaL_argerror(L, index, "expected a color");
			return 0;
	}
}

bool LuaHasOverlay (void)
{
	for (auto &s : LuaEng.scripts)
		if (!s->layer.empty())
			return true;
	return false;
}

void LuaRenderOverlay (uint16 *screen, int pitch, int width, int height)
{
	if (!LuaHasOverlay())
		return;
	BuildFonts();
	Target	t(screen, pitch, width, height);
	for (auto &s : LuaEng.scripts)
		for (auto &c : s->layer)
			Draw(t, c);
}

void LuaOpenGui (lua_State *L)
{
	static const luaL_Reg	image_methods[] =
	{
		{ "width",	image_width },
		{ "height",	image_height },
		{ nullptr, nullptr }
	};
	luaL_newmetatable(L, kImageType);
	luaL_newlib(L, image_methods);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, image_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);

	luaL_newlib(L, gui_functions);
	lua_setglobal(L, "gui");
}
