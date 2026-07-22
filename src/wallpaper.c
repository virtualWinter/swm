// swm - wallpaper support.
//
// A configurable background image rendered behind all client windows.
// Uses a custom wlr_buffer wrapper around stb_image-decoded pixel data
// and per-output wlr_scene_buffer nodes so each output displays the
// image centred at its native size. If no wallpaper is available
// (empty directory, missing file, etc.) nothing is rendered and a
// warning is logged.

#include "swm.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ----- custom wlr_buffer backed by raw pixel data ----- */

struct swm_wall_buf {
	struct wlr_buffer base;
	unsigned char *pixels;
	int width, height;
	uint32_t format;
	int stride;
};

static bool wall_buf_begin_data_ptr_access(struct wlr_buffer *buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	(void)flags;
	struct swm_wall_buf *b = (struct swm_wall_buf *)buffer;
	*data = b->pixels;
	*format = b->format;
	*stride = (size_t)b->stride;
	return true;
}

static void wall_buf_end_data_ptr_access(struct wlr_buffer *buffer) {
	(void)buffer;
}

static bool wall_buf_get_dmabuf(struct wlr_buffer *buffer,
		struct wlr_dmabuf_attributes *attribs) {
	(void)buffer; (void)attribs;
	return false;
}

static bool wall_buf_get_shm(struct wlr_buffer *buffer,
		struct wlr_shm_attributes *attribs) {
	(void)buffer; (void)attribs;
	return false;
}

static void wall_buf_destroy(struct wlr_buffer *buffer) {
	struct swm_wall_buf *b = (struct swm_wall_buf *)buffer;
	free(b->pixels);
	free(b);
}

static const struct wlr_buffer_impl wall_buf_impl = {
	.destroy = wall_buf_destroy,
	.get_dmabuf = wall_buf_get_dmabuf,
	.get_shm = wall_buf_get_shm,
	.begin_data_ptr_access = wall_buf_begin_data_ptr_access,
	.end_data_ptr_access = wall_buf_end_data_ptr_access,
};

/* Create a wlr_buffer from raw ABGR8888 pixel data. The buffer owns the
 * pixel memory (it makes its own copy). */
static struct wlr_buffer *swm_wall_buf_create(const unsigned char *pixels,
		int width, int height) {
	struct swm_wall_buf *b = calloc(1, sizeof(*b));
	if (!b) return NULL;

	size_t stride = (size_t)width * 4;
	size_t sz = (size_t)height * stride;
	b->pixels = malloc(sz);
	if (!b->pixels) { free(b); return NULL; }
	memcpy(b->pixels, pixels, sz);

	b->width = width;
	b->height = height;
	b->stride = (int)stride;
	/* stb_image outputs R,G,B,A in memory order. On little-endian this
	 * matches DRM_FORMAT_ABGR8888 ([31:0] A:B:G:R). */
	b->format = DRM_FORMAT_ABGR8888;

	wlr_buffer_init(&b->base, &wall_buf_impl, width, height);
	return &b->base;
}

/* ----- per-output wallpaper state ----- */

struct wall_output {
	struct wlr_output *output;        // owning output (alive until destroy fires)
	struct wlr_scene_buffer *sbuf;   // scene buffer in wallpaper_tree
	struct wl_listener destroy;       // output destroy listener
};

static void wall_output_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct wall_output *wo = wl_container_of(l, wo, destroy);
	wl_list_remove(&wo->destroy.link);
	free(wo);
}

/* Forward declarations. */
static void wallpaper_on_layout_change(struct wl_listener *l, void *data);

/* ----- helpers: directory scan & random pick ----- */

/* Case-insensitive extension check — only the formats stb_image can decode. */
static bool has_img_ext(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1]) return false;
	dot++; /* skip the dot */
	return !strcasecmp(dot, "png") || !strcasecmp(dot, "jpg") ||
	       !strcasecmp(dot, "jpeg");
}

