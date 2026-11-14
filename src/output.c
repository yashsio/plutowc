/* Outputs and monitors: DRM/output lifecycle, layout and output-management protocol. */
#include "pluto.h"

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
	LayerSurface *l;
	struct wlr_box full_area = m->m;

	wl_list_for_each(l, list, link) {
		struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

		if (!layer_surface->initialized)
			continue;

		if (exclusive != (layer_surface->current.exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
	}
}

void
arrangelayers(Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	LayerSurface *l;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (!wlr_box_equal(&usable_area, &m->w)) {
		m->w = usable_area;
		arrange(m);
	}

	/* Arrange non-exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	/* Find topmost keyboard interactive layer that has indicated it wants
	 * exclusive access to the keyboard, if such a layer exists */
	for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
			if (P.locked ||
					l->layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE ||
					!l->mapped)
				continue;
			focusclient(NULL, l, 0);
			return;
		}
	}
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	struct wlr_virtual_pointer_v1 *pointer;
	size_t i;

	/* wlr_cursor doesn't forget the output of the devices mapped to it */
	wl_list_for_each(pointer, &P.virtual_pointer_mgr->virtual_pointers, link) {
		if (pointer->pointer.base.data == m->wlr_output)
			wlr_cursor_map_input_to_output(P.cursor, &pointer->pointer.base, NULL);
	}

	/* Destroy the scene output first: a client that the layer surfaces
	 * destroyed below uncover would be sent wl_surface.enter for it */
	wlr_scene_output_destroy(m->scene_output);

	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);
	wl_list_remove(&m->request_state.link);
	if (m->lock_surface)
		destroylocksurface(&m->destroy_lock_surface, NULL);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(P.output_layout, m->wlr_output);

	closemon(m);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);
	free(m);
}

void
closemon(Monitor *m)
{
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c;
	Monitor *o;
	if (m == P.selmon || wl_list_empty(&P.mons)) {
		P.selmon = NULL;
		wl_list_for_each(o, &P.mons, link) {
			/* don't switch to disabled mons */
			if (o->wlr_output->enabled) {
				P.selmon = o;
				break;
			}
		}
	}

	wl_list_for_each(c, &P.clients, link) {
		if (c->mon == m)
			setmon(c, P.selmon, c->tags);
	}
	focusclient(focustop(P.selmon), NULL, 1);
	printstatus();
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	size_t i;
	struct wlr_output_state state;
	Monitor *m;

	if (!wlr_output_init_render(wlr_output, P.alloc, P.drw))
		return;

	m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);

	wlr_output_state_init(&state);
	/* Initialize monitor state using configured rules */
	m->tagset[0] = m->tagset[1] = 1;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->m.x = r->x;
			m->m.y = r->y;
			m->mfact = r->mfact;
			m->nmaster = r->nmaster;
			m->lt[0] = r->lt;
			m->lt[1] = &layouts[LENGTH(layouts) > 1 && r->lt != &layouts[1]];
			strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));
			wlr_output_state_set_scale(&state, r->scale);
			wlr_output_state_set_transform(&state, r->rr);
			break;
		}
	}

	/* The mode is a tuple of (width, height, refresh rate), and each
	 * monitor supports only a specific set of modes. We just pick the
	 * monitor's preferred mode; a more sophisticated compositor would let
	 * the user configure it. */
	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
	LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

	/* Some monitors disconnect when they are turned off and reconnect shortly
	 * after, so keep outputs that were turned off disabled. */
	if (!monoff(wlr_output)) {
		wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));
		wlr_output_state_set_enabled(&state, 1);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	wl_list_insert(&P.mons, &m->link);
	printstatus();

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */
	/* updatemons() will resize and set correct position */
	m->fullscreen_bg = wlr_scene_rect_create(P.layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Adds this to the output layout in the order it was configured.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(P.scene, wlr_output);

	if (!wlr_output->enabled) {
		updatemons(NULL, NULL);
		return;
	}

	if (m->m.x == -1 && m->m.y == -1)
		wlr_output_layout_add_auto(P.output_layout, wlr_output);
	else
		wlr_output_layout_add(P.output_layout, wlr_output, m->m.x, m->m.y);
}

