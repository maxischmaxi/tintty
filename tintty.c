/*
 * tintty — Terminal-Core: PTY, VT/ANSI-Parser, Grid, Scrollback.
 * Frontend-agnostisch (kennt kein Wayland). Architektur an suckless st angelehnt.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pty.h>

#include "config.h"
#include "tin.h"
#include "win.h"

#define ESC_BUF_SIZ  (128 * UTF_SIZ)
#define ESC_ARG_SIZ  16
#define STR_BUF_SIZ  ESC_BUF_SIZ
#define STR_ARG_SIZ  ESC_ARG_SIZ

#define IS_SET(flag)     ((term.mode & (flag)) != 0)
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))
#define ISCONTROLC0(c)   ((c) <= 0x1f || (c) == 0x7f)
#define ISCONTROLC1(c)   (BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c)     (ISCONTROLC0(c) || ISCONTROLC1(c))
#define TLINE_HIST(y)    term.hist[(term.histi - term.scr + (y) + HISTSIZE + 1) % HISTSIZE]

/* Escape-Parser-Zustand */
enum escape_state {
	ESC_START      = 1,
	ESC_CSI        = 2,
	ESC_STR        = 4,   /* DCS, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16,  /* String-Sequenz fertig, ST/BEL erwartet */
	ESC_TEST       = 32,  /* ESC # ... */
};

typedef struct {
	char buf[ESC_BUF_SIZ];
	size_t len;
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;
	char mode[2];
} CSIEscape;

typedef struct {
	char type;
	char buf[STR_BUF_SIZ];
	size_t len;
	char *args[STR_ARG_SIZ];
	int narg;
} STREscape;

Term term;
int  cmdfd = -1;
static pid_t pid;
static CSIEscape csiescseq;
static STREscape strescseq;
static TCursor   csaved[2]; /* gespeicherter Cursor: [0]=primary, [1]=alt */

static const char *vtiden = "\033[?6c";

/* DEC Special Graphics (Box-Drawing), 0x41..0x7e */
static const char *vt100_0[62] = {
	"↑", "↓", "→", "←", "█", "▚", "☃",                       /* A - G */
	0, 0, 0, 0, 0, 0, 0, 0,                                   /* H - O */
	0, 0, 0, 0, 0, 0, 0, 0,                                   /* P - W */
	0, 0, 0, 0, 0, 0, 0, " ",                                 /* X - _ */
	"◆", "▒", "␉", "␌", "␍", "␊", "°", "±",                  /* ` - g */
	"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺",                  /* h - o */
	"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬",                  /* p - w */
	"│", "≤", "≥", "π", "≠", "£", "·",                        /* x - ~ */
};

/* ---- Forward-Deklarationen ---- */
static int  twrite_internal(const char *buf, int buflen);
static void tputc(Rune u);
static void tcontrolcode(Rune u);
static void csihandle(void);
static void csiparse(void);
static void csireset(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);
static int  eschandle(unsigned char ascii);
static void tsetchar(Rune u, const Glyph *attr, int x, int y);
static void tsetattr(const int *attr, int l);
static void tclearregion(int x1, int y1, int x2, int y2);
static void tnewline(int first_col);
static void tmoveto(int x, int y);
static void tmoveato(int x, int y);
static void tscrollup(int orig, int n, int copyhist);
static void tscrolldown(int orig, int n);
static void tinsertblank(int n);
static void tinsertblankline(int n);
static void tdeleteline(int n);
static void tdeletechar(int n);
static void tsetscroll(int t, int b);
static void tswapscreen(void);
static void tcursor(int mode);
static void treset(void);
static void tsetmode(int priv, int set, const int *args, int narg);
static int32_t tdefcolor(const int *attr, int *npar, int l);

static _Noreturn void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

/* Geprüfte Allokationen: bei OOM sauber beenden statt NULL zu dereferenzieren. */
static void *
xmalloc(size_t len)
{
	void *p = malloc(len);
	if (!p)
		die("tintty: out of memory\n");
	return p;
}

static void *
xcalloc(size_t n, size_t size)
{
	void *p = calloc(n, size);
	if (!p)
		die("tintty: out of memory\n");
	return p;
}

static void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("tintty: out of memory\n");
	return p;
}

/* ------------------------------------------------------------------ UTF-8 */
size_t
utf8decode(const char *cc, Rune *u, size_t clen)
{
	static const Rune mincp[5] = { 0, 0, 0x80, 0x800, 0x10000 };
	const unsigned char *c = (const unsigned char *)cc;
	Rune cp;
	int len, i;

	*u = UTF_INVALID;
	if (clen == 0)
		return 0;

	if (c[0] < 0x80) { *u = c[0]; return 1; }
	else if ((c[0] & 0xE0) == 0xC0) { len = 2; cp = c[0] & 0x1F; }
	else if ((c[0] & 0xF0) == 0xE0) { len = 3; cp = c[0] & 0x0F; }
	else if ((c[0] & 0xF8) == 0xF0) { len = 4; cp = c[0] & 0x07; }
	else { return 1; } /* ungültiges Leadbyte: 1 verbrauchen */

	if (clen < (size_t)len)
		return 0; /* unvollständig — auf weitere Bytes warten */

	for (i = 1; i < len; i++) {
		if ((c[i] & 0xC0) != 0x80)
			return i; /* fehlerhafte Folge — bis hier verbrauchen */
		cp = (cp << 6) | (c[i] & 0x3F);
	}
	if (cp < mincp[len] || BETWEEN(cp, 0xD800, 0xDFFF) || cp > 0x10FFFF)
		return len;

	*u = cp;
	return len;
}

