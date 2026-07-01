/*
 * tintty — Wayland-Frontend: Display/Registry, xdg-shell, wl_shm-Puffer,
 * xkbcommon-Input mit Key-Repeat, poll-Event-Loop. Enthält main().
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <pixman.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"

#include "config.h"
#include "tin.h"
#include "win.h"
#include "render.h"

struct buffer {
	struct wl_buffer *wlbuf;
	void *data;
	pixman_image_t *pix;
	int busy;
};

static struct {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_keyboard *kbd;
	struct wl_pointer *ptr;
	struct wl_output *output;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct wp_fractional_scale_manager_v1 *frac_mgr;
	struct wp_viewporter *viewporter;

	/* Zwischenablage (nur Paste) */
	struct wl_data_device_manager *dd_mgr;
	struct wl_data_device *data_dev;
	struct wl_data_offer *sel_offer;                  /* aktuelle Clipboard-Auswahl */
	struct zwp_primary_selection_device_manager_v1 *psel_mgr;
	struct zwp_primary_selection_device_v1 *psel_dev;
	struct zwp_primary_selection_offer_v1 *psel_offer; /* aktuelle Primary-Auswahl */

	struct wl_surface *surf;
	struct xdg_surface *xsurf;
	struct xdg_toplevel *toplevel;
	struct zxdg_toplevel_decoration_v1 *deco;
	struct wp_fractional_scale_v1 *frac;
	struct wp_viewport *viewport;
	int have_frac;

	/* shm */
	struct wl_shm_pool *pool;
	void *shm_data;
	size_t shm_size;
	int shm_fd;
	struct buffer bufs[2];
	int bufw, bufh; /* aktuelle Puffergröße in physischen px */

	/* Maus-Cursor (Theme) */
	struct wl_cursor_theme *cur_theme;
	struct wl_cursor *cur_default, *cur_hand;
	struct wl_surface *cur_surf;
	int cur_size, cur_scale;
	uint32_t ptr_serial; /* letzter pointer-enter-serial (für set_cursor) */

	/* xkb */
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	/* key repeat */
	int repeat_fd;
	int repeat_rate;   /* Hz */
	int repeat_delay;  /* ms */
	uint32_t repeat_code;
	char repeat_buf[64];
	int repeat_len;

	int scale120;     /* Anzeige-Scale in 1/120-Einheiten (120 = 1.0) */
	int cfg_w, cfg_h; /* zuletzt konfigurierte logische Größe */
	int logw, logh;   /* aktuelle logische Puffergröße (für viewport) */
	int configured;
	int need_redraw;
	int closed;
} g;

/* Pointer-/Wheel-Zustand */
static int      ptr_col = -1, ptr_row = -1;
static uint32_t ptr_btn;                 /* bit0 links, bit1 mitte, bit2 rechts */
static int      ptr_lcol = -1, ptr_lrow = -1; /* zuletzt gemeldet (Motion-Dedup) */

/* Hover-Link-Zustand */
#define URLBUF 4096
static int      link_active;             /* steht der Pointer über einem Link? */
static int      hl_sr, hl_sc, hl_er, hl_ec; /* aktuelle Hover-Spanne */
static char     hl_url[URLBUF];          /* aktuell gehoverte URL */
static int      press_link;              /* lag der linke Press über einem Link? */
static char     press_url[URLBUF];       /* URL beim Press (für Press==Release-Check) */
static int      ax_v120;                 /* akkumulierter Rest (value120) */
static double   ax_cont;                 /* akkumulierter Rest (Touchpad) */
static int      frame_v120, frame_cont_seen;
static double   frame_cont;

static _Noreturn void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* ------------------------------------------------------------------ Puffer */
static int
alloc_shm(size_t size)
{
	int fd = memfd_create("tintty", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void buffer_release(void *data, struct wl_buffer *b);
static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void
destroy_buffers(void)
{
	int i;
	for (i = 0; i < 2; i++) {
		if (g.bufs[i].pix) {
			pixman_image_unref(g.bufs[i].pix);
			g.bufs[i].pix = NULL;
		}
		if (g.bufs[i].wlbuf) {
			wl_buffer_destroy(g.bufs[i].wlbuf);
			g.bufs[i].wlbuf = NULL;
		}
		g.bufs[i].busy = 0;
	}
	if (g.pool) {
		wl_shm_pool_destroy(g.pool);
		g.pool = NULL;
	}
	if (g.shm_data) {
		munmap(g.shm_data, g.shm_size);
		g.shm_data = NULL;
	}
	if (g.shm_fd >= 0) {
		close(g.shm_fd);
		g.shm_fd = -1;
	}
}

static void
create_buffers(int pw, int ph)
{
	int stride = pw * 4;
	size_t bufsize = (size_t)stride * ph;
	size_t total = bufsize * 2;
	int i;

	destroy_buffers();

	g.shm_fd = alloc_shm(total);
	if (g.shm_fd < 0)
		die("tintty: shm alloc fehlgeschlagen");
	g.shm_data = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, g.shm_fd, 0);
	if (g.shm_data == MAP_FAILED)
		die("tintty: mmap fehlgeschlagen");
	g.shm_size = total;
	g.pool = wl_shm_create_pool(g.shm, g.shm_fd, total);

	for (i = 0; i < 2; i++) {
		size_t off = (size_t)i * bufsize;
		g.bufs[i].wlbuf = wl_shm_pool_create_buffer(g.pool, off, pw, ph,
		    stride, WL_SHM_FORMAT_ARGB8888);
		wl_buffer_add_listener(g.bufs[i].wlbuf, &buffer_listener, &g.bufs[i]);
		g.bufs[i].data = (char *)g.shm_data + off;
		g.bufs[i].pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, pw, ph,
		    (uint32_t *)g.bufs[i].data, stride);
		g.bufs[i].busy = 0;
	}
}

static void
buffer_release(void *data, struct wl_buffer *b)
{
	(void)b;
	((struct buffer *)data)->busy = 0;
}