Monitor *
dirtomon(enum wlr_direction dir)
{
	struct wlr_output *next;
	if (!wlr_output_layout_get(P.output_layout, P.selmon->wlr_output))
		return P.selmon;
	if ((next = wlr_output_layout_adjacent_output(P.output_layout,
			dir, P.selmon->wlr_output, P.selmon->m.x, P.selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(P.output_layout,
			dir ^ (WLR_DIRECTION_LEFT | WLR_DIRECTION_RIGHT |
					WLR_DIRECTION_UP | WLR_DIRECTION_DOWN),
			P.selmon->wlr_output, P.selmon->m.x, P.selmon->m.y)))
		return next->data;
	return P.selmon;
}

Monitor *
xytomon(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(P.output_layout, x, y);
	return o ? o->data : NULL;
}

void
focusmon(const Arg *arg)
{
	int i = 0, nmons = wl_list_length(&P.mons);
	if (!P.selmon)
		return;
	if (nmons) {
		do /* don't switch to disabled mons */
			P.selmon = dirtomon(arg->i);
		while (!P.selmon->wlr_output->enabled && i++ < nmons);
	}
	focusclient(focustop(P.selmon), NULL, 1);
}

void
tagmon(const Arg *arg)
{
	Client *sel = focustop(P.selmon);
	if (sel)
		setmon(sel, dirtomon(arg->i), 0);
}

char **
monoff(struct wlr_output *wlr_output)
{
	char **name;
	wl_array_for_each(name, &P.offmons) {
		if (!strcmp(*name, wlr_output->name))
			return name;
	}
	return NULL;
}

void
setmonoff(struct wlr_output *wlr_output, int off)
{
	char **name = monoff(wlr_output), **last;

	if (off && !name && (name = wl_array_add(&P.offmons, sizeof(*name)))) {
		if (!(*name = strdup(wlr_output->name)))
			P.offmons.size -= sizeof(*name);
	} else if (!off && name) {
		free(*name);
		last = (char **)((char *)P.offmons.data + P.offmons.size) - 1;
		*name = *last;
		P.offmons.size -= sizeof(*name);
	}
}

void
forgetoffmons(void)
{
	/* Forget the outputs that were unplugged while off, or they would stay
	 * off when plugged in again */
	char **name, **kept = P.offmons.data;
	Monitor *m;

	wl_array_for_each(name, &P.offmons) {
		wl_list_for_each(m, &P.mons, link)
			if (!strcmp(*name, m->wlr_output->name))
				break;
		if (&m->link != &P.mons)
			*kept++ = *name;
		else
			free(*name);
	}
	P.offmons.size = (char *)kept - (char *)P.offmons.data;
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	size_t states_len, i;
	struct wlr_output_swapchain_manager swapchain_manager;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &states_len);
	int ok = 0;

	if (!states) {
		wlr_output_configuration_v1_send_failed(config);
		return;
	}

	wlr_output_swapchain_manager_init(&swapchain_manager, P.backend);

	ok = wlr_output_swapchain_manager_prepare(&swapchain_manager, states, states_len);
	if (!ok || test)
		goto out;

	for (i = 0; i < states_len; i++) {
		Monitor *m = states[i].output->data;
		struct wlr_scene_output_state_options opts = {
			.swapchain = wlr_output_swapchain_manager_get_swapchain(
					&swapchain_manager, states[i].output),
		};
		if (!(ok = wlr_scene_output_build_state(m->scene_output, &states[i].base, &opts)))
			goto out;
	}

	if (!(ok = wlr_backend_commit(P.backend, states, states_len)))
		goto out;

	wlr_output_swapchain_manager_apply(&swapchain_manager);

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;

		/* Ensure displays previously disabled by wlr-output-power-management-v1
		 * are properly handled*/
		m->asleep = 0;
		setmonoff(wlr_output, !config_head->state.enabled);

		/* Don't move monitors if position wouldn't change. This avoids
		 * wlroots marking the output as manually configured.
		 * wlr_output_layout_add does not like disabled outputs */
		if (config_head->state.enabled
				&& (m->m.x != config_head->state.x || m->m.y != config_head->state.y
					|| !wlr_output_layout_get(P.output_layout, wlr_output))) {
			wlr_output_layout_add(P.output_layout, wlr_output,
					config_head->state.x, config_head->state.y);
		}
	}