/* If `path` is a directory, pick a random image file from it and return a
 * newly allocated full path.  Otherwise return NULL (caller should try
 * `path` directly). */
char *pick_random_from_dir(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return NULL;

	DIR *dir = opendir(path);
	if (!dir) return NULL;

	/* Collect matching image files. */
	size_t cap = 32, n = 0;
	char **files = malloc(cap * sizeof(char *));
	if (!files) { closedir(dir); return NULL; }

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_type != DT_REG && ent->d_type != DT_LNK &&
		    ent->d_type != DT_UNKNOWN)
			continue;
		if (!has_img_ext(ent->d_name))
			continue;
		if (n == cap) {
			cap *= 2;
			char **tmp = realloc(files, cap * sizeof(char *));
			if (!tmp) break;
			files = tmp;
		}
		files[n] = strdup(ent->d_name);
		if (files[n]) n++;
	}
	closedir(dir);

	if (n == 0) {
		free(files);
		return NULL;
	}

	const char *pick = files[rand() % n];
	size_t plen = strlen(path) + 1 + strlen(pick) + 1;
	char *full = malloc(plen);
	if (full) snprintf(full, plen, "%s/%s", path, pick);

	for (size_t i = 0; i < n; i++) free(files[i]);
	free(files);
	return full;
}

/* ----- wallpaper lifecycle ----- */

/* Wallpaper image buffer (shared across outputs). */
struct wlr_buffer *wallpaper_buffer = NULL;
struct wlr_scene_tree *wallpaper_tree = NULL;

/* Layout change listener — keeps wallpaper positioned correctly. */
static struct wl_listener wallpaper_layout_change;

void wallpaper_init(void) {
	/* Seed the RNG for random wallpaper picking. */
	srand((unsigned)time(NULL) ^ (unsigned)getpid());

	/* Create the wallpaper scene tree behind the window tree. */
	wallpaper_tree = wlr_scene_tree_create(&server.scene->tree);
	if (!wallpaper_tree) return;

	/* Lower it so the window tree sits on top. */
	wlr_scene_node_lower_to_bottom(&wallpaper_tree->node);

	/* Listen for output layout changes. */
	wallpaper_layout_change.notify = wallpaper_on_layout_change;
	wl_signal_add(&server.output_layout->events.change,
		&wallpaper_layout_change);

	/* Try loading the configured wallpaper.  If the path is a directory we
	 * pick a random image from it; otherwise load the file directly. */
	if (wallpaper_path && *wallpaper_path) {
		char *chosen = pick_random_from_dir(wallpaper_path);
		if (chosen) {
			wallpaper_load(chosen);
			free(chosen);
		} else {
			wallpaper_load(wallpaper_path);
		}
	}

	if (!wallpaper_buffer) {
		fprintf(stderr, "swm: no wallpaper loaded (empty path or missing image)\n");
	}
}

void wallpaper_finish(void) {
	/* Free the shared wallpaper buffer. */
	if (wallpaper_buffer) {
		wlr_buffer_drop(wallpaper_buffer);
		wallpaper_buffer = NULL;
	}
	/* Remove the layout change listener. */
	wl_list_remove(&wallpaper_layout_change.link);
	/* The scene tree and all per-output scene buffers are children of
	 * wallpaper_tree and will be destroyed when the scene is cleaned up. */
	wallpaper_tree = NULL;
}

void wallpaper_load(const char *path) {
	/* Free any existing wallpaper buffer. */
	if (wallpaper_buffer) {
		wlr_buffer_drop(wallpaper_buffer);
		wallpaper_buffer = NULL;
	}

	int w, h, n;
	unsigned char *pixels = stbi_load(path, &w, &h, &n, 4);
	if (!pixels) {
		fprintf(stderr, "swm: failed to load wallpaper: %s\n", path);
		return;
	}

	wallpaper_buffer = swm_wall_buf_create(pixels, w, h);
	stbi_image_free(pixels);

	if (!wallpaper_buffer) {
		fprintf(stderr, "swm: failed to create wallpaper buffer\n");
		return;
	}

	printf("swm: loaded wallpaper %s (%dx%d)\n", path, w, h);
}

