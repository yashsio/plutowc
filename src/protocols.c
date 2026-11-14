/* Small protocol managers that do not own windows. */
#include "pluto.h"

void
checkidleinhibitor(struct wlr_surface *exclude)
{
	int inhibited = 0, unused_lx, unused_ly;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &P.idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;
		/* Only one inhibitor is being destroyed: its surface may have others */
		if (exclude == inhibitor->surface) {
			exclude = NULL;
			continue;
		}
		if (bypass_surface_visibility || (!tree
				|| wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly))) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_notifier_v1_set_inhibited(P.idle_notifier, inhibited);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	struct wl_listener *destroy = ecalloc(1, sizeof(*destroy));
	LISTEN(&idle_inhibitor->events.destroy, destroy, destroyidleinhibitor);

	checkidleinhibitor(NULL);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
	/* `data` is the wlr_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager */
	checkidleinhibitor(data);
	wl_list_remove(&listener->link);
	free(listener);
}

void
capturerequest(struct wl_listener *listener, void *data)
{
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
	Client *view = request->toplevel_handle->data;

	if (!view->image_capture_source) {
		view->image_capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
				&view->image_capture_scene->tree.node, P.event_loop, P.alloc, P.drw);
		if (!view->image_capture_source)
			return;
	}

	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
		request, view->image_capture_source);

	client_set_suspended(view, 0);
}