out:
	wlr_output_swapchain_manager_finish(&swapchain_manager);
	for (i = 0; i < states_len; i++)
		wlr_output_state_finish(&states[i].base);

	free(states);

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	/* https://codeberg.org/dwl/dwl/issues/577 */
	updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state = {0};
	Monitor *m = event->output->data;

	if (!m)
		return;

	if (event->mode)
		forgetoffmons();
	m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the output */
	wlr_output_state_set_enabled(&state, event->mode);
	/* The output was reconnected while off and was never modeset */
	if (event->mode && !m->wlr_output->current_mode)
		wlr_output_state_set_mode(&state, wlr_output_preferred_mode(m->wlr_output));
	setmonoff(m->wlr_output, !event->mode);
	wlr_output_commit_state(m->wlr_output, &state);

	m->asleep = !event->mode;
	updatemons(NULL, NULL);
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config
			= wlr_output_configuration_v1_create();
	Client *c;
	LayerSurface *l, *tmp;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m;
	struct wlr_box old;
	size_t i;

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &P.mons, link) {
		if (m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		if (m->asleep)
			continue;

		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(P.output_layout, m->wlr_output);
		/* Its layer surfaces would stay where they are in the layout */
		for (i = 0; i < LENGTH(m->layers); i++) {
			wl_list_for_each_safe(l, tmp, &m->layers[i], link)
				wlr_layer_surface_v1_destroy(l->layer_surface);
		}
		closemon(m);
		m->m = m->w = (struct wlr_box){0};
	}
	/* Insert outputs that need to */
	wl_list_for_each(m, &P.mons, link) {
		if (m->wlr_output->enabled
				&& !wlr_output_layout_get(P.output_layout, m->wlr_output))
			wlr_output_layout_add_auto(P.output_layout, m->wlr_output);
	}

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(P.output_layout, NULL, &P.sgeom);

	wlr_scene_node_set_position(&P.root_bg->node, P.sgeom.x, P.sgeom.y);
	wlr_scene_rect_set_size(P.root_bg, P.sgeom.width, P.sgeom.height);

	/* Make sure the clients are hidden when Pluto is locked */
	wlr_scene_node_set_position(&P.locked_bg->node, P.sgeom.x, P.sgeom.y);
	wlr_scene_rect_set_size(P.locked_bg, P.sgeom.width, P.sgeom.height);

	wl_list_for_each(m, &P.mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		old = m->m;
		wlr_output_layout_get_box(P.output_layout, m->wlr_output, &m->m);
		m->w = m->m;
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);
		/* make sure fullscreen clients have the right size, and that
		 * floating clients move with their monitor */
		wl_list_for_each(c, &P.clients, link) {
			if (c->mon != m)
				continue;
			if (c->isfullscreen)
				resize(c, m->m, 0);
			else if ((c->isfloating || !m->lt[m->sellt]->arrange) && !wlr_box_empty(&old)
					&& (m->m.x != old.x || m->m.y != old.y))
				resize(c, (struct wlr_box){.x = c->geom.x + m->m.x - old.x,
						.y = c->geom.y + m->m.y - old.y,
						.width = c->geom.width, .height = c->geom.height}, 0);
		}

		/* Try to re-set the gamma LUT when updating monitors,
		 * it's only really needed when enabling a disabled output, but meh. */
		m->gamma_lut_changed = 1;

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!P.selmon) {
			P.selmon = m;
		}
	}

	if (P.selmon && P.selmon->wlr_output->enabled) {
		wl_list_for_each(c, &P.clients, link) {
			if (!c->mon && client_surface(c)->mapped)
				setmon(c, P.selmon, c->tags);
		}
		focusclient(focustop(P.selmon), NULL, 1);
		if (P.selmon->lock_surface) {
			client_notify_enter(P.selmon->lock_surface->surface,
					wlr_seat_get_keyboard(P.seat));
			client_activate_surface(P.selmon->lock_surface->surface, 1);
		}
	}

	/* FIXME: figure out why the cursor image is at 0,0 after turning all
	 * the monitors on.
	 * Move the cursor image where it used to be. It does not generate a
	 * wl_pointer.motion event for the clients, it's only the image what it's
	 * at the wrong position after all. */
	wlr_cursor_move(P.cursor, NULL, 0, 0);

	wlr_output_manager_v1_set_configuration(P.output_mgr, config);
}

void
rendermon(struct wl_listener *listener, void *data)
{
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	Monitor *m = wl_container_of(listener, m, frame);
	struct timespec now;

	if (!wlr_scene_output_needs_frame(m->scene_output)) {
		return;
	}

	wlr_scene_output_commit(m->scene_output, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

void
setmon(Client *c, Monitor *m, uint32_t newtags)
{
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	if (!c->isfullscreen || wlr_box_empty(&c->prev))
		c->prev = c->geom;

	/* Scene graph sends surface leave/enter events on move and resize */
	if (oldmon)
		arrange(oldmon);
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
		setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
		setfloating(c, c->isfloating);
	}
	focusclient(focustop(P.selmon), NULL, 1);
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(event->output, event->state);
	updatemons(NULL, NULL);
}
