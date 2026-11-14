/* Pure window-geometry math for the tiling layouts.
 *
 * This module deliberately depends on nothing but the C standard library so
 * that it can be unit-tested without wlroots, a display, or a compositor.
 * src/layout.c is the only caller; it converts Monitor geometry to Box and
 * back.
 */
#ifndef PLUTO_LAYOUT_MATH_H
#define PLUTO_LAYOUT_MATH_H

#include <stdbool.h>

struct Box {
	int x;
	int y;
	int width;
	int height;
};

struct TileOptions {
	int nmaster;   /* clients in the master area */
	float mfact;   /* fraction of the usable width given to the master area */
	int gap;       /* space between adjacent clients */
	int outergap;  /* space between clients and the usable-area edge */
};

/* Geometry of window `index` (0-based) out of `n` in the tile layout.
 * Returns a zero-size box when index >= n or n == 0. */
struct Box layout_tile_slot(struct Box area, int n, int index,
		const struct TileOptions *opts);

/* Geometry of window `index` in the monocle layout (always the full usable
 * area).  Returns a zero-size box when index >= n or n == 0. */
struct Box layout_monocle_slot(struct Box area, int n, int index, int outergap);

/* Shrink `area` by `gap` on all sides, never producing a negative size. */
struct Box layout_inset(struct Box area, int gap);

#endif /* PLUTO_LAYOUT_MATH_H */