size_t
utf8encode(Rune u, char *c)
{
	if (u < 0x80) {
		c[0] = u;
		return 1;
	} else if (u < 0x800) {
		c[0] = 0xC0 | (u >> 6);
		c[1] = 0x80 | (u & 0x3F);
		return 2;
	} else if (u < 0x10000) {
		c[0] = 0xE0 | (u >> 12);
		c[1] = 0x80 | ((u >> 6) & 0x3F);
		c[2] = 0x80 | (u & 0x3F);
		return 3;
	} else if (u <= 0x10FFFF) {
		c[0] = 0xF0 | (u >> 18);
		c[1] = 0x80 | ((u >> 12) & 0x3F);
		c[2] = 0x80 | ((u >> 6) & 0x3F);
		c[3] = 0x80 | (u & 0x3F);
		return 4;
	}
	return utf8encode(UTF_INVALID, c);
}

/* ------------------------------------------------------------------ PTY */
static void
execsh(void)
{
	const struct passwd *pw;
	const char *sh;
	char *args[2];

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL) {
		if (errno)
			die("getpwuid: %s\n", strerror(errno));
		die("unknown uid\n");
	}

	if ((sh = getenv("SHELL")) == NULL)
		sh = (pw->pw_shell[0]) ? pw->pw_shell : default_shell;

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);
	setenv("COLORTERM", "truecolor", 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	args[0] = (char *)sh;
	args[1] = NULL;
	execvp(sh, args);
	_exit(1);
}

static void
sigchld(int a)
{
	(void)a;
	int stat;
	pid_t p;

	if ((p = waitpid(pid, &stat, WNOHANG)) < 0)
		return;
	if (p == pid) {
		/* Shell beendet -> Programm beenden */
		if (WIFEXITED(stat) && WEXITSTATUS(stat))
			_exit(WEXITSTATUS(stat));
		else if (WIFSIGNALED(stat))
			_exit(1);
		_exit(0);
	}
}

int
ttynew(void)
{
	struct winsize w = { term.row, term.col, 0, 0 };

	switch (pid = forkpty(&cmdfd, NULL, NULL, &w)) {
	case -1:
		die("forkpty: %s\n", strerror(errno));
		break;
	case 0:
		execsh(); /* forkpty hat im Child bereits setsid()/TIOCSCTTY erledigt */
		break;
	default:
		signal(SIGCHLD, sigchld);
		break;
	}
	return cmdfd;
}

void
ttyhangup(void)
{
	kill(pid, SIGHUP);
}

void
ttywrite(const char *s, size_t n, int may_echo)
{
	(void)may_echo;
	ssize_t r;

	while (n > 0) {
		r = write(cmdfd, s, n);
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return;
		}
		n -= r;
		s += r;
	}
}

size_t
ttyread(void)
{
	static char buf[BUFSIZ];
	static int buflen = 0;
	int ret, written;

	ret = read(cmdfd, buf + buflen, sizeof(buf) - buflen);
	switch (ret) {
	case 0:
		exit(0);
	case -1:
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		die("read error on tty: %s\n", strerror(errno));
	default:
		buflen += ret;
		written = twrite_internal(buf, buflen);
		buflen -= written;
		if (buflen > 0)
			memmove(buf, buf + written, buflen);
		return ret;
	}
}

void
ttyresize(int tw, int th)
{
	struct winsize w;
	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "could not set window size: %s\n", strerror(errno));
}

/* ------------------------------------------------------------------ Grid */
void
tfulldirt(void)
{
	int i;
	for (i = 0; i < term.row; i++)
		term.dirty[i] = 1;
}

static void
tsetdirt(int top, int bot)
{
	int i;
	LIMIT(top, 0, term.row - 1);
	LIMIT(bot, 0, term.row - 1);
	for (i = top; i <= bot; i++)
		term.dirty[i] = 1;
}

Line
tgetline(int y)
{
#if HISTSIZE > 0
	if (y < term.scr)
		return TLINE_HIST(y);
	return term.line[y - term.scr];
#else
	return term.line[y];
#endif
}

static void
tclearregion(int x1, int y1, int x2, int y2)
{
	int x, y, t;
	Glyph *gp;

	if (x1 > x2) { t = x1; x1 = x2; x2 = t; }
	if (y1 > y2) { t = y1; y1 = y2; y2 = t; }
	LIMIT(x1, 0, term.col - 1);
	LIMIT(x2, 0, term.col - 1);
	LIMIT(y1, 0, term.row - 1);
	LIMIT(y2, 0, term.row - 1);

	for (y = y1; y <= y2; y++) {
		term.dirty[y] = 1;
		for (x = x1; x <= x2; x++) {
			gp = &term.line[y][x];
			gp->fg = term.c.attr.fg;
			gp->bg = term.c.attr.bg;
			gp->mode = 0;
			gp->u = ' ';
		}
	}
}

