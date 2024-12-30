#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/types.h>
#include <poll.h>
#include <uchar.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <getopt.h>

#include <wayland-client.h>
#include <wayland-util.h>
#include <fcft/fcft.h>
#include <pixman.h>
#include <json_object.h>
#include <json_tokener.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#include "cursor-shape-v1-protocol.h"

#if HAVE_SVG
#include <resvg.h>
#endif // HAVE_SVG

#if HAVE_PNG
#include <png.h>
#include <setjmp.h>
#endif // HAVE_PNG

#include "sbar.h"

#include "util.h"

struct output {
	uint32_t wl_name;
	int32_t scale, width, height;
	enum wl_output_transform transform;
	struct wl_output *wl_output;
	char *name;

	ptr_array_t bars; // struct surface * , NULL
};

struct box {
	int32_t x, y;
	int32_t width, height;
};

enum surface_type {
	SURFACE_TYPE_BAR,
	SURFACE_TYPE_POPUP,
};

struct surface {
	enum surface_type type;
	union {
		struct { // bar
			struct output *output;
			struct zwlr_layer_surface_v1 *layer_surface;
			int32_t exclusive_zone;
			int32_t margins[4]; // top, right, bottom, left
			enum zwlr_layer_surface_v1_anchor anchor;
			enum zwlr_layer_shell_v1_layer layer;
		};
		struct { // popup
			struct xdg_surface *xdg_surface;
			struct xdg_popup *xdg_popup;
			struct xdg_positioner *xdg_positioner;
			enum xdg_positioner_gravity gravity;
			enum xdg_positioner_constraint_adjustment constraint_adjustment;
			int32_t wanted_x, wanted_y;
			struct surface *parent;
			struct {
				uint32_t serial;
				struct wl_seat *seat;
			} grab;
		};
	};

	struct wl_surface *wl_surface;
	struct buffer *buffer;
	int32_t width, height;
	int32_t wanted_width, wanted_height;
	enum wp_cursor_shape_device_v1_shape cursor_shape;
	int32_t scale;
	bool vertical, render, dirty;
	array_t input_regions; // struct box

	ptr_array_t blocks; // struct block * , NULL
	array_t block_boxes; // struct block_box
	ptr_array_t popups; // struct surface * , NULL

	json_object *userdata;
};

struct pointer {
	struct wl_pointer *wl_pointer;
	struct wp_cursor_shape_device_v1 *cursor_shape_device;
	struct {
		struct surface *surface;
		double x, y;
		uint32_t wl_pointer_enter_serial;
	} focus;
	struct {
		uint32_t code;
		enum wl_pointer_button_state state;
		uint32_t serial;
	} button;
	struct {
		wl_fixed_t vector_length;
		enum wl_pointer_axis axis;
	} scroll;
};

struct seat {
	struct wl_seat *wl_seat;
	char *name;
	uint32_t wl_name;
	struct pointer pointer;
	struct {
		uint8_t index;
		array_t serials; // uint32_t  wl_pointer_button, TODO: wl_touch_down/up,
	} popup_grab;
};

struct buffer {
	struct wl_buffer *wl_buffer;
	pixman_image_t *image;
	uint32_t *pixels; // ARGB32
	uint32_t size;
	bool busy;
	struct surface *surface;
};

struct block {
	enum sbar_block_type type;
	union {
		struct { // text
			struct fcft_font *font; // fcft ref count
		};
		struct { // composite
			ptr_array_t blocks; // struct block *
		};
	};

	pixman_image_t *content_image;
	struct { // left, right, bottom, top
		int32_t width;
		pixman_image_t *color; // solid fill
	} borders[4];
	pixman_image_t *color; // solid fill
	enum sbar_block_anchor anchor;
	enum sbar_block_content_anchor content_anchor;
	int32_t min_width, max_width; // raw
	int32_t min_height, max_height; // raw
	int32_t content_width, content_height; // raw
	enum sbar_block_content_transform content_transform;
	bool render;

	uint32_t ref_count;
	uint64_t id;
};

#define border_left borders[0]
#define border_right borders[1]
#define border_bottom borders[2]
#define border_top borders[3]

struct block_box {
	int32_t x, y;
	int32_t width, height;
	int32_t content_width, content_height;
};

struct image_cache {
	char *path;
	struct timespec mtim_ts;
	pixman_image_t *image;
};

static char *stdin_buffer, *stdout_buffer;
static size_t stdin_buffer_size, stdin_buffer_index;
static size_t stdout_buffer_size, stdout_buffer_index;

static struct wl_display *wl_display;
static struct wl_registry *wl_registry;
static struct wl_compositor *wl_compositor;
static struct wl_shm *wl_shm;
static struct zwlr_layer_shell_v1 *zwlr_layer_shell_v1;
static struct xdg_wm_base *xdg_wm_base;
static struct wp_cursor_shape_manager_v1 *wp_cursor_shape_manager_v1;

static ptr_array_t outputs; // struct output *
static ptr_array_t seats; // struct seat *
static ptr_array_t blocks_with_id; // struct block *
static ptr_array_t image_cache; // struct image_cache *

static bool state_events = false;
static json_object *state_userdata;
static bool state_dirty = false;

static bool running = true;

static struct pollfd poll_fds[] = {
	{ .fd = STDIN_FILENO, .events = POLLIN },
	{ .fd = -1, .events = POLLOUT }, // stdout
	{ .fd = -1, .events = POLLIN }, // wayland
};

static ATTRIB_PURE struct surface *surface_get_bar(struct surface *surface) {
	while (surface->type == SURFACE_TYPE_POPUP) {
		surface = surface->parent;
	}
	return surface;
}

#if HAVE_SVG
static pixman_image_t *render_svg(resvg_render_tree *tree, int32_t target_width, int32_t target_height);
#endif // HAVE_SVG

static void block_render(pixman_image_t *dest, struct block *block,
		struct block_box *box) {
	if (block->color) {
		pixman_image_composite32(PIXMAN_OP_OVER, block->color, NULL, dest,
			0, 0, 0, 0,
			box->x + block->border_left.width,
			box->y + block->border_top.width,
			box->width - block->border_right.width - block->border_left.width,
			box->height - block->border_bottom.width - block->border_top.width);
	}

	if (block->border_left.width > 0) {
		pixman_image_composite32(PIXMAN_OP_OVER, block->border_left.color, NULL, dest,
			0, 0, 0, 0, box->x, box->y, block->border_left.width, box->height);
	}

	if (block->border_right.width > 0) {
		pixman_image_composite32(PIXMAN_OP_OVER, block->border_right.color, NULL, dest,
			0, 0, 0, 0, box->x + box->width - block->border_right.width, box->y,
			block->border_right.width, box->height);
	}

	if (block->border_bottom.width > 0) {
		pixman_image_composite32(PIXMAN_OP_OVER, block->border_bottom.color, NULL, dest,
			0, 0, 0, 0, box->x + block->border_left.width,
			box->y + box->height - block->border_bottom.width,
			box->width - block->border_left.width - block->border_right.width,
			block->border_bottom.width);
	}

	if (block->border_top.width > 0) {
		pixman_image_composite32(PIXMAN_OP_OVER, block->border_top.color, NULL, dest,
			0, 0, 0, 0, box->x + block->border_left.width, box->y,
			box->width - block->border_left.width - block->border_right.width,
			block->border_top.width);
	}

	if (block->content_image) {
		pixman_transform_t transform;
		pixman_transform_init_identity(&transform);

		int content_image_width = pixman_image_get_width(block->content_image);
		int content_image_height = pixman_image_get_height(block->content_image);
		if ((block->content_transform % 2) == 0) {
			int32_t tmp = content_image_width;
			content_image_width = content_image_height;
			content_image_height = tmp;
		}

		if ((box->content_width != content_image_width)
				|| (box->content_height != content_image_height)) {
#if HAVE_SVG
			resvg_render_tree *svg_tree = pixman_image_get_destroy_data(block->content_image);
			if (svg_tree) {
				pixman_image_t *image = render_svg(svg_tree, box->content_width, box->content_height);
				if (image) {
					pixman_image_unref(block->content_image);
					block->content_image = image;
				}
			} else
#endif // HAVE_SVG
				pixman_transform_scale(&transform, NULL,
					pixman_int_to_fixed(content_image_width) / box->content_width,
					pixman_int_to_fixed(content_image_height) / box->content_height);
		}

		switch (block->content_transform) {
		case SBAR_BLOCK_CONTENT_TRANSFORM_NORMAL:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED:
			break;
		case SBAR_BLOCK_CONTENT_TRANSFORM_90:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED_90:
			pixman_transform_rotate(&transform, NULL, 0, pixman_fixed_1);
			pixman_transform_translate(&transform, NULL,
				pixman_int_to_fixed(pixman_image_get_width(block->content_image)), 0);
			break;
		case SBAR_BLOCK_CONTENT_TRANSFORM_180:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED_180:
			pixman_transform_rotate(&transform, NULL, pixman_fixed_minus_1, 0);
			pixman_transform_translate(&transform, NULL,
				pixman_int_to_fixed(pixman_image_get_width(block->content_image)),
				pixman_int_to_fixed(pixman_image_get_height(block->content_image)));
			break;
		case SBAR_BLOCK_CONTENT_TRANSFORM_270:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED_270:
			pixman_transform_rotate(&transform, NULL, 0, pixman_fixed_minus_1);
			pixman_transform_translate(&transform, NULL, 0,
				pixman_int_to_fixed(pixman_image_get_height(block->content_image)));
			break;
		case SBAR_BLOCK_CONTENT_TRANSFORM_DEFAULT:
		default:
			assert(UNREACHABLE);
		}

		if (block->content_transform >= SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED) {
			pixman_transform_translate(&transform, NULL,
				-pixman_int_to_fixed(pixman_image_get_width(block->content_image)), 0);
			pixman_transform_scale(&transform, NULL, pixman_fixed_minus_1, pixman_fixed_1);
		}

		pixman_image_set_transform(block->content_image, &transform);

		int32_t available_width = box->width - block->border_left.width - block->border_right.width;
		int32_t available_height = box->height - block->border_bottom.width - block->border_top.width;
		int32_t content_x = box->x + block->border_left.width;
		int32_t content_y = box->y + block->border_top.width;
		switch (block->content_anchor) {
		case SBAR_BLOCK_CONTENT_ANCHOR_LEFT_TOP:
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_LEFT_CENTER:
			content_y += ((available_height - box->content_height) / 2);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_LEFT_BOTTOM:
			content_y += (available_height - box->content_height);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_CENTER_TOP:
			content_x += ((available_width - box->content_width) / 2);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER:
			content_x += ((available_width - box->content_width) / 2);
			content_y += ((available_height - box->content_height) / 2);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_CENTER_BOTTOM:
			content_x += ((available_width - box->content_width) / 2);
			content_y += (available_height - box->content_height);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_RIGHT_TOP:
			content_x += (available_width - box->content_width);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_RIGHT_CENTER:
			content_x += (available_width - box->content_width);
			content_y += ((available_height - box->content_height) / 2);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_RIGHT_BOTTOM:
			content_x += (available_width - box->content_width);
			content_y += (available_height - box->content_height);
			break;
		case SBAR_BLOCK_CONTENT_ANCHOR_DEFAULT:
		default:
			assert(UNREACHABLE);
			return;
		}
		if (content_x < block->border_left.width) {
			content_x = block->border_left.width;
		}
		if (content_y < block->border_top.width) {
			content_y = block->border_top.width;
		}
		pixman_image_composite32(PIXMAN_OP_OVER, block->content_image, NULL, dest,
			0, 0, 0, 0, content_x, content_y,
			box->content_width, box->content_height);
	}
}

