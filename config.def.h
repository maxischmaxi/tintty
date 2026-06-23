/*
 * tintty — Konfiguration (compile-time, einzige Konfiguration).
 * Wird vom Makefile nach config.h kopiert. config.h bearbeiten, nicht diese Datei,
 * wenn du nur lokal etwas änderst (config.def.h ist die versionierte Vorlage).
 *
 * Werte 1:1 aus der WezTerm-Config (~/.config/wezterm/{wezterm.lua,colors.lua}).
 */
#ifndef CONFIG_H
#define CONFIG_H

/* ---------- Geometrie / Fenster ---------- */
static const unsigned int default_cols = 120;
static const unsigned int default_rows = 28;
static const int borderpx = 0;                 /* Innenabstand in px (logisch) */

/* ---------- Font ---------- */
static const char *const font_family = "FiraCode Nerd Font";
static const int    font_weight_ot   = 500;    /* OpenType-Gewicht (Medium); via FcWeightFromOpenType */
static const double font_size_pt     = 14.0;   /* WezTerm-Punktgröße */
static const double font_dpi         = 96.0;   /* px = pt * dpi/72 * scale */
/* Ligaturen sind durch zellenweises Rendering ohnehin deaktiviert. */

/* Zoom (Ctrl +/-/0 und Ctrl+Mausrad): Schrittweite und Grenzen in pt.
 * font_size_pt bleibt die Basis; Ctrl+0 setzt darauf zurück. */
static const double zoom_step_pt     = 1.0;
static const double zoom_min_pt      = 6.0;
static const double zoom_max_pt      = 72.0;

/*
 * Geordnete Fallback-Familien für Glyphen, die der Primärfont nicht hat.
 * Reihenfolge zählt: fontconfig-Auto-Scoring würde z.B. für U+F0001 den
 * falschen Font wählen — daher explizit zuerst diese Liste, dann generisch.
 */
static const char *const fallback_fonts[] = {
	"Symbols Nerd Font Mono",
	"Noto Color Emoji",
};

/* ---------- Transparenz ---------- */
/* Nur der Default-Hintergrund ist transparent; Text und explizit gesetzte
 * Hintergrundfarben (Statusbars etc.) bleiben deckend (= WezTerm text_background_opacity 1.0). */
static const double bg_opacity = 0.90;

/* ---------- Scrollback ---------- */
#define HISTSIZE 5000                    /* Zeilen Scrollback (0 = aus) */

/* ---------- Verhalten ---------- */
static const char *const termname = "xterm-256color";
static const char *const default_shell = "/bin/sh"; /* nur Fallback; $SHELL hat Vorrang */
static const unsigned int tabspaces = 8;
static const int allowaltscreen = 1;

/* Befehl zum Öffnen geklickter Links (bekommt die URL als einziges Argument). */
static const char *const browser_cmd = "xdg-open";

/* Ctrl+V als Paste belegen (zusätzlich zu Ctrl+Shift+V und Mittelklick).
 * 0 = nur Ctrl+Shift+V; Ctrl+V bleibt dann 0x16 (vim blockweise Auswahl,
 * readline quoted-insert). */
static const int paste_on_ctrl_v = 1;

/* Draw-Coalescing (ms): nicht öfter als nötig neu zeichnen.
 * maxlatency deckelt die Zeichenrate bei Dauerausgabe: 16 ms ≈ 60 FPS. */
static const double minlatency = 2.0;
static const double maxlatency = 16.0;

/* Key-Repeat-Fallback, falls der Compositor keine repeat_info schickt. */
static const int repeat_rate_default  = 25;    /* Wiederholungen pro Sekunde */
static const int repeat_delay_default = 600;   /* ms bis Start der Wiederholung */

/* ---------- Farbpalette (1:1 WezTerm) ---------- */
/* 0..7 normal, 8..15 hell */
static const char *const colorname[] = {
	"#0f131c", "#f7768e", "#9ece6a", "#e0af68",
	"#7aa2f7", "#bb9af7", "#7dcfff", "#dfe2ef",
	"#8d909c", "#ff7a93", "#b9f27c", "#ff9e64",
	"#7da6ff", "#c8a8ff", "#2ac3de", "#dfe2ef",
};

/* Spezialfarben (Index >= 256) */
enum {
	COL_FG = 256,
	COL_BG,
	COL_CURSOR_FG,
	COL_CURSOR_BG,
	COL_SEL_FG,
	COL_SEL_BG,
	COL_LAST,
};
static const char *const colorname_special[] = {
	[COL_FG        - 256] = "#dfe2ef",
	[COL_BG        - 256] = "#0f131c",
	[COL_CURSOR_FG - 256] = "#002f68",
	[COL_CURSOR_BG - 256] = "#acc7ff",
	[COL_SEL_FG    - 256] = "#d7e2ff",
	[COL_SEL_BG    - 256] = "#004492",
};

#define DEFAULTFG   COL_FG
#define DEFAULTBG   COL_BG
#define DEFAULTCS   COL_CURSOR_BG
#define DEFAULTCSFG COL_CURSOR_FG

#endif /* CONFIG_H */
