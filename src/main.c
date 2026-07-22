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
#include <sys/inotify.h>
#include <limits.h>
#include <libgen.h>
#include <string.h>

#ifndef TESTING
/* Signal-safe event-loop callback for config reload (SIGUSR1). */
static int on_reload_signal(int signum, void *data) {
	(void)signum; (void)data;
	fprintf(stderr, "swm: reload triggered (SIGUSR1)\n");
	reload_all();
	return 1;
}

/* Event-loop callback — fires when the config file changes on disk. */
static int on_config_modified(int fd, uint32_t mask, void *data) {
	(void)data;
	if (!(mask & WL_EVENT_READABLE)) return 1;
	char buf[4096];
	struct inotify_event *ev;
	ssize_t n = read(fd, buf, sizeof buf);
	if (n <= 0) return 1;
	for (char *p = buf; p < buf + n; p += sizeof *ev + ev->len) {
		ev = (struct inotify_event *)p;
		if ((ev->mask & IN_CLOSE_WRITE) && !strcmp(ev->name, "swm.conf")) {
			fprintf(stderr, "swm: config file changed, reloading\n");
			reload_settings();
			break;
		}
	}
	return 1;
}
#endif

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
	return m & ~(uint32_t)WLR_MODIFIER_CAPS &
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

#ifndef TESTING
int main(int argc, char *argv[]) {
	/* Parse flags before anything else. */
	int nested_mode = 0;
	const char *socket_name = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--nested") || !strcmp(argv[i], "-n")) {
			nested_mode = 1;
		} else if (!strcmp(argv[i], "--socket") || !strcmp(argv[i], "-s")) {
			if (i + 1 < argc) socket_name = argv[++i];
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			printf("Usage: swm [--nested|-n] [--socket|-s NAME]\n");
			printf("  --nested / -n    Run as a nested compositor (Wayland backend)\n");
			printf("  --socket / -s    Set the Wayland display socket name\n");
			return 0;
		}
	}

	signal(SIGCHLD, SIG_IGN);

	if (nested_mode) {
		setenv("WLR_BACKEND", "wayland", 1);
		/* Let wlroots auto-detect the best renderer for the Wayland backend. */
		if (!getenv("WLR_WL_OUTPUTS"))
			setenv("WLR_WL_OUTPUTS", "1", 0);
	}

	load_config();

	server.display = wl_display_create();

#ifndef TESTING
	/* Register SIGUSR1 handler for runtime config reload. */
	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	wl_event_loop_add_signal(loop, SIGUSR1, on_reload_signal, NULL);

	/* Write PID file so srun (or any tool) can signal us.
	 * Use XDG_RUNTIME_DIR (user-owned, not world-writable) to
	 * prevent symlink attacks on /tmp/. */
	const char *rt_dir = getenv("XDG_RUNTIME_DIR");
	if (rt_dir && *rt_dir) {
		char pid_path[PATH_MAX];
		snprintf(pid_path, sizeof pid_path, "%s/swm.pid", rt_dir);
		FILE *pf = fopen(pid_path, "w");
		if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
	} else {
		fprintf(stderr, "swm: XDG_RUNTIME_DIR not set, skipping PID file\n");
	}

	/* Watch the config directory for changes with inotify. */
	char *cfg_path = config_file_path();
	if (cfg_path) {
		char cfg_dir[PATH_MAX];
		snprintf(cfg_dir, sizeof cfg_dir, "%s", cfg_path);
		char *dn = dirname(cfg_dir);
		int inot_fd = inotify_init1(IN_CLOEXEC);
		if (inot_fd >= 0) {
			if (inotify_add_watch(inot_fd, dn, IN_CLOSE_WRITE) >= 0)
				wl_event_loop_add_fd(loop, inot_fd, WL_EVENT_READABLE,
					on_config_modified, NULL);
			else
				close(inot_fd);
		}
		free(cfg_path);
	}
#endif

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
	wlr_compositor_create(server.display, 5, server.renderer);

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

	wallpaper_init();

	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER);

	/* Save the parent display before we override WAYLAND_DISPLAY. */
	const char *parent_display = getenv("WAYLAND_DISPLAY");

	const char *socket;
	if (socket_name) {
		if (wl_display_add_socket(server.display, socket_name) < 0) {
			fprintf(stderr, "swm: failed to create socket %s\n", socket_name);
			return 1;
		}
		socket = socket_name;
	} else {
		/* Temporarily clear WAYLAND_DISPLAY so auto-pick doesn't try the
		 * parent's name. wl_display_add_socket_auto will setenv the new name. */
		unsetenv("WAYLAND_DISPLAY");
		socket = wl_display_add_socket_auto(server.display);
		if (!socket) {
			fprintf(stderr, "swm: failed to create wayland socket\n");
			return 1;
		}
	}
	/* Publish our socket name so child processes connect to us.
	 * (wl_display_add_socket_auto already did setenv, but for the explicit
	 * socket_name path we do it here.) */
	if (socket_name)
		setenv("WAYLAND_DISPLAY", socket_name, 1);

	printf("swm: running on WAYLAND_DISPLAY=%s%s%s%s\n",
		socket,
		nested_mode && parent_display ? " (parent: " : "",
		nested_mode && parent_display ? parent_display : "",
		nested_mode && parent_display ? ")" : "");

	if (!wlr_backend_start(server.backend)) {
		fprintf(stderr, "swm: failed to start backend\n");
		return 1;
	}

	wl_display_run(server.display);

#ifndef TESTING
	/* Clean up PID file. */
	{
		const char *rt_dir = getenv("XDG_RUNTIME_DIR");
		if (rt_dir && *rt_dir) {
			char pid_path[PATH_MAX];
			snprintf(pid_path, sizeof pid_path, "%s/swm.pid", rt_dir);
			unlink(pid_path);
		}
	}
#endif

	wallpaper_finish();

	wlr_scene_node_destroy(&server.scene->tree.node);
	wl_display_destroy(server.display);
	return 0;
}
#endif
