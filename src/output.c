// swm - output handling: output lifecycle, frame commits, and the scene
// output layout bridge.

#include "swm.h"

#include <stdlib.h>

void output_destroy_handler(struct wl_listener *l, void *data) {
	(void)data;
	struct swm_output *o = wl_container_of(l, o, destroy);
	/* Detach our frame listener before wlroots finishes the output, otherwise
	 * wlr_output_finish() aborts on the non-empty frame listener list. */
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->destroy.link);
	if (server.last_output == o->output)
		server.last_output = NULL;
	free(o);
}

void output_frame(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_output *output = data;
	struct wlr_scene_output *so = wlr_scene_get_scene_output(
		server.scene, output);
	if (so) wlr_scene_output_commit(so, NULL);
}

void new_output(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_output *output = data;

	wlr_output_init_render(output, server.allocator, server.renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	if (mode) wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(output, &state);

	wlr_output_layout_add_auto(server.output_layout, output);

	struct wlr_scene_output *so = wlr_scene_output_create(server.scene, output);
	wlr_scene_output_layout_add_output(server.scene_layout,
		wlr_output_layout_get(server.output_layout, output), so);

	wlr_xcursor_manager_load(server.cursor_mgr, output->scale);

	struct swm_output *o = calloc(1, sizeof(*o));
	if (!o) return;
	o->output = output;
	o->frame.notify = output_frame;
	wl_signal_add(&output->events.frame, &o->frame);
	o->destroy.notify = output_destroy_handler;
	wl_signal_add(&output->events.destroy, &o->destroy);

	server.last_output = output;
}
