/* Seat and input: keyboard, pointer, cursor, drag-and-drop and constraints. */
#include "pluto.h"

void
axisnotify(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	const Axis *a;
	unsigned int adir;
	struct wlr_keyboard *keyboard;
	uint32_t mods;

	wlr_idle_notifier_v1_notify_activity(P.idle_notifier, P.seat);

	if (!P.locked) {
		if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL)
			adir = event->delta > 0 ? AxisDown : AxisUp;
		else
			adir = event->delta > 0 ? AxisRight : AxisLeft;
		keyboard = wlr_seat_get_keyboard(P.seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (a = axes; a < END(axes); a++) {
			if (CLEANMASK(mods) == CLEANMASK(a->mod) && adir == a->dir && a->func) {
				a->func(&a->arg);
				return;
			}
		}
	}

	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(P.seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c = NULL;
	LayerSurface *l = NULL;
	const Button *b;

	wlr_idle_notifier_v1_notify_activity(P.idle_notifier, P.seat);

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		P.cursor_mode = CurPressed;
		P.selmon = xytomon(P.cursor->x, P.cursor->y);
		if (P.locked)
			break;

		/* Change focus if the button was _pressed_ over a client
		   or a layer surface with on-demand keyboard interactivity */
		xytonode(P.cursor->x, P.cursor->y, NULL, &c, &l, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c))) {
			focusclient(c, NULL, 1);
		} else if (l && l->layer_surface->current.keyboard_interactive) {
			focusclient(NULL, l, 0);
		}

		keyboard = wlr_seat_get_keyboard(P.seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		if (!P.locked && P.cursor_mode != CurNormal && P.cursor_mode != CurPressed) {
			P.cursor_mode = CurNormal;
			/* Update the cursor */
			wlr_seat_pointer_clear_focus(P.seat);
			motionnotify(0, NULL, 0, 0, 0, 0);
			/* Drop the window off on its new monitor */
			P.selmon = xytomon(P.cursor->x, P.cursor->y);
			setmon(P.grabc, P.selmon, 0);
			P.grabc = NULL;
			return;
		}
		P.cursor_mode = CurNormal;
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(P.seat,
			event->time_msec, event->button, event->state);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. Also, some hardware emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (!event->time_msec) /* this is 0 with virtual pointers */
		wlr_cursor_warp_absolute(P.cursor, &event->pointer->base, event->x, event->y);

	wlr_cursor_absolute_to_layout_coords(P.cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	dx = lx - P.cursor->x;
	dy = ly - P.cursor->y;
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	int nx, ny;
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL, *focusedl = NULL;
	struct wlr_surface *surface = NULL;

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				P.relative_pointer_mgr, P.seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		/* Only the constraint of the surface with the pointer is active */
		cursorconstrain(wlr_pointer_constraints_v1_constraint_for_surface(
				P.pointer_constraints, P.seat->pointer_state.focused_surface, P.seat));

		if (P.active_constraint && P.cursor_mode != CurResize && P.cursor_mode != CurMove) {
			toplevel_from_wlr_surface(P.active_constraint->surface, &c, NULL);
			if (c && P.active_constraint->surface == P.seat->pointer_state.focused_surface) {
				sx = P.cursor->x - c->geom.x - c->bw;
				sy = P.cursor->y - c->geom.y - c->bw;
				if (wlr_region_confine(&P.active_constraint->region, sx, sy,
						sx + dx, sy + dy, &sx_confined, &sy_confined)) {
					dx = sx_confined - sx;
					dy = sy_confined - sy;
				}

				if (P.active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			}
		}

		wlr_cursor_move(P.cursor, device, dx, dy);
		wlr_idle_notifier_v1_notify_activity(P.idle_notifier, P.seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			P.selmon = xytomon(P.cursor->x, P.cursor->y);
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&P.drag_icon->node, (int)round(P.cursor->x), (int)round(P.cursor->y));

	/* If we are currently grabbing the mouse, handle and return */
	if (P.cursor_mode == CurMove) {
		if (!P.selmon)
			return;
		/* Move the grabbed client to the new position. */
		nx = (int)round(P.cursor->x) - P.grabcx;
		ny = (int)round(P.cursor->y) - P.grabcy;
		if (abs(P.selmon->w.x - nx) < (int)snap)
			nx = P.selmon->w.x;
		else if (abs((P.selmon->w.x + P.selmon->w.width) - (nx + P.grabc->geom.width)) < (int)snap)
			nx = P.selmon->w.x + P.selmon->w.width - P.grabc->geom.width;
		if (abs(P.selmon->w.y - ny) < (int)snap)
			ny = P.selmon->w.y;
		else if (abs((P.selmon->w.y + P.selmon->w.height) - (ny + P.grabc->geom.height)) < (int)snap)
			ny = P.selmon->w.y + P.selmon->w.height - P.grabc->geom.height;
		resize(P.grabc, (struct wlr_box){.x = nx, .y = ny,
			.width = P.grabc->geom.width, .height = P.grabc->geom.height}, 1);
		return;
	} else if (P.cursor_mode == CurResize) {
		resize(P.grabc, (struct wlr_box){.x = P.grabc->geom.x, .y = P.grabc->geom.y,
			.width = (int)round(P.cursor->x) - P.grabc->geom.x, .height = (int)round(P.cursor->y) - P.grabc->geom.y}, 1);
		return;
	}

	/* Find the client under the pointer and send the event along. */
	xytonode(P.cursor->x, P.cursor->y, &surface, &c, &l, &sx, &sy);

	if (P.cursor_mode == CurPressed && !P.seat->drag
			&& surface != P.seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(P.seat->pointer_state.focused_surface, &w, &focusedl) >= 0) {
		c = w;
		surface = P.seat->pointer_state.focused_surface;
		sx = P.cursor->x - (focusedl ? focusedl->scene->node.x : w->geom.x);
		sy = P.cursor->y - (focusedl ? focusedl->scene->node.y : w->geom.y);
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !P.seat->drag)
		wlr_cursor_set_xcursor(P.cursor, P.cursor_mgr, "default");

	pointerfocus(c, l, surface, sx, sy, time);

	/* The pointer may be on another surface now (moved or warped there) */
	cursorconstrain(wlr_pointer_constraints_v1_constraint_for_surface(
			P.pointer_constraints, P.seat->pointer_state.focused_surface, P.seat));
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
keypress(struct wl_listener *listener, void *data)
{
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	struct wlr_keyboard *kb = &group->wlr_group->keyboard;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
	int handled = 0;

	// Get the keysyms for level 0 (normal) and level 1 (shifted)
	// Only one keysym for each level:
	// multiple keysyms per keycode don't really exist in the real world
	for (int i = 0; i < 2; i++)
		group->keysyms[i] = keymap_get_one_sym_by_level(kb->keymap, keycode, layout, i);
	group->mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

	wlr_idle_notifier_v1_notify_activity(P.idle_notifier, P.seat);

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!P.locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < 2; i++) {
			const Key *key = keybinding(group->mods, group->keysyms[i]);
			if (key) {
				if (event->keycode < LENGTH(P.consumed))
					P.consumed[event->keycode] = 1;
				key->func(&key->arg);
				handled = 1;
				break;
			}
		}
	}

	if (handled && group->wlr_group->keyboard.repeat_info.delay > 0)
		wl_event_source_timer_update(group->key_repeat_source,
				group->wlr_group->keyboard.repeat_info.delay);
	else
		wl_event_source_timer_update(group->key_repeat_source, 0);

	if (handled)
		return;

	if (event->keycode < LENGTH(P.consumed) && P.consumed[event->keycode]) {
		if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED)
			P.consumed[event->keycode] = 0;
		return; /* Don't pass to the client the release event of a handled key-press */
	}

	if (input_method_keyboard_grab_forward_key(group, event))
		return;
	wlr_seat_set_keyboard(P.seat, &group->wlr_group->keyboard);
	/* Pass unhandled keycodes along to the client. */
	wlr_seat_keyboard_notify_key(P.seat, event->time_msec,
			event->keycode, event->state);
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	if (input_method_keyboard_grab_forward_modifiers(group))
		return;

	wlr_seat_set_keyboard(P.seat, &group->wlr_group->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(P.seat,
			&group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
	KeyboardGroup *group = data;
	if (P.locked || group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(group->key_repeat_source,
			1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (int i = 0; i < 2; i++) {
		const Key *key = keybinding(group->mods, group->keysyms[i]);
		if (key) {
			key->func(&key->arg);
			break;
		}
	}

	return 0;
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(keyboard, P.kb_group->wlr_group->keyboard.keymap);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(P.kb_group->wlr_group, keyboard);
}

KeyboardGroup *
createkeyboardgroup(void)
{
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	/* Prepare an XKB keymap and assign it to the keyboard group. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!(keymap = xkb_keymap_new_from_names(context, &xkb_rules,
				XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, repeat_rate, repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

	group->key_repeat_source = wl_event_loop_add_timer(P.event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
	 * all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(P.seat, &group->wlr_group->keyboard);
	return group;
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

void
createpointer(struct wlr_pointer *pointer)
{
	struct libinput_device *device;
	if (wlr_input_device_is_libinput(&pointer->base)
			&& (device = wlr_libinput_get_device_handle(&pointer->base))) {

		if (libinput_device_config_tap_get_finger_count(device)) {
			libinput_device_config_tap_set_enabled(device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock);
			libinput_device_config_tap_set_button_map(device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(device))
			libinput_device_config_dwt_set_enabled(device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(device, scroll_method);

		if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(device, click_method);

		if (libinput_device_config_send_events_get_modes(device))
			libinput_device_config_send_events_set_mode(device, send_events_mode);

		if (libinput_device_config_accel_is_available(device)) {
			libinput_device_config_accel_set_profile(device, accel_profile);
			libinput_device_config_accel_set_speed(device, accel_speed);
		}
	}

	wlr_cursor_attach_input_device(P.cursor, &pointer->base);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In Pluto we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&P.kb_group->wlr_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(P.seat, caps);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
	struct wlr_pointer_constraint_v1 *old = P.active_constraint;
	if (old == constraint)
		return;

	/* Set first: deactivating a oneshot constraint destroys it, and
	 * destroypointerconstraint() would warp the cursor back to its hint */
	P.active_constraint = constraint;
	if (old)
		wlr_pointer_constraint_v1_send_deactivated(old);
	if (constraint)
		wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(P.seat);
}

void
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = P.active_constraint->current.cursor_hint.x;
	double sy = P.active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(P.active_constraint->surface, &c, NULL);
	if (c && P.active_constraint->current.cursor_hint.enabled) {
		wlr_cursor_warp(P.cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
		wlr_seat_pointer_warp(P.active_constraint->seat, sx, sy);
	}
}

void
refocuspointer(void *data)
{
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

	if (P.active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		P.active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void
pointerfocus(Client *c, LayerSurface *l, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;

	/* The keyboard focus can't change during a drag, see destroydrag() */
	if (surface != P.seat->pointer_state.focused_surface && sloppyfocus && time
			&& !P.seat->drag) {
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c))) {
			focusclient(c, NULL, 0);
		} else if (l && l->layer_surface->current.keyboard_interactive) {
			focusclient(NULL, l, 0);
		}
	}

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(P.seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(P.seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(P.seat, time, sx, sy);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(P.seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(P.seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;

	wl_signal_add(&drag->events.destroy, &drag_destroy);
	if (drag->icon)
		drag->icon->data = &wlr_scene_drag_icon_create(P.drag_icon, drag->icon)->node;
}

void
destroydrag(struct wl_listener *listener, void *data)
{
	Client *c = NULL;

	/* Focus enter isn't sent during drag, so the focus didn't follow the
	 * pointer: give it now to the client the drag ended on */
	if (sloppyfocus)
		xytonode(P.cursor->x, P.cursor->y, NULL, &c, NULL, NULL, NULL);
	focusclient(c && !client_is_unmanaged(c) ? c : focustop(P.selmon), NULL, 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (P.cursor_mode != CurNormal && P.cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == P.seat->pointer_state.focused_client)
		wlr_cursor_set_surface(P.cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (P.cursor_mode != CurNormal && P.cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == P.seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(P.cursor, P.cursor_mgr,
				wlr_cursor_shape_v1_name(event->shape));
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in Pluto we always honor them
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(P.seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in Pluto we always honor them
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(P.seat, event->source, event->serial);
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	KeyboardGroup *group = createkeyboardgroup();
	group->virtual_keyboard = &kb->keyboard;
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
	wlr_seat_set_capabilities(P.seat, P.seat->capabilities | WL_SEAT_CAPABILITY_KEYBOARD);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(P.cursor, device);
	if ((device->data = event->suggested_output))
		wlr_cursor_map_input_to_output(P.cursor, device, event->suggested_output);
	wlr_seat_set_capabilities(P.seat, P.seat->capabilities | WL_SEAT_CAPABILITY_POINTER);
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node;
	struct wlr_scene_surface *scene_surface;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	int layer;

	for (layer = NUM_LAYERS - 1; !surface && !c && layer >= (P.locked ? LyrBlock : 0); layer--) {
		if (layer == LyrIMPopup ||
				!(node = wlr_scene_node_at(&P.layers[layer]->node, x, y, nx, ny))
				|| node->type != WLR_SCENE_NODE_BUFFER)
			continue;

		if ((scene_surface = wlr_scene_surface_try_from_buffer(
				wlr_scene_buffer_from_node(node))))
			surface = scene_surface->surface;
		else /* snapshot of a client during a transaction */
			c = node->data;
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	toplevel_from_wlr_surface(surface, pc, pl);
}