static void
configure_size(int lw, int lh)
{
	double sc = g.scale120 / 120.0;
	int bpx = (int)lround(borderpx * sc);
	int cw = rcellw(), ch = rcellh();
	int pw, ph, cols, rows;

	if (lw > 0 && lh > 0) {
		pw = (int)lround(lw * sc);
		ph = (int)lround(lh * sc);
	} else {
		/* keine vom Compositor vorgegebene Größe -> Default-Grid */
		pw = default_cols * cw + 2 * bpx;
		ph = default_rows * ch + 2 * bpx;
		lw = (int)lround(pw / sc);
		lh = (int)lround(ph / sc);
	}

	cols = (pw - 2 * bpx) / cw;
	rows = (ph - 2 * bpx) / ch;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;

	if (pw == g.bufw && ph == g.bufh && cols == term.col && rows == term.row) {
		if (g.have_frac && g.viewport)
			wp_viewport_set_destination(g.viewport, lw, lh);
		return;
	}

	/* Puffer nur neu anlegen, wenn sich die Pixelgröße ändert — beim Zoom
	 * (gleiches Fenster, anderes Grid) bleiben die shm-Puffer erhalten. */
	if (pw != g.bufw || ph != g.bufh)
		create_buffers(pw, ph);
	g.bufw = pw;
	g.bufh = ph;
	g.logw = lw;
	g.logh = lh;
	tresize(cols, rows);
	ttyresize(pw - 2 * bpx, ph - 2 * bpx);
	if (g.have_frac && g.viewport)
		wp_viewport_set_destination(g.viewport, lw, lh);
	rdirtyall();
	g.need_redraw = 1;
}

/* Zoom: Fonts neu laden, dann Grid/Puffer an die neue Zellgröße anpassen.
 * Die Fenstergröße bleibt fix (Hyprland tilt ohnehin) -> cols/rows ändern sich. */
static void
do_zoom(int delta)
{
	if (!rzoom(delta))
		return; /* an Grenze / keine Änderung */
	configure_size(g.logw, g.logh);
	g.need_redraw = 1;
}

static void
draw(void)
{
	struct buffer *b = NULL;
	int i;

	if (!g.configured)
		return;
	for (i = 0; i < 2; i++)
		if (!g.bufs[i].busy) {
			b = &g.bufs[i];
			break;
		}
	if (!b) {
		g.need_redraw = 1; /* beide Puffer in Benutzung — beim Release erneut */
		return;
	}

	rdraw(b->pix, (int)lround(borderpx * g.scale120 / 120.0));

	wl_surface_attach(g.surf, b->wlbuf, 0, 0);
	if (g.have_frac)
		/* Skalierung via viewport (in configure_size gesetzt) */;
	else
		wl_surface_set_buffer_scale(g.surf, g.scale120 / 120);
	wl_surface_damage_buffer(g.surf, 0, 0, g.bufw, g.bufh);
	wl_surface_commit(g.surf);
	b->busy = 1;
	g.need_redraw = 0;
}

/* ------------------------------------------------------------------ Callbacks Core->Frontend */
void
xbell(void)
{
	/* v1: kein akustisches/visuelles Bell */
}

void
xsettitle(const char *title)
{
	if (g.toplevel)
		xdg_toplevel_set_title(g.toplevel, title && *title ? title : "tintty");
}

/* ------------------------------------------------------------------ Zwischenablage (Paste)
 * tintty kann (v1) nur einfügen, nicht kopieren: Clipboard via wl_data_device,
 * Primary Selection via zwp_primary_selection_device_v1. Jedes Offer trägt
 * seinen bevorzugten Text-MIME-Typ als user_data (struct offer_state). */
#define PASTE_MAX (8 * 1024 * 1024)

struct offer_state {
	int  rank;        /* 0 = noch kein brauchbarer Text-MIME gefunden */
	char mime[96];
};

/* Höherer Rang = bevorzugt; 0 = kein Text-Typ (Paste wird zum No-op). */
static int
mime_rank(const char *m)
{
	if (!strcmp(m, "text/plain;charset=utf-8")) return 5;
	if (!strcmp(m, "UTF8_STRING"))              return 4;
	if (!strcmp(m, "text/plain"))               return 3;
	if (!strcmp(m, "STRING"))                   return 2;
	if (!strcmp(m, "TEXT"))                     return 1;
	return 0;
}

static void
offer_consider(struct offer_state *st, const char *mime)
{
	int r = mime_rank(mime);
	if (st && r > st->rank) {
		st->rank = r;
		snprintf(st->mime, sizeof st->mime, "%s", mime);
	}
}

/* --- wl_data_offer (Clipboard) --- */
static void
doffer_offer(void *data, struct wl_data_offer *o, const char *mime)
{ (void)o; offer_consider(data, mime); }
static void
doffer_source_actions(void *data, struct wl_data_offer *o, uint32_t a)
{ (void)data; (void)o; (void)a; }
static void
doffer_action(void *data, struct wl_data_offer *o, uint32_t a)
{ (void)data; (void)o; (void)a; }

static const struct wl_data_offer_listener doffer_listener = {
	doffer_offer, doffer_source_actions, doffer_action,
};

/* --- zwp_primary_selection_offer_v1 (Primary) --- */
static void
poffer_offer(void *data, struct zwp_primary_selection_offer_v1 *o, const char *mime)
{ (void)o; offer_consider(data, mime); }

static const struct zwp_primary_selection_offer_v1_listener poffer_listener = {
	poffer_offer,
};

/* --- wl_data_device (Clipboard-Selektion) --- */
static void
dd_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *id)
{
	(void)data; (void)dev;
	wl_data_offer_add_listener(id, &doffer_listener, calloc(1, sizeof(struct offer_state)));
}
static void
dd_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *id)
{
	(void)data; (void)dev;
	if (g.sel_offer) {
		free(wl_data_offer_get_user_data(g.sel_offer));
		wl_data_offer_destroy(g.sel_offer);
	}
	g.sel_offer = id;
}
/* Kein Drag&Drop-Empfang: ein hereinkommendes Offer sofort verwerfen. */
static void
dd_enter(void *d, struct wl_data_device *dev, uint32_t s, struct wl_surface *sf,
    wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id)
{
	(void)d; (void)dev; (void)s; (void)sf; (void)x; (void)y;
	if (id) { free(wl_data_offer_get_user_data(id)); wl_data_offer_destroy(id); }
}
static void dd_leave(void *d, struct wl_data_device *dev) { (void)d; (void)dev; }
static void dd_motion(void *d, struct wl_data_device *dev, uint32_t t,
    wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dev; (void)t; (void)x; (void)y; }
static void dd_drop(void *d, struct wl_data_device *dev) { (void)d; (void)dev; }

static const struct wl_data_device_listener dd_listener = {
	dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection,
};

