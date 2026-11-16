#include <xcb/xcb.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Create an override-redirect window, map it, then unmap and destroy it.
 *
 * Override-redirect windows are "unmanaged" clients in Pluto.  Mapping one
 * must not create image_capture_scene, and unmapping it must still be safe.
 */
int
main(void)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *screen;
	xcb_window_t win;
	uint32_t mask = XCB_CW_OVERRIDE_REDIRECT;
	uint32_t values[] = { 1 };

	if (xcb_connection_has_error(c)) {
		fprintf(stderr, "override_unmap: cannot connect to X\n");
		return 1;
	}

	screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	win = xcb_generate_id(c);
	xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root,
			10, 10, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			screen->root_visual, mask, values);
	xcb_map_window(c, win);
	xcb_flush(c);
	sleep(1);

	xcb_unmap_window(c, win);
	xcb_flush(c);
	sleep(1);

	xcb_destroy_window(c, win);
	xcb_flush(c);
	xcb_disconnect(c);
	return 0;
}
