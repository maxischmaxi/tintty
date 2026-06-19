/*
 * tintty — Rendering: FreeType-Rasterung, fontconfig-Fallback, pixman-Compositing.
 * Zeichnet das Grid (aus tin.h) in einen premultiplizierten ARGB8888-Puffer.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <pixman.h>

#include "config.h"
#include "tin.h"
#include "render.h"

#define FRC_MAX 16

typedef struct {
	pixman_image_t *img; /* a8-Maske, oder a8r8g8b8 bei color, oder NULL (leer) */
	void *data;          /* zugehöriger Pixelpuffer (pixman besitzt ihn nicht) */
	int left, top, w, h;
	int color;           /* 1 = fertige ARGB-Glyphe (Emoji), 0 = a8-Maske */
} GlyphSlot;

typedef struct {
	FT_Face ft;
	GlyphSlot **glyphs; /* lazy [ft->num_glyphs] */
} Face;

typedef struct {
	Face face;
	FcCharSet *set;     /* Cache: enthält bestätigte Runen */
} Fallback;

static FT_Library ftlib;
static Face pri[4];               /* 0=regular,1=bold,2=italic,3=bolditalic */
static Fallback frc[FRC_MAX];
static int frclen;

/* Negativ-Cache: Runen, für die KEIN Font existiert. Verhindert, dass ein
 * PTY-Strom vieler im Primärfont fehlender Glyphen wiederholt teure
 * fontconfig-/FreeType-Lookups auslöst (CPU-DoS-Schutz). */
#define NEGCACHE 512
static Rune negcache[NEGCACHE];
static int  negcount, negwrite;

static int
neg_has(Rune u)
{
	int i;
	for (i = 0; i < negcount; i++)
		if (negcache[i] == u)
			return 1;
	return 0;
}

static void
neg_add(Rune u)
{
	negcache[negwrite] = u;
	negwrite = (negwrite + 1) % NEGCACHE;
	if (negcount < NEGCACHE)
		negcount++;
}

static pixman_color_t palette[256];
static pixman_color_t special[COL_LAST - 256];

static double cur_scale = 1.0;
static double size_pt;        /* effektive Punktgröße (Basis font_size_pt ± Zoom) */
static double pixelsize;
static int cellw, cellh, baseline;

/* zwischengespeicherter Solid-Fill für die zuletzt genutzte Farbe */
static pixman_image_t *solid_cache;
static pixman_color_t  solid_color;
static int             solid_valid;

/* ------------------------------------------------------------------ Farben */
static void
setrgb(pixman_color_t *c, int r, int g, int b)
{
	c->red   = (r << 8) | r;
	c->green = (g << 8) | g;
	c->blue  = (b << 8) | b;
	c->alpha = 0xffff;
}

static int
parsecolor(const char *s, pixman_color_t *c)
{
	int r, g, b;
	if (s && s[0] == '#' && strlen(s) >= 7 &&
	    sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
		setrgb(c, r, g, b);
		return 0;
	}
	setrgb(c, 0xff, 0x00, 0xff); /* sichtbares Fallback-Magenta */
	return -1;
}

