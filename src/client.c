// swm - client management: focus, fullscreen/center/kill, workspaces, and
// xdg_toplevel lifecycle. Logic mirrors virtualWinter/sowm.

#include "swm.h"

#include <stdlib.h>

/* ----- helpers ----- */

void client_arrange(client *c) {
	int bw = c->fs ? 0 : border_width;
	wlr_scene_node_set_position(&c->border->node, c->x, c->y);
	wlr_scene_rect_set_size(c->border, c->w + 2 * bw, c->h + 2 * bw);
	wlr_scene_node_set_position(&c->scene_tree->node, c->x + bw, c->y + bw);
}

struct wlr_output *output_for_client(client *c) {
	struct wlr_output *o = wlr_output_layout_output_at(server.output_layout,
		c->x + c->w / 2, c->y + c->h / 2);
	if (!o) o = server.last_output;
	return o;
}

/* Return the client under layout coords (lx,ly), plus the surface and
 * surface-local coordinates if a client surface is hit. */
client *client_at(double lx, double ly,
		struct wlr_surface **surf, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server.scene->tree.node, lx, ly, sx, sy);
	*surf = NULL;
	if (!node) return NULL;

	struct wlr_scene_node *n = node;
	while (n && !n->data) n = (struct wlr_scene_node *)n->parent;
	if (!n) return NULL;
	client *c = n->data;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss =
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (ss) *surf = ss->surface;
	}
	return c;
}

/* ----- focus ----- */

