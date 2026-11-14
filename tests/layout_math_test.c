/* Unit tests for the pure layout geometry (no wlroots required). */
#include <assert.h>
#include <stdio.h>

#include "layout_math.h"

static struct Box
area(int x, int y, int width, int height)
{
	return (struct Box){.x = x, .y = y, .width = width, .height = height};
}

static void
test_inset(void)
{
	struct Box box = layout_inset(area(0, 0, 100, 80), 4);

	assert(box.x == 4 && box.y == 4);
	assert(box.width == 92 && box.height == 72);

	/* A gap larger than the area must not produce negative sizes. */
	box = layout_inset(area(0, 0, 4, 4), 8);
	assert(box.width == 1 && box.height == 1);
}

static void
test_single_window_fills_usable_area(void)
{
	const struct TileOptions opts = {.nmaster = 1, .mfact = 0.5f, .gap = 4, .outergap = 4};
	struct Box box = layout_tile_slot(area(0, 0, 100, 80), 1, 0, &opts);

	assert(box.x == 4 && box.y == 4);
	assert(box.width == 92 && box.height == 72);
}

static void
test_master_stack_split(void)
{
	const struct TileOptions opts = {.nmaster = 1, .mfact = 0.5f, .gap = 4, .outergap = 0};
	struct Box master = layout_tile_slot(area(0, 0, 100, 100), 2, 0, &opts);
	struct Box stack = layout_tile_slot(area(0, 0, 100, 100), 2, 1, &opts);

	/* 100px wide, mfact 0.5 -> 50px master column, then a 4px gap. */
	assert(master.x == 0 && master.width == 50);
	assert(stack.x == 54 && stack.width == 46);
	assert(master.height == 100 && stack.height == 100);
}

static void
test_stack_rows_respect_gaps(void)
{
	const struct TileOptions opts = {.nmaster = 1, .mfact = 0.5f, .gap = 4, .outergap = 0};
	struct Box first = layout_tile_slot(area(0, 0, 100, 100), 3, 1, &opts);
	struct Box second = layout_tile_slot(area(0, 0, 100, 100), 3, 2, &opts);

	assert(first.x == second.x && first.width == second.width);
	assert(first.height + 4 + second.height == 100);
	assert(second.y == first.y + first.height + 4);
}

static void
test_fewer_clients_than_master_stack_vertically(void)
{
	const struct TileOptions opts = {.nmaster = 3, .mfact = 0.5f, .gap = 0, .outergap = 0};
	struct Box first = layout_tile_slot(area(0, 0, 100, 100), 2, 0, &opts);
	struct Box second = layout_tile_slot(area(0, 0, 100, 100), 2, 1, &opts);

	assert(first.width == 100 && second.width == 100);
	assert(first.y == 0 && second.y == 50);
	assert(first.height == 50 && second.height == 50);
}

static void
test_zero_master_stacks_vertically(void)
{
	const struct TileOptions opts = {.nmaster = 0, .mfact = 0.5f, .gap = 0, .outergap = 0};
	struct Box first = layout_tile_slot(area(0, 0, 100, 90), 3, 0, &opts);
	struct Box last = layout_tile_slot(area(0, 0, 100, 90), 3, 2, &opts);

	assert(first.width == 100 && last.width == 100);
	assert(first.height == 30 && last.y == 60);
}

static void
test_monocle_uses_usable_area(void)
{
	struct Box box = layout_monocle_slot(area(0, 0, 100, 80), 2, 1, 5);

	assert(box.x == 5 && box.y == 5);
	assert(box.width == 90 && box.height == 70);
}

static void
test_invalid_indices_are_empty(void)
{
	const struct TileOptions opts = {.nmaster = 1, .mfact = 0.5f, .gap = 4, .outergap = 4};
	struct Box box = layout_tile_slot(area(0, 0, 100, 80), 2, 2, &opts);

	assert(box.width == 0 && box.height == 0);
	assert(layout_monocle_slot(area(0, 0, 100, 80), 0, 0, 4).width == 0);
}

int
main(void)
{
	test_inset();
	test_single_window_fills_usable_area();
	test_master_stack_split();
	test_stack_rows_respect_gaps();
	test_fewer_clients_than_master_stack_vertically();
	test_zero_master_stacks_vertically();
	test_monocle_uses_usable_area();
	test_invalid_indices_are_empty();

	puts("layout_math: all tests passed");
	return 0;
}