static void block_get_size(struct block *block, struct surface *surface,
		struct block_box *prev_block_box, struct block_box *dest) {
	if (block == NULL) {
		*dest = (struct block_box) { 0 };
		return;
	}

	int32_t min_width = 0, max_width = 0;
	int32_t min_height = 0, max_height = 0;
	int32_t content_width = 0, content_height = 0;
	struct {
		int32_t source;
		int32_t *dest;
	} block_size[] = {
		{ block->min_width, &min_width, },
		{ block->max_width, &max_width, },
		{ block->min_height, &min_height, },
		{ block->max_height, &max_height, },
		{ block->content_width, &content_width, },
		{ block->content_height, &content_height, },
	};

	for (size_t i = 0; i < LENGTH(block_size); ++i) {
		if (block_size[i].source < 0) {
			if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_WIDTH_PLUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->width +
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_WIDTH_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_WIDTH_MINUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->width -
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_WIDTH_MINUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->height +
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_MINUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->height -
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_MINUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_WIDTH_PLUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->content_width +
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_WIDTH_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_WIDTH_MINUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->content_width -
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_WIDTH_MINUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_HEIGHT_PLUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->content_height +
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_HEIGHT_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_HEIGHT_MINUS) && prev_block_box) {

				*block_size[i].dest = prev_block_box->content_height -
					(block_size[i].source - SBAR_BLOCK_SIZE_PREV_BLOCK_CONTENT_HEIGHT_MINUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_SURFACE_WIDTH_PLUS) && surface) {

				*block_size[i].dest = surface->width +
					(block_size[i].source - SBAR_BLOCK_SIZE_SURFACE_WIDTH_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_SURFACE_WIDTH_MINUS) && surface) {

				*block_size[i].dest = surface->width -
					(block_size[i].source - SBAR_BLOCK_SIZE_SURFACE_WIDTH_MINUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_SURFACE_HEIGHT_PLUS) && surface) {

				*block_size[i].dest = surface->height +
					(block_size[i].source - SBAR_BLOCK_SIZE_SURFACE_HEIGHT_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_SURFACE_HEIGHT_MINUS) && surface) {

				*block_size[i].dest = surface->height -
					(block_size[i].source - SBAR_BLOCK_SIZE_SURFACE_HEIGHT_MINUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_OUTPUT_WIDTH_PLUS) && surface) {

				*block_size[i].dest = surface_get_bar(surface)->output->width +
					(block_size[i].source - SBAR_BLOCK_SIZE_OUTPUT_WIDTH_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_OUTPUT_WIDTH_MINUS) && surface) {

				*block_size[i].dest = surface_get_bar(surface)->output->width -
					(block_size[i].source - SBAR_BLOCK_SIZE_OUTPUT_WIDTH_MINUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_OUTPUT_HEIGHT_PLUS) && surface) {

				*block_size[i].dest = surface_get_bar(surface)->output->height +
					(block_size[i].source - SBAR_BLOCK_SIZE_OUTPUT_HEIGHT_PLUS);

			} else if ((block_size[i].source >= SBAR_BLOCK_SIZE_OUTPUT_HEIGHT_MINUS) && surface) {

				*block_size[i].dest = surface_get_bar(surface)->output->height -
					(block_size[i].source - SBAR_BLOCK_SIZE_OUTPUT_HEIGHT_MINUS);
			}
		} else {
			*block_size[i].dest = block_size[i].source;
		}
	}

	if ((block->content_transform % 2) == 0) {
		int32_t tmp = content_width;
		content_width = content_height;
		content_height = tmp;
	}

	int32_t width = content_width + block->border_left.width + block->border_right.width;
	int32_t height = content_height + block->border_top.width + block->border_bottom.width;
	if (surface && (surface->width > 0) && (surface->height > 0)) {
		if (block->anchor == SBAR_BLOCK_ANCHOR_NONE) {
			width = surface->width;
			height = surface->height;
		} else {
			if (surface->vertical) {
				width = surface->width;
			} else {
				height = surface->height;
			}
		}
	}

	if (width < min_width) {
		width = min_width;
	} else if ((max_width > 0) && (width > max_width)) {
		width = max_width;
	}

	if (height < min_height) {
		height = min_height;
	} else if ((max_height > 0) && (height > max_height)) {
		height = max_height;
	}

	*dest = (struct block_box) {
		.x = 0,
		.y = 0,
		.width = width,
		.height = height,
		.content_width = content_width,
		.content_height = content_height,
	};
}

static void surface_render(struct surface *surface) {
	if (surface->render) {
		if (surface->buffer->busy) {
			surface->dirty = true;
			return;
		}
		memset(surface->buffer->pixels, 0, surface->buffer->size);
	}

	int32_t l = 0, c = 0, r = surface->vertical ?
		surface->height : surface->width;
	{
		struct block_box *prev_block_box = NULL;
		for (size_t i = 0; i < surface->blocks.len; ++i) {
			struct block *block = surface->blocks.items[i];
			struct block_box box;
			block_get_size(block, surface, prev_block_box, &box);
			if (block && block->render && (block->anchor == SBAR_BLOCK_ANCHOR_CENTER)) {
				c += surface->vertical ? box.height : box.width;
			}
			prev_block_box = array_put(&surface->block_boxes, i, &box);
		}
	}
	c = (r - c) / 2;
	for (size_t i = 0; i < surface->blocks.len; ++i) {
		struct block *block = surface->blocks.items[i];
		struct block_box *box = &((struct block_box *)surface->block_boxes.items)[i];
		if (block == NULL) {
			continue;
		}

		if (block->render) {
			switch (block->anchor) {
			case SBAR_BLOCK_ANCHOR_TOP:
			case SBAR_BLOCK_ANCHOR_LEFT:
				if (surface->vertical) {
					box->y = l;
					l += box->height;
				} else {
					box->x = l;
					l += box->width;
				}
				break;
			case SBAR_BLOCK_ANCHOR_CENTER:
				if (surface->vertical) {
					box->y = c;
					c += box->height;
				} else {
					box->x = c;
					c += box->width;
				}
				break;
			case SBAR_BLOCK_ANCHOR_BOTTOM:
			case SBAR_BLOCK_ANCHOR_RIGHT:
				if (surface->vertical) {
					r -= box->height;
					box->y = r;
				} else {
					r -= box->width;
					box->x = r;
				}
				break;
			case SBAR_BLOCK_ANCHOR_NONE:
				break;
			case SBAR_BLOCK_ANCHOR_DEFAULT:
			default:
				assert(UNREACHABLE);
				return;
			}
			if (surface->render) {
				block_render(surface->buffer->image, block, box);
			}
		}
	}

	if (surface->render) {
		wl_surface_set_buffer_scale(surface->wl_surface, surface->scale);
		wl_surface_attach(surface->wl_surface, surface->buffer->wl_buffer, 0, 0);
		wl_surface_damage_buffer(surface->wl_surface, 0, 0, surface->width, surface->height);
		wl_surface_commit(surface->wl_surface);
		surface->buffer->busy = true;
	}

	surface->dirty = false;
	state_dirty = true;
}

static void block_unref(struct block *block) {
	if ((block == NULL) || (--block->ref_count > 0)) {
		return;
	}

	switch (block->type) {
	case SBAR_BLOCK_TYPE_TEXT:
		fcft_destroy(block->font);
		break;
	case SBAR_BLOCK_TYPE_COMPOSITE:
		for (size_t i = 0; i < block->blocks.len; ++i) {
			block_unref(block->blocks.items[i]);
		}
		ptr_array_fini(&block->blocks);
		break;
	case SBAR_BLOCK_TYPE_IMAGE:
	case SBAR_BLOCK_TYPE_DEFAULT:
	case SBAR_BLOCK_TYPE_SPACER:
	default:
		break;
	}

	if (block->content_image) {
		pixman_image_unref(block->content_image);
	}
	if (block->color) {
		pixman_image_unref(block->color);
	}

	for (size_t i = 0; i < LENGTH(block->borders); ++i) {
		if (block->borders[i].color) {
			pixman_image_unref(block->borders[i].color);
		}
	}

	if (block->id > 0) {
		for (size_t i = 0; i < blocks_with_id.len; ++i) {
			if (block == blocks_with_id.items[i]) {
				ptr_array_pop(&blocks_with_id, i);
				break;
			}
		}
	}

	free(block);
}

static pixman_color_t parse_color_argb32(uint32_t color) {
	premultiply_alpha_argb32(&color);
	return (pixman_color_t) {
			.alpha = (uint16_t)(((color >> 24) & 0xFF) * 257),
			.red = (uint16_t)(((color >> 16) & 0xFF) * 257),
			.green = (uint16_t)(((color >> 8) & 0xFF) * 257),
			.blue = (uint16_t)(((color >> 0) & 0xFF) * 257),
	};
}

static pixman_image_t *load_pixmap(const char *path) {
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return NULL;
	}

	struct {
		uint32_t width;
		uint32_t height;
		uint32_t pixels[];
	} size;

	pixman_image_t *image = NULL;
	if (fread(&size, 1, sizeof(size), f) != sizeof(size)) {
		goto cleanup;
	}

	image = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)size.width, (int)size.height,
		NULL, (int)size.width * 4);
	if (image == NULL) {
		goto cleanup;
	}

    uint32_t *image_data = pixman_image_get_data(image);
	if (fread(image_data, 1, (size.width * size.height * 4), f) != (size.width * size.height * 4)) {
		pixman_image_unref(image);
		image = NULL;
		goto cleanup;
	}

    for (uint32_t i = 0; i < size.width * size.height; ++i) {
        premultiply_alpha_argb32(&image_data[i]);
    }

cleanup:
	fclose(f);
    return image;
}

#if HAVE_PNG
static void premultiply_alpha_png(MAYBE_UNUSED png_structp png, png_row_infop row_info,
		png_bytep data) {
	for (size_t i = 0; i < row_info->rowbytes; i += 4) {
		premultiply_alpha_argb32((uint32_t *)(void *)&data[i]);
	}
}

static pixman_image_t *load_png(const char *path) {
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		log_stderr("%s: fopen: %s", path, strerror(errno));
		return NULL;
	}

	pixman_image_t *image = NULL;
	png_bytepp row_pointers = NULL;
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
			NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);

	if (setjmp(png_jmpbuf(png))) {
		log_stderr("%s: libpng error", path);
		goto cleanup;
	}

	png_init_io(png, file);
	png_read_info(png, info);

	png_uint_32 width = png_get_image_width(png, info);
	png_uint_32 height = png_get_image_height(png, info);
	png_uint_32 stride = width * 4;
	image = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)width, (int)height,
			NULL, (int)stride);
	if (image == NULL) {
		goto cleanup;
	}

	png_set_strip_16(png);
	png_set_bgr(png);
	png_set_palette_to_rgb(png);
	png_set_expand_gray_1_2_4_to_8(png);
	png_set_tRNS_to_alpha(png);
	png_set_packing(png);
	png_set_gray_to_rgb(png);
	png_set_interlace_handling(png);
	png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

	png_set_read_user_transform_fn(png, premultiply_alpha_png);

	png_bytep image_data = (png_bytep)pixman_image_get_data(image);
	row_pointers = malloc(height * sizeof(png_bytepp));
    for (size_t i = 0; i < height; ++i) {
		row_pointers[i] = &image_data[i * stride];
	}

    png_read_image(png, row_pointers);

cleanup:
	free(row_pointers);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(file);
	return image;
}
#endif // HAVE_PNG

#if HAVE_SVG
static pixman_image_t *render_svg(resvg_render_tree *tree, int32_t target_width, int32_t target_height) {
	resvg_size image_size = resvg_get_image_size(tree);
	int32_t width = (int32_t)image_size.width;
	int32_t height = (int32_t)image_size.width;

	resvg_transform transform = resvg_transform_identity();
	if (target_width > 0) {
		width = target_width;
		transform.a = (float)target_width / image_size.width;
	}
	if (target_height > 0) {
		height = target_height;
		transform.d = (float)target_height / image_size.height;
	}

	pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, NULL, -1);
	if (image) {
		uint32_t *image_data = pixman_image_get_data(image);
		resvg_render(tree, transform, (uint32_t)width, (uint32_t)height, (char *)image_data);

		for (int32_t i = 0; i < width * height; ++i) {
			uint32_t *p = &image_data[i];
			uint8_t r = (uint8_t)((*p >> 0) & 0xFF);
			uint8_t g = (uint8_t)((*p >> 8) & 0xFF);
			uint8_t b = (uint8_t)((*p >> 16) & 0xFF);
			uint8_t a = (uint8_t)((*p >> 24) & 0xFF);
			*p = (uint32_t)a << 24 | (uint32_t)r << 16
					| (uint32_t)g << 8 | (uint32_t)b << 0;
		}

		pixman_image_set_filter(image, PIXMAN_FILTER_BEST, NULL, 0);
		pixman_image_set_destroy_function(image, NULL, tree);
	}

	return image;
}

static void destroy_resvg_render_tree(MAYBE_UNUSED pixman_image_t *image, void *tree) {
	resvg_tree_destroy((resvg_render_tree *)tree);
}

static pixman_image_t *load_svg(const char *path) {
	pixman_image_t *image = NULL;
	resvg_render_tree *tree = NULL;
	resvg_options *opt = resvg_options_create();
	int32_t ret = resvg_parse_tree_from_file(path, opt, &tree);
    if (ret == RESVG_OK) {
		image = render_svg(tree, -1, -1);
    } else {
		log_stderr("%s: resvg_parse_tree_from_file failed. code = %d", path, ret);
	}

	if (opt) {
		resvg_options_destroy(opt);
	}
	if (tree) {
		if (image) {
			pixman_image_set_destroy_function(image, destroy_resvg_render_tree, tree);
		} else {
			resvg_tree_destroy(tree);
		}
	}

	return image;
}
#endif // HAVE_SVG

static void free_image_cache(struct image_cache *cache) {
	if (cache == NULL) {
		return;
	}

	free(cache->path);
	pixman_image_unref(cache->image);

	free(cache);
}

