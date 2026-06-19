/* tintty — Core-Typen und Schnittstelle (frontend-agnostisch). */
#ifndef TIN_H
#define TIN_H

#include <stddef.h>
#include <stdint.h>

/* Voller 32-bit-Codepoint — niemals 16-bit. Das ist der Kernunterschied zu urxvt:
 * Glyphen jenseits von U+FFFF (Nerd-Font MDI-Icons U+F0001..U+F1AF0) funktionieren. */
typedef uint_least32_t Rune;

#define UTF_SIZ      4
#define UTF_INVALID  0xFFFD

#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define MAX(a, b)        ((a) < (b) ? (b) : (a))
#define LEN(a)           (sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b) ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)   ((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define DIV(n, d)        (((n) + (d) / 2) / (d))

/* Zell-Attribute (Glyph.mode) */
enum glyph_attribute {
	ATTR_NULL       = 0,
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_REVERSE    = 1 << 5,
	ATTR_INVISIBLE  = 1 << 6,
	ATTR_STRUCK     = 1 << 7,
	ATTR_WRAP       = 1 << 8,
	ATTR_WIDE       = 1 << 9,
	ATTR_WDUMMY     = 1 << 10,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
};

/* Farbe: Palette-Index 0..255, Spezial >= 256, oder Truecolor (Bit 24 gesetzt). */
#define TRUECOLOR(r, g, b) (1u << 24 | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define IS_TRUECOLOR(x)    ((x) & (1u << 24))
#define TRUERED(x)         (((x) >> 16) & 0xff)
#define TRUEGREEN(x)       (((x) >> 8) & 0xff)
#define TRUEBLUE(x)        ((x) & 0xff)

typedef struct {
	Rune u;        /* Codepoint */
	uint16_t mode; /* Attribut-Flags */
	uint32_t fg;   /* Vordergrund (Index oder Truecolor) */
	uint32_t bg;   /* Hintergrund (Index oder Truecolor) */
} Glyph;

typedef Glyph *Line;

enum cursor_movement { CURSOR_SAVE, CURSOR_LOAD };
enum cursor_state    { CURSOR_DEFAULT = 0, CURSOR_WRAPNEXT = 1, CURSOR_ORIGIN = 2 };

typedef struct {
	Glyph attr; /* aktueller SGR-Zustand (Template für neue Zellen) */
	int x, y;
	char state;
} TCursor;

/* Terminal-Modus (term.mode) */
enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_APPKEYPAD   = 1 << 4,
	MODE_APPCURSOR   = 1 << 5,
	MODE_HIDE        = 1 << 6, /* Cursor versteckt (DECTCEM aus) */
	MODE_BRCKTPASTE  = 1 << 7,
	MODE_MOUSEBTN    = 1 << 8,  /* DECSET 1000 */
	MODE_MOUSEMOTION = 1 << 9,  /* DECSET 1002 */
	MODE_MOUSEX10    = 1 << 10, /* DECSET 9   */
	MODE_MOUSEMANY   = 1 << 11, /* DECSET 1003 */
	MODE_MOUSESGR    = 1 << 12, /* DECSET 1006 */
	MODE_MOUSE = MODE_MOUSEBTN | MODE_MOUSEMOTION | MODE_MOUSEX10 | MODE_MOUSEMANY,
};

typedef struct {
	int row, col;     /* sichtbare Grid-Größe */
	Line *line;       /* aktueller Screen [row][col] */
	Line *alt;        /* Alternate-Screen */
	int *dirty;       /* dirty-Flag pro sichtbarer Zeile */
	TCursor c;        /* Cursor */
	int ocx, ocy;     /* vorheriger Cursor (für Dirty) */
	int top, bot;     /* Scroll-Region (inklusiv) */
	int mode;         /* term_mode-Flags */
	int esc;          /* Escape-Parser-Zustand (intern) */
	char trantbl[4];  /* Charset-Übersetzung G0..G3 (Finalbyte oder 0) */
	int charset;      /* aktiver Charset-Slot (GL) */
	int icharset;     /* gerade ausgewählter Slot (ESC ( ) ...) */
	int *tabs;        /* Tab-Stops */
	Rune lastc;       /* letztes druckbares Zeichen (für REP) */
	/* Scrollback (nur Primary-Screen) */
	Line *hist;       /* Ringpuffer [HISTSIZE] */
	int histi;        /* Index der zuletzt geschriebenen hist-Zeile */
	int histn;        /* Anzahl gültiger hist-Zeilen */
	int scr;          /* Anzeige-Offset (0 = live/unten) */
} Term;

extern Term term;
extern int  cmdfd; /* PTY-Master-fd */

/* sichtbare Zeile y unter Berücksichtigung des Scrollback-Offsets */
Line tgetline(int y);

/* Core-API (Frontend -> Core) */
void   tnew(int col, int row);
void   tresize(int col, int row);
int    ttynew(void);
size_t ttyread(void);
void   ttywrite(const char *s, size_t n, int may_echo);
void   ttyhangup(void);
void   ttyresize(int tw, int th);
void   tfulldirt(void);
void   kscrollup(int n);
void   kscrolldown(int n);
void   tresetfull(void);
void   tclearall(void);

/* UTF-8 */
size_t utf8encode(Rune u, char *c);
size_t utf8decode(const char *c, Rune *u, size_t clen);

#endif /* TIN_H */
