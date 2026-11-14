/* XWayland integration. */
#include "pluto.h"

#ifdef XWAYLAND

void
activatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, activate);
	struct wlr_surface *surface = client_surface(c);

	/* Only "managed" windows can be activated */
	if (client_is_unmanaged(c))
		return;

	/* The X11 input focus goes with the keyboard focus: a client that
	 * doesn't have it is marked urgent, as with xdg-activation */
	if (c == focustop(P.selmon)) {
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
		return;
	}

	c->isurgent = 1;
	printstatus();

	if (surface && surface->mapped)
		client_set_border_color(c, urgentcolor);
}

void
associatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, associate);

	LISTEN(&client_surface(c)->events.commit, &c->commit, commitx11);
	LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
	LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
commitx11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);
	struct wlr_surface *surface = client_surface(c);

	transactioncommit(c, surface->current.width == c->geom.width - 2 * (int)c->bw
			&& surface->current.height == c->geom.height - 2 * (int)c->bw);
}

void
configurex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	if (!client_surface(c) || !client_surface(c)->mapped
			|| client_is_unmanaged(c) || !c->mon) {
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if (c->isfullscreen) {
		/* It keeps the whole monitor, and is told so */
		resize(c, c->mon->m, 0);
	} else if ((c->isfloating && c != P.grabc) || !c->mon->lt[c->mon->sellt]->arrange) {
		resize(c, (struct wlr_box){.x = event->x - c->bw,
				.y = event->y - c->bw, .width = event->width + c->bw * 2,
				.height = event->height + c->bw * 2}, 0);
	} else {
		arrange(c->mon);
	}
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xsurface = data;
	Client *c;

	/* Allocate a Client for this surface */
	c = xsurface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xsurface;
	c->type = X11;
	c->bw = client_is_unmanaged(c) ? 0 : borderpx;

	/* Listen to the various events it can emit */
	LISTEN(&xsurface->events.associate, &c->associate, associatex11);
	LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
	LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xsurface->events.set_geometry, &c->set_geometry, setgeometryx11);
	LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
	LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, dissociate);
	wl_list_remove(&c->commit.link);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
}

void
setgeometryx11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_geometry);

	if (!client_is_unmanaged(c) || !client_surface(c)
			|| !client_surface(c)->mapped)
		return;

	client_get_geometry(c, &c->geom);
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
}

void
sethints(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_hints);
	struct wlr_surface *surface = client_surface(c);
	if (c == focustop(P.selmon) || client_is_unmanaged(c)
			|| !c->surface.xwayland->hints)
		return;

	c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
	printstatus();

	if (c->isurgent && surface && surface->mapped)
		client_set_border_color(c, urgentcolor);
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;

	/* assign the one and only seat */
	wlr_xwayland_set_seat(P.xwayland, P.seat);

	/* Set the default XWayland cursor to match the rest of Pluto. */
	if ((xcursor = wlr_xcursor_manager_get_xcursor(P.cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(P.xwayland, wlr_xcursor_image_get_buffer(xcursor->images[0]),
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}

#endif /* XWAYLAND */