static struct block *block_get(json_object *block_json, uint64_t id) {
	if (id > 0) {
		for (size_t i = 0; i < blocks_with_id.len; ++i) {
			struct block *block = blocks_with_id.items[i];
			if (block->id == id) {
				block->ref_count++;
				return block;
			}
		}
	}

	struct block *block = calloc(1, sizeof(struct block));
	block->id = id;
	block->ref_count = 1;

	json_object *type, *anchor, *color_json, *render, *borders[4];
	json_object *min_width, *max_width, *min_height, *max_height;
	json_object *content_width, *content_height, *content_transform, *content_anchor;
	json_object_object_get_ex(block_json, "type", &type);
	json_object_object_get_ex(block_json, "anchor", &anchor);
	json_object_object_get_ex(block_json, "color", &color_json);
	json_object_object_get_ex(block_json, "render", &render);
	json_object_object_get_ex(block_json, "min_width", &min_width);
	json_object_object_get_ex(block_json, "max_width", &max_width);
	json_object_object_get_ex(block_json, "min_height", &min_height);
	json_object_object_get_ex(block_json, "max_height", &max_height);
	json_object_object_get_ex(block_json, "content_width", &content_width);
	json_object_object_get_ex(block_json, "content_height", &content_height);
	json_object_object_get_ex(block_json, "content_transform", &content_transform);
	json_object_object_get_ex(block_json, "content_anchor", &content_anchor);

	switch ((enum sbar_block_type)(json_object_is_type(type, json_type_int)
			? json_object_get_int(type)
			: SBAR_BLOCK_TYPE_SPACER)) {
	default:
	case SBAR_BLOCK_TYPE_DEFAULT:
	case SBAR_BLOCK_TYPE_SPACER:
		block->type = SBAR_BLOCK_TYPE_SPACER;
		break;
	case SBAR_BLOCK_TYPE_TEXT: {
		json_object *text_json;
		json_object_object_get_ex(block_json, "text", &text_json);
		int len;
		if ((len = json_object_get_string_len(text_json)) <= 0) {
			goto error;
		}

		const char *raw_text = json_object_get_string(text_json);
		size_t raw_text_len = (size_t)len + 1, text_len = 0, end = (size_t)raw_text + raw_text_len;
		char32_t *text = malloc(raw_text_len * sizeof(char32_t));
		mbstate_t ps = { 0 };
		size_t ret;
		while ((ret = mbrtoc32(&text[text_len], raw_text, end - (size_t)raw_text, &ps)) != 0) {
			switch (ret) {
			case (size_t)-1:
			case (size_t)-2:
			case (size_t)-3:
				free(text);
				log_stderr("mbrtoc32 failed. code = %zu", ret);
				goto error;
			default:
				raw_text += ret;
				++text_len;
				break;
			}
		}

		json_object *font_names_array, *font_attributes_json, *text_color_json;
		json_object_object_get_ex(block_json, "font_names", &font_names_array);
		json_object_object_get_ex(block_json, "font_attributes", &font_attributes_json);
		json_object_object_get_ex(block_json, "text_color", &text_color_json);

		ptr_array_t font_names; // char *
		ptr_array_init(&font_names, 4);
		if (json_object_is_type(font_names_array, json_type_array)) {
			for (size_t f = 0; f < json_object_array_length(font_names_array); ++f) {
				json_object *font_name = json_object_array_get_idx(font_names_array, f);
				if (json_object_is_type(font_name, json_type_string)) {
					ptr_array_add(&font_names, (char *)json_object_get_string(font_name));
				}
			}
		}
		ptr_array_add(&font_names, (char *)"monospace:size=16");
		const char *font_attributes = json_object_is_type(font_attributes_json, json_type_string)
				? json_object_get_string(font_attributes_json)
				: NULL;
		block->font = fcft_from_name(font_names.len,
				(const char **)font_names.items, font_attributes);
		ptr_array_fini(&font_names);
		if (block->font == NULL) {
			free(text);
			log_stderr("fcft_from_name failed");
			goto error;
		}

		struct fcft_text_run *text_run = fcft_rasterize_text_run_utf32(
				block->font, text_len, text, FCFT_SUBPIXEL_NONE);
		free(text);
		if (text_run == NULL) {
			log_stderr("fcft_rasterize_text_run_utf32 failed");
			goto error;
		}

		pixman_color_t color = parse_color_argb32(
				json_object_is_type(text_color_json, json_type_int)
				? (uint32_t)json_object_get_uint64(text_color_json)
				: 0xFFFFFFFF);
		pixman_image_t *text_color = pixman_image_create_solid_fill(&color);

		int image_width = 0, image_height;
		for (size_t i = 0; i < text_run->count; i++) {
			image_width += text_run->glyphs[i]->advance.x;
		}
		image_height = block->font->height;
		block->content_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, image_width,
				image_height, NULL, image_width * 4);
		if (block->content_image == NULL) {
			goto error;
		}

		int x = 0, y = block->font->height - block->font->descent;
		for (size_t i = 0; i < text_run->count; ++i) {
			const struct fcft_glyph *glyph = text_run->glyphs[i];
			if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
				pixman_image_composite32(PIXMAN_OP_OVER, glyph->pix, NULL, block->content_image,
						0, 0, 0, 0, x + glyph->x, y - glyph->y,
						glyph->width, glyph->height);
			} else {
				pixman_image_composite32(PIXMAN_OP_OVER, text_color, glyph->pix, block->content_image,
						0, 0, 0, 0, x + glyph->x, y - glyph->y,
						glyph->width, glyph->height);
			}
			x += glyph->advance.x;
		}

		pixman_image_unref(text_color);
		fcft_text_run_destroy(text_run);
		block->type = SBAR_BLOCK_TYPE_TEXT;
		break;
	}
	case SBAR_BLOCK_TYPE_IMAGE: {
		json_object *path_json;
		json_object_object_get_ex(block_json, "path", &path_json);
		const char *path;
		struct stat sb;
		if (!json_object_is_type(path_json, json_type_string)
				|| !*(path = json_object_get_string(path_json))
				|| (stat(path, &sb) == -1)) {
			goto error;
		}

		for (size_t i = 0; i < image_cache.len; ++i) { // ? TODO: limit cache size
			struct image_cache *cache = image_cache.items[i];
			if (strcmp(path, cache->path) == 0) {
				if (memcmp(&sb.st_mtim, &cache->mtim_ts, sizeof(struct timespec)) == 0) {
					block->content_image = pixman_image_ref(cache->image);
				} else {
					free_image_cache(cache);
					ptr_array_pop(&image_cache, i);
				}
				break;
			}
		}

		if (block->content_image == NULL) {
			json_object *image_type;
			json_object_object_get_ex(block_json, "image_type", &image_type);
			switch ((enum sbar_block_type_image_image_type)(
					json_object_is_type(image_type, json_type_int) ? json_object_get_int(image_type)
					: SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_PIXMAP)) {
			default:
			case SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_DEFAULT:
			case SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_RESERVED:
			case SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_PIXMAP:
				block->content_image = load_pixmap(path);
				break;
			case SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_PNG:
#if HAVE_PNG
				block->content_image = load_png(path);
#endif // HAVE_PNG
				break;
			case SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_SVG:
#if HAVE_SVG
				block->content_image = load_svg(path);
#endif // HAVE_SVG
				break;
			}
			if (block->content_image) {
				struct image_cache *cache = malloc(sizeof(struct image_cache));
				cache->path = strdup(path);
				cache->mtim_ts = sb.st_mtim;
				cache->image = pixman_image_ref(block->content_image);
				ptr_array_add(&image_cache, cache);

				pixman_image_set_filter(block->content_image, PIXMAN_FILTER_BEST, NULL, 0);
				}
		}
		if (block->content_image == NULL) {
			goto error;
		}

		block->type = SBAR_BLOCK_TYPE_IMAGE;
		break;
	}
	case SBAR_BLOCK_TYPE_COMPOSITE: {
		json_object *blocks_array;
		json_object_object_get_ex(block_json, "blocks", &blocks_array);
		size_t blocks_len;
		if (!json_object_is_type(blocks_array, json_type_array)
				|| ((blocks_len = json_object_array_length(blocks_array)) == 0)) {
			goto error;
		}

		ptr_array_init(&block->blocks, blocks_len);
		array_t block_boxes; // struct block_box
		array_init(&block_boxes, blocks_len, sizeof(struct block_box));

		struct block_box *prev_block_box = NULL;
		for (size_t i = 0; i < blocks_len; ++i) {
			json_object *blk_json = json_object_array_get_idx(blocks_array, i);
			json_object *id_json;
			json_object_object_get_ex(blk_json, "id", &id_json);
			struct block *blk = block_get(blk_json,
				json_object_is_type(id_json, json_type_int) ?
					json_object_get_uint64(id_json) : 0);
			struct block_box box;
			block_get_size(blk, NULL, prev_block_box, &box);
			if ((box.width == 0) || (box.height == 0)) {
				block_unref(blk);
				continue;
			}

			json_object *x, *y;
			json_object_object_get_ex(blk_json, "x", &x);
			json_object_object_get_ex(blk_json, "y", &y);
			if (json_object_is_type(x, json_type_int) && json_object_is_type(y, json_type_int)) {
				box.x = (int32_t)json_object_get_uint64(x);
				box.y = (int32_t)json_object_get_uint64(y);
			} else if (prev_block_box) {
				switch (blk->anchor) {
				case SBAR_BLOCK_ANCHOR_LEFT:
					box.x = prev_block_box->x - box.width;
					box.y = prev_block_box->y + ((prev_block_box->height - box.height) / 2);
					break;
				case SBAR_BLOCK_ANCHOR_RIGHT:
					box.x = prev_block_box->x + prev_block_box->width;
					box.y = prev_block_box->y + ((prev_block_box->height - box.height) / 2);
					break;
				case SBAR_BLOCK_ANCHOR_BOTTOM:
					box.x = prev_block_box->x + ((prev_block_box->width - box.width) / 2);
					box.y = prev_block_box->y + prev_block_box->height;
					break;
				case SBAR_BLOCK_ANCHOR_TOP:
					box.x = prev_block_box->x + ((prev_block_box->width - box.width) / 2);
					box.y = prev_block_box->y - box.height;
					break;
				case SBAR_BLOCK_ANCHOR_CENTER:
				case SBAR_BLOCK_ANCHOR_NONE:
				case SBAR_BLOCK_ANCHOR_DEFAULT:
				default:
					break;
				}
			}

			if (box.x < 0) {
				for (size_t j = 0; j < block_boxes.len; ++j) {
					struct block_box *block_box = &((struct block_box *)block_boxes.items)[j];
					block_box->x += -box.x;
				}
				box.x = 0;
			}
			if (box.y < 0) {
				for (size_t j = 0; j < block_boxes.len; ++j) {
					struct block_box *block_box = &((struct block_box *)block_boxes.items)[j];
					block_box->y += -box.y;
				}
				box.y = 0;
			}

			prev_block_box = array_add(&block_boxes, &box);
			ptr_array_add(&block->blocks, blk);
		}

		int content_image_width = 0, content_image_height = 0;
		for (size_t i = 0; i < block_boxes.len; ++i) {
			struct block_box *block_box = &((struct block_box *)block_boxes.items)[i];
			if ((block_box->x + block_box->width) > content_image_width) {
				content_image_width = (block_box->x + block_box->width);
			}
			if ((block_box->y + block_box->height) > content_image_height) {
				content_image_height = (block_box->y + block_box->height);
			}
		}
		block->content_image = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			content_image_width, content_image_height, NULL, content_image_width * 4);
		if (block->content_image == NULL) {
			goto error;
		}

		for (size_t i = 0; i < block->blocks.len; ++i) {
			block_render(block->content_image, block->blocks.items[i],
					&((struct block_box *)block_boxes.items)[i]);
		}

		array_fini(&block_boxes);
		block->type = SBAR_BLOCK_TYPE_COMPOSITE;
		break;
	}
	}

	block->content_width = json_object_is_type(content_width, json_type_int)
		? json_object_get_int(content_width) : 0;
	block->content_height = json_object_is_type(content_height, json_type_int)
		? json_object_get_int(content_height) : 0;

	int32_t tmp;
	if (block->content_image) {
		tmp = json_object_is_type(content_transform, json_type_int)
				? (int32_t)json_object_get_uint64(content_transform)
				: SBAR_BLOCK_CONTENT_TRANSFORM_NORMAL;
		switch ((enum sbar_block_content_transform)tmp) {
		case SBAR_BLOCK_CONTENT_TRANSFORM_NORMAL:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED:
		case SBAR_BLOCK_CONTENT_TRANSFORM_90:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED_90:
		case SBAR_BLOCK_CONTENT_TRANSFORM_180:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED_180:
		case SBAR_BLOCK_CONTENT_TRANSFORM_270:
		case SBAR_BLOCK_CONTENT_TRANSFORM_FLIPPED_270:
			block->content_transform = (enum sbar_block_content_transform)tmp;
			break;
		case SBAR_BLOCK_CONTENT_TRANSFORM_DEFAULT:
		default:
			block->content_transform = SBAR_BLOCK_CONTENT_TRANSFORM_NORMAL;
			break;
		}

		if (block->content_width == SBAR_BLOCK_SIZE_AUTO) {
			block->content_width = pixman_image_get_width(block->content_image);
		}
		if (block->content_height == SBAR_BLOCK_SIZE_AUTO) {
			block->content_height = pixman_image_get_height(block->content_image);
		}
	}

	for (size_t i = 0; i < LENGTH(block->borders); ++i) {
		static const char *keys[] = {
			"border_left",
			"border_right",
			"border_bottom",
			"border_top",
		};
		json_object_object_get_ex(block_json, keys[i], &borders[i]);
		json_object *border_width;
		json_object_object_get_ex(borders[i], "width", &border_width);
		tmp = json_object_is_type(border_width, json_type_int)
				? json_object_get_int(border_width) : -1;
		block->borders[i].width = (tmp >= 0) ? tmp : 0;
		if (block->borders[i].width > 0) {
			json_object *border_color_json;
			json_object_object_get_ex(borders[i], "color", &border_color_json);
			pixman_color_t border_color = parse_color_argb32(
					json_object_is_type(border_color_json, json_type_int)
					? (uint32_t)json_object_get_uint64(border_color_json)
					: 0);
			block->borders[i].color = pixman_image_create_solid_fill(&border_color);
		}
	}

	block->min_width = json_object_is_type(min_width, json_type_int)
		? json_object_get_int(min_width) : 0;
	block->max_width = json_object_is_type(max_width, json_type_int)
		? json_object_get_int(max_width) : 0;
	if ((block->min_width > 0) && (block->max_width > 0)
			&& (block->max_width < block->min_width)) {
		block->min_width = block->max_width = 0;
	}

	block->min_height = json_object_is_type(min_height, json_type_int)
		? json_object_get_int(min_height) : 0;
	block->max_height = json_object_is_type(max_height, json_type_int)
		? json_object_get_int(max_height) : 0;
	if ((block->min_height > 0) && (block->max_height > 0)
			&& (block->max_height < block->min_height)) {
		block->min_height = block->max_height = 0;
	}

	tmp = json_object_is_type(anchor, json_type_int)
		? json_object_get_int(anchor) : -1;
	switch ((enum sbar_block_anchor)tmp) {
	default:
	case SBAR_BLOCK_ANCHOR_DEFAULT:
		block->anchor = SBAR_BLOCK_ANCHOR_LEFT;
		break;
	case SBAR_BLOCK_ANCHOR_TOP:
	case SBAR_BLOCK_ANCHOR_RIGHT:
	case SBAR_BLOCK_ANCHOR_BOTTOM:
	case SBAR_BLOCK_ANCHOR_LEFT:
	case SBAR_BLOCK_ANCHOR_CENTER:
	case SBAR_BLOCK_ANCHOR_NONE:
		block->anchor = (enum sbar_block_anchor)tmp;
	}

	tmp = json_object_is_type(content_anchor, json_type_int)
		? json_object_get_int(content_anchor) : -1;
	switch ((enum sbar_block_content_anchor)tmp) {
	default:
	case SBAR_BLOCK_CONTENT_ANCHOR_DEFAULT:
		block->content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_LEFT_CENTER;
		break;
	case SBAR_BLOCK_CONTENT_ANCHOR_LEFT_TOP:
	case SBAR_BLOCK_CONTENT_ANCHOR_LEFT_CENTER:
	case SBAR_BLOCK_CONTENT_ANCHOR_LEFT_BOTTOM:
	case SBAR_BLOCK_CONTENT_ANCHOR_CENTER_TOP:
	case SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER:
	case SBAR_BLOCK_CONTENT_ANCHOR_CENTER_BOTTOM:
	case SBAR_BLOCK_CONTENT_ANCHOR_RIGHT_TOP:
	case SBAR_BLOCK_CONTENT_ANCHOR_RIGHT_CENTER:
	case SBAR_BLOCK_CONTENT_ANCHOR_RIGHT_BOTTOM:
		block->content_anchor = (enum sbar_block_content_anchor)tmp;
		break;
	}

	if (json_object_is_type(color_json, json_type_int)) {
		pixman_color_t color = parse_color_argb32(
			(uint32_t)json_object_get_uint64(color_json));
		block->color = pixman_image_create_solid_fill(&color);
	}

	block->render = json_object_is_type(render, json_type_boolean)
		? json_object_get_boolean(render) : true;

	if (id > 0) {
		ptr_array_add(&blocks_with_id, block);
	}
	return block;