static void
tscrolldown(int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot - orig + 1);
	tsetdirt(orig, term.bot - n);
	tclearregion(0, term.bot - n + 1, term.col - 1, term.bot);

	for (i = term.bot; i >= orig + n; i--) {
		temp = term.line[i];
		term.line[i] = term.line[i - n];
		term.line[i - n] = temp;
	}
}

static void
tscrollup(int orig, int n, int copyhist)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot - orig + 1);

#if HISTSIZE > 0
	if (copyhist && !IS_SET(MODE_ALTSCREEN) && orig == term.top) {
		for (i = 0; i < n; i++) {
			term.histi = (term.histi + 1) % HISTSIZE;
			temp = term.hist[term.histi];            /* ältester Eintrag oder NULL */
			term.hist[term.histi] = term.line[orig + i];
			if (temp == NULL)                        /* lazy: erst bei Bedarf */
				temp = xmalloc(term.col * sizeof(Glyph));
			term.line[orig + i] = temp;              /* recyceln statt neu zu allozieren */
			if (term.histn < HISTSIZE)
				term.histn++;
		}
	}
#else
	(void)copyhist;
#endif

	tclearregion(0, orig, term.col - 1, orig + n - 1);
	tsetdirt(orig + n, term.bot);

	for (i = orig; i <= term.bot - n; i++) {
		temp = term.line[i];
		term.line[i] = term.line[i + n];
		term.line[i + n] = temp;
	}
}

static void
tmoveto(int x, int y)
{
	int miny, maxy;

	if (term.c.state & CURSOR_ORIGIN) {
		miny = term.top;
		maxy = term.bot;
	} else {
		miny = 0;
		maxy = term.row - 1;
	}
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.c.x = (x < 0) ? 0 : (x > term.col - 1 ? term.col - 1 : x);
	term.c.y = (y < miny) ? miny : (y > maxy ? maxy : y);
}

static void
tmoveato(int x, int y)
{
	tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top : 0));
}

static void
tnewline(int first_col)
{
	int y = term.c.y;

	if (y == term.bot)
		tscrollup(term.top, 1, 1);
	else
		y++;

	tmoveto(first_col ? 0 : term.c.x, y);
}

static void
tsetchar(Rune u, const Glyph *attr, int x, int y)
{
	if (term.trantbl[term.charset] == '0' && BETWEEN(u, 0x41, 0x7e) &&
	    vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	if (term.line[y][x].mode & ATTR_WIDE) {
		if (x + 1 < term.col) {
			term.line[y][x + 1].u = ' ';
			term.line[y][x + 1].mode &= ~ATTR_WDUMMY;
		}
	} else if (term.line[y][x].mode & ATTR_WDUMMY) {
		if (x - 1 >= 0) {
			term.line[y][x - 1].u = ' ';
			term.line[y][x - 1].mode &= ~ATTR_WIDE;
		}
	}

	term.dirty[y] = 1;
	term.line[y][x] = *attr;
	term.line[y][x].u = u;
}

static void
tinsertblank(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);
	dst = term.c.x + n;
	src = term.c.x;
	size = term.col - dst;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

static void
tdeletechar(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);
	dst = term.c.x;
	src = term.c.x + n;
	size = term.col - src;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(term.col - n, term.c.y, term.col - 1, term.c.y);
}

static void
tinsertblankline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrolldown(term.c.y, n);
}

static void
tdeleteline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrollup(term.c.y, n, 0);
}

static void
tsetscroll(int t, int b)
{
	int temp;
	LIMIT(t, 0, term.row - 1);
	LIMIT(b, 0, term.row - 1);
	if (t > b) { temp = t; t = b; b = temp; }
	term.top = t;
	term.bot = b;
}

static void
tswapscreen(void)
{
	Line *tmp = term.line;
	term.line = term.alt;
	term.alt = tmp;
	term.mode ^= MODE_ALTSCREEN;
	tfulldirt();
}

static void
tcursor(int mode)
{
	int alt = IS_SET(MODE_ALTSCREEN) ? 1 : 0;

	if (mode == CURSOR_SAVE) {
		csaved[alt] = term.c;
	} else if (mode == CURSOR_LOAD) {
		term.c = csaved[alt];
		tmoveto(term.c.x, term.c.y);
	}
}

static void
treset(void)
{
	unsigned int i;

	term.c = (TCursor){
		.attr = { .mode = ATTR_NULL, .fg = DEFAULTFG, .bg = DEFAULTBG },
		.x = 0, .y = 0, .state = CURSOR_DEFAULT
	};

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for (i = tabspaces; (int)i < term.col; i += tabspaces)
		term.tabs[i] = 1;
	term.top = 0;
	term.bot = term.row - 1;
	term.mode = MODE_WRAP;
	memset(term.trantbl, 0, sizeof(term.trantbl));
	term.charset = 0;

	for (i = 0; i < 2; i++) {
		tmoveto(0, 0);
		tcursor(CURSOR_SAVE);
		tclearregion(0, 0, term.col - 1, term.row - 1);
		tswapscreen();
	}
}

/* Hard Reset (Ctrl+Shift+R): vollständiger Zustand + Scrollback verwerfen.
 * Rettet das Terminal nach kaputten Escape-Sequenzen. */