/* Set up a wallpaper scene buffer for a new output. */
void wallpaper_setup_output(struct wlr_output *output) {
	if (!wallpaper_tree) return;

	struct wall_output *wo = calloc(1, sizeof(*wo));
	if (!wo) return;

	wo->output = output;

	/* Create a scene buffer for this output. Starts with NULL buffer
	 * (invisible) – we set it below. */
	wo->sbuf = wlr_scene_buffer_create(wallpaper_tree, NULL);
	if (!wo->sbuf) { free(wo); return; }

	if (wallpaper_buffer) {
		wlr_scene_buffer_set_buffer(wo->sbuf, wallpaper_buffer);
	}

	/* Centre the image on the output using its native pixel size. */
	if (wallpaper_buffer) {
		int x = (output->width  - wallpaper_buffer->width)  / 2;
		int y = (output->height - wallpaper_buffer->height) / 2;
		struct wlr_output_layout_output *lo =
			wlr_output_layout_get(server.output_layout, output);
		if (lo) {
			x += lo->x;
			y += lo->y;
		}
		wlr_scene_node_set_position(&wo->sbuf->node, x, y);
	}

	/* Listen for output destroy. */
	wo->destroy.notify = wall_output_destroy;
	wl_signal_add(&output->events.destroy, &wo->destroy);

	wo->sbuf->node.data = wo;
}

/* Tear down the wallpaper node for a removed output. */
void wallpaper_teardown_output(struct wlr_output *output) {
	(void)output;
	/* The output destroy listener on wall_output handles cleanup. Since the
	 * scene node is a child of wallpaper_tree, it will be destroyed when
	 * the scene is cleaned up. */
}

/* Called when the output layout changes (output added/removed/resized). */
static void wallpaper_on_layout_change(struct wl_listener *l, void *data) {
	(void)l; (void)data;
	if (!wallpaper_tree) return;

	struct wlr_scene_node *node, *tmp;
	wl_list_for_each_safe(node, tmp, &wallpaper_tree->children, link) {
		if (node->type != WLR_SCENE_NODE_BUFFER) continue;
		struct wall_output *wo = node->data;
		if (!wo || !wo->output || !wallpaper_buffer) continue;

		/* Re-centre the image on the output. */
		int x = (wo->output->width  - wallpaper_buffer->width)  / 2;
		int y = (wo->output->height - wallpaper_buffer->height) / 2;
		struct wlr_output_layout_output *lo =
			wlr_output_layout_get(server.output_layout, wo->output);
		if (lo) {
			x += lo->x;
			y += lo->y;
		}
		wlr_scene_node_set_position(&wo->sbuf->node, x, y);
	}
}

/* Explicitly refresh wallpaper layout (called externally when needed). */
void wallpaper_update_output_layout(void) {
	wallpaper_on_layout_change(NULL, NULL);
}

/* Update all per-output scene buffers to point at the current
 * wallpaper_buffer (e.g. after loading a new image at runtime). */
void wallpaper_update_all_outputs(void) {
	if (!wallpaper_tree) return;
	struct wlr_scene_node *node;
	wl_list_for_each(node, &wallpaper_tree->children, link) {
		if (node->type != WLR_SCENE_NODE_BUFFER) continue;
		struct wall_output *wo = node->data;
		if (!wo || !wo->sbuf) continue;
		if (wallpaper_buffer) {
			wlr_scene_buffer_set_buffer(wo->sbuf, wallpaper_buffer);
		} else {
			wlr_scene_buffer_set_buffer(wo->sbuf, NULL);
		}
	}
	wallpaper_update_output_layout();
}
