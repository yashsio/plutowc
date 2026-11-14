#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>

struct input_method_relay {
	struct wl_list text_inputs; /* struct text_input.link */
	struct wlr_input_method_v2 *input_method;

	struct wlr_surface *focused_surface;
	struct text_input *active_text_input;

	struct wl_list popups; /* input_method_popup.link */
	struct wlr_scene_tree *popup_tree;

	struct wl_listener new_text_input;
	struct wl_listener new_input_method;

	struct wl_listener input_method_commit;
	struct wl_listener input_method_grab_keyboard;
	struct wl_listener input_method_destroy;
	struct wl_listener input_method_new_popup_surface;

	struct wl_listener keyboard_grab_destroy;
	struct wl_listener focused_surface_destroy;
};

struct input_method_popup {
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *scene_surface;
	struct input_method_relay *relay;
	struct wl_list link; /* input_method_relay.popups */

	struct wl_listener destroy;
	struct wl_listener commit;
};

struct text_input {
	struct input_method_relay *relay;
	struct wlr_text_input_v3 *input;
	struct wl_list link;

	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
};

/* functions used by the compositor core */
int input_method_keyboard_grab_forward_key(
		KeyboardGroup *keyboard, struct wlr_keyboard_key_event *event);
int input_method_keyboard_grab_forward_modifiers(KeyboardGroup *keyboard);
struct input_method_relay *input_method_relay_create(void);
void input_method_relay_finish(struct input_method_relay *relay);
void input_method_relay_set_focus(struct input_method_relay *relay,
		struct wlr_surface *surface);

/* internal functions */
struct text_input *get_active_text_input(struct input_method_relay *relay);
struct wlr_input_method_keyboard_grab_v2 *get_keyboard_grab(KeyboardGroup *keyboard);
int keyboard_is_emulated_by_input_method(struct wlr_keyboard *keyboard,
		struct wlr_input_method_v2 *input_method);
void handle_focused_surface_destroy(struct wl_listener *listener, void *data);
void handle_input_method_commit(struct wl_listener *listener, void *data);
void handle_input_method_destroy(struct wl_listener *listener, void *data);
void handle_input_method_grab_keyboard(struct wl_listener *listener, void *data);
void handle_input_method_new_popup_surface(struct wl_listener *listener, void *data);
void handle_keyboard_grab_destroy(struct wl_listener *listener, void *data);
void handle_new_input_method(struct wl_listener *listener, void *data);
void handle_new_text_input(struct wl_listener *listener, void *data);
void handle_popup_surface_commit(struct wl_listener *listener, void *data);
void handle_popup_surface_destroy(struct wl_listener *listener, void *data);
void handle_text_input_commit(struct wl_listener *listener, void *data);
void handle_text_input_destroy(struct wl_listener *listener, void *data);
void handle_text_input_disable(struct wl_listener *listener, void *data);
void handle_text_input_enable(struct wl_listener *listener, void *data);
Monitor *output_from_wlr_output(struct wlr_output *wlr_output);
Monitor *output_nearest_to(int lx, int ly);
void send_state_to_input_method(struct input_method_relay *relay);
void update_active_text_input(struct input_method_relay *relay);
void update_popup_position(struct input_method_popup *popup);
void update_popups_position(struct input_method_relay *relay);
void update_text_inputs_focused_surface(struct input_method_relay *relay);