/* --- zwp_primary_selection_device_v1 (Primary-Selektion) --- */
static void
psel_data_offer(void *data, struct zwp_primary_selection_device_v1 *dev,
    struct zwp_primary_selection_offer_v1 *id)
{
	(void)data; (void)dev;
	zwp_primary_selection_offer_v1_add_listener(id, &poffer_listener,
	    calloc(1, sizeof(struct offer_state)));
}
static void
psel_selection(void *data, struct zwp_primary_selection_device_v1 *dev,
    struct zwp_primary_selection_offer_v1 *id)
{
	(void)data; (void)dev;
	if (g.psel_offer) {
		free(zwp_primary_selection_offer_v1_get_user_data(g.psel_offer));
		zwp_primary_selection_offer_v1_destroy(g.psel_offer);
	}
	g.psel_offer = id;
}
static const struct zwp_primary_selection_device_v1_listener psel_listener = {
	psel_data_offer, psel_selection,
};

/* --- Paste-Kern --- */
/* receive()-Trampoline: void* -> konkreter Offer-Typ (implizite C-Konvertierung). */
static void do_recv_clip(void *o, const char *m, int fd) { wl_data_offer_receive(o, m, fd); }
static void do_recv_prim(void *o, const char *m, int fd)
{ zwp_primary_selection_offer_v1_receive(o, m, fd); }

/* Offer-Inhalt blockierend in einen Puffer lesen (die Quelle schreibt
 * asynchron, unabhängig von unserem Event-Dispatch -> kein Deadlock). */
static char *
clip_recv(void *offer, const char *mime, size_t *outlen,
    void (*recv)(void *, const char *, int))
{
	int fds[2];
	char *buf = NULL;
	size_t cap = 0, len = 0;

	*outlen = 0;
	if (pipe2(fds, O_CLOEXEC) < 0)
		return NULL;
	recv(offer, mime, fds[1]);
	close(fds[1]);
	wl_display_flush(g.dpy);

	for (;;) {
		ssize_t r;
		if (cap - len < 4096) {
			size_t ncap = cap ? cap * 2 : 65536;
			char *nb;
			if (ncap > PASTE_MAX) ncap = PASTE_MAX;
			if (ncap <= cap) break; /* Cap erreicht */
			if (!(nb = realloc(buf, ncap))) break;
			buf = nb;
			cap = ncap;
		}
		r = read(fds[0], buf + len, cap - len);
		if (r < 0) { if (errno == EINTR) continue; break; }
		if (r == 0) break; /* EOF */
		len += (size_t)r;
	}
	close(fds[0]);
	*outlen = len;
	return buf;
}

/* Zeilenenden für die Terminal-Eingabe normalisieren: \r\n und \n -> \r
 * (Enter ist \r; nackte \n verwirren readline/vim). In-place, gibt die
 * neue Länge zurück. */
static size_t
paste_normalize(char *b, size_t len)
{
	size_t r, w = 0;

	for (r = 0; r < len; r++) {
		if (b[r] == '\n') {
			if (r > 0 && b[r - 1] == '\r')
				continue; /* \r\n: \r wurde schon ausgegeben */
			b[w++] = '\r';
		} else {
			b[w++] = b[r];
		}
	}
	return w;
}

/* Puffer ans PTY. Im Bracketed-Paste-Modus wrappen und einen eingebetteten
 * Terminator \e[201~ herausfiltern (sonst könnte Inhalt aus dem Paste
 * ausbrechen). */
static void
clip_send(char *buf, size_t len)
{
	if (!buf || len == 0)
		return;
	len = paste_normalize(buf, len);
	if (term.mode & MODE_BRCKTPASTE) {
		size_t i, start = 0;
		ttywrite("\033[200~", 6, 0);
		for (i = 0; i + 6 <= len; ) {
			if (!memcmp(buf + i, "\033[201~", 6)) {
				if (i > start) ttywrite(buf + start, i - start, 0);
				start = i += 6;
			} else
				i++;
		}
		if (len > start) ttywrite(buf + start, len - start, 0);
		ttywrite("\033[201~", 6, 0);
	} else {
		ttywrite(buf, len, 0);
	}
}

static void
clip_paste_clipboard(void)
{
	struct offer_state *st;
	char *buf;
	size_t len;

	if (!g.sel_offer)
		return;
	st = wl_data_offer_get_user_data(g.sel_offer);
	if (!st || st->rank == 0)
		return;
	buf = clip_recv(g.sel_offer, st->mime, &len, do_recv_clip);
	clip_send(buf, len);
	free(buf);
	g.need_redraw = 1; /* ttywrite kann Echo bereits konsumiert haben */
}

static void
clip_paste_primary(void)
{
	struct offer_state *st;
	char *buf;
	size_t len;

	if (!g.psel_offer)
		return;
	st = zwp_primary_selection_offer_v1_get_user_data(g.psel_offer);
	if (!st || st->rank == 0)
		return;
	buf = clip_recv(g.psel_offer, st->mime, &len, do_recv_prim);
	clip_send(buf, len);
	free(buf);
	g.need_redraw = 1; /* ttywrite kann Echo bereits konsumiert haben */
}

/* ------------------------------------------------------------------ Tastatur */
static int
modon(const char *name)
{
	return g.xkb_state &&
	    xkb_state_mod_name_is_active(g.xkb_state, name, XKB_STATE_MODS_EFFECTIVE) == 1;
}

static int
ctrlchar(xkb_keysym_t s)
{
	if (s >= 'a' && s <= 'z') return s - 'a' + 1;
	if (s >= 'A' && s <= 'Z') return s - 'A' + 1;
	switch (s) {
	case ' ': case '2': case '@': return 0;
	case '3': return 0x1b;
	case '4': return 0x1c;
	case '5': return 0x1d;
	case '6': case '^': return 0x1e;
	case '7': case '_': case '/': return 0x1f;
	case '8': case '?': return 0x7f;
	}
	return -1;
}