void client_focus(client *c) {
	if (server.cur && server.cur != c)
		wlr_scene_rect_set_color(server.cur->border, normal_rgba);
	server.cur = c;
	if (!c) {
		wlr_seat_keyboard_clear_focus(server.seat);
		return;
	}
	wlr_scene_rect_set_color(c->border, focus_rgba);
	wlr_scene_node_raise_to_top(&c->border->node);
	wlr_scene_node_raise_to_top(&c->scene_tree->node);
	wlr_xdg_toplevel_set_activated(c->toplevel, true);

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server.seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(server.seat, c->toplevel->base->surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

/* ----- fullscreen / center / kill ----- */

void client_set_fullscreen(client *c, bool fs) {
	if (c->fs == fs) return;
	c->fs = fs;
	if (fs) {
		struct wlr_box ob;
		struct wlr_output *o = output_for_client(c);
		wlr_output_layout_get_box(server.output_layout, o, &ob);
		c->saved_x = c->x; c->saved_y = c->y;
		c->saved_w = c->w; c->saved_h = c->h;
		c->x = ob.x;
		c->y = ob.y + gap_size;
		c->w = ob.width;
		c->h = ob.height - gap_size;
	} else {
		c->x = c->saved_x; c->y = c->saved_y;
		c->w = c->saved_w; c->h = c->saved_h;
	}
	wlr_xdg_toplevel_set_fullscreen(c->toplevel, fs);
	client_arrange(c);
	client_focus(c);
}

void win_fs(const Arg arg) {
	(void)arg;
	if (server.cur) client_set_fullscreen(server.cur, !server.cur->fs);
}

void win_center(const Arg arg) {
	(void)arg;
	if (!server.cur) return;
	client *c = server.cur;
	struct wlr_box ob;
	struct wlr_output *o = output_for_client(c);
	wlr_output_layout_get_box(server.output_layout, o, &ob);
	c->x = ob.x + (ob.width - c->w) / 2;
	c->y = ob.y + (ob.height - c->h) / 2;
	client_arrange(c);
}

void win_kill(const Arg arg) {
	(void)arg;
	if (server.cur) wlr_xdg_toplevel_send_close(server.cur->toplevel);
}

void win_next(const Arg arg) {
	(void)arg;
	if (!server.cur) return;
	client *n = wl_container_of(server.cur->link.next, (struct client *)0, link);
	if (n == server.cur) return;
	client_focus(n);
}

void win_prev(const Arg arg) {
	(void)arg;
	if (!server.cur) return;
	client *n = wl_container_of(server.cur->link.prev, (struct client *)0, link);
	if (n == server.cur) return;
	client_focus(n);
}

/* ----- workspaces ----- */

void ws_go(const Arg arg) {
	int target = arg.i - 1;
	if (target < 0 || target >= NUM_WS || target == server.ws) return;

	client *c;
	wl_list_for_each(c, &server.ws_clients[server.ws], link) {
		wlr_scene_node_set_enabled(&c->scene_tree->node, false);
		wlr_scene_node_set_enabled(&c->border->node, false);
		wlr_xdg_toplevel_set_activated(c->toplevel, false);
	}
	server.ws = target;
	server.cur = NULL;

	client *first = NULL;
	wl_list_for_each(c, &server.ws_clients[server.ws], link) {
		wlr_scene_node_set_enabled(&c->scene_tree->node, true);
		wlr_scene_node_set_enabled(&c->border->node, true);
		wlr_xdg_toplevel_set_activated(c->toplevel, false);
		if (!first) first = c;
	}
	if (first) client_focus(first);
	else wlr_seat_keyboard_clear_focus(server.seat);
}

void win_to_ws(const Arg arg) {
	int target = arg.i - 1;
	if (!server.cur || target < 0 || target >= NUM_WS ||
			target == server.cur->ws)
		return;
	client *c = server.cur;
	wl_list_remove(&c->link);
	c->ws = target;
	wl_list_insert(server.ws_clients[target].prev, &c->link);
	wlr_scene_node_set_enabled(&c->scene_tree->node, false);
	wlr_scene_node_set_enabled(&c->border->node, false);
	server.cur = NULL;
	if (!wl_list_empty(&server.ws_clients[server.ws]))
		client_focus(wl_container_of(server.ws_clients[server.ws].next,
			(struct client *)0, link));
	else
		wlr_seat_keyboard_clear_focus(server.seat);
}

/* ----- xdg toplevel lifecycle ----- */

void client_map(struct wl_listener *l, void *data) {
	(void)data;
	client *c = wl_container_of(l, c, map);
	struct wlr_box *g = &c->toplevel->base->geometry;
	if (c->w <= 0) c->w = g->width > 0 ? g->width : 640;
	if (c->h <= 0) c->h = g->height > 0 ? g->height : 480;
	c->mapped = true;
	wl_list_insert(server.ws_clients[c->ws].prev, &c->link);
	wlr_scene_node_set_enabled(&c->scene_tree->node, true);
	wlr_scene_node_set_enabled(&c->border->node, true);
	client_arrange(c);
	client_focus(c);
}

void client_unmap(struct wl_listener *l, void *data) {
	(void)data;
	client *c = wl_container_of(l, c, unmap);
	c->mapped = false;
	wl_list_remove(&c->link);
	if (server.cur == c) server.cur = NULL;
	wlr_scene_node_set_enabled(&c->scene_tree->node, false);
	wlr_scene_node_set_enabled(&c->border->node, false);
	if (!server.cur && !wl_list_empty(&server.ws_clients[server.ws]))
		client_focus(wl_container_of(server.ws_clients[server.ws].next,
			(struct client *)0, link));
	else if (!server.cur)
		wlr_seat_keyboard_clear_focus(server.seat);
}

void client_destroy(struct wl_listener *l, void *data) {
	(void)data;
	client *c = wl_container_of(l, c, destroy);
	if (c->mapped) {
		wl_list_remove(&c->link);
		c->mapped = false;
	}
	wl_list_remove(&c->request_fullscreen.link);
	wl_list_remove(&c->request_maximize.link);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->destroy.link);
	wlr_scene_node_destroy(&c->scene_tree->node);
	wlr_scene_node_destroy(&c->border->node);
	free(c);
}

void toplevel_request_fullscreen(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_xdg_toplevel *t = data;
	wlr_xdg_toplevel_set_fullscreen(t, t->requested.fullscreen);
	client *c = t->base->data;
	if (c) client_set_fullscreen(c, t->requested.fullscreen);
}

void toplevel_request_maximize(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_xdg_toplevel *t = data;
	wlr_xdg_toplevel_set_maximized(t, t->requested.maximized);
}

void xdg_toplevel_new(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_xdg_toplevel *toplevel = data;
	client *c = calloc(1, sizeof(*c));
	if (!c) return;

	c->toplevel = toplevel;
	c->ws = server.ws;
	int k = server.nclients++ % 6;
	c->x = 40 + k * 24;
	c->y = 40 + k * 24;
	c->w = 0; c->h = 0;

	c->border = wlr_scene_rect_create(server.window_tree, 1, 1, normal_rgba);
	c->border->node.data = c;
	c->scene_tree = wlr_scene_xdg_surface_create(server.window_tree,
		toplevel->base);
	c->scene_tree->node.data = c;
	toplevel->base->data = c;
	client_arrange(c);

	/* Do NOT send an initial configure here: the xdg_surface is not yet
	 * initialized (the client hasn't committed its role). wlroots sends the
	 * required initial configure once the surface is committed. */
	c->map.notify = client_map;
	wl_signal_add(&toplevel->base->surface->events.map, &c->map);
	c->unmap.notify = client_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &c->unmap);
	c->destroy.notify = client_destroy;
	wl_signal_add(&toplevel->events.destroy, &c->destroy);
	c->request_fullscreen.notify = toplevel_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &c->request_fullscreen);
	c->request_maximize.notify = toplevel_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &c->request_maximize);
}