void
tresetfull(void)
{
	treset();
#if HISTSIZE > 0
	term.histi = 0;
	term.histn = 0;
	term.scr = 0;
#endif
	tfulldirt();
}

/* Schirm + Scrollback leeren, Cursor nach home (Ctrl+Shift+L).
 * Attribute/Modi/Scroll-Region bleiben erhalten. */
void
tclearall(void)
{
#if HISTSIZE > 0
	term.histi = 0;
	term.histn = 0;
	term.scr = 0;
#endif
	tclearregion(0, 0, term.col - 1, term.row - 1);
	tmoveto(0, 0);
	tfulldirt();
}

void
tnew(int col, int row)
{
	term = (Term){ .c = { .attr = { .fg = DEFAULTFG, .bg = DEFAULTBG } } };
	term.row = row;
	term.col = col;

	term.line = xcalloc(row, sizeof(Line));
	term.alt = xcalloc(row, sizeof(Line));
	term.dirty = xcalloc(row, sizeof(*term.dirty));
	term.tabs = xcalloc(col, sizeof(*term.tabs));
	for (int i = 0; i < row; i++) {
		term.line[i] = xcalloc(col, sizeof(Glyph));
		term.alt[i] = xcalloc(col, sizeof(Glyph));
	}
#if HISTSIZE > 0
	/* Scrollback-Zeilen werden lazy angelegt (erst beim Scrollen), nicht vorab —
	 * ein frisch geöffnetes Terminal kostet so fast keinen Scrollback-Speicher. */
	term.hist = xcalloc(HISTSIZE, sizeof(Line));
	term.histi = 0;
	term.histn = 0;
	term.scr = 0;
#endif
	treset();
}

void
tresize(int col, int row)
{
	int i, j;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);

	if (col < 1 || row < 1)
		return;

	/* überschüssige Zeilen freigeben */
	for (i = row; i < term.row; i++) {
		free(term.line[i]);
		free(term.alt[i]);
	}
	term.line = xrealloc(term.line, row * sizeof(Line));
	term.alt = xrealloc(term.alt, row * sizeof(Line));
	term.dirty = xrealloc(term.dirty, row * sizeof(*term.dirty));
	term.tabs = xrealloc(term.tabs, col * sizeof(*term.tabs));

	/* bestehende Zeilen auf neue Breite bringen */
	for (i = 0; i < minrow; i++) {
		term.line[i] = xrealloc(term.line[i], col * sizeof(Glyph));
		term.alt[i] = xrealloc(term.alt[i], col * sizeof(Glyph));
	}
	/* neue Zeilen anlegen */
	for (i = minrow; i < row; i++) {
		term.line[i] = xmalloc(col * sizeof(Glyph));
		term.alt[i] = xmalloc(col * sizeof(Glyph));
	}

#if HISTSIZE > 0
	/* Scrollback über den Resize hinweg erhalten: nur belegte Zeilen auf die neue
	 * Breite bringen (kein Reflow -> beim Verschmälern rechts abgeschnitten, beim
	 * Verbreitern rechts aufgefüllt). Lazy: ungenutzte Slots bleiben NULL. */
	for (i = 0; i < HISTSIZE; i++) {
		if (!term.hist[i])
			continue;
		term.hist[i] = xrealloc(term.hist[i], col * sizeof(Glyph));
		if (col > mincol) {
			Glyph blank = { .u = ' ', .fg = DEFAULTFG, .bg = DEFAULTBG };
			for (j = mincol; j < col; j++)
				term.hist[i][j] = blank;
		}
	}
	term.scr = 0; /* Anzeige ans untere Ende; histi/histn bleiben gültig */
#endif

	if (col > term.col) {
		int *bp = term.tabs + term.col;
		memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
		while (--bp > term.tabs && !*bp)
			;
		for (bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
			*bp = 1;
	}

	term.col = col;
	term.row = row;

	/* neue/verbreiterte Bereiche leeren */
	{
		int oldcol = mincol, oldrow = minrow;
		Glyph g = { .u = ' ', .fg = DEFAULTFG, .bg = DEFAULTBG };
		for (i = 0; i < row; i++) {
			for (j = (i < oldrow ? oldcol : 0); j < col; j++) {
				term.line[i][j] = g;
				term.alt[i][j] = g;
			}
		}
	}

	/* Cursor in den gültigen Bereich klemmen */
	LIMIT(term.c.x, 0, col - 1);
	LIMIT(term.c.y, 0, row - 1);
	tsetscroll(0, row - 1);
	for (i = 0; i < 2; i++)
		LIMIT(csaved[i].x, 0, col - 1), LIMIT(csaved[i].y, 0, row - 1);

	tfulldirt();
#ifdef __GLIBC__
	malloc_trim(0); /* beim Verkleinern freigewordenen Heap ans OS zurückgeben */
#endif
}

/* ------------------------------------------------------------------ Scrollback */
void
kscrollup(int n)
{
#if HISTSIZE > 0
	if (IS_SET(MODE_ALTSCREEN))
		return;
	if (n < 0)
		n = term.row;
	if (term.scr + n > term.histn)
		n = term.histn - term.scr;
	if (n <= 0)
		return;
	term.scr += n;
	tfulldirt();
#else
	(void)n;
#endif
}