/* xterm-Modifier-Parameter: 1 + (Shift1 + Alt2 + Ctrl4) */
static int
specialkey(xkb_keysym_t sym, int mods, char *out)
{
	int app = (term.mode & MODE_APPCURSOR) != 0;
	int m = 1 + mods;

#define ARROW(fin) \
	(mods ? sprintf(out, "\033[1;%d%c", m, fin) \
	      : app ? sprintf(out, "\033O%c", fin) \
	            : sprintf(out, "\033[%c", fin))
#define TILDE(n) \
	(mods ? sprintf(out, "\033[%d;%d~", n, m) : sprintf(out, "\033[%d~", n))
#define FN14(fin) \
	(mods ? sprintf(out, "\033[1;%d%c", m, fin) : sprintf(out, "\033O%c", fin))

	switch (sym) {
	/* KP_*: Ziffernblock ohne NumLock (mit NumLock liefern die Tasten
	 * Ziffern-Syms und laufen über den utf8-Pfad). */
	case XKB_KEY_KP_Up:
	case XKB_KEY_Up:        return ARROW('A');
	case XKB_KEY_KP_Down:
	case XKB_KEY_Down:      return ARROW('B');
	case XKB_KEY_KP_Right:
	case XKB_KEY_Right:     return ARROW('C');
	case XKB_KEY_KP_Left:
	case XKB_KEY_Left:      return ARROW('D');
	case XKB_KEY_KP_Home:
	case XKB_KEY_Home:      return ARROW('H');
	case XKB_KEY_KP_End:
	case XKB_KEY_End:       return ARROW('F');
	case XKB_KEY_KP_Begin:  return ARROW('E');
	case XKB_KEY_KP_Insert:
	case XKB_KEY_Insert:    return TILDE(2);
	case XKB_KEY_KP_Delete:
	case XKB_KEY_Delete:    return TILDE(3);
	case XKB_KEY_KP_Prior:
	case XKB_KEY_Prior:     return TILDE(5);
	case XKB_KEY_KP_Next:
	case XKB_KEY_Next:      return TILDE(6);
	case XKB_KEY_F1:        return FN14('P');
	case XKB_KEY_F2:        return FN14('Q');
	case XKB_KEY_F3:        return FN14('R');
	case XKB_KEY_F4:        return FN14('S');
	case XKB_KEY_F5:        return TILDE(15);
	case XKB_KEY_F6:        return TILDE(17);
	case XKB_KEY_F7:        return TILDE(18);
	case XKB_KEY_F8:        return TILDE(19);
	case XKB_KEY_F9:        return TILDE(20);
	case XKB_KEY_F10:       return TILDE(21);
	case XKB_KEY_F11:       return TILDE(23);
	case XKB_KEY_F12:       return TILDE(24);
	case XKB_KEY_BackSpace:
		if (mods & 2) { out[0] = 033; out[1] = 0x7f; return 2; } /* Alt: ESC DEL (readline backward-kill-word) */
		out[0] = 0x7f; return 1;
	case XKB_KEY_Tab:
		if (mods & 1) { return sprintf(out, "\033[Z"); } /* Shift+Tab */
		out[0] = '\t'; return 1;
	case XKB_KEY_ISO_Left_Tab: return sprintf(out, "\033[Z");
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (mods & 2) { out[0] = 033; out[1] = '\r'; return 2; } /* Alt+Return */
		out[0] = '\r'; return 1;
	case XKB_KEY_Escape:    out[0] = 033; return 1;
	}
	return 0;
#undef ARROW
#undef TILDE
#undef FN14
}

static void
disarm_repeat(void)
{
	struct itimerspec z = { { 0, 0 }, { 0, 0 } };
	timerfd_settime(g.repeat_fd, 0, &z, NULL);
	g.repeat_code = 0;
	g.repeat_len = 0;
}

static void
arm_repeat(uint32_t code, const char *buf, int len)
{
	struct itimerspec its = { { 0, 0 }, { 0, 0 } };
	int interval_ms;

	if (g.repeat_rate <= 0 || len <= 0 || len > (int)sizeof(g.repeat_buf))
		return;
	if (!g.xkb_keymap || !xkb_keymap_key_repeats(g.xkb_keymap, code))
		return;

	memcpy(g.repeat_buf, buf, len);
	g.repeat_len = len;
	g.repeat_code = code;

	interval_ms = 1000 / g.repeat_rate;
	if (interval_ms < 1)
		interval_ms = 1;
	its.it_value.tv_sec = g.repeat_delay / 1000;
	its.it_value.tv_nsec = (long)(g.repeat_delay % 1000) * 1000000L;
	its.it_interval.tv_sec = interval_ms / 1000;
	its.it_interval.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
	timerfd_settime(g.repeat_fd, 0, &its, NULL);
}

static void
onkey(uint32_t code)
{
	xkb_keysym_t sym;
	char out[128];
	int len, ctrl, alt, shift, mods, c;

	if (!g.xkb_state)
		return;
	sym = xkb_state_key_get_one_sym(g.xkb_state, code);
	ctrl = modon(XKB_MOD_NAME_CTRL);
	alt = modon(XKB_MOD_NAME_ALT);
	shift = modon(XKB_MOD_NAME_SHIFT);

	/* Scrollback (Tastatur). Keysym-basiert -> layoutunabhängig (DE/US/…). */
	if (shift && !ctrl && !alt) {
		switch (sym) {
		case XKB_KEY_Prior: kscrollup(term.row);   g.need_redraw = 1; return;
		case XKB_KEY_Next:  kscrolldown(term.row);  g.need_redraw = 1; return;
		case XKB_KEY_Home:  kscrollup(term.histn);  g.need_redraw = 1; return;
		case XKB_KEY_End:   kscrolldown(term.scr);  g.need_redraw = 1; return;
		}
	}

	/* Zoom (Ctrl +/-/0). Über Keysyms statt Keycodes: auf DE liefert die
	 * +-Taste XKB_KEY_plus, auf US Shift+= ebenfalls -> beide Layouts ok.
	 * KP_*-Varianten decken den Ziffernblock layoutunabhängig ab. */
	if (ctrl && !alt) {
		switch (sym) {
		case XKB_KEY_plus:
		case XKB_KEY_equal:
		case XKB_KEY_KP_Add:      do_zoom(+1); return;
		case XKB_KEY_minus:
		case XKB_KEY_KP_Subtract: do_zoom(-1); return;
		case XKB_KEY_0:
		case XKB_KEY_KP_0:        do_zoom(0);  return;
		case XKB_KEY_V:           clip_paste_clipboard(); return; /* Ctrl+Shift+V */
		case XKB_KEY_v:           /* Ctrl+V: optional als Paste */
			if (paste_on_ctrl_v) { clip_paste_clipboard(); return; }
			break;
		}
	}

	/* Hard Reset / Clear (Ctrl+Shift+R / Ctrl+Shift+L). Shift verlangt ->
	 * kein Konflikt mit readline (Ctrl+R reverse-search, Ctrl+L clear). */
	if (ctrl && shift && !alt) {
		switch (sym) {
		case XKB_KEY_R: case XKB_KEY_r:
			tresetfull(); g.need_redraw = 1; return;
		case XKB_KEY_L: case XKB_KEY_l:
			tclearall();  g.need_redraw = 1; return;
		}
	}

	mods = (shift ? 1 : 0) | (alt ? 2 : 0) | (ctrl ? 4 : 0);

	if ((len = specialkey(sym, mods, out)) > 0) {
		ttywrite(out, len, 1);
		arm_repeat(code, out, len);
		return;
	}

	if (ctrl && !alt && (c = ctrlchar(sym)) >= 0) {
		out[0] = (char)c;
		ttywrite(out, 1, 1);
		arm_repeat(code, out, 1);
		return;
	}

	len = xkb_state_key_get_utf8(g.xkb_state, code, out, sizeof(out));
	if (len > (int)sizeof(out) - 1)
		len = sizeof(out) - 1; /* get_utf8 kann die nötige Länge zurückgeben (> Puffer) */
	if (len > 0) {
		if (alt) {
			char esc[1 + sizeof(out)];
			esc[0] = 033;
			memcpy(esc + 1, out, len);
			ttywrite(esc, len + 1, 1);
			arm_repeat(code, esc, len + 1);
		} else {
			ttywrite(out, len, 1);
			arm_repeat(code, out, len);
		}
	}
}