error:
	block_unref(block);
	return NULL;
}

static const unsigned jso_add_flags =
	JSON_C_OBJECT_ADD_KEY_IS_NEW | JSON_C_OBJECT_ADD_CONSTANT_KEY;

static void describe_blocks(json_object *dest, ptr_array_t *blocks, // struct block * , NULL
		array_t *boxes) { // struct block_box
	json_object *blocks_array = json_object_new_array_ext((int)blocks->len);
	json_object_object_add_ex(dest, "blocks", blocks_array, jso_add_flags);
	for (size_t i = 0; i < blocks->len; ++i) {
		struct block *block = blocks->items[i];
		if (block == NULL) {
			json_object_array_add(blocks_array, NULL);
			continue;
		}
		struct block_box *box = &((struct block_box *)boxes->items)[i];
		json_object *block_json = json_object_new_object();
		json_object_array_add(blocks_array, block_json);
		json_object_object_add_ex(block_json, "x",
			json_object_new_int64(box->x), jso_add_flags);
		json_object_object_add_ex(block_json, "y",
			json_object_new_int64(box->y), jso_add_flags);
		json_object_object_add_ex(block_json, "width",
			json_object_new_int64(box->width), jso_add_flags);
		json_object_object_add_ex(block_json, "height",
			json_object_new_int64(box->height), jso_add_flags);
	}
}

static void describe_surfaces(json_object *dest_array, ptr_array_t *source) { // struct surface * , NULL
	for (size_t i = 0; i < source->len; ++i) {
		struct surface *surface = source->items[i];
		if (surface == NULL) {
			json_object_array_add(dest_array, NULL);
			continue;
		}
		json_object *surface_json = json_object_new_object();
		json_object_array_add(dest_array, surface_json);
		//json_object_object_add_ex(surface_json, "userdata",
		//	json_object_get(surface->userdata), jso_add_flags);
		json_object_object_add_ex(surface_json, "width",
			json_object_new_int64(surface->width), jso_add_flags);
		json_object_object_add_ex(surface_json, "height",
			json_object_new_int64(surface->height), jso_add_flags);
		json_object_object_add_ex(surface_json, "scale",
			json_object_new_int64(surface->scale), jso_add_flags);
		describe_blocks(surface_json, &surface->blocks, &surface->block_boxes);
		json_object *popups_array = json_object_new_array_ext((int)surface->popups.len);
		json_object_object_add_ex(surface_json, "popups", popups_array, jso_add_flags);
		describe_surfaces(popups_array, &surface->popups);
	}
}

static void describe_outputs(json_object *dest) {
	json_object *outputs_array = json_object_new_array_ext((int)outputs.len);
	json_object_object_add_ex(dest, "outputs", outputs_array, jso_add_flags);
	for (size_t i = 0; i < outputs.len; ++i) {
		struct output *output = outputs.items[i];
		if (output->name == NULL) {
			continue;
		}
		json_object *output_json = json_object_new_object();
		json_object_array_add(outputs_array, output_json);
		json_object_object_add_ex(output_json, "name",
			json_object_new_string(output->name), jso_add_flags);
		json_object_object_add_ex(output_json, "width",
			json_object_new_int64(output->width), jso_add_flags);
		json_object_object_add_ex(output_json, "height",
			json_object_new_int64(output->height), jso_add_flags);
		json_object_object_add_ex(output_json, "scale",
			json_object_new_int64(output->scale), jso_add_flags);
		json_object_object_add_ex(output_json, "transform",
			json_object_new_int64(output->transform), jso_add_flags);
		json_object *bars_array = json_object_new_array_ext((int)output->bars.len);
		json_object_object_add_ex(output_json, "bars", bars_array, jso_add_flags);
		describe_surfaces(bars_array, &output->bars);
	}
}

static void describe_seats(json_object *dest) {
	json_object *seats_array = json_object_new_array_ext((int)seats.len);
	json_object_object_add_ex(dest, "seats", seats_array, jso_add_flags);
	for (size_t i = 0; i < seats.len; ++i) {
		struct seat *seat = seats.items[i];
		if (seat->name == NULL) {
			continue;
		}
		json_object *seat_json = json_object_new_object();
		json_object_array_add(seats_array, seat_json);
		json_object_object_add_ex(seat_json, "name",
			json_object_new_string(seat->name), jso_add_flags);
		json_object *pointer_json = NULL;
		if (seat->pointer.wl_pointer != NULL) {
			struct pointer *pointer = &seat->pointer;
			pointer_json = json_object_new_object();
			json_object *focus = NULL, *button = NULL, *scroll = NULL;
			if (pointer->focus.surface != NULL) {
				focus = json_object_new_object();
				json_object_object_add_ex(focus, "surface_userdata",
					json_object_get(pointer->focus.surface->userdata),
					jso_add_flags);
				json_object_object_add_ex(focus, "x",
					json_object_new_double(pointer->focus.x), jso_add_flags);
				json_object_object_add_ex(focus, "y",
					json_object_new_double(pointer->focus.y), jso_add_flags);
			}
			if (pointer->button.code != 0) {
				button = json_object_new_object();
				json_object_object_add_ex(button, "code",
					json_object_new_int64(pointer->button.code), jso_add_flags);
				json_object_object_add_ex(button, "state",
					json_object_new_int64(pointer->button.state), jso_add_flags);
				json_object_object_add_ex(button, "serial",
					json_object_new_int64(pointer->button.serial), jso_add_flags);
			}
			if (pointer->scroll.vector_length != 0) {
				scroll = json_object_new_object();
				json_object_object_add_ex(scroll, "axis",
					json_object_new_int64(pointer->scroll.axis), jso_add_flags);
				json_object_object_add_ex(scroll, "vector_length",
					json_object_new_double(wl_fixed_to_double(pointer->scroll.vector_length)),
					jso_add_flags);
			}
			json_object_object_add_ex(pointer_json, "focus", focus, jso_add_flags);
			json_object_object_add_ex(pointer_json, "button", button, jso_add_flags);
			json_object_object_add_ex(pointer_json, "scroll", scroll, jso_add_flags);
		}
		json_object_object_add_ex(seat_json, "pointer", pointer_json, jso_add_flags);
	}
}

static void send_state(bool force) {
	if (!state_events || (!state_dirty && !force)) {
		return;
	}

	json_object *state_json = json_object_new_object();

	json_object_object_add_ex(state_json, "userdata",
		json_object_get(state_userdata), jso_add_flags);

	describe_outputs(state_json);
	describe_seats(state_json);

	//struct timespec ts;
	//clock_gettime(CLOCK_MONOTONIC, &ts);
	//json_object_object_add_ex(state_json, "time",
	//	json_object_new_int64(ts.tv_sec * 1000 + ts.tv_nsec / 1000000),
	//	jso_add_flags);

	size_t state_len;
	const char *state = json_object_to_json_string_length(
		state_json, JSON_C_TO_STRING_PLAIN, &state_len);

	log_debug("sending state:\n%s", state);

	if ((state_len + stdout_buffer_index + 1) > stdout_buffer_size) {
		stdout_buffer_size = (state_len + stdout_buffer_index + 1) * 2;
		stdout_buffer = realloc(stdout_buffer, stdout_buffer_size);
	}

	memcpy(&stdout_buffer[stdout_buffer_index], state, state_len);
	stdout_buffer_index += state_len;
	stdout_buffer[stdout_buffer_index++] = '\n';

	json_object_put(state_json);

	state_dirty = false;
}

