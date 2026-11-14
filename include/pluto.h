/* Pluto: a small, modular wlroots compositor.
 *
 * This is a fork of dwl (GPL-3.0-or-later).  The upstream single-file design
 * has been split into subsystems.  All mutable compositor state lives in one
 * `struct Pluto` instance (`P`), so ownership is explicit and modules do not
 * scatter file-scope globals.
 *
 * Module boundaries:
 *   server/main   process lifecycle, backend and protocol wiring
 *   output        wlr_output / Monitor lifecycle and wlr-output-management
 *   client        xdg-shell and XWayland clients, transactions, configure
 *   seat          keyboard/pointer/cursor, drag-and-drop, constraints
 *   layer         layer-shell surfaces and session lock
 *   layout        pure tiling logic and the dwm-style tag/focus model
 *   protocols     small protocol managers (idle inhibit, capture, ...)
 *   config        compile-time configuration and keybinding lookup
 *   ime           input method / text input bridge
 */
#ifndef PLUTO_H
#define PLUTO_H

/*
 * See LICENSE file for copyright and license details.
 */
#include <getopt.h>
#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_output_swapchain_manager.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif

#include "xdg-shell-protocol.h"
#include "util.h"

/* macros */
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define TAGMASK                 ((1u << TAGCOUNT) - 1)
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
/* wait at most this long for a client to commit its new buffer size */
#define TRANSACTION_TIMEOUT     100

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize }; /* cursor */
enum { XDGShell, LayerShell, X11 }; /* client types */
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrUnmanaged, LyrOverlay,
	LyrIMPopup, LyrBlock, NUM_LAYERS }; /* scene layers */
enum { AxisUp, AxisRight, AxisDown, AxisLeft };

typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int dir;
	void (*func)(const Arg *);
	const Arg arg;
} Axis;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom; /* layout-relative, includes border */
	struct wlr_box prev; /* layout-relative, includes border */
	struct wlr_box current; /* geom currently shown in the scene graph */
	struct wlr_scene_node *snapshot; /* content shown while resizing */
	uint32_t resize_serial; /* nonzero while waiting for a resized buffer */
	uint32_t late_serial; /* one a transaction stopped waiting for */
	struct wlr_box bounds; /* only width and height are used */
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wlr_ext_foreign_toplevel_handle_v1 *foreign_toplevel_handle;
	struct wlr_scene *image_capture_scene;
	struct wlr_ext_image_capture_source_v1 *image_capture_source;
	union {
		struct wlr_scene_tree *image_capture_tree;
		struct wlr_scene_surface *image_capture_scene_surface;
	} capture;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener set_geometry;
	struct wl_listener set_hints;
#endif
	unsigned int bw;
	uint32_t tags;
	int isfloating, isurgent, isfullscreen;
} Client;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	struct wlr_keyboard_group *wlr_group;
	struct wlr_keyboard *virtual_keyboard;

	xkb_keysym_t keysyms[2];
	uint32_t mods;
	struct wl_event_source *key_repeat_source;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
} KeyboardGroup;

typedef struct {
	/* Must keep this field first */
	unsigned int type; /* LayerShell */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
} LayerSurface;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener commit_lock_surface;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m; /* monitor area, layout-relative */
	struct wlr_box w; /* window area, layout-relative */
	struct wl_list layers[4]; /* LayerSurface.link */
	const Layout *lt[2];
	unsigned int seltags;
	unsigned int sellt;
	uint32_t tagset[2];
	float mfact;
	int gamma_lut_changed;
	int nmaster;
	char ltsymbol[16];
	int asleep;
};

typedef struct {
	const char *name;
	float mfact;
	int nmaster;
	float scale;
	const Layout *lt;
	enum wl_output_transform rr;
	int x, y;
} MonitorRule;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	struct wl_listener commit;
	struct wl_listener destroy;
} Popup;

typedef struct {
	const char *id;
	const char *title;
	uint32_t tags;
	int isfloating;
	int monitor;
} Rule;