static void
parsepalette(void)
{
	static const int cube[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
	int i, n;

	for (i = 0; i < 16 && i < (int)LEN(colorname); i++)
		parsecolor(colorname[i], &palette[i]);
	for (i = 16; i < 232; i++) {
		n = i - 16;
		setrgb(&palette[i], cube[(n / 36) % 6], cube[(n / 6) % 6], cube[n % 6]);
	}
	for (i = 232; i < 256; i++) {
		int v = 8 + (i - 232) * 10;
		setrgb(&palette[i], v, v, v);
	}
	for (i = 0; i < COL_LAST - 256; i++)
		parsecolor(colorname_special[i], &special[i]);
}

static void
loadcolor(uint32_t c, int isbg, pixman_color_t *out)
{
	if (IS_TRUECOLOR(c)) {
		setrgb(out, TRUERED(c), TRUEGREEN(c), TRUEBLUE(c));
	} else if (c < 256) {
		*out = palette[c];
	} else if (c < (uint32_t)COL_LAST) {
		*out = special[c - 256];
	} else {
		*out = palette[7];
	}
	if (isbg)
		out->alpha = (c == (uint32_t)DEFAULTBG)
		    ? (uint16_t)(bg_opacity * 0xffff) : 0xffff;
	else
		out->alpha = 0xffff;
}

static pixman_image_t *
getsolid(const pixman_color_t *c)
{
	if (solid_valid && !memcmp(c, &solid_color, sizeof(*c)))
		return solid_cache;
	if (solid_cache)
		pixman_image_unref(solid_cache);
	solid_cache = pixman_image_create_solid_fill(c);
	solid_color = *c;
	solid_valid = 1;
	return solid_cache;
}

/* ------------------------------------------------------------------ Faces */
/* Setzt die Pixelgröße: skalierbare Fonts exakt, Bitmap-Fonts (Emoji-Strikes)
 * auf den nächstgelegenen verfügbaren Strike. */
static void
set_face_size(FT_Face f)
{
	int target = (int)lround(pixelsize);
	int i, best = 0, bestd = 1 << 30;

	if (FT_IS_SCALABLE(f)) {
		FT_Set_Pixel_Sizes(f, 0, target);
		return;
	}
	if (f->num_fixed_sizes <= 0)
		return;
	for (i = 0; i < f->num_fixed_sizes; i++) {
		int d = abs(f->available_sizes[i].height - target);
		if (d < bestd) { bestd = d; best = i; }
	}
	FT_Select_Size(f, best);
}

static FT_Face
openface(const char *family, int weight, int slant, FcCharSet *cs)
{
	FcPattern *pat, *m;
	FcResult res;
	FcChar8 *file;
	int idx;
	FT_Face face;

	pat = FcPatternCreate();
	if (family)
		FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
	FcPatternAddInteger(pat, FC_WEIGHT, weight);
	FcPatternAddInteger(pat, FC_SLANT, slant);
	FcPatternAddDouble(pat, FC_PIXEL_SIZE, pixelsize);
	FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
	if (cs)
		FcPatternAddCharSet(pat, FC_CHARSET, cs);

	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	m = FcFontMatch(NULL, pat, &res);
	FcPatternDestroy(pat);
	if (!m)
		return NULL;

	if (FcPatternGetString(m, FC_FILE, 0, &file) != FcResultMatch) {
		FcPatternDestroy(m);
		return NULL;
	}
	if (FcPatternGetInteger(m, FC_INDEX, 0, &idx) != FcResultMatch)
		idx = 0;

	if (FT_New_Face(ftlib, (const char *)file, idx, &face)) {
		FcPatternDestroy(m);
		return NULL;
	}
	FcPatternDestroy(m);
	set_face_size(face);
	return face;
}

static void
freecache(Face *f)
{
	FT_Long i;
	if (!f->glyphs)
		return;
	for (i = 0; i < f->ft->num_glyphs; i++) {
		if (f->glyphs[i]) {
			if (f->glyphs[i]->img)
				pixman_image_unref(f->glyphs[i]->img);
			free(f->glyphs[i]->data);
			free(f->glyphs[i]);
		}
	}
	free(f->glyphs);
	f->glyphs = NULL;
}

static void
freefonts(void)
{
	int i;
	for (i = 0; i < 4; i++) {
		freecache(&pri[i]);
		if (pri[i].ft)
			FT_Done_Face(pri[i].ft);
		pri[i].ft = NULL;
	}
	for (i = 0; i < frclen; i++) {
		freecache(&frc[i].face);
		if (frc[i].face.ft)
			FT_Done_Face(frc[i].face.ft);
		if (frc[i].set)
			FcCharSetDestroy(frc[i].set);
	}
	frclen = 0;
	negcount = negwrite = 0; /* Negativ-Cache verwerfen (Fonts werden neu geladen) */
	if (solid_cache) {
		pixman_image_unref(solid_cache);
		solid_cache = NULL;
		solid_valid = 0;
	}
}

static int
loadfonts(void)
{
	int w_reg = FcWeightFromOpenType(font_weight_ot);
	int w_bold = FcWeightFromOpenType(MIN(font_weight_ot + 200, 900));
	int adv, asc, desc, h;

	pixelsize = size_pt * font_dpi / 72.0 * cur_scale;

	pri[0].ft = openface(font_family, w_reg, FC_SLANT_ROMAN, NULL);
	pri[1].ft = openface(font_family, w_bold, FC_SLANT_ROMAN, NULL);
	pri[2].ft = openface(font_family, w_reg, FC_SLANT_ITALIC, NULL);
	pri[3].ft = openface(font_family, w_bold, FC_SLANT_ITALIC, NULL);
	if (!pri[0].ft) {
		fprintf(stderr, "tintty: konnte Font '%s' nicht laden\n", font_family);
		return -1;
	}
	/* fehlende Stile auf regular zurückfallen lassen */
	if (!pri[1].ft) pri[1].ft = pri[0].ft;
	if (!pri[2].ft) pri[2].ft = pri[0].ft;
	if (!pri[3].ft) pri[3].ft = pri[2].ft ? pri[2].ft : pri[0].ft;

	/* Zellbreite aus einem einbreiten ASCII-Glyph (nicht max_advance,
	 * das bei FiraCode Nerd Font wegen breiter Icons doppelt sein kann) */
	if (FT_Load_Char(pri[0].ft, 'M', FT_LOAD_DEFAULT) == 0)
		adv = pri[0].ft->glyph->advance.x >> 6;
	else
		adv = pri[0].ft->size->metrics.max_advance >> 6;

	asc = pri[0].ft->size->metrics.ascender >> 6;
	desc = -(pri[0].ft->size->metrics.descender >> 6);
	h = pri[0].ft->size->metrics.height >> 6;

	cellw = adv > 0 ? adv : (int)lround(pixelsize / 2);
	cellh = MAX(h, asc + desc);
	baseline = asc;
	return 0;
}

/* ------------------------------------------------------------------ Fallback */
static Face *
findfallback(Rune u)
{
	int i;
	FT_Face face = NULL;
	int w = FcWeightFromOpenType(font_weight_ot);

	for (i = 0; i < frclen; i++)
		if (FcCharSetHasChar(frc[i].set, u) &&
		    FT_Get_Char_Index(frc[i].face.ft, u))
			return &frc[i].face;

	if (neg_has(u)) /* schon bekannt: kein Font hat diese Rune */
		return NULL;

	/* 1) explizite, geordnete Fallback-Liste aus config.h */
	for (i = 0; i < (int)LEN(fallback_fonts); i++) {
		face = openface(fallback_fonts[i], w, FC_SLANT_ROMAN, NULL);
		if (face && FT_Get_Char_Index(face, u))
			break;
		if (face) { FT_Done_Face(face); face = NULL; }
	}

	/* 2) generisch: fontconfig nach einem Font mit genau dieser Rune fragen */
	if (!face) {
		FcCharSet *cs = FcCharSetCreate();
		FcCharSetAddChar(cs, u);
		face = openface(NULL, w, FC_SLANT_ROMAN, cs);
		FcCharSetDestroy(cs);
		if (face && !FT_Get_Char_Index(face, u)) {
			FT_Done_Face(face);
			face = NULL;
		}
	}

	if (!face) {
		neg_add(u); /* merken, dass diese Rune nirgends existiert */
		return NULL;
	}

	/* in den Cache aufnehmen (ältesten Eintrag verdrängen) */
	if (frclen >= FRC_MAX) {
		freecache(&frc[0].face);
		FT_Done_Face(frc[0].face.ft);
		FcCharSetDestroy(frc[0].set);
		memmove(&frc[0], &frc[1], (FRC_MAX - 1) * sizeof(Fallback));
		frclen--;
	}
	frc[frclen].face.ft = face;
	frc[frclen].face.glyphs = NULL;
	frc[frclen].set = FcCharSetCreate();
	FcCharSetAddChar(frc[frclen].set, u);
	return &frc[frclen++].face;
}

/* ------------------------------------------------------------------ Glyph-Cache */
static GlyphSlot *
getglyph(Face *f, FT_UInt gi)
{
	GlyphSlot *s;
	FT_GlyphSlot g;
	FT_Bitmap *bm;
	int y, stride;
	uint8_t *data;

	if (gi >= (FT_UInt)f->ft->num_glyphs)
		gi = 0;
	if (!f->glyphs)
		f->glyphs = calloc(f->ft->num_glyphs, sizeof(GlyphSlot *));
	if ((s = f->glyphs[gi]))
		return s;

	s = calloc(1, sizeof(GlyphSlot));
	{
		int32_t flags = FT_LOAD_RENDER |
		    (FT_HAS_COLOR(f->ft) ? FT_LOAD_COLOR : FT_LOAD_TARGET_LIGHT);
		if (FT_Load_Glyph(f->ft, gi, flags)) {
			f->glyphs[gi] = s;
			return s;
		}
	}
	g = f->ft->glyph;
	bm = &g->bitmap;
	s->left = g->bitmap_left;
	s->top = g->bitmap_top;
	s->w = bm->width;
	s->h = bm->rows;

	if (s->w <= 0 || s->h <= 0) {
		f->glyphs[gi] = s;
		return s;
	}

	if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
		/* Color-Emoji: BGRA ist premultipliziert == pixman a8r8g8b8 (LE).
		 * Bitmap-Strike ggf. auf Zellhöhe herunterskalieren. */
		int sw = s->w, sh = s->h;
		double sc = (double)cellh / sh;
		stride = sw * 4;
		data = malloc((size_t)stride * sh);
		for (y = 0; y < sh; y++)
			memcpy(data + (size_t)y * stride, bm->buffer + (size_t)y * bm->pitch,
			    (size_t)sw * 4);
		pixman_image_t *raw = pixman_image_create_bits(PIXMAN_a8r8g8b8, sw, sh,
		    (uint32_t *)data, stride);

		if (sc < 0.98 || sc > 1.02) {
			int dw = MAX(1, (int)lround(sw * sc));
			int dh = MAX(1, (int)lround(sh * sc));
			int dstride = dw * 4;
			uint8_t *ddata = calloc((size_t)dstride, dh);
			pixman_image_t *dst = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			    dw, dh, (uint32_t *)ddata, dstride);
			pixman_transform_t t;
			pixman_transform_init_scale(&t, pixman_double_to_fixed(1.0 / sc),
			    pixman_double_to_fixed(1.0 / sc));
			pixman_image_set_transform(raw, &t);
			pixman_image_set_filter(raw, PIXMAN_FILTER_BILINEAR, NULL, 0);
			pixman_image_composite32(PIXMAN_OP_SRC, raw, NULL, dst,
			    0, 0, 0, 0, 0, 0, dw, dh);
			pixman_image_unref(raw);
			free(data);
			s->img = dst;
			s->data = ddata;
			s->w = dw;
			s->h = dh;
			s->left = (int)lround(s->left * sc);
			s->top = (int)lround(s->top * sc);
		} else {
			s->img = raw;
			s->data = data;
		}
		s->color = 1;
	} else if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
		stride = (s->w + 3) & ~3;
		data = malloc((size_t)stride * s->h);
		for (y = 0; y < s->h; y++)
			memcpy(data + (size_t)y * stride,
			    bm->buffer + (size_t)y * bm->pitch, s->w);
		s->data = data;
		s->img = pixman_image_create_bits(PIXMAN_a8, s->w, s->h,
		    (uint32_t *)data, stride);
	} else if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
		stride = (s->w + 3) & ~3;
		data = calloc((size_t)stride, s->h);
		for (y = 0; y < s->h; y++) {
			unsigned char *row = bm->buffer + (size_t)y * bm->pitch;
			for (int x = 0; x < s->w; x++)
				data[(size_t)y * stride + x] =
				    (row[x >> 3] & (0x80 >> (x & 7))) ? 0xff : 0;
		}
		s->data = data;
		s->img = pixman_image_create_bits(PIXMAN_a8, s->w, s->h,
		    (uint32_t *)data, stride);
	}
	f->glyphs[gi] = s;
	return s;
}

/* ------------------------------------------------------------------ Zeichnen */
static void
fillrect(pixman_image_t *buf, int x, int y, int w, int h, const pixman_color_t *c)
{
	pixman_box32_t box = { x, y, x + w, y + h };
	pixman_image_fill_boxes(PIXMAN_OP_SRC, buf, c, 1, &box);
}

static void
drawglyph(pixman_image_t *buf, int px, int py, Rune u, uint16_t mode,
    const pixman_color_t *color)
{
	int style = ((mode & ATTR_BOLD) ? 1 : 0) | ((mode & ATTR_ITALIC) ? 2 : 0);
	Face *f = &pri[style];
	FT_UInt gi = FT_Get_Char_Index(f->ft, u);
	GlyphSlot *s;
	pixman_image_t *src;

	if (!gi) {
		Face *fb = findfallback(u);
		if (fb) {
			f = fb;
			gi = FT_Get_Char_Index(f->ft, u);
		}
	}
	if (!gi)
		return;

	s = getglyph(f, gi);
	if (!s->img)
		return;

	if (s->color) {
		/* fertige ARGB-Glyphe (Emoji): direkt komponieren, ohne fg-Farbe */
		pixman_image_composite32(PIXMAN_OP_OVER, s->img, NULL, buf,
		    0, 0, 0, 0, px + s->left, py + baseline - s->top, s->w, s->h);
	} else {
		src = getsolid(color);
		pixman_image_composite32(PIXMAN_OP_OVER, src, s->img, buf,
		    0, 0, 0, 0, px + s->left, py + baseline - s->top, s->w, s->h);
	}
}

static void
drawcell(pixman_image_t *buf, int px, int py, const Glyph *gp)
{
	uint32_t fg = gp->fg, bg = gp->bg, t;
	uint16_t mode = gp->mode;
	pixman_color_t cfg, cbg;
	int w = (mode & ATTR_WIDE) ? 2 : 1;

	if ((mode & ATTR_BOLD) && !IS_TRUECOLOR(fg) && fg < 8)
		fg += 8; /* bold_brightens_ansi_colors (WezTerm-Default) */

	if (mode & ATTR_REVERSE) { t = fg; fg = bg; bg = t; }

	loadcolor(bg, 1, &cbg);
	loadcolor(fg, 0, &cfg);

	if (mode & ATTR_FAINT) {
		cfg.red /= 2; cfg.green /= 2; cfg.blue /= 2;
	}

	fillrect(buf, px, py, cellw * w, cellh, &cbg);

	if (mode & ATTR_INVISIBLE)
		return;
	if (gp->u && gp->u != ' ')
		drawglyph(buf, px, py, gp->u, mode, &cfg);

	if (mode & ATTR_UNDERLINE)
		fillrect(buf, px, py + baseline + 1, cellw * w, MAX(1, cur_scale), &cfg);
	if (mode & ATTR_STRUCK)
		fillrect(buf, px, py + baseline * 2 / 3, cellw * w, MAX(1, cur_scale), &cfg);
}