static void wl_buffer_release(void *data, MAYBE_UNUSED struct wl_buffer *wl_buffer) {
	struct buffer *buffer = data;
	buffer->busy = false;
	if (buffer->surface->dirty) {
		surface_render(buffer->surface);
	}
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static struct buffer *buffer_create(int32_t width, int32_t height,
		struct surface *surface) {
	struct buffer *buffer = malloc(sizeof(struct buffer));

	struct timespec ts = { 0 };
	pid_t pid = getpid();
	char shm_name[NAME_MAX];

generate_shm_name:
	clock_gettime(CLOCK_MONOTONIC, &ts);
	snprintf(shm_name, sizeof(shm_name),"/sbar-%d-%ld-%ld",
			pid, ts.tv_sec, ts.tv_nsec);

	int shm_fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (shm_fd == -1) {
		if (errno == EEXIST) {
			goto generate_shm_name;
		} else {
			abort_(errno, "shm_open: %s", strerror(errno));
		}
	} else {
		shm_unlink(shm_name);
	}

	int32_t stride = width * 4;
	buffer->size = (uint32_t)stride * (uint32_t)height;
	while (ftruncate(shm_fd, buffer->size) == -1) {
		if (errno == EINTR) {
			continue;
		} else {
			abort_(errno, "ftruncate: %s", strerror(errno));
		}
	}

	buffer->pixels = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (buffer->pixels == MAP_FAILED) {
		abort_(errno, "mmap: %s", strerror(errno));
	}

	buffer->image = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			buffer->pixels, stride);
	if (buffer->image == NULL) {
		abort_(1, "pixman_image_create_bits_no_clear failed");
	}

	struct wl_shm_pool *wl_shm_pool =
		wl_shm_create_pool(wl_shm, shm_fd, (int32_t)buffer->size);
	buffer->wl_buffer = wl_shm_pool_create_buffer(wl_shm_pool, 0, width,
			height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer->wl_buffer, &wl_buffer_listener, buffer);
	wl_shm_pool_destroy(wl_shm_pool);
	close(shm_fd);

	buffer->surface = surface;
	buffer->busy = false;

	return buffer;
}

static void buffer_destroy(struct buffer *buffer) {
	if (!buffer) {
		return;
	}

	if (buffer->image) {
		pixman_image_unref(buffer->image);
	}
	if (buffer->wl_buffer) {
		wl_buffer_destroy(buffer->wl_buffer);
	}
	if (buffer->pixels) {
		munmap(buffer->pixels, buffer->size);
	}

	free(buffer);
}

static void surface_init(struct surface *surface) {
	surface->wl_surface = wl_compositor_create_surface(wl_compositor);
	wl_surface_set_user_data(surface->wl_surface, surface);

	ptr_array_init(&surface->blocks, 20);
	ptr_array_init(&surface->popups, 4);
	array_init(&surface->block_boxes, 20, sizeof(struct block_box));
	array_init(&surface->input_regions, 4, sizeof(struct box));
}

static void popup_destroy(struct surface *popup);

static void surface_fini(struct surface *surface) {
	for (size_t i = 0; i < surface->popups.len; ++i) {
		popup_destroy(surface->popups.items[i]);
	}

	buffer_destroy(surface->buffer);

	for (size_t i = 0; i < surface->blocks.len; ++i) {
		block_unref(surface->blocks.items[i]);
	}

	ptr_array_fini(&surface->popups);
	ptr_array_fini(&surface->blocks);
	array_fini(&surface->block_boxes);
	array_fini(&surface->input_regions);

	json_object_put(surface->userdata);

	for (size_t i = 0; i < seats.len; ++i) {
		struct seat *seat = seats.items[i];
		if (seat->pointer.focus.surface == surface) {
			seat->pointer.focus.surface = NULL;
		}
	}
}

static void popup_destroy(struct surface *popup) {
	if (popup == NULL) {
		return;
	}

	surface_fini(popup);

	if (popup->xdg_positioner) {
		xdg_positioner_destroy(popup->xdg_positioner);
	}
	if (popup->xdg_popup) {
		xdg_popup_destroy(popup->xdg_popup);
	}
	if (popup->xdg_surface) {
		xdg_surface_destroy(popup->xdg_surface);
	}
	if (popup->wl_surface) {
		wl_surface_destroy(popup->wl_surface);
	}

	free(popup);
}

static void xdg_popup_configure(void *data, MAYBE_UNUSED struct xdg_popup *xdg_popup,
		MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y, int32_t width, int32_t height) {
	struct surface *popup = data;
	assert(width > 0);
	assert(height > 0);

	width *= popup->scale;
	height *= popup->scale;
	if ((popup->width != width) || (popup->height != height)) {
		buffer_destroy(popup->buffer);
		popup->buffer = buffer_create(width, height, popup);
		popup->width = width;
		popup->height = height;
		popup->dirty = true;
	}
}

static void xdg_popup_done(void *data, MAYBE_UNUSED struct xdg_popup *xdg_popup) {
	struct surface *popup = data;
	for (size_t i = 0; i < popup->parent->popups.len; ++i) {
		if (popup->parent->popups.items[i] == popup) {
			ptr_array_put(&popup->parent->popups, i, NULL);
			popup_destroy(popup);
			state_dirty = true;
			return;
		}
	}
}

static void xdg_popup_repositioned(void *data, MAYBE_UNUSED struct xdg_popup *xdg_popup,
		MAYBE_UNUSED uint32_t token)
{
	struct surface *popup = data;
	popup->dirty = true;
}

static const struct xdg_popup_listener xdg_popup_listener = {
	.configure = xdg_popup_configure,
	.popup_done = xdg_popup_done,
	.repositioned = xdg_popup_repositioned,
};

static void popup_xdg_surface_configure(void *data, MAYBE_UNUSED struct xdg_surface *xdg_surface,
		uint32_t serial) {
	struct surface *popup = data;

	xdg_surface_ack_configure(popup->xdg_surface, serial);

	if (popup->dirty) {
		surface_render(popup);
	}
}

static const struct xdg_surface_listener popup_xdg_surface_listener = {
	.configure = popup_xdg_surface_configure,
};

static void parse_cursor_shape(json_object *cursor_shape_json, struct surface *surface) {
	int32_t tmp = json_object_is_type(cursor_shape_json, json_type_int)
			? json_object_get_int(cursor_shape_json) : -1;
	enum wp_cursor_shape_device_v1_shape cursor_shape;
	switch ((enum wp_cursor_shape_device_v1_shape)tmp) {
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT:
		cursor_shape = (enum wp_cursor_shape_device_v1_shape)tmp;
		break;
	default:
		cursor_shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
		break;
	}
	if (surface->cursor_shape != cursor_shape) {
		for (size_t i = 0; i < seats.len; ++i) {
			struct seat *seat = seats.items[i];
			if (seat->pointer.cursor_shape_device
					&& (seat->pointer.focus.surface == surface)) {
				wp_cursor_shape_device_v1_set_shape(seat->pointer.cursor_shape_device,
					seat->pointer.focus.wl_pointer_enter_serial, cursor_shape);
			}
		}
		surface->cursor_shape = cursor_shape;
	}
}

static bool parse_blocks(json_object *blocks_array, struct surface *surface) {
	bool r = false;

	size_t _blocks_len = 0;
	if (json_object_is_type(blocks_array, json_type_array)) {
		_blocks_len = json_object_array_length(blocks_array);
		for (size_t i = 0; i < _blocks_len; ++i) {
			json_object *block_json = json_object_array_get_idx(blocks_array, i);
			json_object *id_json;
			json_object_object_get_ex(block_json, "id", &id_json);
			uint64_t id = json_object_is_type(id_json, json_type_int)
					? json_object_get_uint64(id_json) : 0;
			struct block *block = (i < surface->blocks.len) ? surface->blocks.items[i] : NULL;
			if ((block == NULL) || (id == 0) || (block->id != id)) {
				ptr_array_insert(&surface->blocks, i, block_get(block_json, id));
				r = true;
			}
		}
	}
	if (surface->blocks.len > _blocks_len) {
		for (size_t i = _blocks_len; i < surface->blocks.len; ++i) {
			block_unref(surface->blocks.items[i]);
		}
		surface->blocks.len = _blocks_len;
		r = true;
	}
	if (r) {
		if (surface->block_boxes.size < _blocks_len) {
			array_resize(&surface->block_boxes, _blocks_len * 2);
		}
		if (_blocks_len > 0) {
			memset(surface->block_boxes.items, 0, _blocks_len * sizeof(struct block_box));
		}
		surface->block_boxes.len = _blocks_len;
	}

	return r;
}

static void popup_configure_xdg_positioner(struct surface *popup) {
	xdg_positioner_set_size(popup->xdg_positioner,
		popup->wanted_width / popup->scale,
		popup->wanted_height / popup->scale);
	xdg_positioner_set_anchor_rect(popup->xdg_positioner,
		popup->wanted_x / popup->scale,
		popup->wanted_y / popup->scale,
		1, 1);
	xdg_positioner_set_gravity(popup->xdg_positioner, popup->gravity);
	xdg_positioner_set_constraint_adjustment(
		popup->xdg_positioner, popup->constraint_adjustment);
	xdg_positioner_set_reactive(popup->xdg_positioner); // ? TODO: make this an option
	//xdg_positioner_set_offset(popup->xdg_positioner, 0, 0);

	//xdg_positioner_set_anchor(popup->xdg_positioner, );
	//xdg_positioner_set_parent_size(popup->xdg_positioner, , );
	//xdg_positioner_set_parent_configure(popup->xdg_positioner, );
}

static bool parse_input_regions(struct surface *surface, json_object *input_regions_array) {
	array_t new_input_regions = { 0 };
	if (json_object_is_type(input_regions_array, json_type_array)) {
		size_t len = json_object_array_length(input_regions_array);
		if (len > 0) {
			array_init(&new_input_regions, len, sizeof(struct box));
			for (size_t i = 0; i < len; ++i) {
				json_object *box_json = json_object_array_get_idx(input_regions_array, i);
				json_object *x, *y, *width, *height;
				json_object_object_get_ex(box_json, "x", &x);
				json_object_object_get_ex(box_json, "y", &y);
				json_object_object_get_ex(box_json, "width", &width);
				json_object_object_get_ex(box_json, "height", &height);

				struct box box = {
					.x = json_object_is_type(x, json_type_int) ?
						json_object_get_int(x) : 0,
					.y = json_object_is_type(y, json_type_int) ?
						json_object_get_int(y) : 0,
					.width = json_object_is_type(width, json_type_int) ?
						json_object_get_int(width) : 0,
					.height = json_object_is_type(height, json_type_int) ?
						json_object_get_int(height) : 0,
				};
				array_add(&new_input_regions, &box);
			}
		}
	}

	if ((surface->input_regions.len == new_input_regions.len) &&
			(!new_input_regions.items ||
				(memcmp(surface->input_regions.items, new_input_regions.items,
				new_input_regions.elm_size * new_input_regions.len) == 0))) {
		array_fini(&new_input_regions);
		return false;
	} else {
		array_fini(&surface->input_regions);
		if (new_input_regions.len > 0) {
			struct wl_region *input_region = wl_compositor_create_region(wl_compositor);
			for (size_t i = 0; i < new_input_regions.len; ++i) {
				struct box *box = &((struct box *)new_input_regions.items)[i];
				wl_region_add(input_region, box->x, box->y, box->width, box->height);
			}
			wl_surface_set_input_region(surface->wl_surface, input_region);
			wl_region_destroy(input_region);
		} else {
			wl_surface_set_input_region(surface->wl_surface, NULL);
		}
		surface->input_regions = new_input_regions;
		return true;
	}
}

static void parse_popups(json_object *popups_array, ptr_array_t *dest, // struct surface * , NULL
		struct surface *parent);

