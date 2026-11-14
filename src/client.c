/* Client lifecycle: xdg-shell and XWayland mapping, configuration and transactions. */
#include "pluto.h"

void
applybounds(Client *c, struct wlr_box *bbox)
{
	/* set minimum client size to 1 */
	c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
	c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

	if (!c->isfullscreen) {
		struct wlr_box min = {0}, max = {0};
		client_get_size_hints(c, &max, &min);
		c->geom.width = MAX(min.width + 2 * (int)c->bw, c->geom.width);
		c->geom.height = MAX(min.height + 2 * (int)c->bw, c->geom.height);
		/* Some clients set their max size to INT_MAX, which does not violate the
		 * protocol but it's unnecessary, as they can set their max size to zero. */
		if (max.width > 0 && 2 * (int)c->bw <= INT_MAX - max.width) /* Checks for overflow */
			c->geom.width = MIN(max.width + 2 * (int)c->bw, c->geom.width);
		if (max.height > 0 && 2 * (int)c->bw <= INT_MAX - max.height) /* Checks for overflow */
			c->geom.height = MIN(max.height + 2 * (int)c->bw, c->geom.height);
	}

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

void
applygeom(Client *c)
{
	struct wlr_box clip;
	Client *p = client_get_parent(c);

	/* We move all floating clients to LyrTile while in floating layout
	 * to avoid "real" floating clients be always on top */
	wlr_scene_node_reparent(&c->scene->node, P.layers[c->isfullscreen
			|| (p && p->isfullscreen) ? LyrFS
			: c->isfloating && c->mon->lt[c->mon->sellt]->arrange ? LyrFloat
			: LyrTile]);

	c->current = c->geom;
	if (c->snapshot) {
		wlr_scene_node_destroy(c->snapshot);
		c->snapshot = NULL;
		wlr_scene_node_set_enabled(&c->scene_surface->node, 1);
	}

	/* Update scene-graph, including borders */
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);

	client_get_clip(c, &clip);
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
applyrules(Client *c)
{
	/* rule matching */
	const char *appid, *title;
	uint32_t newtags = 0;
	int i;
	const Rule *r;
	Monitor *mon = P.selmon, *m;

	appid = client_get_appid(c);
	title = client_get_title(c);

	for (r = rules; r < END(rules); r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			newtags |= r->tags;
			i = 0;
			wl_list_for_each(m, &P.mons, link) {
				if (r->monitor == i++)
					mon = m;
			}
		}
	}

	c->isfloating |= client_is_float_type(c);
	setmon(c, mon, newtags);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);

	if (c->surface.xdg->initial_commit) {
		/*
		 * Get the monitor this client will be rendered on
		 * Note that if the user set a rule in which the client is placed on
		 * a different monitor based on its title, this will likely select
		 * a wrong monitor.
		 */
		applyrules(c);
		if (c->mon) {
			client_set_scale(client_surface(c), c->mon->wlr_output->scale);
		}
		setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

		wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
				WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (c->decoration)
			requestdecorationmode(&c->set_decoration_mode, c->decoration);
		wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
		if (!c->isfloating)
			client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
					WLR_EDGE_RIGHT);
		return;
	}

	/* The transaction showed this client before it had drawn at its new
	 * size, so what is under the cursor changes now: look again, once the
	 * scene graph has handled this commit */
	if (c->late_serial && c->surface.xdg->current.configure_serial >= c->late_serial) {
		c->late_serial = 0;
		wl_event_loop_add_idle(P.event_loop, refocuspointer, NULL);
	}

	transactioncommit(c, c->resize_serial && c->surface.xdg->current.configure_serial
			>= c->resize_serial);
	resize(c, c->geom, (c->isfloating && !c->isfullscreen));
}