void
kscrolldown(int n)
{
#if HISTSIZE > 0
	if (n < 0)
		n = term.row;
	if (n > term.scr)
		n = term.scr;
	if (n <= 0)
		return;
	term.scr -= n;
	tfulldirt();
#else
	(void)n;
#endif
}

/* ------------------------------------------------------------------ Farben (SGR) */
static int32_t
tdefcolor(const int *attr, int *npar, int l)
{
	int32_t idx = -1;
	int r, g, b;

	if (*npar + 1 >= l)
		return -1; /* kein Typ-Byte vorhanden -> kein OOB-Read auf attr[] */

	switch (attr[*npar + 1]) {
	case 2: /* direct color in RGB space */
		if (*npar + 4 >= l)
			break;
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			break;
		idx = TRUECOLOR(r, g, b);
		break;
	case 5: /* indexed color */
		if (*npar + 2 >= l)
			break;
		*npar += 2;
		if (!BETWEEN(attr[*npar], 0, 255))
			break;
		idx = attr[*npar];
		break;
	default:
		break;
	}
	return idx;
}

static void
tsetattr(const int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT | ATTR_ITALIC |
			    ATTR_UNDERLINE | ATTR_BLINK | ATTR_REVERSE |
			    ATTR_INVISIBLE | ATTR_STRUCK);
			term.c.attr.fg = DEFAULTFG;
			term.c.attr.bg = DEFAULTBG;
			break;
		case 1:  term.c.attr.mode |= ATTR_BOLD; break;
		case 2:  term.c.attr.mode |= ATTR_FAINT; break;
		case 3:  term.c.attr.mode |= ATTR_ITALIC; break;
		case 4:  term.c.attr.mode |= ATTR_UNDERLINE; break;
		case 5: /* slow blink */
		case 6:  term.c.attr.mode |= ATTR_BLINK; break;
		case 7:  term.c.attr.mode |= ATTR_REVERSE; break;
		case 8:  term.c.attr.mode |= ATTR_INVISIBLE; break;
		case 9:  term.c.attr.mode |= ATTR_STRUCK; break;
		case 22: term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT); break;
		case 23: term.c.attr.mode &= ~ATTR_ITALIC; break;
		case 24: term.c.attr.mode &= ~ATTR_UNDERLINE; break;
		case 25: term.c.attr.mode &= ~ATTR_BLINK; break;
		case 27: term.c.attr.mode &= ~ATTR_REVERSE; break;
		case 28: term.c.attr.mode &= ~ATTR_INVISIBLE; break;
		case 29: term.c.attr.mode &= ~ATTR_STRUCK; break;
		case 38:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.fg = idx;
			break;
		case 39: term.c.attr.fg = DEFAULTFG; break;
		case 48:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.bg = idx;
			break;
		case 49: term.c.attr.bg = DEFAULTBG; break;
		default:
			if (BETWEEN(attr[i], 30, 37))
				term.c.attr.fg = attr[i] - 30;
			else if (BETWEEN(attr[i], 40, 47))
				term.c.attr.bg = attr[i] - 40;
			else if (BETWEEN(attr[i], 90, 97))
				term.c.attr.fg = attr[i] - 90 + 8;
			else if (BETWEEN(attr[i], 100, 107))
				term.c.attr.bg = attr[i] - 100 + 8;
			break;
		}
	}
}