static bool popup_configure(struct surface *popup, json_object *popup_json) {
	json_object *x_json, *y_json, *width, *height, *vertical_json;
	json_object *gravity_json, *constraint_adjustment_json, *render_json;
	json_object *input_regions_array, *cursor_shape, *blocks_array, *userdata;
	json_object_object_get_ex(popup_json, "x", &x_json);
	json_object_object_get_ex(popup_json, "y", &y_json);
	json_object_object_get_ex(popup_json, "width", &width);
	json_object_object_get_ex(popup_json, "height", &height);
	json_object_object_get_ex(popup_json, "vertical", &vertical_json);
	json_object_object_get_ex(popup_json, "gravity", &gravity_json);
	json_object_object_get_ex(
		popup_json, "constraint_adjustment", &constraint_adjustment_json);
	json_object_object_get_ex(popup_json, "render", &render_json);
	json_object_object_get_ex(popup_json, "cursor_shape", &cursor_shape);
	json_object_object_get_ex(popup_json, "input_regions", &input_regions_array);
	json_object_object_get_ex(popup_json, "blocks", &blocks_array);
	json_object_object_get_ex(popup_json, "userdata", &userdata);

	if ((!json_object_is_type(x_json, json_type_int))
			|| (!json_object_is_type(y_json, json_type_int))) {
		return false;
	}

	bool render = false, reposition = false;
	bool vertical = json_object_is_type(vertical_json, json_type_boolean)
			? json_object_get_boolean(vertical_json) : true;
	if (popup->vertical != vertical) {
		popup->vertical = vertical;
		render = true;
	}

	if (parse_blocks(blocks_array, popup)) {
		render = true;
	}
	int32_t wanted_width = json_object_is_type(width, json_type_int)
			? (int32_t)json_object_get_uint64(width) : 0;
	if (wanted_width == 0) {
		struct block_box box;
		for (size_t i = 0; i < popup->blocks.len; ++i) {
			struct block *block = popup->blocks.items[i];
			block_get_size(block, popup, (i > 0) ? &box : NULL, &box);
			if (block && block->render && (block->anchor != SBAR_BLOCK_ANCHOR_NONE)) {
				if (vertical) {
					if (box.width > wanted_width) {
						wanted_width = box.width;
					}
				} else {
					wanted_width += box.width;
				}
			}
		}
	}
	int32_t wanted_height = json_object_is_type(height, json_type_int)
			? (int32_t)json_object_get_uint64(height) : 0;
	if (wanted_height == 0) {
		struct block_box box;
		for (size_t i = 0; i < popup->blocks.len; ++i) {
			struct block *block = popup->blocks.items[i];
			block_get_size(block, popup, (i > 0) ? &box : NULL, &box);
			if (block && block->render && (block->anchor != SBAR_BLOCK_ANCHOR_NONE)) {
				if (vertical) {
					wanted_height += box.height;
				} else if (box.height > wanted_height) {
					wanted_height = box.height;
				}
			}
		}
	}
	if ((wanted_width == 0) || (wanted_height == 0)) {
		return false;
	}

	int32_t x = json_object_get_int(x_json), y = json_object_get_int(y_json);
	if ((x != popup->wanted_x) || (y != popup->wanted_y)
			|| (popup->wanted_width != wanted_width)
			|| (popup->wanted_height != wanted_height)) {
		popup->wanted_x = x;
		popup->wanted_y = y;
		popup->wanted_width = wanted_width;
		popup->wanted_height = wanted_height;
		reposition = true;
	}

	parse_cursor_shape(cursor_shape, popup);

	int32_t tmp = json_object_is_type(gravity_json, json_type_int)
		? json_object_get_int(gravity_json) : -1;
	enum xdg_positioner_gravity gravity;
	switch ((enum sbar_popup_gravity)tmp) {
	default:
	case SBAR_POPUP_GRAVITY_DEFAULT:
	case SBAR_POPUP_GRAVITY_NONE:
		gravity = XDG_POSITIONER_GRAVITY_NONE;
		break;
	case SBAR_POPUP_GRAVITY_TOP:
		gravity = XDG_POSITIONER_GRAVITY_TOP;
		break;
	case SBAR_POPUP_GRAVITY_BOTTOM:
		gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
		break;
	case SBAR_POPUP_GRAVITY_LEFT:
		gravity = XDG_POSITIONER_GRAVITY_LEFT;
		break;
	case SBAR_POPUP_GRAVITY_RIGHT:
		gravity = XDG_POSITIONER_GRAVITY_RIGHT;
		break;
	case SBAR_POPUP_GRAVITY_TOP_LEFT:
		gravity = XDG_POSITIONER_GRAVITY_TOP_LEFT;
		break;
	case SBAR_POPUP_GRAVITY_BOTTOM_LEFT:
		gravity = XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
		break;
	case SBAR_POPUP_GRAVITY_TOP_RIGHT:
		gravity = XDG_POSITIONER_GRAVITY_TOP_RIGHT;
		break;
	case SBAR_POPUP_GRAVITY_BOTTOM_RIGHT:
		gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
		break;
	}
	if (popup->gravity != gravity) {
		popup->gravity = gravity;
		reposition = true;
	}

	tmp = json_object_is_type(constraint_adjustment_json, json_type_int)
		? (int32_t)json_object_get_uint64(constraint_adjustment_json) : 0;
	enum xdg_positioner_constraint_adjustment constraint_adjustment = // TODO: proper error check
		(tmp > 0) ? (enum xdg_positioner_constraint_adjustment)tmp
		: XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE;
	if (popup->constraint_adjustment != constraint_adjustment) {
		popup->constraint_adjustment = constraint_adjustment;
		reposition = true;
	}

	tmp = json_object_is_type(render_json, json_type_boolean)
		? json_object_get_boolean(render_json) : true;
	if (tmp != popup->render) {
		popup->render = tmp;
		render = true;
	}

	bool commit = parse_input_regions(popup, input_regions_array);

	json_object_put(popup->userdata);
	popup->userdata = json_object_get(userdata);

	if (reposition) {
		popup_configure_xdg_positioner(popup);
	}

	if (popup->xdg_popup) {
		if (reposition) {
			xdg_popup_reposition(popup->xdg_popup, popup->xdg_positioner, 0);
		} else if (render && popup->buffer) {
			surface_render(popup);
		} else if (commit) {
			wl_surface_commit(popup->wl_surface);
		}
		json_object *popups_array;
		json_object_object_get_ex(popup_json, "popups", &popups_array);
		parse_popups(popups_array, &popup->popups, popup);
	}

	return true;
}

static void wl_surface_enter(MAYBE_UNUSED void *data, MAYBE_UNUSED struct wl_surface *wl_surface,
		MAYBE_UNUSED struct wl_output *output)
{
}

static void wl_surface_leave(MAYBE_UNUSED void *data, MAYBE_UNUSED struct wl_surface *wl_surface,
		MAYBE_UNUSED struct wl_output *output)
{
}

static void wl_surface_preferred_buffer_transform(MAYBE_UNUSED void *data,
		MAYBE_UNUSED struct wl_surface *wl_surface, MAYBE_UNUSED uint32_t transform)
{
}

static void popup_wl_surface_preferred_buffer_scale(void *data,
		MAYBE_UNUSED struct wl_surface *wl_surface, int32_t factor) {
	struct surface *popup = data;
	if (popup->scale != factor) {
		popup->scale = factor;
		popup_configure_xdg_positioner(popup);
		xdg_popup_reposition(popup->xdg_popup, popup->xdg_positioner, 0);
	}
}

static const struct wl_surface_listener popup_wl_surface_listener = {
	.enter = wl_surface_enter,
	.leave = wl_surface_leave,
	.preferred_buffer_transform = wl_surface_preferred_buffer_transform,
	.preferred_buffer_scale = popup_wl_surface_preferred_buffer_scale,
};

static struct surface *popup_create(json_object *popup_json, struct surface *parent) {
	struct surface *popup = calloc(1, sizeof(struct surface));
	popup->type = SURFACE_TYPE_POPUP;
	surface_init(popup);
	popup->scale = surface_get_bar(parent)->output->scale;
	popup->xdg_surface =
		xdg_wm_base_get_xdg_surface(xdg_wm_base, popup->wl_surface);
	popup->xdg_positioner = xdg_wm_base_create_positioner(xdg_wm_base);
	popup->parent = parent;

	if (!popup_configure(popup, popup_json)) {
		goto error;
	}

	switch (parent->type) {
	case SURFACE_TYPE_BAR:
		popup->xdg_popup = xdg_surface_get_popup(
			popup->xdg_surface, NULL, popup->xdg_positioner);
		zwlr_layer_surface_v1_get_popup(parent->layer_surface, popup->xdg_popup);
		json_object *grab;
		json_object_object_get_ex(popup_json, "grab", &grab);
		if (json_object_is_type(grab, json_type_int)) {
			int64_t serial = json_object_get_int64(grab);
			for (size_t i = 0; i < seats.len; ++i) {
				struct seat *seat = seats.items[i];
				for (uint8_t j = (uint8_t)(seat->popup_grab.index - 1);
						(j < seat->popup_grab.serials.len) && (j != seat->popup_grab.index);
						--j) {
					if (((uint32_t *)seat->popup_grab.serials.items)[j] == serial) {
						popup->grab.seat = seat->wl_seat;
						popup->grab.serial = (uint32_t)serial;
						goto grab;
					}
				}
			}
			goto error;
		}
		break;
	case SURFACE_TYPE_POPUP:
		popup->xdg_popup = xdg_surface_get_popup(
			popup->xdg_surface, parent->xdg_surface, popup->xdg_positioner);
		popup->grab = parent->grab;
		break;
	default:
		assert(UNREACHABLE);
		goto error;
	}

	if (popup->grab.seat != NULL) {
grab:
		xdg_popup_grab(popup->xdg_popup,
			popup->grab.seat, popup->grab.serial);
	}

	json_object *popups_array;
	json_object_object_get_ex(popup_json, "popups", &popups_array);
	parse_popups(popups_array, &popup->popups, popup);

	wl_surface_add_listener(popup->wl_surface, &popup_wl_surface_listener, popup);
	xdg_surface_add_listener(popup->xdg_surface, &popup_xdg_surface_listener, popup);
	xdg_popup_add_listener(popup->xdg_popup, &xdg_popup_listener, popup);

	wl_surface_commit(popup->wl_surface);

	return popup;
error:
	popup_destroy(popup);
	return NULL;
}

static void bar_destroy(struct surface *bar) {
	if (bar == NULL) {
		return;
	}

	surface_fini(bar);

	if (bar->layer_surface) {
		zwlr_layer_surface_v1_destroy(bar->layer_surface);
	}
	if (bar->wl_surface) {
		wl_surface_destroy(bar->wl_surface);
	}

	free(bar);
}

static bool bar_configure(struct surface *bar, json_object *bar_json) {
	json_object *width, *height, *exclusive_zone_json, *anchor_json;
	json_object *layer_json, *margins_json[4], *cursor_shape, *render_json;
	json_object *input_regions_array, *userdata, *popups_array, *blocks_array;
	json_object_object_get_ex(bar_json, "width", &width);
	json_object_object_get_ex(bar_json, "height", &height);
	json_object_object_get_ex(bar_json, "exclusive_zone", &exclusive_zone_json);
	json_object_object_get_ex(bar_json, "anchor", &anchor_json);
	json_object_object_get_ex(bar_json, "layer", &layer_json);
	json_object_object_get_ex(bar_json, "margin_top", &margins_json[0]);
	json_object_object_get_ex(bar_json, "margin_right", &margins_json[1]);
	json_object_object_get_ex(bar_json, "margin_bottom", &margins_json[2]);
	json_object_object_get_ex(bar_json, "margin_left", &margins_json[3]);
	json_object_object_get_ex(bar_json, "cursor_shape", &cursor_shape);
	json_object_object_get_ex(bar_json, "render", &render_json);
	json_object_object_get_ex(bar_json, "input_regions", &input_regions_array);
	json_object_object_get_ex(bar_json, "userdata", &userdata);
	json_object_object_get_ex(bar_json, "popups", &popups_array);
	json_object_object_get_ex(bar_json, "blocks", &blocks_array);

	bool render = false, commit = false;
	if (parse_blocks(blocks_array, bar)) {
		render = true;
	}

	int32_t tmp = json_object_is_type(anchor_json, json_type_int)
		? json_object_get_int(anchor_json) : -1;
	bool vertical = false;
	enum zwlr_layer_surface_v1_anchor anchor;
	switch ((enum sbar_bar_anchor)tmp) {
	case SBAR_BAR_ANCHOR_TOP:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
		break;
	default:
	case SBAR_BAR_ANCHOR_DEFAULT:
	case SBAR_BAR_ANCHOR_BOTTOM:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		break;
	case SBAR_BAR_ANCHOR_LEFT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		vertical = true;
		break;
	case SBAR_BAR_ANCHOR_RIGHT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		vertical = true;
		break;
	}
	int32_t wanted_width = json_object_is_type(width, json_type_int)
			? (int32_t)json_object_get_uint64(width) : 0;
	if ((wanted_width == 0) && vertical) {
		struct block_box box;
		for (size_t i = 0; i < bar->blocks.len; ++i) {
			struct block *block = bar->blocks.items[i];
			block_get_size(block, bar, (i > 0) ? &box : NULL, &box);
			if ((box.width > wanted_width) && block && block->render
					&& (block->anchor != SBAR_BLOCK_ANCHOR_NONE)) {
				wanted_width = box.width;
			}
		}
	}
	int32_t wanted_height = json_object_is_type(height, json_type_int)
			? (int32_t)json_object_get_uint64(height) : 0;
	if ((wanted_height == 0) && !vertical) {
		struct block_box box;
		for (size_t i = 0; i < bar->blocks.len; ++i) {
			struct block *block = bar->blocks.items[i];
			block_get_size(block, bar, (i > 0) ? &box : NULL, &box);
			if (((box.height > wanted_height)) && block && block->render
					&& (block->anchor != SBAR_BLOCK_ANCHOR_NONE)) {
				wanted_height = box.height;
			}
		}
	}
	if ((wanted_width == 0) && (wanted_height == 0)) {
		return false;
	}
	if ((bar->wanted_width != wanted_width) || (bar->wanted_height != wanted_height)
			|| (bar->vertical != vertical)) {
		zwlr_layer_surface_v1_set_size(bar->layer_surface,
			(uint32_t)(wanted_width / bar->scale),
			(uint32_t)(wanted_height / bar->scale));
		bar->wanted_width = wanted_width;
		bar->wanted_height = wanted_height;
		bar->vertical = vertical;
		render = true;
	}

	tmp = json_object_is_type(exclusive_zone_json, json_type_int)
		? json_object_get_int(exclusive_zone_json) : -1;
	int32_t exclusive_zone = (tmp >= 0) ? tmp :
		(vertical ? wanted_width : wanted_height);
	if (bar->exclusive_zone != exclusive_zone) {
		zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface,
			exclusive_zone / bar->scale);
		bar->exclusive_zone = exclusive_zone;
		commit = true;
	}

	parse_cursor_shape(cursor_shape, bar);

	if (bar->anchor != anchor) {
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface, vertical
			? (anchor | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
			: (anchor | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT));
		bar->anchor = anchor;
		commit = true;
	}

	tmp = json_object_is_type(layer_json, json_type_int)
		? json_object_get_int(layer_json) : -1;
	enum zwlr_layer_shell_v1_layer layer;
	switch ((enum sbar_bar_layer)tmp) {
	case SBAR_BAR_LAYER_BACKGROUND:
		layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
		break;
	case SBAR_BAR_LAYER_BOTTOM:
		layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
		break;
	default:
	case SBAR_BAR_LAYER_DEFAULT:
	case SBAR_BAR_LAYER_TOP:
		layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
		break;
	case SBAR_BAR_LAYER_OVERLAY:
		layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
		break;
	}
	if (bar->layer != layer) {
		zwlr_layer_surface_v1_set_layer(bar->layer_surface, layer);
		bar->layer = layer;
		commit = true;
	}

	int32_t margins[4];
	for (size_t i = 0; i < LENGTH(margins); ++i) {
		margins[i] = json_object_is_type(margins_json[i], json_type_int)
			? (int32_t)json_object_get_uint64(margins_json[i]) : 0;
	}
	if (memcmp(bar->margins, margins, sizeof(margins)) != 0) {
		zwlr_layer_surface_v1_set_margin(bar->layer_surface,
			margins[0] / bar->scale,
			margins[1] / bar->scale,
			margins[2] / bar->scale,
			margins[3] / bar->scale);
		memcpy(bar->margins, margins, sizeof(margins));
		commit = true;
	}

	tmp = json_object_is_type(render_json, json_type_boolean)
		? json_object_get_boolean(render_json) : true;
	if (tmp != bar->render) {
		bar->render = tmp;
		render = true;
	}

	if (parse_input_regions(bar, input_regions_array)) {
		commit = true;
	}

	json_object_put(bar->userdata);
	bar->userdata = json_object_get(userdata);

	if (bar->buffer == NULL) {
		wl_surface_commit(bar->wl_surface);
	} else if (render) {
		surface_render(bar);
	} else if (commit) {
		wl_surface_commit(bar->wl_surface);
	}

	parse_popups(popups_array, &bar->popups, bar);

	return true;
}