void
commitpopup(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
	Popup *p = wl_container_of(listener, p, commit);
	LayerSurface *l = NULL;
	Client *c = NULL;
	struct wlr_box box;
	int type = -1;

	if (!popup->base->initial_commit)
		return;

	type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
	if (!popup->parent || type < 0)
		return;
	/* The parent has no scene tree until it is mapped (a toplevel) or has
	 * made its initial commit (a popup) */
	if (!popup->parent->data || (l && !l->mon) || (c && !c->mon)) {
		wlr_xdg_popup_destroy(popup);
		return;
	}
	popup->base->surface->data = wlr_scene_xdg_surface_create(
			popup->parent->data, popup->base);
	box = type == LayerShell ? l->mon->m : c->mon->w;
	box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x - c->surface.xdg->geometry.x);
	box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y - c->surface.xdg->geometry.y);
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
	destroypopup(&p->destroy, NULL);
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client creates a new toplevel (application window). */
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = toplevel->base->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = toplevel->base;
	c->bw = borderpx;

	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

void
createdecoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = deco->toplevel->base->data;
	c->decoration = deco;

	LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
	LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

	requestdecorationmode(&c->set_decoration_mode, deco);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_decoration_mode);
	if (c->surface.xdg->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy_decoration);
	c->decoration = NULL;

	wl_list_remove(&c->destroy_decoration.link);
	wl_list_remove(&c->set_decoration_mode.link);
}

void
destroynotify(struct wl_listener *listener, void *data)
{
	/* Called when the xdg_toplevel is destroyed. */
	Client *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
#ifdef XWAYLAND
	if (c->type != XDGShell) {
		wl_list_remove(&c->activate.link);
		wl_list_remove(&c->associate.link);
		wl_list_remove(&c->configure.link);
		wl_list_remove(&c->dissociate.link);
		wl_list_remove(&c->set_geometry.link);
		wl_list_remove(&c->set_hints.link);
	} else
#endif
	{
		wl_list_remove(&c->commit.link);
		wl_list_remove(&c->map.link);
		wl_list_remove(&c->unmap.link);
		wl_list_remove(&c->maximize.link);
		if (c->decoration) {
			wl_list_remove(&c->set_decoration_mode.link);
			wl_list_remove(&c->destroy_decoration.link);
		}
	}
	free(c);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
	struct wlr_scene_node *ghost, **slot;
	if (c == P.grabc) {
		P.cursor_mode = CurNormal;
		P.grabc = NULL;
	}
	if (c == P.focused_client)
		P.focused_client = NULL;

	if (!client_is_unmanaged(c)) {
		/* Keep showing the client until the transaction rearranging the
		 * remaining ones is applied, to not show a gap */
		if ((ghost = snapshot(&c->scene->node, c->scene->node.parent, NULL))) {
			if ((slot = wl_array_add(&P.ghosts, sizeof(ghost))))
				*slot = ghost;
			else
				wlr_scene_node_destroy(ghost);
			transactionschedule();
		}
		wl_list_remove(&c->link);
		setmon(c, NULL, 0);
		wl_list_remove(&c->flink);
	} else if (client_wants_focus(c)) {
		focusclient(focustop(P.selmon), NULL, 1);
	}

	if (c->foreign_toplevel_handle) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(c->foreign_toplevel_handle);
		c->foreign_toplevel_handle = NULL;
	}
#ifdef XWAYLAND
	if (c->type != XDGShell && c->capture.image_capture_scene_surface) {
		wlr_scene_node_destroy(&c->capture.image_capture_scene_surface->buffer->node);
		c->capture.image_capture_scene_surface = NULL;
	}