typedef struct {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;

/* Mutable compositor state.  One instance exists, defined in src/main.c. */
struct Pluto {
	pid_t child_pid;
	int locked;
	Client *focused_client;
	struct wl_display *dpy;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[NUM_LAYERS];
	struct wlr_scene_tree *drag_icon;
	struct wlr_renderer *drw;
	struct wlr_allocator *alloc;
	struct wlr_compositor *compositor;
	struct wlr_session *session;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_activation_v1 *activation;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
	struct wl_list clients;
	struct wl_list fstack;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_output_manager_v1 *output_mgr;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wlr_output_power_manager_v1 *power_mgr;
	struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *ext_foreign_toplevel_image_capture_source_manager_v1;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_scene_rect *root_bg;
	struct wlr_session_lock_manager_v1 *session_lock_mgr;
	struct wlr_scene_rect *locked_bg;
	struct wlr_session_lock_v1 *cur_lock;
	struct wlr_seat *seat;
	KeyboardGroup *kb_group;
	unsigned int cursor_mode;
	Client *grabc;
	int grabcx;
	int grabcy;
	struct wlr_output_layout *output_layout;
	struct wlr_box sgeom;
	struct wl_list mons;
	Monitor *selmon;
	int consumed[KEY_MAX + 1];
	struct wl_event_source *transaction_idle;
	struct wl_event_source *transaction_timer;
	int transaction_waiting;
	struct wl_array ghosts;
	struct wl_array offmons;
	struct wlr_xwayland *xwayland;
	struct wlr_input_method_manager_v2 *input_method_manager;
	struct wlr_text_input_manager_v3 *text_input_manager;
	struct input_method_relay *input_method_relay;
};

extern struct Pluto P;

/* global event handlers (stateless listener objects, defined in listeners.c) */
extern struct wl_listener backend_destroy;
extern struct wl_listener cursor_axis;
extern struct wl_listener cursor_button;
extern struct wl_listener cursor_frame;
extern struct wl_listener cursor_motion;
extern struct wl_listener cursor_motion_absolute;
extern struct wl_listener gpu_reset;
extern struct wl_listener layout_change;
extern struct wl_listener new_idle_inhibitor;
extern struct wl_listener new_input_device;
extern struct wl_listener new_virtual_keyboard;
extern struct wl_listener new_virtual_pointer;
extern struct wl_listener new_pointer_constraint;
extern struct wl_listener new_output;
extern struct wl_listener new_xdg_toplevel;
extern struct wl_listener new_xdg_popup;
extern struct wl_listener new_xdg_decoration;
extern struct wl_listener new_layer_surface;
extern struct wl_listener output_mgr_apply;
extern struct wl_listener output_mgr_test;
extern struct wl_listener output_power_mgr_set_mode;
extern struct wl_listener request_activate;
extern struct wl_listener request_cursor;
extern struct wl_listener request_set_psel;
extern struct wl_listener request_set_sel;
extern struct wl_listener request_set_cursor_shape;
extern struct wl_listener request_start_drag;
extern struct wl_listener start_drag;
extern struct wl_listener drag_destroy;
extern struct wl_listener new_session_lock;
extern struct wl_listener new_foreign_toplevel_capture_request;
#ifdef XWAYLAND
extern struct wl_listener new_xwayland_surface;
extern struct wl_listener xwayland_ready;
#endif

/* exported layout tables */
extern const int layermap[];

/* subsystem entry points */
void applybounds(Client *c, struct wlr_box *bbox);
void applygeom(Client *c);
void applyrules(Client *c);
void arrange(Monitor *m);
void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
void arrangelayers(Monitor *m);
void axisnotify(struct wl_listener *listener, void *data);
void buttonpress(struct wl_listener *listener, void *data);
void capturerequest(struct wl_listener *listener, void *data);
void chvt(const Arg *arg);
void checkidleinhibitor(struct wlr_surface *exclude);
void cleanup(void);
void cleanupmon(struct wl_listener *listener, void *data);
void cleanuplisteners(void);
void closemon(Monitor *m);
void commitlayersurfacenotify(struct wl_listener *listener, void *data);
void commitlocksurface(struct wl_listener *listener, void *data);
void commitnotify(struct wl_listener *listener, void *data);
void commitpopup(struct wl_listener *listener, void *data);
void createdecoration(struct wl_listener *listener, void *data);
void createidleinhibitor(struct wl_listener *listener, void *data);
void createkeyboard(struct wlr_keyboard *keyboard);
KeyboardGroup *createkeyboardgroup(void);
void createlayersurface(struct wl_listener *listener, void *data);
void createlocksurface(struct wl_listener *listener, void *data);
void createmon(struct wl_listener *listener, void *data);
void createnotify(struct wl_listener *listener, void *data);
void createpointer(struct wlr_pointer *pointer);
void createpointerconstraint(struct wl_listener *listener, void *data);
void createpopup(struct wl_listener *listener, void *data);
void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
void cursorframe(struct wl_listener *listener, void *data);
void cursorwarptohint(void);
void destroybackend(struct wl_listener *listener, void *data);
void destroydecoration(struct wl_listener *listener, void *data);
void destroydrag(struct wl_listener *listener, void *data);
void destroyidleinhibitor(struct wl_listener *listener, void *data);
void destroylayersurfacenotify(struct wl_listener *listener, void *data);
void destroylock(SessionLock *lock, int unlocked);
void destroylocksurface(struct wl_listener *listener, void *data);
void destroynotify(struct wl_listener *listener, void *data);
void destroypointerconstraint(struct wl_listener *listener, void *data);
void destroypopup(struct wl_listener *listener, void *data);
void destroysessionlock(struct wl_listener *listener, void *data);
void destroykeyboardgroup(struct wl_listener *listener, void *data);
Monitor *dirtomon(enum wlr_direction dir);
void focusclient(Client *c, LayerSurface *l, int lift);
void focusmon(const Arg *arg);
void focusstack(const Arg *arg);
Client *focustop(Monitor *m);
void forgetoffmons(void);
void fullscreennotify(struct wl_listener *listener, void *data);
void gpureset(struct wl_listener *listener, void *data);
void handlesig(int signo);
void incnmaster(const Arg *arg);
void inputdevice(struct wl_listener *listener, void *data);
const Key *keybinding(uint32_t mods, xkb_keysym_t sym);
void keypress(struct wl_listener *listener, void *data);
void keypressmod(struct wl_listener *listener, void *data);
int keyrepeat(void *data);
void killclient(const Arg *arg);
void locksession(struct wl_listener *listener, void *data);
void mapnotify(struct wl_listener *listener, void *data);
void maximizenotify(struct wl_listener *listener, void *data);
void monocle(Monitor *m);
void motionabsolute(struct wl_listener *listener, void *data);
char **monoff(struct wlr_output *wlr_output);
void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
void motionrelative(struct wl_listener *listener, void *data);
void moveresize(const Arg *arg);
void outputmgrapply(struct wl_listener *listener, void *data);
void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
void outputmgrtest(struct wl_listener *listener, void *data);
void pointerfocus(Client *c, LayerSurface* l, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
void printstatus(void);
void powermgrsetmode(struct wl_listener *listener, void *data);
void quit(const Arg *arg);
void refocuspointer(void *data);
void rendermon(struct wl_listener *listener, void *data);
void requestdecorationmode(struct wl_listener *listener, void *data);
void requeststartdrag(struct wl_listener *listener, void *data);
void requestmonstate(struct wl_listener *listener, void *data);
void resize(Client *c, struct wlr_box geo, int interact);
void run(char *startup_cmd);
void setcursor(struct wl_listener *listener, void *data);
void setcursorshape(struct wl_listener *listener, void *data);
void setfloating(Client *c, int floating);
void setfullscreen(Client *c, int fullscreen);
void setlayout(const Arg *arg);
void setmfact(const Arg *arg);
void setmon(Client *c, Monitor *m, uint32_t newtags);
void setmonoff(struct wlr_output *wlr_output, int off);
void setpsel(struct wl_listener *listener, void *data);
void setsel(struct wl_listener *listener, void *data);
void setup(void);
struct wlr_scene_node *snapshot(struct wlr_scene_node *node,
		struct wlr_scene_tree *parent, void *data);
void spawn(const Arg *arg);
void startdrag(struct wl_listener *listener, void *data);
void tag(const Arg *arg);
void tagmon(const Arg *arg);
void tile(Monitor *m);
void togglefloating(const Arg *arg);
void togglefullscreen(const Arg *arg);
void toggletag(const Arg *arg);
void toggleview(const Arg *arg);
void transactionapply(void);
void transactioncheck(void *data);
void transactioncommit(Client *c, int ready);
void transactionschedule(void);
int transactiontimeout(void *data);
void unlocksession(struct wl_listener *listener, void *data);
void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
void unmapnotify(struct wl_listener *listener, void *data);
void updatemons(struct wl_listener *listener, void *data);
void updatetitle(struct wl_listener *listener, void *data);
void urgent(struct wl_listener *listener, void *data);
void view(const Arg *arg);
void virtualkeyboard(struct wl_listener *listener, void *data);
void virtualpointer(struct wl_listener *listener, void *data);
Monitor *xytomon(double x, double y);
void xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);
void zoom(const Arg *arg);

#ifdef XWAYLAND
void activatex11(struct wl_listener *listener, void *data);
void associatex11(struct wl_listener *listener, void *data);
void commitx11(struct wl_listener *listener, void *data);
void configurex11(struct wl_listener *listener, void *data);
void createnotifyx11(struct wl_listener *listener, void *data);
void dissociatex11(struct wl_listener *listener, void *data);
void setgeometryx11(struct wl_listener *listener, void *data);
void sethints(struct wl_listener *listener, void *data);
void xwaylandready(struct wl_listener *listener, void *data);
#endif

/* Compile-time configuration and client helpers.  config.h needs the layout
 * function declarations above; client.h/ime.h need the core types. */
/* config.h declares every knob as a file-scope static, so any given module
 * uses only a subset of them.  Silence the expected unused-variable warnings
 * from GCC without weakening the warning for module code. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "config.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "client.h"
#include "ime.h"

#endif /* PLUTO_H */
