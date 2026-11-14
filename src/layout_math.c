/* Pure window-geometry math for the tiling layouts.  See layout_math.h. */
#include <math.h>

#include "layout_math.h"

static struct Box
empty_box(void)
{
	return (struct Box){.x = 0, .y = 0, .width = 0, .height = 0};
}

static int
clamp_int(int value, int low, int high)
{
	if (value < low)
		return low;
	if (value > high)
		return high;
	return value;
}

struct Box
layout_inset(struct Box area, int gap)
{
	if (gap < 0)
		gap = 0;
	area.x += gap;
	area.y += gap;
	area.width = area.width > 2 * gap ? area.width - 2 * gap : 1;
	area.height = area.height > 2 * gap ? area.height - 2 * gap : 1;
	return area;
}

/* Split a column into `rows` rows separated by `gap`.  The first
 * `height % rows` rows receive one extra pixel, matching the Pluto reference
 * layout's remainder handling. */
static struct Box
split_row(struct Box column, int rows, int row, int gap)
{
	int available, base, extra, y, height;

	if (rows <= 1)
		return column;

	available = column.height - gap * (rows - 1);
	if (available < rows)
		available = rows;
	base = available / rows;
	extra = available % rows;

	y = column.y + row * (base + gap);
	if (row < extra)
		y += row;
	else
		y += extra;

	height = base + (row < extra ? 1 : 0);
	return (struct Box){
		.x = column.x,
		.y = y,
		.width = column.width,
		.height = height,
	};
}

struct Box
layout_tile_slot(struct Box area, int n, int index, const struct TileOptions *opts)
{
	struct Box usable;
	int nmaster, mw, gap;

	if (!opts || n <= 0 || index < 0 || index >= n)
		return empty_box();

	gap = opts->gap < 0 ? 0 : opts->gap;
	usable = layout_inset(area, opts->outergap);

	if (usable.width < 1)
		usable.width = 1;
	if (usable.height < 1)
		usable.height = 1;

	nmaster = clamp_int(opts->nmaster, 0, n);

	/* With no master area, or fewer clients than master slots, everything is
	 * stacked vertically. */
	if (nmaster == 0 || n <= nmaster)
		return split_row(usable, n, index, gap);

	/* Reserve at least one column of space for the stack. */
	mw = (int)roundf((float)usable.width * opts->mfact);
	mw = clamp_int(mw, 1, usable.width > 1 ? usable.width - 1 : 1);
	if (usable.width - mw - gap < 1)
		mw = usable.width > gap + 1 ? usable.width - gap - 1 : 1;

	if (index < nmaster) {
		struct Box master = {
			.x = usable.x,
			.y = usable.y,
			.width = mw,
			.height = usable.height,
		};
		return split_row(master, nmaster, index, gap);
	} else {
		struct Box stack = {
			.x = usable.x + mw + gap,
			.y = usable.y,
			.width = usable.width - mw - gap,
			.height = usable.height,
		};
		if (stack.width < 1)
			stack.width = 1;
		return split_row(stack, n - nmaster, index - nmaster, gap);
	}
}

struct Box
layout_monocle_slot(struct Box area, int n, int index, int outergap)
{
	struct Box usable;

	if (n <= 0 || index < 0 || index >= n)
		return empty_box();

	usable = layout_inset(area, outergap);
	if (usable.width < 1)
		usable.width = 1;
	if (usable.height < 1)
		usable.height = 1;
	return usable;
}
