/* Layer-shell surfaces and the session lock. */
#include "pluto.h"

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
	struct wlr_scene_tree *scene_layer = P.layers[layermap[layer_surface->current.layer]];
	struct wlr_layer_surface_v1_state old_state;

	if (l->layer_surface->initial_commit) {
		client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

		/* Temporarily set the layer's current state to pending
		 * so that we can easily arrange it */
		old_state = l->layer_surface->current;
		l->layer_surface->current = l->layer_surface->pending;
		arrangelayers(l->mon);
		l->layer_surface->current = old_state;
		return;
	}

	if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
		return;
	l->mapped = layer_surface->surface->mapped;
	wlr_scene_node_set_enabled(&l->scene->node, l->mapped);

	if (scene_layer != l->scene->node.parent) {
		wlr_scene_node_reparent(&l->scene->node, scene_layer);
		wl_list_remove(&l->link);
		wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
		wlr_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
				< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? P.layers[LyrTop] : scene_layer));
	}

	arrangelayers(l->mon);
	/* A layer surface that no longer wants the keyboard gives it back */
	if (layer_surface->surface == P.seat->keyboard_state.focused_surface
			&& !layer_surface->current.keyboard_interactive)
		focusclient(focustop(P.selmon), NULL, 1);
	/* Give pointer focus to a surface mapped under the cursor without waiting
	 * for the cursor to move, once the scene graph has handled this commit */
	wl_event_loop_add_idle(P.event_loop, refocuspointer, NULL);
}

void
commitlocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, commit_lock_surface);

	if (!m->lock_surface->surface->mapped)
		return;

	wl_list_remove(&m->commit_lock_surface.link);
	wl_list_init(&m->commit_lock_surface.link);

	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *layer_surface = data;
	LayerSurface *l;
	struct wlr_surface *surface = layer_surface->surface;
	struct wlr_scene_tree *scene_layer = P.layers[layermap[layer_surface->pending.layer]];

	if (!layer_surface->output
			&& !(layer_surface->output = P.selmon ? P.selmon->wlr_output : NULL)) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	l = layer_surface->data = ecalloc(1, sizeof(*l));
	l->type = LayerShell;
	LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
	LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
	LISTEN(&layer_surface->events.destroy, &l->destroy, destroylayersurfacenotify);

	l->layer_surface = layer_surface;
	l->mon = layer_surface->output->data;
	l->scene_layer = wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
	l->scene = l->scene_layer->tree;
	l->popups = surface->data = wlr_scene_tree_create(layer_surface->current.layer
			< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? P.layers[LyrTop] : scene_layer);
	l->scene->node.data = l->popups->node.data = l;

	wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
	wlr_surface_send_enter(surface, layer_surface->output);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->surface_commit.link);
	wlr_scene_node_destroy(&l->scene->node);
	wlr_scene_node_destroy(&l->popups->node);
	/* The wl_surface can outlive its role, e.g. with an idle inhibitor */
	l->layer_surface->surface->data = NULL;
	free(l);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);
	struct wl_list *prev = l->link.prev;

	l->mapped = 0;
	wlr_scene_node_set_enabled(&l->scene->node, 0);
	/* Arrange the others without this surface: wlroots forgets its configures
	 * right after unmapping it, so it couldn't acknowledge one sent now */
	wl_list_remove(&l->link);
	if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
		arrangelayers(l->mon);
	wl_list_insert(prev, &l->link);
	if (l->layer_surface->surface == P.seat->keyboard_state.focused_surface)
		focusclient(focustop(P.selmon), NULL, 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data
			= wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);
	LISTEN(&lock_surface->surface->events.commit, &m->commit_lock_surface, commitlocksurface);

	if (m == P.selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(P.seat));
}

void
destroylock(SessionLock *lock, int unlock)
{
	Monitor *m;

	wlr_seat_keyboard_notify_clear_focus(P.seat);
	if ((P.locked = !unlock))
		goto destroy;

	wlr_scene_node_set_enabled(&P.locked_bg->node, 0);
	/* The lock surfaces are destroyed after this: don't give them the pointer */
	wlr_scene_node_set_enabled(&lock->scene->node, 0);

	/* Give the keyboard back to a layer surface that wants it exclusively,
	 * which focusclient() then leaves it to */
	wl_list_for_each(m, &P.mons, link)
		arrangelayers(m);
	focusclient(focustop(P.selmon), NULL, 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	P.cur_lock = NULL;
	free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	lock_surface->surface->data = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);
	wl_list_remove(&m->commit_lock_surface.link);

	if (lock_surface->surface != P.seat->keyboard_state.focused_surface)
		return;

	if (!P.locked) {
		focusclient(focustop(P.selmon), NULL, 1);
		return;
	}

	/* The surface being destroyed is still in the list */
	if (P.cur_lock) {
		wl_list_for_each(surface, &P.cur_lock->surfaces, link) {
			if (surface != lock_surface) {
				client_notify_enter(surface->surface, wlr_seat_get_keyboard(P.seat));
				return;
			}
		}
	}
	wlr_seat_keyboard_clear_focus(P.seat);
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void
locksession(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	wlr_scene_node_set_enabled(&P.locked_bg->node, 1);
	if (P.cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = session_lock->data = ecalloc(1, sizeof(*lock));
	/* The keyboard grab of a popup or of a drag would keep the focus from
	 * changing, and focusclient() leaves it to a layer surface that has it
	 * exclusively */
	wlr_seat_keyboard_end_grab(P.seat);
	focusclient(NULL, NULL, 0);
	input_method_relay_set_focus(P.input_method_relay, NULL);
	wlr_seat_keyboard_notify_clear_focus(P.seat);

	lock->scene = wlr_scene_tree_create(P.layers[LyrBlock]);
	P.cur_lock = lock->lock = session_lock;
	P.locked = 1;

	/* Cancel any interactive move/resize and take the pointer focus away
	 * from the clients below the lock screen */
	P.cursor_mode = CurNormal;
	P.grabc = NULL;
	motionnotify(0, NULL, 0, 0, 0, 0);

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

void
unlocksession(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };
