# tintty

Schlanker, nativer **Wayland**-Terminal-Emulator in C — urxvt-RAM (~34 MB inkl.
5000 Zeilen Scrollback), aber **voller Unicode** inkl. Supplementary Plane
(Nerd-Fonts-v3-Icons jenseits U+FFFF, die urxvt nicht kann). Look 1:1 wie eine
feste WezTerm-Config, ohne Runtime-Konfiguration.

## Bauen & Installieren

Voraussetzungen (Arch): `wayland wayland-protocols libxkbcommon freetype2 fontconfig pixman`.

```sh
make            # baut ./tintty (~69 KB)
make install    # nach ~/.local (kein sudo); danach in rofi/wofi 'drun' sichtbar
```

System-weit: `sudo make install PREFIX=/usr/local`. Entfernen: `make uninstall`.

## Konfiguration

Keine Config-Datei, keine Flags. Alles steht in [`config.def.h`](config.def.h)
und wird einkompiliert (wie suckless st). Ändern = `config.h` editieren + `make`.

## Shortcuts

| Eingabe | Funktion |
|---|---|
| `Shift`+`PageUp`/`PageDown` | Scrollback blättern |
| `Shift`+`Home`/`End` | Scrollback-Anfang / -Ende |
| Mausrad | Scrollback (normal) bzw. Pfeiltasten (Alt-Screen) |
| `Ctrl`+`+` / `-` / `0` | Schrift größer / kleiner / Reset |
| `Ctrl`+Mausrad | Schrift stufenweise zoomen |
| `Ctrl`+`Shift`+`R` / `L` | Hard Reset / Schirm + Scrollback leeren |
| `Ctrl`+`V` / `Ctrl`+`Shift`+`V` | Einfügen aus der Zwischenablage |
| Mittelklick | Einfügen aus der Primary Selection |

Alle Shortcuts sind über Keysyms gebunden → layoutunabhängig (DE, US, …).
Maus-Reporting (DECSET 9/1000/1002/1003, SGR 1006) geht an die App; `Shift`
umgeht es. Einfügen ist eingebaut (Bracketed-Paste-aware); `Ctrl`+`V` lässt
sich via `paste_on_ctrl_v` abschalten (dann nur `Ctrl`+`Shift`+`V`, und `Ctrl`+`V`
bleibt `0x16` für vim/readline). Kopieren läuft weiter extern (tmux, `wl-copy`).

## Architektur

| Datei | Inhalt |
|---|---|
| `tintty.c` | Core: PTY, VT/ANSI-Parser, Grid, Scrollback, UTF-8 → 32-bit-Rune |
| `render.c` | FreeType-Rasterung, fontconfig-Fallback, Glyph-Cache, pixman-Compositing |
| `wl.c` | Wayland: xdg-shell, wl_shm, xkbcommon-Input, Event-Loop, `main()` |
| `config.def.h` | gesamte Konfiguration |

Reines Software-Rendering (kein GPU-Stack → niedriger RAM). Event-getrieben mit
Draw-Coalescing (Idle = 0 FPS, Dauerlast ≈ 60 FPS, in `config.def.h` justierbar).
Fallback > U+FFFF nutzt eine geordnete Liste (`Symbols Nerd Font Mono`) vor
generischem `FcFontSort`. Transparenz via premultipliziertem ARGB8888.

## Sicherheit

Bedrohungsmodell: nicht-vertrauenswürdige PTY-Bytes. Kein Write-back
angreifer-kontrollierter Strings (kein OSC 52 / Title-Query / Answerback);
Parser-Puffer fest dimensioniert und grenzgeprüft. Auditiert per Code-Review +
ASan/UBSan-Fuzzing — keine über PTY auslösbaren Schwachstellen.