#endif

	wlr_scene_node_destroy(&c->image_capture_scene->tree.node);
	c->image_capture_scene = NULL;
	c->image_capture_source = NULL;
	wlr_scene_node_destroy(&c->scene->node);
	client_surface(c)->data = NULL;
	c->snapshot = NULL;
	c->resize_serial = c->late_serial = 0;
	c->current = (struct wlr_box){0};
	printstatus();
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p = NULL;
	Client *w, *c = wl_container_of(listener, c, map);
	Monitor *m;
	int i;

	struct wlr_ext_foreign_toplevel_handle_v1_state foreign_toplevel_state = {
		.app_id = client_get_appid(c),
		.title = client_get_title(c),
	};

	/* Create scene tree for this client and its border */
	c->scene = client_surface(c)->data = wlr_scene_tree_create(P.layers[LyrTile]);
	/* Enabled later by a call to arrange() */
	wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	c->scene->node.data = c->scene_surface->node.data = c;

	client_get_geometry(c, &c->geom);

	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(&c->scene->node, P.layers[LyrUnmanaged]);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		if (client_wants_focus(c))
			focusclient(c, NULL, 1);
		goto unset_fullscreen;
	}

	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
				c->isurgent ? urgentcolor : bordercolor);
		c->border[i]->node.data = c;
	}

	/* Initialize client geometry with room for border */
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;
	if (client_is_x11(c)) {
		int x = c->geom.x - (int)c->bw, y = c->geom.y - (int)c->bw;
		c->geom.x = c->geom.x < P.sgeom.x ? x : MAX(x, P.sgeom.x);
		c->geom.y = c->geom.y < P.sgeom.y ? y : MAX(y, P.sgeom.y);
		/* X11 clients set the fullscreen state before mapping, with no request */
		c->isfullscreen = client_wants_fullscreen(c);
	}

	/* Insert this client into client lists. */
	wl_list_insert(&P.clients, &c->link);
	wl_list_insert(&P.fstack, &c->flink);

	c->foreign_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(
			P.foreign_toplevel_list, &foreign_toplevel_state);
	c->foreign_toplevel_handle->data = c;
	c->image_capture_scene = wlr_scene_create();
	if (c->type == XDGShell)
		c->capture.image_capture_tree = wlr_scene_xdg_surface_create(&c->image_capture_scene->tree, c->surface.xdg);
#ifdef XWAYLAND
	else
		c->capture.image_capture_scene_surface = wlr_scene_surface_create(&c->image_capture_scene->tree, c->surface.xwayland->surface);
#endif


	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor as its parent.
	 * If there is no parent, apply rules */
	if ((p = client_get_parent(c))) {
		c->isfloating = 1;
		/* An unmanaged parent has no monitor */
		setmon(c, p->mon ? p->mon : P.selmon, p->tags);
	} else {
		applyrules(c);
	}
	printstatus();

unset_fullscreen:
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	wl_list_for_each(w, &P.clients, link) {
		if (w != c && w != p && w->isfullscreen && m == w->mon && (w->tags & c->tags)) {
			setfullscreen(w, 0);
			/* setmon() focused w because focustop() prefers fullscreen clients */
			focusclient(focustop(P.selmon), NULL, 1);
		}
	}
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. Pluto doesn't support maximization, but
	 * to conform to xdg-shell protocol we still must send a configure.
	 * Since xdg-shell protocol v5 we should ignore request of unsupported
	 * capabilities, just schedule a empty configure when the client uses <5
	 * protocol version
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
	Client *c = wl_container_of(listener, c, maximize);
	if (c->surface.xdg->initialized
			&& wl_resource_get_version(c->surface.xdg->toplevel->resource)
					< XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
		wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	if (c == focustop(c->mon))
		printstatus();

	if (c->foreign_toplevel_handle) {
		struct wlr_ext_foreign_toplevel_handle_v1_state foreign_toplevel_state = {
			.app_id = client_get_appid(c),
			.title = client_get_title(c),
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(c->foreign_toplevel_handle,
				&foreign_toplevel_state);
	}
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (!c || c == focustop(P.selmon))
		return;

	c->isurgent = 1;
	printstatus();

	if (client_surface(c)->mapped)
		client_set_border_color(c, urgentcolor);
}

void
transactionapply(void)
{
	struct wlr_scene_node **ghost;
	Client *c, *p;
	Monitor *m;

	wl_event_source_timer_update(P.transaction_timer, 0);
	P.transaction_waiting = 0;

	wl_array_for_each(ghost, &P.ghosts)
		wlr_scene_node_destroy(*ghost);
	P.ghosts.size = 0;

	wl_list_for_each(c, &P.clients, link) {
		if (c->resize_serial)
			c->late_serial = c->resize_serial;
		c->resize_serial = 0;
		if (!c->mon)
			continue;
		if (c->mon->wlr_output->enabled)
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, c->mon));
		applygeom(c);
	}

	wl_list_for_each(m, &P.mons, link) {
		if (m->wlr_output->enabled)
			wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
					(c = focustop(m)) && (c->isfullscreen
					|| ((p = client_get_parent(c)) && p->isfullscreen)));
	}

	motionnotify(0, NULL, 0, 0, 0, 0);
	checkidleinhibitor(NULL);
}