static void
kbd_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t size)
{
	(void)d; (void)k;
	char *map;
	struct xkb_keymap *km;
	struct xkb_state *st;

	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}
	km = xkb_keymap_new_from_string(g.xkb_ctx, map,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);
	if (!km)
		return;
	st = xkb_state_new(km);
	if (!st) {
		xkb_keymap_unref(km);
		return;
	}
	if (g.xkb_state) xkb_state_unref(g.xkb_state);
	if (g.xkb_keymap) xkb_keymap_unref(g.xkb_keymap);
	g.xkb_keymap = km;
	g.xkb_state = st;
}

static void
kbd_enter(void *d, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *s, struct wl_array *keys)
{
	(void)d; (void)k; (void)serial; (void)s; (void)keys;
}

static void
kbd_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)k; (void)serial; (void)s;
	disarm_repeat();
}

static void
kbd_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time,
    uint32_t key, uint32_t state)
{
	(void)d; (void)k; (void)serial; (void)time;
	uint32_t code = key + 8;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		disarm_repeat();
		onkey(code);
	} else if (code == g.repeat_code) {
		disarm_repeat();
	}
}

static void
kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
    uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)d; (void)k; (void)serial;
	if (g.xkb_state)
		xkb_state_update_mask(g.xkb_state, dep, lat, lock, 0, 0, group);
}

static void
kbd_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)d; (void)k;
	g.repeat_rate = rate;
	g.repeat_delay = delay;
}

static const struct wl_keyboard_listener kbd_listener = {
	.keymap = kbd_keymap,
	.enter = kbd_enter,
	.leave = kbd_leave,
	.key = kbd_key,
	.modifiers = kbd_modifiers,
	.repeat_info = kbd_repeat_info,
};

/* ------------------------------------------------------------------ Maus */
static int
mouse_mods(void)
{
	int m = 0;
	if (modon(XKB_MOD_NAME_SHIFT)) m |= 4;
	if (modon(XKB_MOD_NAME_ALT))   m |= 8;
	if (modon(XKB_MOD_NAME_CTRL))  m |= 16;
	return m;
}

static void
mouse_send(int code, int col, int row, int release)
{
	char buf[40];
	int len;

	if (col < 0 || row < 0 || col >= term.col || row >= term.row)
		return;

	if (term.mode & MODE_MOUSESGR) {
		len = snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c",
		    code, col + 1, row + 1, release ? 'm' : 'M');
	} else {
		int cb = release ? (3 | (code & 0x1c)) : code;
		if (cb >= 0x80 || col >= 223 || row >= 223)
			return;
		buf[0] = 033; buf[1] = '['; buf[2] = 'M';
		buf[3] = (char)(32 + cb);
		buf[4] = (char)(32 + col + 1);
		buf[5] = (char)(32 + row + 1);
		len = 6;
	}
	ttywrite(buf, len, 0);
}

/* Cursor-Theme in der zum aktuellen Scale passenden Größe laden (idempotent). */
static void
cursor_load(void)
{
	int s = (g.scale120 + 60) / 120;
	int size;

	if (s < 1)
		s = 1;
	size = 24 * s;
	if (g.cur_theme && g.cur_size == size)
		return;
	if (g.cur_theme)
		wl_cursor_theme_destroy(g.cur_theme);
	g.cur_theme = wl_cursor_theme_load(NULL, size, g.shm);
	g.cur_size = size;
	g.cur_scale = s;
	g.cur_default = g.cur_hand = NULL;
	if (g.cur_theme) {
		static const char *const dn[] = { "left_ptr", "default", "arrow" };
		static const char *const hn[] = { "pointer", "hand2", "hand1", "hand" };
		size_t i;
		for (i = 0; i < LEN(dn) && !g.cur_default; i++)
			g.cur_default = wl_cursor_theme_get_cursor(g.cur_theme, dn[i]);
		for (i = 0; i < LEN(hn) && !g.cur_hand; i++)
			g.cur_hand = wl_cursor_theme_get_cursor(g.cur_theme, hn[i]);
	}
	if (!g.cur_surf && g.comp)
		g.cur_surf = wl_compositor_create_surface(g.comp);
}

/* Mauszeiger setzen: hand!=0 -> Hand-Cursor (über Link), sonst Standard. */
static void
cursor_apply(int hand)
{
	struct wl_cursor *cur = hand ? g.cur_hand : g.cur_default;
	struct wl_cursor_image *img;
	struct wl_buffer *b;
	int sc = g.cur_scale < 1 ? 1 : g.cur_scale;

	if (!g.ptr || !g.cur_surf)
		return;
	if (!cur)
		cur = g.cur_default;
	if (!cur || !cur->image_count)
		return;
	img = cur->images[0];
	b = wl_cursor_image_get_buffer(img);
	if (!b)
		return;
	wl_surface_set_buffer_scale(g.cur_surf, sc);
	wl_surface_attach(g.cur_surf, b, 0, 0);
	wl_surface_damage_buffer(g.cur_surf, 0, 0, img->width, img->height);
	wl_surface_commit(g.cur_surf);
	wl_pointer_set_cursor(g.ptr, g.ptr_serial, g.cur_surf,
	    img->hotspot_x / sc, img->hotspot_y / sc);
}

