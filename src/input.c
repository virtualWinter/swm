// swm - input handling: keyboard events and pointer/cursor interaction
// (focus-under-cursor, MOD+drag move/resize).

#include "swm.h"

#include <linux/input-event-codes.h>

/* ----- keyboard ----- */

/* Base (level-0, unshifted) keysym for a keycode, used so that bindings
 * specified with the unshifted key still match when Shift is held.
 * Wayland keycodes are evdev codes; xkb expects them offset by 8. */
static xkb_keysym_t base_keysym(struct wlr_keyboard *kb, uint32_t keycode) {
	xkb_keycode_t kc = (xkb_keycode_t)keycode + 8;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, kc);
	const xkb_keysym_t *syms;
	int n = xkb_keymap_key_get_syms_by_level(kb->keymap, kc, layout, 0, &syms);
	if (n > 0) return syms[0];
	return XKB_KEY_NoSymbol;
}

void keyboard_key(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_keyboard_key_event *ev = data;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server.seat);
	if (!kb) return;

	/* Match against the base (level-0) keysym so bindings like
	 * SUPER+SHIFT+1 work even though Shift makes the key produce '!'. */
	xkb_keysym_t sym = base_keysym(kb, ev->keycode);
	if (sym == XKB_KEY_NoSymbol) return;

	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		uint32_t mask = clean_mask(server.mods);
		for (size_t i = 0; i < nkeys; i++) {
			if (keys[i].keysym == sym &&
					clean_mask(keys[i].mod) == mask) {
				keys[i].function(keys[i].arg);
				return;
			}
		}
	}
	wlr_seat_keyboard_notify_key(server.seat, ev->time_msec,
		ev->keycode, ev->state);
}

void keyboard_mod(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_keyboard *kb = data;
	wlr_seat_keyboard_notify_modifiers(server.seat, &kb->modifiers);
	server.mods = wlr_keyboard_get_modifiers(kb);
}

void new_input(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_input_device *dev = data;

	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
		struct wlr_keyboard *kb = wlr_keyboard_from_input_device(dev);
		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!km) {
		struct xkb_rule_names rules = { .rules = "evdev", .model = "pc105", .layout = "us" };
		km = xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
		if (km) {
			wlr_keyboard_set_keymap(kb, km);
			xkb_keymap_unref(km);
		}
		wlr_keyboard_set_repeat_info(kb, 25, 600);
		xkb_context_unref(ctx);

		server.keyboard = kb;
		server.keyboard_key.notify = keyboard_key;
		wl_signal_add(&kb->events.key, &server.keyboard_key);
		server.keyboard_mod.notify = keyboard_mod;
		wl_signal_add(&kb->events.modifiers, &server.keyboard_mod);
		wlr_seat_set_keyboard(server.seat, kb);
	} else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
		wlr_cursor_attach_input_device(server.cursor, dev);
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (wlr_seat_get_keyboard(server.seat))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(server.seat, caps);
}

/* ----- pointer ----- */

void cursor_process(uint32_t time) {
	double lx = server.cursor->x, ly = server.cursor->y;

	if (server.drag_active && server.drag_c) {
		client *c = server.drag_c;
		double dx = lx - server.drag_x;
		double dy = ly - server.drag_y;
		if (server.drag_mode == 0) {
			c->x = server.drag_cx + (int)dx;
			c->y = server.drag_cy + (int)dy;
		} else {
			int nw = MAX(1, server.drag_cw + (int)dx);
			int nh = MAX(1, server.drag_ch + (int)dy);
			c->w = nw; c->h = nh;
			wlr_xdg_toplevel_set_size(c->toplevel, nw, nh);
		}
		client_arrange(c);
		return;
	}

	struct wlr_surface *surf = NULL;
	double sx, sy;
	client *c = client_at(lx, ly, &surf, &sx, &sy);
	if (surf) {
		wlr_seat_pointer_notify_enter(server.seat, surf, sx, sy);
		wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
	}
	if (c && c != server.cur) client_focus(c);
}

void cursor_motion(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(server.cursor, NULL, ev->delta_x, ev->delta_y);
	cursor_process(ev->time_msec);
}

void cursor_motion_abs(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(server.cursor, NULL, ev->x, ev->y);
	cursor_process(ev->time_msec);
}

void cursor_button(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_button_event *ev = data;
	wlr_seat_pointer_notify_button(server.seat, ev->time_msec,
		ev->button, ev->state);

	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		server.drag_active = false;
		server.drag_c = NULL;
		return;
	}

	double lx = server.cursor->x, ly = server.cursor->y;
	struct wlr_surface *surf;
	double sx, sy;
	client *c = client_at(lx, ly, &surf, &sx, &sy);
	if (!c) return;

	if (server.mods & config_mod) {
		client_focus(c);
		server.drag_active = true;
		server.drag_c = c;
		server.drag_x = lx; server.drag_y = ly;
		if (ev->button == BTN_LEFT) {
			server.drag_mode = 0;
			server.drag_cx = c->x; server.drag_cy = c->y;
		} else if (ev->button == BTN_RIGHT) {
			server.drag_mode = 1;
			server.drag_cw = c->w; server.drag_ch = c->h;
		} else {
			server.drag_active = false;
		}
	} else {
		client_focus(c);
	}
}

void cursor_axis(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_axis_event *ev = data;
	wlr_seat_pointer_notify_axis(server.seat, ev->time_msec, ev->orientation,
		ev->delta, ev->delta_discrete, ev->source, ev->relative_direction);
}

void cursor_frame(struct wl_listener *l, void *data) {
	(void)l; (void)data;
	wlr_seat_pointer_notify_frame(server.seat);
}

void seat_request_cursor(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	struct wlr_seat_client *focused = server.seat->pointer_state.focused_client;
	if (focused == ev->seat_client)
		wlr_cursor_set_surface(server.cursor, ev->surface,
			ev->hotspot_x, ev->hotspot_y);
}