void
transactioncheck(void *data)
{
	Client *c;

	P.transaction_idle = NULL;

	/* Hidden clients don't have to redraw before the layout is shown */
	wl_list_for_each(c, &P.clients, link) {
		if (c->resize_serial && VISIBLEON(c, c->mon) && c->mon->wlr_output->enabled) {
			if (!P.transaction_waiting) {
				/* Don't restart the timer on updates of the pending
				 * layout, so the delay is bounded */
				wl_event_source_timer_update(P.transaction_timer, TRANSACTION_TIMEOUT);
				P.transaction_waiting = 1;
			}
			return;
		}
	}

	transactionapply();
}

void
transactioncommit(Client *c, int ready)
{
	if (c->resize_serial && ready) {
		c->resize_serial = 0;
		transactionschedule();
	} else if (c->snapshot) {
		/* This buffer is not shown, let the client draw again */
		client_send_frame_done(c);
	}
}

void
transactionschedule(void)
{
	if (!P.transaction_idle)
		P.transaction_idle = wl_event_loop_add_idle(P.event_loop, transactioncheck, NULL);
}

int
transactiontimeout(void *data)
{
	transactionapply();
	return 0;
}

struct wlr_scene_node *
snapshot(struct wlr_scene_node *node, struct wlr_scene_tree *parent, void *data)
{
	/* Copy the visible part of a scene graph subtree, keeping references to
	 * the buffers so that they can be shown after the client changes them */
	struct wlr_scene_node *child, *copy;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *rect;
	struct wlr_scene_buffer *src, *buffer;

	if (!node->enabled)
		return NULL;

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		tree = wlr_scene_tree_create(parent);
		wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
			snapshot(child, tree, data);
		copy = &tree->node;
		break;
	case WLR_SCENE_NODE_RECT:
		rect = wlr_scene_rect_from_node(node);
		copy = &wlr_scene_rect_create(parent, rect->width, rect->height, rect->color)->node;
		break;
	case WLR_SCENE_NODE_BUFFER:
		src = wlr_scene_buffer_from_node(node);
		buffer = wlr_scene_buffer_create(parent, NULL);
		wlr_scene_buffer_set_dest_size(buffer, src->dst_width, src->dst_height);
		wlr_scene_buffer_set_opaque_region(buffer, &src->opaque_region);
		wlr_scene_buffer_set_opacity(buffer, src->opacity);
		wlr_scene_buffer_set_filter_mode(buffer, src->filter_mode);
		wlr_scene_buffer_set_transfer_function(buffer, src->transfer_function);
		wlr_scene_buffer_set_primaries(buffer, src->primaries);
		wlr_scene_buffer_set_color_encoding(buffer, src->color_encoding);
		wlr_scene_buffer_set_color_range(buffer, src->color_range);
		wlr_scene_buffer_set_source_box(buffer, &src->src_box);
		wlr_scene_buffer_set_transform(buffer, src->transform);
		wlr_scene_buffer_set_buffer(buffer, src->buffer);
		copy = &buffer->node;
		break;
	default:
		return NULL;
	}

	copy->data = data;
	wlr_scene_node_set_position(copy, node->x, node->y);
	if (node->parent == parent)
		wlr_scene_node_place_above(copy, node);
	return copy;
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox;
	uint32_t serial;

	if (!c->mon || !client_surface(c)->mapped)
		return;

	bbox = interact ? &P.sgeom : &c->mon->w;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	if ((serial = client_set_size(c, c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw)))
		c->resize_serial = serial;

	if (interact || (!c->resize_serial && wlr_box_equal(&c->geom, &c->current))) {
		c->resize_serial = 0;
		applygeom(c);
		return;
	}

	/* Until the transaction is applied, keep showing the current content
	 * instead of buffers with the new size at the old position. */
	if (c->resize_serial) {
		if (!c->snapshot && c->scene->node.enabled
				&& (c->snapshot = snapshot(&c->scene_surface->node, c->scene, c)))
			wlr_scene_node_set_enabled(&c->scene_surface->node, 0);
		/* The scene graph doesn't send frame done events to hidden
		 * surfaces, but the client may be waiting for one to redraw */
		client_send_frame_done(c);
	}
	transactionschedule();
}