/* Link unter dem Pointer neu bestimmen; bei Änderung Unterstreichung,
 * Cursorform und Redraw aktualisieren. */
static void
link_update(void)
{
	char url[URLBUF];
	int sr, sc, er, ec, hit;

	hit = ptr_col >= 0 && ptr_row >= 0 &&
	      turlat(ptr_col, ptr_row, url, sizeof url, &sr, &sc, &er, &ec);

	if (hit && link_active &&
	    sr == hl_sr && sc == hl_sc && er == hl_er && ec == hl_ec)
		return; /* unveränderter Link */
	if (!hit && !link_active)
		return; /* war und ist kein Link */

	if (hit) {
		link_active = 1;
		hl_sr = sr; hl_sc = sc; hl_er = er; hl_ec = ec;
		snprintf(hl_url, sizeof hl_url, "%s", url);
		rset_link(sr, sc, er, ec);
	} else {
		link_active = 0;
		rset_link(-1, 0, 0, 0);
	}
	cursor_apply(link_active);
	g.need_redraw = 1;
}

static void
ptr_setpos(wl_fixed_t fx, wl_fixed_t fy)
{
	double sc = g.scale120 / 120.0;
	int bpx = (int)lround(borderpx * sc);
	int cw = rcellw(), ch = rcellh();
	int px = (int)(wl_fixed_to_double(fx) * sc) - bpx;
	int py = (int)(wl_fixed_to_double(fy) * sc) - bpx;
	int c = (cw > 0) ? px / cw : 0;
	int r = (ch > 0) ? py / ch : 0;

	if (c < 0) c = 0;
	if (c >= term.col) c = term.col - 1;
	if (r < 0) r = 0;
	if (r >= term.row) r = term.row - 1;
	ptr_col = c;
	ptr_row = r;
}

static void
ptr_report_motion(void)
{
	int mods, b;

	if (!(term.mode & (MODE_MOUSEMOTION | MODE_MOUSEMANY)))
		return;
	if (modon(XKB_MOD_NAME_SHIFT))
		return;
	if ((term.mode & MODE_MOUSEMOTION) && !(term.mode & MODE_MOUSEMANY) && !ptr_btn)
		return;
	if (ptr_col == ptr_lcol && ptr_row == ptr_lrow)
		return;
	ptr_lcol = ptr_col;
	ptr_lrow = ptr_row;

	mods = mouse_mods() & ~4; /* Shift wird nicht gemeldet (Bypass) */
	b = (ptr_btn & 1) ? 0 : (ptr_btn & 2) ? 1 : (ptr_btn & 4) ? 2 : 3;
	mouse_send(b + 32 + mods, ptr_col, ptr_row, 0);
}

static void
ptr_report_button(int btn, int pressed)
{
	int b, mods;

	switch (btn) {
	case BTN_LEFT:   b = 0; break;
	case BTN_MIDDLE: b = 1; break;
	case BTN_RIGHT:  b = 2; break;
	default: return;
	}
	if (pressed) ptr_btn |= (1u << b);
	else ptr_btn &= ~(1u << b);

	/* Mittelklick fügt die Primary Selection ein — aber nur, wenn der Klick
	 * ohnehin nicht an die App gemeldet würde (kein Maus-Modus oder Shift-Bypass). */
	if (btn == BTN_MIDDLE && pressed &&
	    (!(term.mode & MODE_MOUSE) || modon(XKB_MOD_NAME_SHIFT))) {
		clip_paste_primary();
		return;
	}

	/* Linksklick auf einen gehoverten Link öffnet ihn beim Loslassen, wenn
	 * Press und Release über demselben Link lagen. Im App-Maus-Modus nur mit
	 * Shift (sonst geht der Klick wie gewohnt an die Anwendung). */
	if (btn == BTN_LEFT &&
	    (!(term.mode & MODE_MOUSE) || modon(XKB_MOD_NAME_SHIFT))) {
		if (pressed) {
			press_link = link_active;
			if (link_active)
				snprintf(press_url, sizeof press_url, "%s", hl_url);
		} else {
			if (press_link && link_active && !strcmp(press_url, hl_url))
				uopen(hl_url);
			press_link = 0;
		}
	}

	if (!(term.mode & MODE_MOUSE) || modon(XKB_MOD_NAME_SHIFT))
		return;

	mods = mouse_mods() & ~4;
	if (term.mode & MODE_MOUSEX10) {
		if (pressed)
			mouse_send(b + mods, ptr_col, ptr_row, 0);
	} else {
		mouse_send(b + mods, ptr_col, ptr_row, !pressed);
	}
}

static void
ptr_wheel(int notches)
{
	int up = notches < 0;
	int n = notches < 0 ? -notches : notches;
	int ctrl = modon(XKB_MOD_NAME_CTRL);
	int shift = modon(XKB_MOD_NAME_SHIFT);
	int i;

	if (ctrl) { /* Ctrl+Mausrad = Zoom (hat Vorrang vor Maus-Report/Scroll) */
		for (i = 0; i < n; i++)
			do_zoom(up ? +1 : -1);
		return;
	}

	if ((term.mode & MODE_MOUSE) && !shift) {
		int code = (up ? 64 : 65) + (mouse_mods() & ~4);
		for (i = 0; i < n; i++)
			mouse_send(code, ptr_col, ptr_row, 0);
	} else if (term.mode & MODE_ALTSCREEN) {
		/* alternateScroll: Rad -> Pfeiltasten (less/man/vim ohne Maus-Modus) */
		const char *seq = (term.mode & MODE_APPCURSOR)
		    ? (up ? "\033OA" : "\033OB") : (up ? "\033[A" : "\033[B");
		for (i = 0; i < n * 3; i++)
			ttywrite(seq, 3, 0);
	} else {
		if (up) kscrollup(n * 3);
		else kscrolldown(n * 3);
		link_update(); /* Inhalt unter dem Pointer hat sich verschoben */
		g.need_redraw = 1;
	}
}

