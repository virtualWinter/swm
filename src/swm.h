#ifndef SWM_H
#define SWM_H

/* Shared declarations for swm (a wlroots port of sowm).
 * Types, the global server state, and function prototypes used across the
 * modules (client.c, input.c, output.c, main.c, config.c). */

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/box.h>
#include <stddef.h>

#define NUM_WS 6
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
	const char **com;
	int i;
} Arg;

typedef struct client client;
typedef struct key key;

struct client {
	struct wl_list link;                 // server.ws_clients[ws]
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_scene_tree *scene_tree;   // surface (+subsurface) node
	struct wlr_scene_rect *border;       // focus indicator
	int x, y, w, h;
	int saved_x, saved_y, saved_w, saved_h;
	bool fs;
	bool mapped;
	int ws;
	struct wl_listener map, unmap, destroy;
	struct wl_listener request_fullscreen, request_maximize;
};

struct key {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*function)(const Arg arg);
	Arg arg;
};

struct swm_output {
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wlr_output *output;
};

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_tree *window_tree;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output *last_output;
	struct wlr_keyboard *keyboard;

	struct wl_list ws_clients[NUM_WS];
	int ws;
	client *cur;
	uint32_t mods;

	/* interactive move/resize */
	bool drag_active;
	int drag_mode;          // 0 = move, 1 = resize
	client *drag_c;
	double drag_x, drag_y;
	int drag_cx, drag_cy, drag_cw, drag_ch;

	int nclients;

	struct wl_listener new_output, new_input, new_xdg_toplevel;
	struct wl_listener cursor_motion, cursor_motion_abs, cursor_button;
	struct wl_listener cursor_axis, cursor_frame, request_cursor;
	struct wl_listener keyboard_key, keyboard_mod;
};

/* Global state. */
extern struct server server;
extern float normal_rgba[4], focus_rgba[4];

/* Runtime config (loaded from the XDG config file by load_config()). */
extern int border_width;
extern int gap_size;
extern uint32_t config_mod;

/* Keybinding table, built by load_config() in config.c. */
extern key *keys;
extern size_t nkeys;

/* config.c */
void load_config(void);

/* main.c */
void hex_to_rgba(const char *hex, float *c);
uint32_t clean_mask(uint32_t m);
void run(const Arg arg);

/* client.c */
void client_arrange(client *c);
struct wlr_output *output_for_client(client *c);
client *client_at(double lx, double ly,
		struct wlr_surface **surf, double *sx, double *sy);
void client_focus(client *c);
void client_set_fullscreen(client *c, bool fs);
void win_fs(const Arg arg);
void win_center(const Arg arg);
void win_kill(const Arg arg);
void win_next(const Arg arg);
void win_prev(const Arg arg);
void ws_go(const Arg arg);
void win_to_ws(const Arg arg);
void client_map(struct wl_listener *l, void *data);
void client_unmap(struct wl_listener *l, void *data);
void client_destroy(struct wl_listener *l, void *data);
void toplevel_request_fullscreen(struct wl_listener *l, void *data);
void toplevel_request_maximize(struct wl_listener *l, void *data);
void xdg_toplevel_new(struct wl_listener *l, void *data);

/* input.c */
void keyboard_key(struct wl_listener *l, void *data);
void keyboard_mod(struct wl_listener *l, void *data);
void new_input(struct wl_listener *l, void *data);
void cursor_process(uint32_t time);
void cursor_motion(struct wl_listener *l, void *data);
void cursor_motion_abs(struct wl_listener *l, void *data);
void cursor_button(struct wl_listener *l, void *data);
void cursor_axis(struct wl_listener *l, void *data);
void cursor_frame(struct wl_listener *l, void *data);
void seat_request_cursor(struct wl_listener *l, void *data);

/* output.c */
void output_destroy_handler(struct wl_listener *l, void *data);
void output_frame(struct wl_listener *l, void *data);
void new_output(struct wl_listener *l, void *data);

#endif
