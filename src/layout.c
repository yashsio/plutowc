/* Window-management policy: tags, focus and tiling/monocle layouts. */
#include "pluto.h"
#include "layout_math.h"

void
arrange(Monitor *m)
{
	Client *c;

	if (!m->wlr_output->enabled)
		return;

	wl_list_for_each(c, &P.clients, link) {
		if (c->mon == m)
			client_set_suspended(c, !VISIBLEON(c, m)
					&& (!c->image_capture_source
					|| wl_list_empty(&c->image_capture_source->resources)));
	}

	snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "%s",
			m->lt[m->sellt]->symbol);

	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);

	transactionschedule();
}

void
tile(Monitor *m)
{
	const struct TileOptions opts = {
		.nmaster = m->nmaster,
		.mfact = m->mfact,
		.gap = gappx,
		.outergap = outergappx,
	};
	const struct Box area = {
		.x = m->w.x,
		.y = m->w.y,
		.width = m->w.width,
		.height = m->w.height,
	};
	Client *c;
	int i = 0, n = 0;

	wl_list_for_each(c, &P.clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	if (n == 0)
		return;

	wl_list_for_each(c, &P.clients, link) {
		struct Box box;

		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		box = layout_tile_slot(area, n, i++, &opts);
		resize(c, (struct wlr_box){
			.x = box.x,
			.y = box.y,
			.width = box.width,
			.height = box.height,
		}, 0);
	}
}

void
monocle(Monitor *m)
{
	const struct Box area = {
		.x = m->w.x,
		.y = m->w.y,
		.width = m->w.width,
		.height = m->w.height,
	};
	Client *c;
	int i = 0, n = 0;

	wl_list_for_each(c, &P.clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	if (n == 0)
		return;

	wl_list_for_each(c, &P.clients, link) {
		struct Box box;

		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		box = layout_monocle_slot(area, n, i++, outergappx);
		resize(c, (struct wlr_box){
			.x = box.x,
			.y = box.y,
			.width = box.width,
			.height = box.height,
		}, 0);
	}
	snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
	if ((c = focustop(m)))
		wlr_scene_node_raise_to_top(&c->scene->node);
}

void
setlayout(const Arg *arg)
{
	if (!P.selmon)
		return;
	if (!arg || !arg->v || arg->v != P.selmon->lt[P.selmon->sellt])
		P.selmon->sellt ^= 1;
	if (arg && arg->v)
		P.selmon->lt[P.selmon->sellt] = (Layout *)arg->v;
	snprintf(P.selmon->ltsymbol, LENGTH(P.selmon->ltsymbol), "%s",
			P.selmon->lt[P.selmon->sellt]->symbol);
	arrange(P.selmon);
	printstatus();
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !P.selmon || !P.selmon->lt[P.selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0f ? arg->f + P.selmon->mfact : arg->f - 1.0f;
	if (f < 0.1 || f > 0.9)
		return;
	P.selmon->mfact = f;
	arrange(P.selmon);
}

void
incnmaster(const Arg *arg)
{
	if (!arg || !P.selmon)
		return;
	P.selmon->nmaster = MAX(P.selmon->nmaster + arg->i, 0);
	arrange(P.selmon);
}

void
tag(const Arg *arg)
{
	Client *sel = focustop(P.selmon);
	if (!sel || (arg->ui & TAGMASK) == 0)
		return;

	sel->tags = arg->ui & TAGMASK;
	focusclient(focustop(P.selmon), NULL, 1);
	arrange(P.selmon);
	printstatus();
}

void
toggletag(const Arg *arg)
{
	uint32_t newtags;
	Client *sel = focustop(P.selmon);
	if (!sel || !(newtags = sel->tags ^ (arg->ui & TAGMASK)))
		return;

	sel->tags = newtags;
	focusclient(focustop(P.selmon), NULL, 1);
	arrange(P.selmon);
	printstatus();
}

void
toggleview(const Arg *arg)
{
	uint32_t newtagset;
	if (!(newtagset = P.selmon ? P.selmon->tagset[P.selmon->seltags] ^ (arg->ui & TAGMASK) : 0))
		return;

	P.selmon->tagset[P.selmon->seltags] = newtagset;
	focusclient(focustop(P.selmon), NULL, 1);
	arrange(P.selmon);
	printstatus();
}

void
view(const Arg *arg)
{
	if (!P.selmon || (arg->ui & TAGMASK) == P.selmon->tagset[P.selmon->seltags])
		return;
	P.selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		P.selmon->tagset[P.selmon->seltags] = arg->ui & TAGMASK;
	focusclient(focustop(P.selmon), NULL, 1);
	arrange(P.selmon);
	printstatus();
}

void
zoom(const Arg *arg)
{
	Client *c, *sel = focustop(P.selmon);

	if (!sel || !P.selmon || !P.selmon->lt[P.selmon->sellt]->arrange || sel->isfloating)
		return;

	/* Search for the first tiled window that is not sel, marking sel as
	 * NULL if we pass it along the way */
	wl_list_for_each(c, &P.clients, link) {
		if (VISIBLEON(c, P.selmon) && !c->isfloating) {
			if (c != sel)
				break;
			sel = NULL;
		}
	}

	/* Return if no other tiled window was found */
	if (&c->link == &P.clients)
		return;

	/* If we passed sel, move c to the front; otherwise, move sel to the
	 * front */
	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&P.clients, &sel->link);

	focusclient(sel, NULL, 1);
	arrange(P.selmon);
}

void
focusclient(Client *c, LayerSurface *l, int lift)
{
	struct wlr_surface *surface, *old = P.seat->keyboard_state.focused_surface;
	int unused_lx, unused_ly, old_client_type;
	Client *old_c = NULL;
	LayerSurface *old_l = NULL;

	if (P.locked)
		return;

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c && !client_is_unmanaged(c)) {
		wl_list_remove(&c->flink);
		wl_list_insert(&P.fstack, &c->flink);
		P.selmon = c->mon;
		c->isurgent = 0;

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!(old_l && old_l->layer_surface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
				&& !P.seat->drag)
			client_set_border_color(c, focuscolor);
	}

	/* If an overlay is focused, don't focus or activate the client,
	 * but only update its position in fstack to render its border with focuscolor
	 * and focus it after the overlay is closed. */
	if (!l && old && (!c || client_surface(c) != old)
			&& old_client_type == LayerShell && wlr_scene_node_coords(
				&old_l->scene->node, &unused_lx, &unused_ly)
			&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP
			&& old_l->layer_surface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
		return;

	if (P.focused_client && P.focused_client != c && !(c && client_is_unmanaged(c))) {
		struct wlr_surface *s = client_surface(P.focused_client);
		if (c)
			client_set_border_color(P.focused_client, bordercolor);
		if (s && s->mapped)
			client_activate_surface(s, 0);
		P.focused_client = NULL;
	}
	printstatus();

	if (!c && !l) {
		/* With no client, all we have left is to clear focus */
		input_method_relay_set_focus(P.input_method_relay, NULL);
		wlr_seat_keyboard_notify_clear_focus(P.seat);
		return;
	}

	surface = c ? client_surface(c) : l->layer_surface->surface;
	input_method_relay_set_focus(P.input_method_relay, surface);
	client_notify_enter(surface, wlr_seat_get_keyboard(P.seat));

	if (l)
		return;

	client_activate_surface(surface, 1);
	motionnotify(0, NULL, 0, 0, 0, 0);

	if (!client_is_unmanaged(c))
		P.focused_client = c;
}

/* We probably should change the name of this: it sounds like it
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *
focustop(Monitor *m)
{
	Client *c, *fs = NULL;

	wl_list_for_each(c, &P.fstack, flink) {
		if (VISIBLEON(c, m) && c->isfullscreen) {
			fs = c;
			break;
		}
	}

	/* A fullscreen client's children are shown above it */
	wl_list_for_each(c, &P.fstack, flink)
		if (VISIBLEON(c, m) && (!fs || c == fs || client_get_parent(c) == fs))
			return c;
	return NULL;
}

void
focusstack(const Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = focustop(P.selmon);
	if (!sel || (sel->isfullscreen && !client_has_children(sel)))
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &P.clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, P.selmon))
				break; /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &P.clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, P.selmon))
				break; /* found it */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, NULL, 1);
}