static void
p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
    wl_fixed_t sx, wl_fixed_t sy)
{
	(void)d; (void)p; (void)s;
	g.ptr_serial = serial;
	ptr_setpos(sx, sy);
	cursor_apply(0); /* Compositor erwartet nach enter ein set_cursor */
	link_update();
}
static void
p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)p; (void)serial; (void)s;
	ptr_btn = 0;
	ptr_col = ptr_row = -1;
	if (link_active) {
		link_active = 0;
		rset_link(-1, 0, 0, 0);
		g.need_redraw = 1;
	}
}
static void
p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)d; (void)p; (void)t;
	ptr_setpos(sx, sy);
	link_update();
	ptr_report_motion();
}
static void
p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
    uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)serial; (void)t;
	ptr_report_button(button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}
static void
p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value)
{
	(void)d; (void)p; (void)t;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		frame_cont += wl_fixed_to_double(value);
		frame_cont_seen = 1;
	}
}
static void
p_axis_value120(void *d, struct wl_pointer *p, uint32_t axis, int32_t v120)
{
	(void)d; (void)p;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		ax_v120 += v120;
		frame_v120 = 1;
	}
}
static void
p_frame(void *d, struct wl_pointer *p)
{
	(void)d; (void)p;
	int notches = 0;

	if (frame_v120) {
		notches = ax_v120 / 120;
		ax_v120 -= notches * 120;
	} else if (frame_cont_seen) {
		ax_cont += frame_cont;
		notches = (int)(ax_cont / 15.0);
		ax_cont -= notches * 15.0;
	}
	if (notches)
		ptr_wheel(notches);
	frame_v120 = frame_cont_seen = 0;
	frame_cont = 0;
}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t dis)
{ (void)d; (void)p; (void)a; (void)dis; }

static const struct wl_pointer_listener ptr_listener = {
	.enter = p_enter,
	.leave = p_leave,
	.motion = p_motion,
	.button = p_button,
	.axis = p_axis,
	.frame = p_frame,
	.axis_source = p_axis_source,
	.axis_stop = p_axis_stop,
	.axis_discrete = p_axis_discrete,
	.axis_value120 = p_axis_value120,
};

static void
seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
	(void)d;
	int have_kbd = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	int have_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	if (have_kbd && !g.kbd) {
		g.kbd = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(g.kbd, &kbd_listener, NULL);
	} else if (!have_kbd && g.kbd) {
		wl_keyboard_release(g.kbd);
		g.kbd = NULL;
	}
	if (have_ptr && !g.ptr) {
		g.ptr = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(g.ptr, &ptr_listener, NULL);
	} else if (!have_ptr && g.ptr) {
		wl_pointer_release(g.ptr);
		g.ptr = NULL;
	}
}

static void
seat_name(void *d, struct wl_seat *s, const char *name)
{
	(void)d; (void)s; (void)name;
}

