/* Process lifecycle: entry point, backend/protocol wiring and teardown. */
#include "pluto.h"

/* The single compositor instance.  Everything mutable lives here. */
struct Pluto P;

int
main(int argc, char *argv[])
{
	char *startup_cmd = NULL;
	int c;

	while ((c = getopt(argc, argv, "s:hdv")) != -1) {
		if (c == 's')
			startup_cmd = optarg;
		else if (c == 'd')
			log_level = WLR_DEBUG;
		else if (c == 'v')
			die("pluto " VERSION);
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");
	setup();
	run(startup_cmd);
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-d] [-s startup command]", argv[0]);
}

void
run(char *startup_cmd)
{
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(P.dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(P.backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the startup command */
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((P.child_pid = fork()) < 0)
			die("startup: fork:");
		if (P.child_pid == 0) {
			setsid();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Mark stdout as non-blocking to avoid the startup script
	 * causing Pluto to freeze when a user neither closes stdin
	 * nor consumes standard input in his startup script */

	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	P.selmon = xytomon(P.cursor->x, P.cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. Still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(P.cursor, NULL, P.cursor->x, P.cursor->y);
	wlr_cursor_set_xcursor(P.cursor, P.cursor_mgr, "default");

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(P.dpy);
}

void
setup(void)
{
	int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);

	wlr_log_init(log_level, NULL);

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, managing Wayland globals, and so on. */
	P.dpy = wl_display_create();
	P.event_loop = wl_display_get_event_loop(P.dpy);

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(P.backend = wlr_backend_autocreate(P.event_loop, &P.session)))
		die("couldn't create backend");
	wl_signal_add(&P.backend->events.destroy, &backend_destroy);

	/* Initialize the scene graph used to lay out windows */
	P.scene = wlr_scene_create();
	P.root_bg = wlr_scene_rect_create(&P.scene->tree, 0, 0, rootcolor);
	for (i = 0; i < NUM_LAYERS; i++)
		P.layers[i] = wlr_scene_tree_create(&P.scene->tree);
	P.drag_icon = wlr_scene_tree_create(&P.scene->tree);
	wlr_scene_node_place_below(&P.drag_icon->node, &P.layers[LyrBlock]->node);

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	if (!(P.drw = wlr_renderer_autocreate(P.backend)))
		die("couldn't create renderer");
	wl_signal_add(&P.drw->events.lost, &gpu_reset);

	/* Create shm, drm and linux_dmabuf interfaces by ourselves.
	 * The simplest way is to call:
	 *      wlr_renderer_init_wl_display(drw);
	 * but we need to create the linux_dmabuf interface manually to integrate it
	 * with wlr_scene. */
	wlr_renderer_init_wl_shm(P.drw, P.dpy);

	if (wlr_renderer_get_texture_formats(P.drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(P.dpy, P.drw);
		wlr_scene_set_linux_dmabuf_v1(P.scene,
				wlr_linux_dmabuf_v1_create_with_renderer(P.dpy, 5, P.drw));
	}

	if ((drm_fd = wlr_renderer_get_drm_fd(P.drw)) >= 0 && P.drw->features.timeline
			&& P.backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(P.dpy, 1, drm_fd);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	if (!(P.alloc = wlr_allocator_autocreate(P.backend, P.drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	P.compositor = wlr_compositor_create(P.dpy, 6, P.drw);
	wlr_subcompositor_create(P.dpy);
	wlr_data_device_manager_create(P.dpy);
	wlr_export_dmabuf_manager_v1_create(P.dpy);
	wlr_screencopy_manager_v1_create(P.dpy);
	wlr_ext_image_copy_capture_manager_v1_create(P.dpy, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(P.dpy, 1);
	wlr_data_control_manager_v1_create(P.dpy);
	wlr_ext_data_control_manager_v1_create(P.dpy, 1);
	wlr_primary_selection_v1_device_manager_create(P.dpy);
	wlr_viewporter_create(P.dpy);
	wlr_single_pixel_buffer_manager_v1_create(P.dpy);
	wlr_fractional_scale_manager_v1_create(P.dpy, 1);
	wlr_presentation_create(P.dpy, P.backend, 2);
	wlr_alpha_modifier_v1_create(P.dpy);

	/* Initializes the interface used to implement urgency hints */
	P.activation = wlr_xdg_activation_v1_create(P.dpy);
	wl_signal_add(&P.activation->events.request_activate, &request_activate);

	wlr_scene_set_gamma_control_manager_v1(P.scene, wlr_gamma_control_manager_v1_create(P.dpy));

	P.power_mgr = wlr_output_power_manager_v1_create(P.dpy);
	wl_signal_add(&P.power_mgr->events.set_mode, &output_power_mgr_set_mode);

	/* Creates an output layout, which is a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	P.output_layout = wlr_output_layout_create(P.dpy);
	wl_signal_add(&P.output_layout->events.change, &layout_change);

    wlr_xdg_output_manager_v1_create(P.dpy, P.output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&P.mons);
	wl_signal_add(&P.backend->events.new_output, &new_output);

	/* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&P.clients);
	wl_list_init(&P.fstack);

	P.transaction_timer = wl_event_loop_add_timer(P.event_loop, transactiontimeout, NULL);
	wl_array_init(&P.ghosts);
	wl_array_init(&P.offmons);

	P.xdg_shell = wlr_xdg_shell_create(P.dpy, 6);
	wl_signal_add(&P.xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	wl_signal_add(&P.xdg_shell->events.new_popup, &new_xdg_popup);

	P.layer_shell = wlr_layer_shell_v1_create(P.dpy, 4);
	wl_signal_add(&P.layer_shell->events.new_surface, &new_layer_surface);

	P.idle_notifier = wlr_idle_notifier_v1_create(P.dpy);

	P.idle_inhibit_mgr = wlr_idle_inhibit_v1_create(P.dpy);
	wl_signal_add(&P.idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

	P.session_lock_mgr = wlr_session_lock_manager_v1_create(P.dpy);
	wl_signal_add(&P.session_lock_mgr->events.new_lock, &new_session_lock);
	P.locked_bg = wlr_scene_rect_create(P.layers[LyrBlock], P.sgeom.width, P.sgeom.height,
			(float [4]){0.1f, 0.1f, 0.1f, 1.0f});
	wlr_scene_node_set_enabled(&P.locked_bg->node, 0);

	P.foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(P.dpy,1);
	P.ext_foreign_toplevel_image_capture_source_manager_v1 =
            wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(P.dpy, 1);
	wl_signal_add(&P.ext_foreign_toplevel_image_capture_source_manager_v1->events.new_request,
			&new_foreign_toplevel_capture_request);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(P.dpy),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	P.xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(P.dpy);
	wl_signal_add(&P.xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

	P.pointer_constraints = wlr_pointer_constraints_v1_create(P.dpy);
	wl_signal_add(&P.pointer_constraints->events.new_constraint, &new_pointer_constraint);

	P.relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(P.dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	P.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(P.cursor, P.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	P.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&P.cursor->events.motion, &cursor_motion);
	wl_signal_add(&P.cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&P.cursor->events.button, &cursor_button);
	wl_signal_add(&P.cursor->events.axis, &cursor_axis);
	wl_signal_add(&P.cursor->events.frame, &cursor_frame);

	P.cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(P.dpy, 1);
	wl_signal_add(&P.cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_signal_add(&P.backend->events.new_input, &new_input_device);
	P.virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(P.dpy);
	wl_signal_add(&P.virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	P.virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(P.dpy);
    wl_signal_add(&P.virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);

	P.seat = wlr_seat_create(P.dpy, "seat0");
	wl_signal_add(&P.seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&P.seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&P.seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&P.seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&P.seat->events.start_drag, &start_drag);

	P.kb_group = createkeyboardgroup();
	wl_list_init(&P.kb_group->destroy.link);

	P.output_mgr = wlr_output_manager_v1_create(P.dpy);
	wl_signal_add(&P.output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&P.output_mgr->events.test, &output_mgr_test);

	P.input_method_manager = wlr_input_method_manager_v2_create(P.dpy);
	P.text_input_manager = wlr_text_input_manager_v3_create(P.dpy);
	P.input_method_relay = input_method_relay_create();

	/* Make sure XWayland clients don't connect to the parent X server,
	 * e.g when running in the x11 backend or the wayland backend and the
	 * compositor has Xwayland support */
	unsetenv("DISPLAY");
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	if ((P.xwayland = wlr_xwayland_create(P.dpy, P.compositor, 1))) {
		wl_signal_add(&P.xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&P.xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", P.xwayland->display_name, 1);
	} else {
		fprintf(stderr, "failed to setup XWayland X server, continuing without it\n");
	}
#endif
}

void
cleanup(void)
{
	char **name;

	cleanuplisteners();
#ifdef XWAYLAND
	wlr_xwayland_destroy(P.xwayland);
	P.xwayland = NULL;
#endif
	wl_display_destroy_clients(P.dpy);
	if (P.child_pid > 0) {
		kill(-P.child_pid, SIGTERM);
		waitpid(P.child_pid, NULL, 0);
	}
	wlr_xcursor_manager_destroy(P.cursor_mgr);

	destroykeyboardgroup(&P.kb_group->destroy, NULL);

	input_method_relay_finish(P.input_method_relay);
	P.input_method_relay = NULL;

	/* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
	 * Destroy it until it's fixed on the wlroots side */
	if (P.backend)
		wlr_backend_destroy(P.backend);

	if (P.transaction_idle)
		wl_event_source_remove(P.transaction_idle);
	wl_event_source_remove(P.transaction_timer);
	wl_array_release(&P.ghosts);
	wl_array_for_each(name, &P.offmons)
		free(*name);
	wl_array_release(&P.offmons);

	wl_display_destroy(P.dpy);
	/* Destroy after the wayland display (when the monitors are already destroyed)
	   to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&P.scene->tree.node);
}

void
cleanuplisteners(void)
{
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_frame.link);
	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&gpu_reset.link);
	wl_list_remove(&new_idle_inhibitor.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&new_input_device.link);
	wl_list_remove(&backend_destroy.link);
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	wl_list_remove(&new_pointer_constraint.link);
	wl_list_remove(&new_output.link);
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_decoration.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&output_mgr_apply.link);
	wl_list_remove(&output_mgr_test.link);
	wl_list_remove(&output_power_mgr_set_mode.link);
	wl_list_remove(&request_activate.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&request_set_psel.link);
	wl_list_remove(&request_set_sel.link);
	wl_list_remove(&request_set_cursor_shape.link);
	wl_list_remove(&request_start_drag.link);
	wl_list_remove(&start_drag.link);
	wl_list_remove(&new_session_lock.link);
	wl_list_remove(&new_foreign_toplevel_capture_request.link);
#ifdef XWAYLAND
	if (P.xwayland) {
		wl_list_remove(&new_xwayland_surface.link);
		wl_list_remove(&xwayland_ready.link);
	}
#endif
}

void
handlesig(int signo)
{
	if (signo == SIGCHLD)
		while (waitpid(-1, NULL, WNOHANG) > 0);
	else if (signo == SIGINT || signo == SIGTERM)
		quit(NULL);
}

void
quit(const Arg *arg)
{
	wl_display_terminate(P.dpy);
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("pluto: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(P.session, arg->ui);
}

void
printstatus(void)
{
	Monitor *m = NULL;
	Client *c;
	uint32_t occ, urg, sel;

	wl_list_for_each(m, &P.mons, link) {
		occ = urg = 0;
		wl_list_for_each(c, &P.clients, link) {
			if (c->mon != m)
				continue;
			occ |= c->tags;
			if (c->isurgent)
				urg |= c->tags;
		}
		if ((c = focustop(m))) {
			printf("%s title %s\n", m->wlr_output->name, client_get_title(c));
			printf("%s appid %s\n", m->wlr_output->name, client_get_appid(c));
			printf("%s fullscreen %d\n", m->wlr_output->name, c->isfullscreen);
			printf("%s floating %d\n", m->wlr_output->name, c->isfloating);
			sel = c->tags;
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s appid \n", m->wlr_output->name);
			printf("%s fullscreen \n", m->wlr_output->name);
			printf("%s floating \n", m->wlr_output->name);
			sel = 0;
		}

		printf("%s selmon %u\n", m->wlr_output->name, m == P.selmon);
		printf("%s tags %"PRIu32" %"PRIu32" %"PRIu32" %"PRIu32"\n",
			m->wlr_output->name, occ, m->tagset[m->seltags], sel, urg);
		printf("%s layout %s\n", m->wlr_output->name, m->ltsymbol);
	}
	fflush(stdout);
}

void
destroybackend(struct wl_listener *listener, void *data)
{
	/* wlroots destroys the backend itself when the session goes away (e.g.
	 * seatd stopped at shutdown), and asserts that its listeners are gone */
	wl_list_remove(&new_output.link);
	wl_list_init(&new_output.link);
	wl_list_remove(&new_input_device.link);
	wl_list_init(&new_input_device.link);
	wl_list_remove(&backend_destroy.link);
	wl_list_init(&backend_destroy.link);
	P.backend = NULL;
	quit(NULL);
}

void
gpureset(struct wl_listener *listener, void *data)
{
	struct wlr_renderer *old_drw = P.drw;
	struct wlr_allocator *old_alloc = P.alloc;
	struct Monitor *m;
	if (!(P.drw = wlr_renderer_autocreate(P.backend)))
		die("couldn't recreate renderer");

	if (!(P.alloc = wlr_allocator_autocreate(P.backend, P.drw)))
		die("couldn't recreate allocator");

	wl_list_remove(&gpu_reset.link);
	wl_signal_add(&P.drw->events.lost, &gpu_reset);

	wlr_compositor_set_renderer(P.compositor, P.drw);

	wl_list_for_each(m, &P.mons, link) {
		wlr_output_init_render(m->wlr_output, P.alloc, P.drw);
	}

	wlr_allocator_destroy(old_alloc);
	wlr_renderer_destroy(old_drw);
}
