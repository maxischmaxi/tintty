/* tintty — Render-Schnittstelle (in render.c implementiert, von wl.c genutzt). */
#ifndef RENDER_H
#define RENDER_H

#include <pixman.h>

/* FreeType/fontconfig initialisieren, Primärfont laden. 0 = ok, -1 = Fehler. */
int  rinit(void);
void rfree(void);

/* Anzeige-Scale (HiDPI, auch fraktional) setzen; lädt Fonts in passender
 * Pixelgröße neu. Gibt 1 zurück, wenn sich die Zellgeometrie geändert hat. */
int  rsetscale(double scale);

/* Zoom: delta>0 größer, delta<0 kleiner, 0 = zurück auf Default-Größe.
 * Lädt Fonts neu; gibt 1 zurück, wenn sich die Größe geändert hat. */
int  rzoom(int delta);

/* Zellmaße in physischen Pixeln (beim aktuellen Scale). */
int  rcellw(void);
int  rcellh(void);

/* Das Terminal-Grid in den (premultiplizierten ARGB8888-) Puffer rendern.
 * borderpx_phys = Rand in physischen Pixeln. */
void rdraw(pixman_image_t *buf, int borderpx_phys);

/* erzwingt vollständiges Neuzeichnen beim nächsten rdraw */
void rdirtyall(void);

/* Hover-Link-Spanne (sichtbare Zell-Koordinaten) für die Unterstreichung
 * setzen; sr < 0 = kein Link. Mehrzeilige Links als (sr,sc)..(er,ec). */
void rset_link(int sr, int sc, int er, int ec);

#endif /* RENDER_H */