static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* ------------------------------------------------------------------ Output (Scale) */
static void
out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
    int32_t ph, int32_t sub, const char *make, const char *model, int32_t tr)
{
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)make; (void)model; (void)tr;
}
static void
out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{
	(void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void
out_scale(void *d, struct wl_output *o, int32_t s)
{
	(void)d; (void)o;
	/* nur Integer-Fallback; bei fractional scale übernimmt wp_fractional_scale */
	if (!g.have_frac && s * 120 > g.scale120)
		g.scale120 = s * 120;
}
static void out_name(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static void out_desc(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }

static const struct wl_output_listener output_listener = {
	out_geometry, out_mode, out_done, out_scale, out_name, out_desc,
};

/* ------------------------------------------------------------------ xdg-shell */
static void
wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void
xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(s, serial);
	configure_size(g.cfg_w, g.cfg_h);
	g.configured = 1;
	g.need_redraw = 1;
}
static const struct xdg_surface_listener xsurf_listener = { xsurf_configure };

static void
top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
    struct wl_array *states)
{
	(void)d; (void)t; (void)states;
	g.cfg_w = w;
	g.cfg_h = h;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; g.closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d; (void)t; (void)w; (void)h; }
static void top_wmcaps(void *d, struct xdg_toplevel *t, struct wl_array *a)
{ (void)d; (void)t; (void)a; }

static const struct xdg_toplevel_listener top_listener = {
	top_configure, top_close, top_bounds, top_wmcaps,
};

/* ------------------------------------------------------------------ Fractional scale */
static void
frac_preferred(void *d, struct wp_fractional_scale_v1 *f, uint32_t scale)
{
	(void)d; (void)f;
	if ((int)scale == g.scale120)
		return;
	g.scale120 = scale;
	rsetscale(scale / 120.0);
	cursor_load();
	if (g.configured) {
		g.bufw = g.bufh = 0; /* Rebuild der Puffer erzwingen */
		configure_size(g.cfg_w, g.cfg_h);
	}
	g.need_redraw = 1;
}
static const struct wp_fractional_scale_v1_listener frac_listener = { frac_preferred };

/* ------------------------------------------------------------------ Registry */
static void
reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface,
    uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		g.comp = wl_registry_bind(r, name, &wl_compositor_interface, MIN(ver, 4));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		g.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		g.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, MIN(ver, 4));
		xdg_wm_base_add_listener(g.wm_base, &wm_listener, NULL);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		g.seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 8));
		wl_seat_add_listener(g.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		if (!g.output) {
			g.output = wl_registry_bind(r, name, &wl_output_interface, MIN(ver, 2));
			wl_output_add_listener(g.output, &output_listener, NULL);
		}
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		g.deco_mgr = wl_registry_bind(r, name,
		    &zxdg_decoration_manager_v1_interface, 1);
	} else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
		g.frac_mgr = wl_registry_bind(r, name,
		    &wp_fractional_scale_manager_v1_interface, 1);
	} else if (!strcmp(iface, wp_viewporter_interface.name)) {
		g.viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
		g.dd_mgr = wl_registry_bind(r, name, &wl_data_device_manager_interface, MIN(ver, 3));
	} else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name)) {
		g.psel_mgr = wl_registry_bind(r, name,
		    &zwp_primary_selection_device_manager_v1_interface, 1);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ------------------------------------------------------------------ Event-Loop */
static double
timediff(struct timespec a, struct timespec b)
{
	return (a.tv_sec - b.tv_sec) * 1000.0 + (a.tv_nsec - b.tv_nsec) / 1e6;
}

static void
run(void)
{
	struct pollfd pfds[3];
	struct timespec now, trigger = { 0, 0 };
	double timeout = -1;
	int drawing = 0;
	int wlfd = wl_display_get_fd(g.dpy);

	while (!g.closed) {
		/* Wayland: kanonisches prepare_read/poll/read-Muster */
		while (wl_display_prepare_read(g.dpy) != 0)
			wl_display_dispatch_pending(g.dpy);
		wl_display_flush(g.dpy);

		pfds[0] = (struct pollfd){ wlfd, POLLIN, 0 };
		pfds[1] = (struct pollfd){ cmdfd, POLLIN, 0 };
		pfds[2] = (struct pollfd){ g.repeat_fd, POLLIN, 0 };

		int ms = (timeout < 0) ? -1 : (int)ceil(timeout);
		if (poll(pfds, 3, ms) < 0) {
			wl_display_cancel_read(g.dpy);
			if (errno == EINTR)
				continue;
			die("poll: %s", strerror(errno));
		}

		if (pfds[0].revents & POLLIN)
			wl_display_read_events(g.dpy);
		else
			wl_display_cancel_read(g.dpy);
		if (wl_display_dispatch_pending(g.dpy) < 0)
			die("tintty: Wayland-Verbindung verloren"); /* sonst Busy-Spin auf totem fd */

		if (g.closed)
			break;

		clock_gettime(CLOCK_MONOTONIC, &now);

		/* Kam in *diesem* Durchlauf neue Aktivität? PTY-Output oder
		 * Wayland-Events (Input/Resize/Scroll). Nur dann lohnt sich
		 * weiteres Sammeln; ein Durchlauf ohne Aktivität = Idle → sofort
		 * zeichnen (sonst läge die Latenz konstant bei maxlatency). */
		int sawevent = (pfds[0].revents & POLLIN) || (pfds[1].revents & POLLIN);

		/* POLLHUP ohne POLLIN (Slave zu, nichts mehr zu lesen): ttyread
		 * beendet sauber via read()==0/EIO — sonst poll-Busy-Spin, bis
		 * das SIGCHLD eintrifft. */
		if (pfds[1].revents & (POLLIN | POLLHUP)) {
			ttyread();
			g.need_redraw = 1;
		}
		if (pfds[2].revents & POLLIN) {
			uint64_t exp;
			if (read(g.repeat_fd, &exp, sizeof(exp)) == sizeof(exp) &&
			    g.repeat_len > 0)
				ttywrite(g.repeat_buf, g.repeat_len, 1);
		}

		if (g.need_redraw) {
			if (!drawing) {
				trigger = now;
				drawing = 1;
			}
			if (sawevent) {
				timeout = (maxlatency - timediff(now, trigger))
				          / maxlatency * minlatency;
				if (timeout > 0)
					continue; /* Aktivität fließt noch — sammeln (bis maxlatency) */
			}
			/* Idle erkannt oder maxlatency erreicht → zeichnen */
			timeout = -1;
			draw();
			drawing = 0;
		} else {
			timeout = -1;
		}
	}
}

/* ------------------------------------------------------------------ main */
int
main(void)
{
	setlocale(LC_CTYPE, "");

	g.scale120 = 120;
	g.shm_fd = -1;
	g.repeat_rate = repeat_rate_default;
	g.repeat_delay = repeat_delay_default;
	g.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (g.repeat_fd < 0)
		die("timerfd_create: %s", strerror(errno));

	g.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!g.xkb_ctx)
		die("xkb_context_new fehlgeschlagen");

	if (rinit() < 0)
		die("tintty: Font-Initialisierung fehlgeschlagen");

	g.dpy = wl_display_connect(NULL);
	if (!g.dpy)
		die("tintty: kann nicht zu Wayland verbinden ($WAYLAND_DISPLAY)");

	g.reg = wl_display_get_registry(g.dpy);
	wl_registry_add_listener(g.reg, &reg_listener, NULL);
	wl_display_roundtrip(g.dpy); /* Globals binden */

	if (!g.comp || !g.shm || !g.wm_base)
		die("tintty: benötigte Wayland-Globals fehlen (compositor/shm/xdg_wm_base)");

	g.have_frac = (g.frac_mgr != NULL && g.viewporter != NULL);

	/* Datengeräte für Paste (Clipboard + Primary Selection). */
	if (g.dd_mgr && g.seat) {
		g.data_dev = wl_data_device_manager_get_data_device(g.dd_mgr, g.seat);
		wl_data_device_add_listener(g.data_dev, &dd_listener, NULL);
	}
	if (g.psel_mgr && g.seat) {
		g.psel_dev = zwp_primary_selection_device_manager_v1_get_device(g.psel_mgr, g.seat);
		zwp_primary_selection_device_v1_add_listener(g.psel_dev, &psel_listener, NULL);
	}

	wl_display_roundtrip(g.dpy); /* Output-Scale (nur Integer-Fallback) */
	rsetscale(g.scale120 / 120.0);
	cursor_load();

	/* Core + PTY vor der Surface anlegen, damit configure resizen kann */
	tnew(default_cols, default_rows);
	ttynew();

	g.surf = wl_compositor_create_surface(g.comp);
	if (g.have_frac) {
		g.viewport = wp_viewporter_get_viewport(g.viewporter, g.surf);
		g.frac = wp_fractional_scale_manager_v1_get_fractional_scale(
		    g.frac_mgr, g.surf);
		wp_fractional_scale_v1_add_listener(g.frac, &frac_listener, NULL);
	}
	g.xsurf = xdg_wm_base_get_xdg_surface(g.wm_base, g.surf);
	xdg_surface_add_listener(g.xsurf, &xsurf_listener, NULL);
	g.toplevel = xdg_surface_get_toplevel(g.xsurf);
	xdg_toplevel_add_listener(g.toplevel, &top_listener, NULL);
	xdg_toplevel_set_title(g.toplevel, "tintty");
	xdg_toplevel_set_app_id(g.toplevel, "tintty");

	if (g.deco_mgr) {
		g.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
		    g.deco_mgr, g.toplevel);
		zxdg_toplevel_decoration_v1_set_mode(g.deco,
		    ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}

	wl_surface_commit(g.surf);
	wl_display_roundtrip(g.dpy); /* erstes configure */

	run();

	destroy_buffers();
	rfree();
	if (g.cur_theme)
		wl_cursor_theme_destroy(g.cur_theme);
	wl_display_disconnect(g.dpy);
	return 0;
}
