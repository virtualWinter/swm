// swm - a port of sowm (Simple/Shitty Opinionated Window Manager) to wlroots.
// Floating-only Wayland compositor: focus-under-cursor, MOD+drag move/resize,
// fullscreen toggle, center, kill, 6 workspaces, alt-tab cycling, key-launched
// programs. Logic mirrors virtualWinter/sowm.
//
// Modules:
//   config.h  (removed) — config is now loaded at runtime, see config.c
//   config.c  runtime config loader/parser (XDG config file)
//   swm.h     shared types and prototypes
//   client.c  client lifecycle, focus, window ops, workspaces, xdg toplevel
//   input.c   keyboard and pointer/cursor handling
//   output.c  output lifecycle and frame commits
//   main.c    globals, helpers, and compositor setup/teardown

#include "swm.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

/* Global state. */
struct server server;
float normal_rgba[4], focus_rgba[4];

/* ----- helpers ----- */

void hex_to_rgba(const char *hex, float *c) {
	unsigned int r = 0, g = 0, b = 0;
	sscanf(hex, "#%02x%02x%02x", &r, &g, &b);
	c[0] = r / 255.0f; c[1] = g / 255.0f; c[2] = b / 255.0f; c[3] = 1.0f;
}

uint32_t clean_mask(uint32_t m) {
	return m & ~(WLR_MODIFIER_CAPS) &
		(WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
		 WLR_MODIFIER_MOD2 | WLR_MODIFIER_MOD3 | WLR_MODIFIER_LOGO |
		 WLR_MODIFIER_MOD5);
}

/* ----- spawning programs ----- */

void run(const Arg arg) {
	if (fork() == 0) {
		setsid();
		execvp(arg.com[0], (char **)arg.com);
		fprintf(stderr, "swm: exec %s failed\n", arg.com[0]);
		exit(1);
	}
}

/* ----- main ----- */

int main(int argc, char *argv[]) {
	(void)argc; (void)argv;

	signal(SIGCHLD, SIG_IGN);

	load_config();

	server.display = wl_display_create();
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), NULL);
	if (!server.backend) {
		fprintf(stderr, "swm: failed to create backend\n");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

	server.scene = wlr_scene_create();
	server.window_tree = wlr_scene_tree_create(&server.scene->tree);
	server.output_layout = wlr_output_layout_create(server.display);
	server.scene_layout = wlr_scene_attach_output_layout(
		server.scene, server.output_layout);

	server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
	wlr_compositor_create(server.display, 1, server.renderer);

	server.seat = wlr_seat_create(server.display, "seat0");
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create("left_ptr", 24);
	if (server.cursor_mgr) {
		wlr_xcursor_manager_load(server.cursor_mgr, 1);
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "left_ptr");
	}

	for (int i = 0; i < NUM_WS; i++)
		wl_list_init(&server.ws_clients[i]);
	server.ws = 0;

	server.new_output.notify = new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.new_input.notify = new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.new_xdg_toplevel.notify = xdg_toplevel_new;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

	server.cursor_motion.notify = cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_abs.notify = cursor_motion_abs;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_abs);
	server.cursor_button.notify = cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);

	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		fprintf(stderr, "swm: failed to create wayland socket\n");
		return 1;
	}
	printf("swm: running on WAYLAND_DISPLAY=%s\n", socket);

	if (!wlr_backend_start(server.backend)) {
		fprintf(stderr, "swm: failed to start backend\n");
		return 1;
	}

	wl_display_run(server.display);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wl_display_destroy(server.display);
	return 0;
}