/* ------------------------------------------------------------------ CSI */
static void
csiparse(void)
{
	char *p = csiescseq.buf, *np;
	long int v;

	csiescseq.narg = 0;
	if (*p == '?') {
		csiescseq.priv = 1;
		p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while (p < csiescseq.buf + csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		/* Auf sane Bereich klemmen: verhindert signed-overflow-UB in
		 * Operationen wie (term.c.x + arg) bei riesigen Parametern und
		 * ignoriert via '-' injizierte Negativwerte. Kein echtes CSI
		 * braucht einen Parameter > 0xffff. */
		if (v < 0)
			v = 0;
		else if (v > 0xffff)
			v = 0xffff;
		csiescseq.arg[csiescseq.narg++] = (int)v;
		p = np;
		if (*p != ';' || csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode[0] = *p++;
	csiescseq.mode[1] = (p < csiescseq.buf + csiescseq.len) ? *p : '\0';
}

static void
csireset(void)
{
	memset(&csiescseq, 0, sizeof(csiescseq));
}

#define DEFAULT(arg, def) ((arg) = (arg) ? (arg) : (def))

static void
csihandle(void)
{
	char buf[40];
	int len;
	int n;

	switch (csiescseq.mode[0]) {
	default:
	unknown:
		/* unbekannte Sequenz still ignorieren */
		break;
	case '@': /* ICH -- insert blank chars */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(csiescseq.arg[0]);
		break;
	case 'A': /* CUU */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y - csiescseq.arg[0]);
		break;
	case 'B': /* CUD */
	case 'e': /* VPR */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y + csiescseq.arg[0]);
		break;
	case 'i': /* MC -- media copy, ignorieren */
	case 'c': /* DA -- device attributes */
		if (csiescseq.arg[0] == 0)
			ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'C': /* CUF */
	case 'a': /* HPR */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x + csiescseq.arg[0], term.c.y);
		break;
	case 'D': /* CUB */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x - csiescseq.arg[0], term.c.y);
		break;
	case 'E': /* CNL */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y + csiescseq.arg[0]);
		break;
	case 'F': /* CPL */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y - csiescseq.arg[0]);
		break;
	case 'g': /* TBC */
		switch (csiescseq.arg[0]) {
		case 0:
			term.tabs[term.c.x] = 0;
			break;
		case 3:
			memset(term.tabs, 0, term.col * sizeof(*term.tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA */
	case '`': /* HPA */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(csiescseq.arg[0] - 1, term.c.y);
		break;
	case 'H': /* CUP */
	case 'f': /* HVP */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(csiescseq.arg[1] - 1, csiescseq.arg[0] - 1);
		break;
	case 'I': /* CHT -- cursor forward tabulation */
		DEFAULT(csiescseq.arg[0], 1);
		for (n = csiescseq.arg[0]; n > 0 && term.c.x < term.col - 1; n--) {
			for (++term.c.x; term.c.x < term.col - 1 && !term.tabs[term.c.x]; ++term.c.x)
				;
		}
		break;
	case 'J': /* ED -- erase in display */
		switch (csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
			if (term.c.y < term.row - 1)
				tclearregion(0, term.c.y + 1, term.col - 1, term.row - 1);
			break;
		case 1: /* above */
			if (term.c.y > 0)
				tclearregion(0, 0, term.col - 1, term.c.y - 1);
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
		case 3:
			tclearregion(0, 0, term.col - 1, term.row - 1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- erase in line */
		switch (csiescseq.arg[0]) {
		case 0:
			tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
			break;
		case 1:
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2:
			tclearregion(0, term.c.y, term.col - 1, term.c.y);
			break;
		}
		break;
	case 'S': /* SU -- scroll up */
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(term.top, csiescseq.arg[0], 0);
		break;
	case 'T': /* SD -- scroll down */
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(term.top, csiescseq.arg[0]);
		break;
	case 'L': /* IL -- insert blank lines */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(csiescseq.arg[0]);
		break;
	case 'l': /* RM -- reset mode */
		tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M': /* DL -- delete lines */
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(csiescseq.arg[0]);
		break;
	case 'X': /* ECH -- erase n chars */
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(term.c.x, term.c.y,
		    term.c.x + csiescseq.arg[0] - 1, term.c.y);
		break;
	case 'P': /* DCH -- delete chars */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- cursor backward tabulation */
		DEFAULT(csiescseq.arg[0], 1);
		for (n = csiescseq.arg[0]; n > 0 && term.c.x > 0; n--) {
			for (--term.c.x; term.c.x > 0 && !term.tabs[term.c.x]; --term.c.x)
				;
		}
		break;
	case 'd': /* VPA -- move to row */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(term.c.x, csiescseq.arg[0] - 1);
		break;
	case 'h': /* SM -- set mode */
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR */
		tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'n': /* DSR -- device status report */
		switch (csiescseq.arg[0]) {
		case 5: /* operating status */
			ttywrite("\033[0n", 4, 0);
			break;
		case 6: /* cursor position */
			len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
			    term.c.y + 1, term.c.x + 1);
			ttywrite(buf, len, 0);
			break;
		}
		break;
	case 'r': /* DECSTBM -- set scrolling region */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], term.row);
		tsetscroll(csiescseq.arg[0] - 1, csiescseq.arg[1] - 1);
		tmoveato(0, 0);
		break;
	case 's': /* DECSC -- save cursor (ANSI.SYS) */
		tcursor(CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- restore cursor */
		tcursor(CURSOR_LOAD);
		break;
	case 'q': /* DECSCUSR -- cursor style: fix Block, ignorieren */
	case 't': /* window ops -- ignorieren */
	case 'p': /* soft reset etc -- ignorieren */
		break;
	}
}

static void
tsetmode(int priv, int set, const int *args, int narg)
{
	int alt;
	const int *lim;

	for (lim = args + narg; args < lim; ++args) {
		if (priv) {
			switch (*args) {
			case 1: /* DECCKM -- application cursor keys */
				if (set) term.mode |= MODE_APPCURSOR;
				else term.mode &= ~MODE_APPCURSOR;
				break;
			case 7: /* DECAWM -- auto wrap */
				if (set) term.mode |= MODE_WRAP;
				else term.mode &= ~MODE_WRAP;
				break;
			case 25: /* DECTCEM -- cursor visibility */
				if (set) term.mode &= ~MODE_HIDE;
				else term.mode |= MODE_HIDE;
				break;
			case 9: /* X10 mouse compatibility */
				MODBIT(term.mode, set, MODE_MOUSEX10);
				break;
			case 1000: /* report button press/release */
				MODBIT(term.mode, set, MODE_MOUSEBTN);
				break;
			case 1002: /* report motion while a button is pressed */
				MODBIT(term.mode, set, MODE_MOUSEMOTION);
				break;
			case 1003: /* report any motion */
				MODBIT(term.mode, set, MODE_MOUSEMANY);
				break;
			case 1006: /* SGR mouse encoding */
				MODBIT(term.mode, set, MODE_MOUSESGR);
				break;
			case 1015: /* urxvt encoding -- nicht implementiert */
			case 1016: /* SGR pixel encoding -- nicht implementiert */
			case 1004: /* focus events */
			case 12:   /* cursor blink */
				break;
			case 1049: /* swap screen & save/restore cursor */
				if (!allowaltscreen)
					break;
				tcursor(set ? CURSOR_SAVE : CURSOR_LOAD);
				/* FALLTHROUGH */
			case 47:
			case 1047:
				if (!allowaltscreen)
					break;
				alt = IS_SET(MODE_ALTSCREEN);
				if (alt)
					tclearregion(0, 0, term.col - 1, term.row - 1);
				if (set ^ alt)
					tswapscreen();
				break;
			case 1048:
				if (!allowaltscreen)
					break;
				tcursor(set ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			case 2004: /* bracketed paste */
				if (set) term.mode |= MODE_BRCKTPASTE;
				else term.mode &= ~MODE_BRCKTPASTE;
				break;
			default:
				break;
			}
		} else {
			switch (*args) {
			case 4: /* IRM -- insertion-replacement */
				if (set) term.mode |= MODE_INSERT;
				else term.mode &= ~MODE_INSERT;
				break;
			case 20: /* LNM -- linefeed/newline */
				if (set) term.mode |= MODE_CRLF;
				else term.mode &= ~MODE_CRLF;
				break;
			default:
				break;
			}
		}
	}
}

/* ------------------------------------------------------------------ STR (OSC) */
static void
strreset(void)
{
	memset(&strescseq, 0, sizeof(strescseq));
}

static void
strparse(void)
{
	int c;
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';

	if (*p == '\0')
		return;

	while (strescseq.narg < STR_ARG_SIZ) {
		strescseq.args[strescseq.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

static void
strhandle(void)
{
	int par;

	term.esc &= ~(ESC_STR_END | ESC_STR);
	strparse();
	par = (strescseq.narg) ? atoi(strescseq.args[0]) : 0;

	switch (strescseq.type) {
	case ']': /* OSC */
		switch (par) {
		case 0:
		case 1:
		case 2:
			if (strescseq.narg > 1)
				xsettitle(strescseq.args[1]);
			return;
		default:
			/* 4 (Palette), 52 (Clipboard), 10/11/12 ... -> post-v1 */
			break;
		}
		break;
	case 'k': /* alter Titel (rxvt) */
		xsettitle(strescseq.args[0]);
		break;
	case 'P': /* DCS */
	case '_': /* APC */
	case '^': /* PM */
		break;
	}
}

static void
tstrsequence(unsigned char c)
{
	strreset();
	switch (c) {
	case 0x90: c = 'P'; break; /* DCS */
	case 0x9f: c = '_'; break; /* APC */
	case 0x9e: c = '^'; break; /* PM */
	case 0x9d: c = ']'; break; /* OSC */
	}
	strescseq.type = c;
	term.esc |= ESC_STR;
}

/* ------------------------------------------------------------------ Control / ESC */
static void
tcontrolcode(Rune u)
{
	switch (u) {
	case '\t': /* HT */
		{
			int x = term.c.x;
			if (x < term.col - 1) {
				do { x++; } while (x < term.col - 1 && !term.tabs[x]);
				tmoveto(x, term.c.y);
			}
		}
		return;
	case '\b': /* BS */
		tmoveto(term.c.x - 1, term.c.y);
		return;
	case '\r': /* CR */
		tmoveto(0, term.c.y);
		return;
	case '\f': /* LF */
	case '\v': /* VT */
	case '\n': /* LF */
		tnewline(IS_SET(MODE_CRLF));
		return;
	case '\a': /* BEL */
		if (term.esc & ESC_STR_END)
			strhandle();
		else
			xbell();
		break;
	case '\033': /* ESC */
		csireset();
		term.esc &= ~(ESC_CSI | ESC_ALTCHARSET | ESC_TEST);
		term.esc |= ESC_START;
		return;
	case '\016': /* SO -- LS1 */
	case '\017': /* SI -- LS0 */
		term.charset = 1 - (u - '\016');
		return;
	case 0x84: /* IND */
		tnewline(0);
		break;
	case 0x85: /* NEL */
		tnewline(1);
		break;
	case 0x88: /* HTS */
		term.tabs[term.c.x] = 1;
		break;
	case 0x90: /* DCS */
	case 0x9d: /* OSC */
	case 0x9e: /* PM */
	case 0x9f: /* APC */
		tstrsequence(u);
		return;
	case 0x9b: /* CSI */
		csireset();
		term.esc |= ESC_START | ESC_CSI;
		return;
	default:
		break;
	}
	/* C0/C1 unterbrechen sonstige Escape-Verarbeitung nicht */
	term.esc &= ~(ESC_STR_END | ESC_STR);
}

static int
eschandle(unsigned char ascii)
{
	switch (ascii) {
	case '[':
		term.esc |= ESC_CSI;
		return 0;
	case '#':
		term.esc |= ESC_TEST;
		return 0;
	case '%': /* charset auswahl utf8 etc. -- ignorieren */
		return 0;
	case 'P': /* DCS */
	case '_': /* APC */
	case '^': /* PM */
	case ']': /* OSC */
	case 'k': /* alter Titel */
		tstrsequence(ascii);
		return 0;
	case 'n': /* LS2 */
	case 'o': /* LS3 */
		term.charset = 2 + (ascii - 'n');
		break;
	case '(': /* G0 */
	case ')': /* G1 */
	case '*': /* G2 */
	case '+': /* G3 */
		term.icharset = ascii - '(';
		term.esc |= ESC_ALTCHARSET;
		return 0;
	case 'D': /* IND */
		if (term.c.y == term.bot)
			tscrollup(term.top, 1, 1);
		else
			tmoveto(term.c.x, term.c.y + 1);
		break;
	case 'E': /* NEL */
		tnewline(1);
		break;
	case 'H': /* HTS */
		term.tabs[term.c.x] = 1;
		break;
	case 'M': /* RI -- reverse index */
		if (term.c.y == term.top)
			tscrolldown(term.top, 1);
		else
			tmoveto(term.c.x, term.c.y - 1);
		break;
	case 'Z': /* DECID */
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'c': /* RIS -- reset */
		treset();
		xsettitle(NULL);
		break;
	case '=': /* DECPAM */
		term.mode |= MODE_APPKEYPAD;
		break;
	case '>': /* DECPNM */
		term.mode &= ~MODE_APPKEYPAD;
		break;
	case '7': /* DECSC */
		tcursor(CURSOR_SAVE);
		break;
	case '8': /* DECRC */
		tcursor(CURSOR_LOAD);
		break;
	case '\\': /* ST */
		if (term.esc & ESC_STR_END)
			strhandle();
		break;
	default:
		break;
	}
	return 1;
}

/* ------------------------------------------------------------------ tputc */
static void
tdeftran(unsigned char ascii)
{
	term.trantbl[term.icharset] = ascii;
}

static void
tputc(Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len;
	Glyph *gp;

	control = ISCONTROL(u);
	if (u < 127) {
		c[0] = u;
		len = 1;
	} else {
		len = utf8encode(u, c);
	}
	if (control)
		width = 1;
	else if ((width = wcwidth(u)) < 1)
		width = 1;

	/* STR-Sequenz hat Vorrang: sammelt Bytes bis zum Terminator */
	if (term.esc & ESC_STR) {
		if (u == '\a' || u == 030 || u == 031 || u == 033 || ISCONTROLC1(u)) {
			term.esc &= ~(ESC_START | ESC_STR);
			term.esc |= ESC_STR_END;
			goto check_control;
		}
		if (strescseq.len + len < sizeof(strescseq.buf) - 1) {
			memmove(&strescseq.buf[strescseq.len], c, len);
			strescseq.len += len;
		}
		return;
	}

check_control:
	if (control) {
		tcontrolcode(u);
		if (!term.esc)
			term.lastc = 0;
		return;
	} else if (term.esc & ESC_START) {
		if (term.esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = u;
			if (BETWEEN(u, 0x40, 0x7E) ||
			    csiescseq.len >= sizeof(csiescseq.buf) - 1) {
				term.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		} else if (term.esc & ESC_ALTCHARSET) {
			tdeftran(u);
		} else if (term.esc & ESC_TEST) {
			/* DECALN: Bildschirm mit 'E' füllen */
			if (u == '8') {
				int x, y;
				Glyph g = term.c.attr;
				g.u = 'E';
				for (y = 0; y < term.row; y++)
					for (x = 0; x < term.col; x++)
						tsetchar('E', &g, x, y);
			}
		} else if (!eschandle(u)) {
			return;
		}
		term.esc = 0;
		return;
	}

	gp = &term.line[term.c.y][term.c.x];
	if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
		gp->mode |= ATTR_WRAP;
		tnewline(1);
		gp = &term.line[term.c.y][term.c.x];
	}

	if (IS_SET(MODE_INSERT) && term.c.x + width < term.col)
		memmove(gp + width, gp, (term.col - term.c.x - width) * sizeof(Glyph));

	if (term.c.x + width > term.col) {
		tnewline(1);
		gp = &term.line[term.c.y][term.c.x];
	}

	tsetchar(u, &term.c.attr, term.c.x, term.c.y);
	term.lastc = u;

	if (width == 2) {
		gp->mode |= ATTR_WIDE;
		if (term.c.x + 1 < term.col) {
			if (gp[1].mode == ATTR_WIDE && term.c.x + 2 < term.col) {
				gp[2].u = ' ';
				gp[2].mode &= ~ATTR_WDUMMY;
			}
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if (term.c.x + width < term.col) {
		tmoveto(term.c.x + width, term.c.y);
	} else {
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

static int
twrite_internal(const char *buf, int buflen)
{
	int charsize, n;
	Rune u;

#if HISTSIZE > 0
	/* neue Ausgabe: ans untere Ende springen */
	if (term.scr != 0) {
		term.scr = 0;
		tfulldirt();
	}
#endif

	for (n = 0; n < buflen; n += charsize) {
		charsize = utf8decode(buf + n, &u, buflen - n);
		if (charsize == 0)
			break; /* unvollständige UTF-8-Folge am Pufferende */
		tputc(u);
	}
	return n;
}