static void bar_layer_surface_configure(void *data, MAYBE_UNUSED struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t _width, uint32_t _height) {
	struct surface *bar = data;

	zwlr_layer_surface_v1_ack_configure(bar->layer_surface, serial);

	int32_t width = (int32_t)_width * bar->scale;
	int32_t height = (int32_t)_height * bar->scale;
	if (((bar->height != height) || (bar->width != width))
			&& (width != 0) && (height != 0)) {
		buffer_destroy(bar->buffer);
		bar->buffer = buffer_create(width, height, bar);
		bar->width = width;
		bar->height = height;
		surface_render(bar);
	}
}

static void bar_layer_surface_closed(void *data, MAYBE_UNUSED struct zwlr_layer_surface_v1 *layer_surface) {
	struct surface *bar = data;
	struct output *output = bar->output;
	for (size_t i = 0; i < output->bars.len; ++i) {
		if (output->bars.items[i] == bar) {
			bar_destroy(bar);
			ptr_array_put(&output->bars, i, NULL);
			state_dirty = true;
			return;
		}
	}
}

static const struct zwlr_layer_surface_v1_listener bar_layer_surface_listener = {
	.configure = bar_layer_surface_configure, .closed = bar_layer_surface_closed,
};

static void bar_wl_surface_preferred_buffer_scale(void *data,
		MAYBE_UNUSED struct wl_surface *wl_surface, int32_t factor) {
	struct surface *bar = data;
	if (bar->scale != factor) {
		bar->scale = factor;
		zwlr_layer_surface_v1_set_size(bar->layer_surface,
				(uint32_t)(bar->wanted_width / factor),
				(uint32_t)(bar->wanted_height / factor));
		zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface,
				bar->exclusive_zone / factor);
		zwlr_layer_surface_v1_set_margin(bar->layer_surface,
				bar->margins[0] / factor,
				bar->margins[1] / factor,
				bar->margins[2] / factor,
				bar->margins[3] / factor);
		wl_surface_commit(bar->wl_surface);
	}
}

static const struct wl_surface_listener bar_wl_surface_listener = {
	.enter = wl_surface_enter,
	.leave = wl_surface_leave,
	.preferred_buffer_transform = wl_surface_preferred_buffer_transform,
	.preferred_buffer_scale = bar_wl_surface_preferred_buffer_scale,
};

static struct surface *bar_create(json_object *bar_json, struct output *output) {
	struct surface *bar = calloc(1, sizeof(struct surface));
	bar->type = SURFACE_TYPE_BAR;
	surface_init(bar);
	bar->scale = output->scale;
	bar->layer_surface =
		zwlr_layer_shell_v1_get_layer_surface(zwlr_layer_shell_v1, bar->wl_surface,
		output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "sbar");
	bar->output = output;
	bar->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;

	wl_surface_add_listener(bar->wl_surface, &bar_wl_surface_listener, bar);
	zwlr_layer_surface_v1_add_listener(bar->layer_surface, &bar_layer_surface_listener, bar);

	if (!bar_configure(bar, bar_json)) {
		bar_destroy(bar);
		return NULL;
	}

	return bar;
}

static void parse_popups(json_object *popups_array, ptr_array_t *dest, // struct surface * , NULL
		struct surface *parent) {
	size_t _popups_len = 0;
	if (json_object_is_type(popups_array, json_type_array)) {
		_popups_len = json_object_array_length(popups_array);
		for (size_t i = 0; i < _popups_len; ++i) {
			json_object *popup_json = json_object_array_get_idx(popups_array, i);
			struct surface *popup = (i < dest->len) ? dest->items[i] : NULL;
			if (popup == NULL) {
				ptr_array_put(dest, i, popup_create(popup_json, parent));
			} else if (!popup_configure(popup, popup_json)) {
				popup_destroy(popup);
				ptr_array_put(dest, i, NULL);
			}
		}
	}
	if (dest->len > _popups_len) {
		for (size_t i = _popups_len; i < dest->len; ++i) {
			popup_destroy(dest->items[i]);
		}
		dest->len = _popups_len;
	}
}

static void parse_json(const char *json_str) {
	json_object *json = json_tokener_parse(json_str);
	if (!json_object_is_type(json, json_type_object)) {
		log_debug("discard invalid json: %s", json_str);
		goto cleanup;
	}

	log_debug("parsing json:\n%s", json_str);

	json_object *userdata, *state_events_json;
	json_object_object_get_ex(json, "userdata", &userdata);
	json_object_object_get_ex(json, "state_events", &state_events_json);

	json_object_put(state_userdata);
	state_userdata = json_object_get(userdata);

	state_events = json_object_is_type(state_events_json, json_type_boolean)
		? json_object_get_boolean(state_events_json) : false;

	for (size_t o = 0; o < outputs.len; ++o) {
		struct output *output = outputs.items[o];
		if (output->name == NULL) {
			continue;
		}

		json_object *bars_array;
		size_t _bars_len = 0;
		json_object_object_get_ex(json, output->name, &bars_array);
		if (json_object_is_type(bars_array, json_type_array)) {
			_bars_len = json_object_array_length(bars_array);
			for (size_t i = 0; i < _bars_len; ++i) {
				json_object *bar_json = json_object_array_get_idx(bars_array, i);
				struct surface *bar = (i < output->bars.len) ? output->bars.items[i] : NULL;
				if (bar == NULL) {
					ptr_array_put(&output->bars, i, bar_create(bar_json, output));
				} else if (!bar_configure(bar, bar_json)) {
					bar_destroy(bar);
					ptr_array_put(&output->bars, i, NULL);
				}
			}
		}
		if (output->bars.len > _bars_len) {
			for (size_t i = _bars_len; i < output->bars.len; ++i) {
				bar_destroy(output->bars.items[i]);
			}
			output->bars.len = _bars_len;
		}
	}

	state_dirty = true;

cleanup:
	json_object_put(json);
}

static void read_stdin(void) {
    for (;;) {
		ssize_t read_bytes = read(STDIN_FILENO,
			&stdin_buffer[stdin_buffer_index],
			stdin_buffer_size - stdin_buffer_index);
		if (read_bytes <= 0) {
			if (read_bytes == 0) {
				errno = EPIPE;
			}
			if (errno == EAGAIN) {
				break;
			} else if (errno == EINTR) {
				continue;
			} else {
				abort_(errno, "read: %s", strerror(errno));
			}
		} else {
			stdin_buffer_index += (size_t)read_bytes;
			if (stdin_buffer_index == stdin_buffer_size) {
				stdin_buffer_size *= 2;
				stdin_buffer = realloc(stdin_buffer, stdin_buffer_size);
			}
		}
    }

	if (stdin_buffer_index > 0) {
		for (size_t n = stdin_buffer_index - 1; n > 0; --n) {
			if (stdin_buffer[n] == '\n') {
				stdin_buffer[n] = '\0';
				char *tmp = NULL;
				const char *json = strtok_r(stdin_buffer, "\n", &tmp);
				while (json) {
					parse_json(json);
					json = strtok_r(NULL, "\n", &tmp);
				}
				stdin_buffer_index -= ++n;
				memmove(stdin_buffer, &stdin_buffer[n], stdin_buffer_index);
			}
		}
	}
}

static void flush_stdout(void) {
	while (stdout_buffer_index > 0) {
		ssize_t written = write(STDOUT_FILENO, stdout_buffer, stdout_buffer_index);
		if (written == -1) {
			if (errno == EAGAIN) {
				poll_fds[1].fd = STDOUT_FILENO;
				break;
			} else if (errno == EINTR) {
				continue;
			} else {
				abort_(errno, "write: %s", strerror(errno));
			}
		} else {
			stdout_buffer_index -= (size_t)written;
			memmove(stdout_buffer, &stdout_buffer[written], stdout_buffer_index);
			poll_fds[1].fd = -1;
		}
	}
}

static void wl_pointer_enter(void *data, MAYBE_UNUSED struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *wl_surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	if (wl_surface == NULL) {
		return;
	}

	struct seat *seat = data;
	struct pointer *pointer = &seat->pointer;

	struct surface *surface = wl_surface_get_user_data(wl_surface);

	if (pointer->cursor_shape_device) {
		wp_cursor_shape_device_v1_set_shape(pointer->cursor_shape_device,
				serial, surface->cursor_shape);
	}

	pointer->focus.surface = surface;
	pointer->focus.wl_pointer_enter_serial = serial;
	pointer->focus.x = wl_fixed_to_double(surface_x);
	pointer->focus.y = wl_fixed_to_double(surface_y);

	send_state(true);
}

static void wl_pointer_leave(void *data, MAYBE_UNUSED struct wl_pointer *wl_pointer,
		MAYBE_UNUSED uint32_t serial, MAYBE_UNUSED struct wl_surface *surface) {
	struct seat *seat = data;
	struct pointer *pointer = &seat->pointer;

	memset(&pointer->focus, 0, sizeof(pointer->focus));
	send_state(true);
}

static void wl_pointer_motion(void *data, MAYBE_UNUSED struct wl_pointer *wl_pointer,
		MAYBE_UNUSED uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct seat *seat = data;
	struct pointer *pointer = &seat->pointer;
	if (pointer->focus.surface == NULL) {
		return;
	}

	pointer->focus.x = wl_fixed_to_double(surface_x);
	pointer->focus.y = wl_fixed_to_double(surface_y);

	send_state(true);
}

static void wl_pointer_button(void *data, MAYBE_UNUSED struct wl_pointer *wl_pointer,
		uint32_t serial, MAYBE_UNUSED uint32_t time, uint32_t button, uint32_t state) {
	struct seat *seat = data;
	struct pointer *pointer = &seat->pointer;
	if (pointer->focus.surface == NULL) {
		return;
	}

	assert((state == WL_POINTER_BUTTON_STATE_PRESSED) || (state == WL_POINTER_BUTTON_STATE_RELEASED));

	pointer->button.code = button;
	pointer->button.state = state;
	pointer->button.serial = serial;

	send_state(true);

	array_put(&seat->popup_grab.serials, seat->popup_grab.index++, &serial);
	memset(&pointer->button, 0, sizeof(pointer->button));
}