void
moveresize(const Arg *arg)
{
	if (P.cursor_mode != CurNormal && P.cursor_mode != CurPressed)
		return;
	xytonode(P.cursor->x, P.cursor->y, NULL, &P.grabc, NULL, NULL, NULL);
	if (!P.grabc || client_is_unmanaged(P.grabc) || P.grabc->isfullscreen)
		return;

	/* Float the window and tell motionnotify to grab it */
	setfloating(P.grabc, 1);
	switch (P.cursor_mode = arg->ui) {
	case CurMove:
		P.grabcx = (int)round(P.cursor->x) - P.grabc->geom.x;
		P.grabcy = (int)round(P.cursor->y) - P.grabc->geom.y;
		wlr_cursor_set_xcursor(P.cursor, P.cursor_mgr, "all-scroll");
		break;
	case CurResize:
		/* Doesn't work for X11 output - the next absolute motion event
		 * returns the cursor to where it started */
		wlr_cursor_warp_closest(P.cursor, NULL,
				P.grabc->geom.x + P.grabc->geom.width,
				P.grabc->geom.y + P.grabc->geom.height);
		wlr_cursor_set_xcursor(P.cursor, P.cursor_mgr, "se-resize");
		break;
	}
}

void
setfloating(Client *c, int floating)
{
	c->isfloating = floating;
	/* If in floating layout do not change the client's layer */
	if (!c->mon || !client_surface(c)->mapped || !c->mon->lt[c->mon->sellt]->arrange)
		return;
	arrange(c->mon);
	printstatus();
}

void
setfullscreen(Client *c, int fullscreen)
{
	int wasfullscreen = c->isfullscreen;

	c->isfullscreen = fullscreen;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);

	if (fullscreen) {
		/* Keep the geometry saved when the client became fullscreen */
		if (!wasfullscreen)
			c->prev = c->geom;
		resize(c, c->mon->m, 0);
	} else if (wasfullscreen) {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
	}
	arrange(c->mon);
	printstatus();
}

void
togglefloating(const Arg *arg)
{
	Client *sel = focustop(P.selmon);
	/* return if fullscreen */
	if (sel && !sel->isfullscreen)
		setfloating(sel, !sel->isfloating);
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = focustop(P.selmon);
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

void
killclient(const Arg *arg)
{
	Client *sel = focustop(P.selmon);
	if (sel)
		client_send_close(sel);
}

void
createpopup(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client (either xdg-shell or layer-shell)
	 * creates a new popup. */
	struct wlr_xdg_popup *popup = data;
	Popup *p = ecalloc(1, sizeof(*p));
	LISTEN(&popup->base->surface->events.commit, &p->commit, commitpopup);
	LISTEN(&popup->events.destroy, &p->destroy, destroypopup);
}

void
destroypopup(struct wl_listener *listener, void *data)
{
	Popup *p = wl_container_of(listener, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}