static void
drawcursor(pixman_image_t *buf, int border)
{
	int cx = term.c.x, cy = term.c.y;
	Line line = tgetline(cy);
	Glyph g;
	pixman_color_t cbg, cfg;
	int w;

	if (cx > 0 && (line[cx].mode & ATTR_WDUMMY))
		cx--;
	g = line[cx];
	w = (g.mode & ATTR_WIDE) ? 2 : 1;

	loadcolor(DEFAULTCS, 0, &cbg);
	loadcolor(DEFAULTCSFG, 0, &cfg);

	fillrect(buf, border + cx * cellw, border + cy * cellh, cellw * w, cellh, &cbg);
	if (g.u && g.u != ' ')
		drawglyph(buf, border + cx * cellw, border + cy * cellh, g.u, g.mode, &cfg);
}

void
rdraw(pixman_image_t *buf, int border)
{
	pixman_color_t dbg;
	int x, y, bw, bh;
	Line line;

	/* Doppelpufferung: jeder Puffer muss vollständig konsistent sein, daher
	 * Komplett-Redraw. Zuerst gesamten Puffer (inkl. Padding) mit Default-bg
	 * füllen — premultipliziertes ARGB, damit die Transparenz exakt stimmt. */
	bw = pixman_image_get_width(buf);
	bh = pixman_image_get_height(buf);
	loadcolor(DEFAULTBG, 1, &dbg);
	fillrect(buf, 0, 0, bw, bh, &dbg);

	for (y = 0; y < term.row; y++) {
		line = tgetline(y);
		for (x = 0; x < term.col; x++) {
			if (line[x].mode & ATTR_WDUMMY)
				continue;
			drawcell(buf, border + x * cellw, border + y * cellh, &line[x]);
		}
		term.dirty[y] = 0;
	}

	if (!(term.mode & MODE_HIDE) && term.scr == 0)
		drawcursor(buf, border);
}

void
rdirtyall(void)
{
	tfulldirt();
}

/* ------------------------------------------------------------------ Init / Scale */
int
rinit(void)
{
	if (FT_Init_FreeType(&ftlib))
		return -1;
	if (!FcInit())
		return -1;
	parsepalette();
	cur_scale = 1;
	size_pt = font_size_pt;
	return loadfonts();
}

void
rfree(void)
{
	freefonts();
	FT_Done_FreeType(ftlib);
}

int
rsetscale(double scale)
{
	if (scale < 0.5)
		scale = 1.0;
	if (scale > cur_scale - 0.001 && scale < cur_scale + 0.001)
		return 0;
	freefonts();
	cur_scale = scale;
	loadfonts();
	tfulldirt();
	return 1;
}

/* Zoom: delta>0 größer, delta<0 kleiner, delta==0 zurück auf font_size_pt.
 * Gibt 1 zurück, wenn sich die Größe geändert hat (sonst 0, z.B. an Grenze). */
int
rzoom(int delta)
{
	double want = (delta == 0) ? font_size_pt
	            : size_pt + (delta > 0 ? zoom_step_pt : -zoom_step_pt);

	if (want < zoom_min_pt) want = zoom_min_pt;
	if (want > zoom_max_pt) want = zoom_max_pt;
	if (want > size_pt - 0.001 && want < size_pt + 0.001)
		return 0;

	freefonts();
	size_pt = want;
	loadfonts();
	tfulldirt();
	return 1;
}

int rcellw(void) { return cellw; }
int rcellh(void) { return cellh; }