static void wl_pointer_axis(void *data, MAYBE_UNUSED struct wl_pointer *wl_pointer,
		MAYBE_UNUSED uint32_t time, uint32_t axis, wl_fixed_t value) {
	struct seat *seat = data;
	struct pointer *pointer = &seat->pointer;
	if (pointer->focus.surface == NULL) {
		return;
	}

	assert((axis == WL_POINTER_AXIS_VERTICAL_SCROLL) || (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL));

	pointer->scroll.axis = axis;
	pointer->scroll.vector_length = value;

	send_state(true);

	memset(&pointer->scroll, 0, sizeof(pointer->scroll));
}

static const struct wl_pointer_listener wl_pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
};

static void xdg_wm_base_ping(MAYBE_UNUSED void *data, MAYBE_UNUSED struct xdg_wm_base *xdg_wm_base_,
		uint32_t serial) {
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void wl_output_geometry(void *data, MAYBE_UNUSED struct wl_output *wl_output,
		MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y,
		MAYBE_UNUSED int32_t physical_width, MAYBE_UNUSED int32_t physical_height,
		MAYBE_UNUSED int32_t subpixel, MAYBE_UNUSED const char *make,
		MAYBE_UNUSED const char *model, int32_t transform) {
	assert((transform >= 0) && (transform <= 7));
	struct output *output = data;
	if (output->transform != (enum wl_output_transform)transform) {
		output->transform = (enum wl_output_transform)transform;
		state_dirty = true;
	}
}

static void wl_output_mode(void *data, MAYBE_UNUSED struct wl_output *wl_output,
		MAYBE_UNUSED uint32_t flags, int32_t width, int32_t height, MAYBE_UNUSED int32_t refresh) {
	struct output *output = data;
	if ((output->width != width) || (output->height != height)) {
		output->width = width;
		output->height = height;
		state_dirty = true;
	}
}

static void wl_output_done(MAYBE_UNUSED void *data, MAYBE_UNUSED struct wl_output *wl_output)
{
}

static void wl_output_scale(void *data, MAYBE_UNUSED struct wl_output *wl_output,
		int32_t factor) {
	assert(factor > 0);
	struct output *output = data;
	if (output->scale != factor) {
		output->scale = factor;
		state_dirty = true;
	}
}

static void wl_output_name(void *data, MAYBE_UNUSED struct wl_output *wl_output,
		const char *name) {
	struct output *output = data;
	assert(output->name == NULL);
	output->name = strdup(name);
	state_dirty = true;
}

static void wl_output_description(MAYBE_UNUSED void *data, MAYBE_UNUSED struct wl_output *wl_output,
		MAYBE_UNUSED const char *description)
{
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = wl_output_geometry,
	.mode = wl_output_mode,
	.done = wl_output_done,
	.scale = wl_output_scale,
	.name = wl_output_name,
	.description = wl_output_description,
};

static void wl_seat_capabilities(void *data, MAYBE_UNUSED struct wl_seat *wl_seat,
		uint32_t capabilities) {
	struct seat *seat = data;

	bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	// TODO: touch

	if (have_pointer && !seat->pointer.wl_pointer) {
		seat->pointer.wl_pointer = wl_seat_get_pointer(seat->wl_seat);
		if (wp_cursor_shape_manager_v1 && !seat->pointer.cursor_shape_device) {
			seat->pointer.cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
				wp_cursor_shape_manager_v1, seat->pointer.wl_pointer);
		}
		wl_pointer_add_listener(
			seat->pointer.wl_pointer, &wl_pointer_listener, seat);
		state_dirty = true;
	} else if (!have_pointer && seat->pointer.wl_pointer) {
		wl_pointer_destroy(seat->pointer.wl_pointer);
		if (seat->pointer.cursor_shape_device) {
			wp_cursor_shape_device_v1_destroy(seat->pointer.cursor_shape_device);
		}
		memset(&seat->pointer, 0, sizeof(seat->pointer));
		state_dirty = true;
	}
}

static void wl_seat_name(void *data, MAYBE_UNUSED struct wl_seat *wl_seat,
		const char *name) {
	struct seat *seat = data;
	assert(seat->name == NULL);
	seat->name = strdup(name);
	state_dirty = true;
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_capabilities, .name = wl_seat_name,
};

static void output_free(struct output *output) {
	if (!output) {
		return;
	}

	for (size_t i = 0; i < output->bars.len; ++i) {
		bar_destroy(output->bars.items[i]);
	}
	ptr_array_fini(&output->bars);

	if (output->wl_output) {
		wl_output_destroy(output->wl_output);
	}
	free(output->name);

	free(output);
}

static void seat_free(struct seat *seat) {
	if (!seat) {
		return;
	}

	if (seat->pointer.wl_pointer) {
		wl_pointer_destroy(seat->pointer.wl_pointer);
	}
	if (seat->pointer.cursor_shape_device) {
		wp_cursor_shape_device_v1_destroy(seat->pointer.cursor_shape_device);
	}
	free(seat->name);
	if (seat->wl_seat) {
		wl_seat_destroy(seat->wl_seat);
	}

	array_fini(&seat->popup_grab.serials);

	free(seat);
}

static void wl_registry_global(MAYBE_UNUSED void *data, MAYBE_UNUSED struct wl_registry *registry,
		uint32_t name, const char *interface, MAYBE_UNUSED uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *output = calloc(1, sizeof(struct output));
		output->wl_output = wl_registry_bind(wl_registry,
			name, &wl_output_interface, 4);
		output->wl_name = name;
		output->scale = 1;
		ptr_array_init(&output->bars, 4);
		wl_output_add_listener(output->wl_output, &wl_output_listener, output);
		ptr_array_add(&outputs, output);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct seat *seat = calloc(1, sizeof(struct seat));
		seat->wl_seat = wl_registry_bind(wl_registry,
			name, &wl_seat_interface, 2);
		seat->wl_name = name;
		array_init(&seat->popup_grab.serials, 256, sizeof(uint32_t));
		wl_seat_add_listener(seat->wl_seat, &wl_seat_listener, seat);
		ptr_array_add(&seats, seat);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		wl_compositor = wl_registry_bind(wl_registry, name,
			&wl_compositor_interface, 6);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
		// ? TODO: wl_shm_add_listener (check for ARGB32)
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		zwlr_layer_shell_v1 = wl_registry_bind(wl_registry, name,
			&zwlr_layer_shell_v1_interface, 2);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(wl_registry,
			name, &xdg_wm_base_interface, 3);
		xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		wp_cursor_shape_manager_v1 = wl_registry_bind(wl_registry, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	}
}

static void wl_registry_global_remove(MAYBE_UNUSED void *data, MAYBE_UNUSED struct wl_registry *registry,
		uint32_t name) {
	for (size_t i = 0; i < outputs.len; ++i) {
		struct output *output = outputs.items[i];
		if (output->wl_name == name) {
			output_free(output);
			ptr_array_pop(&outputs, i);
			state_dirty = true;
			return;
		}
	}

	for (size_t i = 0; i < seats.len; ++i) {
		struct seat *seat = seats.items[i];
		if (seat->wl_name == name) {
			seat_free(seat);
			ptr_array_pop(&seats, i);
			state_dirty = true;
			return;
		}
	}
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_global, .global_remove = wl_registry_global_remove,
};

static void signal_handler(MAYBE_UNUSED int sig) {
	running = false;
}

static const struct sigaction sigact = {
	.sa_handler = &signal_handler
};

static void setup(void) {
	wl_display = wl_display_connect(NULL);
	if (wl_display == NULL) {
		abort_(1, "wl_display_connect failed");
	}
	ptr_array_init(&outputs, 4);
	ptr_array_init(&seats, 4);
	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &wl_registry_listener, NULL);
	if (wl_display_roundtrip(wl_display) == -1) {
		abort_(1, "wl_display_roundtrip failed");
	}
	assert(wl_compositor != NULL);
	assert(wl_shm != NULL);
	if (zwlr_layer_shell_v1 == NULL) {
		abort_(EPROTONOSUPPORT, "Missing wayland protocol: zwlr_layer_shell_v1");
	}
	if (xdg_wm_base == NULL) {
		abort_(EPROTONOSUPPORT, "Missing wayland protocol: xdg_wm_base");
	}
	if (wp_cursor_shape_manager_v1 == NULL) {
		log_stderr("Missing wayland protocol: wp_cursor_shape_manager_v1."
			"Cursor may be rendered incorrectly");
	}
	if (wl_display_roundtrip(wl_display) == -1) {
		abort_(1, "wl_display_roundtrip failed");
	}
	poll_fds[2].fd = wl_display_get_fd(wl_display);

	char *locale = setlocale(LC_CTYPE, "");
	if ((locale == NULL) || (strstr(locale, "UTF-8") == NULL)) {
		abort_(1, "failed to set a UTF-8 locale");
	}
	if (!fcft_init(FCFT_LOG_COLORIZE_NEVER, false, FCFT_LOG_CLASS_WARNING)) {
		abort_(1, "fcft_init failed");
	}
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);

#if HAVE_SVG
	resvg_init_log();
#endif // HAVE_SVG

	if (fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK) == -1) {
		abort_(errno, "STDOUT_FILENO O_NONBLOCK fcntl: %s", strerror(errno));
	}
	if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1) {
		abort_(errno, "STDIN_FILENO O_NONBLOCK fcntl: %s", strerror(errno));
	}

	stdin_buffer_size = stdout_buffer_size = 4096;
	stdin_buffer = malloc(stdin_buffer_size);
	stdout_buffer = malloc(stdout_buffer_size);

	ptr_array_init(&blocks_with_id, 100);
	ptr_array_init(&image_cache, 100);

	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
}

static void run(void) {
	while (running) {
		if ((poll(poll_fds, LENGTH(poll_fds), -1) == -1) && (errno != EINTR)) {
			abort_(errno, "poll: %s", strerror(errno));
		}
		if (poll_fds[0].revents & POLLHUP) {
			break;
		}
		if (poll_fds[0].revents & (POLLERR | POLLNVAL)) {
			abort_(poll_fds[0].revents, "stdin poll error");
		}
		if (poll_fds[1].fd != -1) {
			if (poll_fds[1].revents & POLLHUP) {
				break;
			}
			if (poll_fds[1].revents & (POLLERR | POLLNVAL)) {
				abort_(poll_fds[1].revents, "stdout poll error");
			}
		}
		if (poll_fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			abort_(poll_fds[2].revents, "wayland display poll error");
		}

		if (poll_fds[0].revents & POLLIN) {
			read_stdin();
		}

		if (poll_fds[2].revents & POLLIN) {
			if (wl_display_dispatch(wl_display) == -1) {
				abort_(errno, "wl_display_dispatch: %s", strerror(errno));
			}
		}

		send_state(false);
		flush_stdout();

		if (wl_display_flush(wl_display) == -1) {
			if (errno == EAGAIN) {
				poll_fds[2].events = (POLLIN | POLLOUT);
			} else {
				abort_(errno, "wl_display_flush: %s", strerror(errno));
			}
		} else {
			poll_fds[2].events = POLLIN;
		}
	}
}

#if DEBUG
static void cleanup(void) {
	for (size_t i = 0; i < outputs.len; ++i) {
		output_free(outputs.items[i]);
	}
	ptr_array_fini(&outputs);

	for (size_t i = 0; i < seats.len; ++i) {
		seat_free(seats.items[i]);
	}
	ptr_array_fini(&seats);

	ptr_array_fini(&blocks_with_id);

	for (size_t i = 0; i < image_cache.len; ++i) {
		free_image_cache(image_cache.items[i]);
	}
	ptr_array_fini(&image_cache);

	fcft_fini();

	if (wp_cursor_shape_manager_v1) {
		wp_cursor_shape_manager_v1_destroy(wp_cursor_shape_manager_v1);
	}
	zwlr_layer_shell_v1_destroy(zwlr_layer_shell_v1);
	xdg_wm_base_destroy(xdg_wm_base);
	wl_compositor_destroy(wl_compositor);
	wl_shm_destroy(wl_shm);
	wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);

	json_object_put(state_userdata);

	free(stdin_buffer);
	free(stdout_buffer);
}
#endif // DEBUG

int main(int argc, char **argv) {
	static const struct option long_options[] = {
		{"version", no_argument, NULL, 'v'},
		{ 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "v", long_options, NULL)) != -1) {
		switch (c) {
		case 'v':
			abort_(0, VERSION);
		default:
			break;
		}
	}

	setup();
	run();
#if DEBUG
	cleanup();
#endif // DEBUG

	return EXIT_SUCCESS;
}
