#if !defined(SBAR_JSON_H)
#define SBAR_JSON_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>

#include <json_tokener.h>
#include <json_object.h>

#include "sbar.h"
#include "macros.h"
#include "util.h"

enum sbar_json_surface_type {
	SBAR_JSON_SURFACE_TYPE_BAR,
	SBAR_JSON_SURFACE_TYPE_POPUP,
};

struct sbar_json_surface_type_bar {
	int32_t exclusive_zone;
	enum sbar_bar_anchor anchor;
	enum sbar_bar_layer layer;
	int32_t margins[4]; // top, right, bottom, left
};

struct sbar_json_surface_type_popup {
	int32_t x, y;
	enum sbar_popup_gravity gravity;
	uint32_t constraint_adjustment; // enum sbar_popup_constraint_adjustment |
	uint32_t grab_serial;
	bool vertical, grab;
};

struct sbar_json_surface;
typedef void (*sbar_json_surface_destroy_t)(struct sbar_json_surface *);

typedef void (*sbar_json_pointer_motion_callback_t)(struct sbar_json_surface *surface,
		double x, double y);

struct sbar_json_surface {
	struct sbar_json_connection *connection;

	enum sbar_json_surface_type type;
	union {
		struct sbar_json_surface_type_bar bar;
		struct sbar_json_surface_type_popup popup;
	};

	uint64_t id;
	int32_t wanted_width, wanted_height;
	enum sbar_surface_cursor_shape cursor_shape;
	bool render;
	array_t input_regions; // struct box
	list_t popups;
	ptr_array_t blocks; // struct sbar_json_block *
	list_t link;

	void (*updated_callback)(struct sbar_json_surface *surface);

	sbar_json_pointer_motion_callback_t pointer_enter_callback;
	sbar_json_pointer_motion_callback_t pointer_motion_callback;
	void (*pointer_button_callback)(struct sbar_json_surface *surface,
		uint32_t code, enum sbar_pointer_button_state state,
		uint32_t serial, double x, double y);
	void (*pointer_scroll_callback)(struct sbar_json_surface *surface,
		enum sbar_pointer_axis axis, double vec_len, double x, double y);
	void (*pointer_leave_callback)(struct sbar_json_surface *surface);

	sbar_json_surface_destroy_t destroy;

	array_t block_hotspots; // struct sbar_json_box
	int32_t width, height;
	int32_t scale;
};

struct sbar_json_block_type_text {
	char *text;
	ptr_array_t font_names; // char *
	char *font_attributes;
	uint32_t color; // ARGB32
};

struct sbar_json_block_type_image {
	char *path;
	enum sbar_block_type_image_image_type type;
};

struct sbar_json_block_type_composite {
	ptr_array_t blocks; // struct sbar_json_block *
};

struct sbar_json_block_border {
	int32_t width;
	uint32_t color; // ARGB32
};

struct sbar_json_box {
	int32_t x, y;
	int32_t width, height;
};

struct sbar_json_block;
typedef void (*sbar_json_block_destroy_t)(struct sbar_json_block *);

struct sbar_json_block {
	struct sbar_json_connection *connection;

	enum sbar_block_type type;
	union {
		//struct  spacer;
		struct sbar_json_block_type_text text;
		struct sbar_json_block_type_image image;
		struct sbar_json_block_type_composite composite;
		json_object *json;
	};

	bool raw;
	bool render;
	bool dirty;

	enum sbar_block_anchor anchor;
	uint32_t color; // ARGB32
	int32_t min_width, max_width;
	int32_t min_height, max_height;
	enum sbar_block_content_anchor content_anchor;
	enum sbar_block_content_transform content_transform;
	int32_t content_width, content_height;
	struct sbar_json_block_border borders[4]; // left, right, bottom, top

	int32_t x, y; // ignored when block is not in block.composite.blocks

	sbar_json_block_destroy_t destroy;

	uint64_t id;
};

struct sbar_json_output;
typedef void (*sbar_json_output_destroy_t)(struct sbar_json_output *output);

struct sbar_json_output {
	struct sbar_json_connection *connection;

	char *name;
	int32_t scale, width, height;
	enum sbar_output_transform transform;

	sbar_json_output_destroy_t destroy;

	list_t bars;
	list_t link;
};

struct sbar_json_pointer {
	struct sbar_json_surface *focused_surface;
};

struct sbar_json_seat;
typedef void (*sbar_json_seat_destroy_t)(struct sbar_json_seat *seat);

struct sbar_json_seat {
	struct sbar_json_connection *connection;

	char *name;
	struct sbar_json_pointer *pointer;

	sbar_json_seat_destroy_t destroy;

	list_t link;
};

struct sbar_json_connection_buffer {
	size_t size, index;
	char *data;
};

typedef struct sbar_json_output *(*sbar_json_output_create_t)(struct sbar_json_connection *connection, const char *name);
typedef struct sbar_json_seat *(*sbar_json_seat_create_t)(struct sbar_json_connection *connection, const char *name);

struct sbar_json_connection {
	pid_t pid;
	int read_fd;
	int write_fd;
	struct sbar_json_connection_buffer read_buffer;
	struct sbar_json_connection_buffer write_buffer;

	list_t outputs;
	list_t seats;

	sbar_json_output_create_t output_create;
	sbar_json_seat_create_t seat_create;

	bool state_events;
	bool dirty;
	uint64_t sync;
};

static void sbar_json_state_set_dirty(struct sbar_json_connection *connection) {
	if (!connection->dirty) {
		connection->sync++;
		connection->dirty = true;
	}
}

static void sbar_json_block_set_dirty(struct sbar_json_block *block) {
	if (!block->dirty) {
		static uint64_t next_id = 1;
		block->id = next_id++;
		block->dirty = true;
		if (block->type == SBAR_BLOCK_TYPE_COMPOSITE) {
			for (size_t i = 0; i < block->composite.blocks.len; ++i) {
				sbar_json_block_set_dirty(block->composite.blocks.items[i]);
			}
		}
		sbar_json_state_set_dirty(block->connection);
	}
}

static MAYBE_UNUSED void sbar_json_block_init(struct sbar_json_connection *connection,
		struct sbar_json_block *block, sbar_json_block_destroy_t destroy) {
	memset(block, 0, sizeof(struct sbar_json_block));
	block->connection = connection;
	block->destroy = destroy;
	block->x = -1;
	block->y = -1;
	block->render = true;
	sbar_json_block_set_dirty(block);
}

static MAYBE_UNUSED void sbar_json_block_fini(struct sbar_json_block *block) {
	if (block->raw) {
		json_object_put(block->json);
		return;
	}

	switch (block->type) {
	case SBAR_BLOCK_TYPE_TEXT:
		free(block->text.text);
		free(block->text.font_attributes);
		for (size_t i = 0; i < block->text.font_names.len; ++i) {
			free(block->text.font_names.items[i]);
		}
		ptr_array_fini(&block->text.font_names);
		break;
	case SBAR_BLOCK_TYPE_IMAGE:
		free(block->image.path);
		break;
	case SBAR_BLOCK_TYPE_COMPOSITE:
		for (size_t i = 0; i < block->composite.blocks.len; ++i) {
			block->destroy(block->composite.blocks.items[i]);
		}
		ptr_array_fini(&block->composite.blocks);
		break;
	case SBAR_BLOCK_TYPE_DEFAULT:
	case SBAR_BLOCK_TYPE_SPACER:
	default:
		break;
	}

	sbar_json_state_set_dirty(block->connection);
}

static void sbar_json_init_surface_private(struct sbar_json_connection *connection,
		struct sbar_json_surface *surface, sbar_json_surface_destroy_t destroy) {
	ptr_array_init(&surface->blocks, 30);
	array_init(&surface->block_hotspots, 30, sizeof(struct sbar_json_box));
	list_init(&surface->popups);
	array_init(&surface->input_regions, 4, sizeof(struct sbar_json_box));

	static uint64_t next_id = 0;
	surface->id = next_id++;
	surface->render = true;
	surface->connection = connection;
	surface->destroy = destroy;

	sbar_json_state_set_dirty(connection);
}

static MAYBE_UNUSED void sbar_json_bar_init(struct sbar_json_connection *connection,
		struct sbar_json_surface *bar, sbar_json_surface_destroy_t destroy) {
	memset(bar, 0, sizeof(struct sbar_json_surface));
	bar->type = SBAR_JSON_SURFACE_TYPE_BAR;
	bar->bar.exclusive_zone = -1;
	for (size_t i = 0; i < LENGTH(bar->bar.margins); ++i) {
		bar->bar.margins[i] = -1;
	}

	sbar_json_init_surface_private(connection, bar, destroy);
}

static MAYBE_UNUSED void sbar_json_popup_init(struct sbar_json_connection *connection,
		struct sbar_json_surface *popup, int32_t x, int32_t y,
		sbar_json_surface_destroy_t destroy) {
	memset(popup, 0, sizeof(struct sbar_json_surface));
	popup->type = SBAR_JSON_SURFACE_TYPE_POPUP;
	popup->popup.vertical = true;
	popup->popup.x = x;
	popup->popup.y = y;

	sbar_json_init_surface_private(connection, popup, destroy);
}

static MAYBE_UNUSED void sbar_json_surface_fini(struct sbar_json_surface *surface) {
	struct sbar_json_surface *popup, *popup_tmp;
	list_for_each_safe(popup, popup_tmp, &surface->popups, link) {
		popup->destroy(popup);
	}

	for (size_t i = 0; i < surface->blocks.len; ++i) {
		struct sbar_json_block *block = surface->blocks.items[i];
		block->destroy(block);
	}

	ptr_array_fini(&surface->blocks);
	array_fini(&surface->block_hotspots);
	array_fini(&surface->input_regions);

	struct sbar_json_seat *seat;
	list_for_each(seat, &surface->connection->seats, link) {
		struct sbar_json_pointer *pointer = seat->pointer;
		if (pointer && (pointer->focused_surface == surface)) {
			pointer->focused_surface = NULL;
		}
	}

	sbar_json_state_set_dirty(surface->connection);
}

static MAYBE_UNUSED void sbar_json_output_init(struct sbar_json_output *output,
		struct sbar_json_connection *connection, const char *name,
		sbar_json_output_destroy_t destroy) {
	memset(output, 0, sizeof(struct sbar_json_output));
	output->connection = connection;
	output->name = strdup(name);
	output->destroy = destroy;

	list_init(&output->bars);
}

static MAYBE_UNUSED void sbar_json_output_fini(struct sbar_json_output *output) {
	struct sbar_json_surface *bar, *bar_tmp;
	list_for_each_safe(bar, bar_tmp, &output->bars, link) {
		bar->destroy(bar);
	}

	free(output->name);
}

static MAYBE_UNUSED void sbar_json_seat_init(struct sbar_json_seat *seat,
		struct sbar_json_connection *connection, const char *name,
		sbar_json_seat_destroy_t destroy) {
	memset(seat, 0, sizeof(struct sbar_json_seat));
	seat->connection = connection;
	seat->name = strdup(name);
	seat->destroy = destroy;
}

static MAYBE_UNUSED void sbar_json_seat_fini(struct sbar_json_seat *seat) {
	free(seat->pointer);
	free(seat->name);
}

static MAYBE_UNUSED struct sbar_json_connection *sbar_json_connect(char * const *cmd,
		sbar_json_output_create_t output_create, sbar_json_seat_create_t seat_create) {
	int pipe_read_fd[2], pipe_write_fd[2];
	if ((pipe(pipe_read_fd) == -1) || (pipe(pipe_write_fd) == -1)) {
		goto error;
	}

    pid_t pid = fork();
	if (pid == -1) {
		goto error;
	} else if (pid == 0) {
		// TODO: error check

		dup2(pipe_write_fd[0], STDIN_FILENO);
		close(pipe_write_fd[0]);
		close(pipe_write_fd[1]);

		dup2(pipe_read_fd[1], STDOUT_FILENO);
		close(pipe_read_fd[0]);
		close(pipe_read_fd[1]);

		if (cmd) {
			execvp(cmd[0], cmd);
		}

		// ? TODO: do not fallback on execvp failure

		static const char *fallback_cmd[] = { "sbar", NULL };
		execvp(fallback_cmd[0], (char **)fallback_cmd);
		exit(1);
	}

	close(pipe_read_fd[1]);
    close(pipe_write_fd[0]);

	if (fcntl(pipe_read_fd[0], F_SETFL, O_NONBLOCK) == -1) {
		goto error;
	}
	if (fcntl(pipe_write_fd[1], F_SETFL, O_NONBLOCK) == -1) {
		goto error;
	}

	struct sbar_json_connection *connection = calloc(1, sizeof(struct sbar_json_connection));
	connection->output_create = output_create;
	connection->seat_create = seat_create;
	connection->pid = pid;
	connection->read_fd = pipe_read_fd[0];
	connection->write_fd = pipe_write_fd[1];

	static const size_t initial_buffer_size = 4096;
	connection->read_buffer.size = initial_buffer_size;
	connection->read_buffer.data = malloc(initial_buffer_size);
	connection->write_buffer.size = initial_buffer_size;
	connection->write_buffer.data = malloc(initial_buffer_size);

	list_init(&connection->outputs);
	list_init(&connection->seats);

	return connection;
error:
	close(pipe_read_fd[0]);
	close(pipe_read_fd[1]);
	close(pipe_write_fd[0]);
	close(pipe_write_fd[1]);
	return NULL;
}

static MAYBE_UNUSED void sbar_json_disconnect(struct sbar_json_connection *connection) {
	if (connection == NULL) {
		return;
	}

	kill(connection->pid, SIGTERM);
	close(connection->read_fd);
	close(connection->write_fd);

	free(connection->write_buffer.data);
	free(connection->read_buffer.data);

	struct sbar_json_output *output, *output_tmp;
	list_for_each_safe(output, output_tmp, &connection->outputs, link) {
		output->destroy(output);
	}

	struct sbar_json_seat *seat, *seat_tmp;
	list_for_each_safe(seat, seat_tmp, &connection->seats, link) {
		seat->destroy(seat);
	}

	free(connection);
}

static const unsigned jso_add_flags =
	JSON_C_OBJECT_ADD_KEY_IS_NEW | JSON_C_OBJECT_ADD_CONSTANT_KEY;

static json_object *sbar_json_describe_block(struct sbar_json_block *block) {
	if (block->raw) {
		assert(block->json != NULL);
		return json_object_get(block->json);
	}

	json_object *block_json = json_object_new_object();
	json_object_object_add_ex(block_json, "id",
		json_object_new_uint64(block->id), jso_add_flags);
	if (!block->dirty) {
		return block_json;
	}

	if (block->type != SBAR_BLOCK_TYPE_DEFAULT) {
		json_object_object_add_ex(block_json, "type",
			json_object_new_uint64(block->type), jso_add_flags);
	}
	if (block->anchor != SBAR_BLOCK_ANCHOR_DEFAULT) {
		json_object_object_add_ex(block_json, "anchor",
			json_object_new_uint64(block->anchor), jso_add_flags);
	}
	if (block->color > 0) {
		json_object_object_add_ex(block_json, "color",
			json_object_new_uint64(block->color), jso_add_flags);
	}
	if (block->min_width != 0) {
		json_object_object_add_ex(block_json, "min_width",
			json_object_new_int64(block->min_width), jso_add_flags);
	}
	if (block->max_width != 0) {
		json_object_object_add_ex(block_json, "max_width",
			json_object_new_int64(block->max_width), jso_add_flags);
	}
	if (block->min_height != 0) {
		json_object_object_add_ex(block_json, "min_height",
			json_object_new_int64(block->min_height), jso_add_flags);
	}
	if (block->max_height != 0) {
		json_object_object_add_ex(block_json, "max_height",
			json_object_new_int64(block->max_height), jso_add_flags);
	}
	if (block->content_anchor != SBAR_BLOCK_CONTENT_ANCHOR_DEFAULT) {
		json_object_object_add_ex(block_json, "content_anchor",
			json_object_new_uint64(block->content_anchor), jso_add_flags);
	}
	if (block->content_transform != SBAR_BLOCK_CONTENT_TRANSFORM_DEFAULT) {
		json_object_object_add_ex(block_json, "content_transform",
			json_object_new_uint64(block->content_transform), jso_add_flags);
	}
	if (block->content_width != 0) {
		json_object_object_add_ex(block_json, "content_width",
			json_object_new_int64(block->content_width), jso_add_flags);
	}
	if (block->content_height != 0) {
		json_object_object_add_ex(block_json, "content_height",
			json_object_new_int64(block->content_height), jso_add_flags);
	}
	for (size_t j = 0; j < LENGTH(block->borders); ++j) {
		if (block->borders[j].width > 0) {
			json_object *border = json_object_new_object();
			json_object_object_add_ex(border, "width",
				json_object_new_int64(block->borders[j].width), jso_add_flags);
			json_object_object_add_ex(border, "color",
				json_object_new_int64(block->borders[j].color), jso_add_flags);
			static const char *keys[] = {
				"border_left",
				"border_right",
				"border_bottom",
				"border_top",
			};
			json_object_object_add_ex(block_json, keys[j], border, jso_add_flags);
		}
	}
	if (!block->render) {
		json_object_object_add_ex(block_json, "render",
			json_object_new_boolean(block->render), jso_add_flags);
	}
	switch (block->type) {
	case SBAR_BLOCK_TYPE_DEFAULT:
	case SBAR_BLOCK_TYPE_SPACER:
		break;
	case SBAR_BLOCK_TYPE_TEXT:
		assert(block->text.text != NULL);
		json_object_object_add_ex(block_json, "text",
			json_object_new_string(block->text.text), jso_add_flags);
		if (block->text.font_names.len > 0) {
			json_object *font_names_array = json_object_new_array_ext(
				(int)block->text.font_names.len);
			json_object_object_add_ex(block_json, "font_names", font_names_array, jso_add_flags);
			for (size_t j = 0; j < block->text.font_names.len; ++j) {
				json_object_array_add(font_names_array,
					json_object_new_string(block->text.font_names.items[j]));
			}
		}
		if (block->text.font_attributes) {
			json_object_object_add_ex(block_json, "font_attributes",
				json_object_new_string(block->text.font_attributes), jso_add_flags);
		}
		if (block->text.color != 0xFFFFFFFF) {
			json_object_object_add_ex(block_json, "text_color",
				json_object_new_uint64(block->text.color), jso_add_flags);
		}
		break;
	case SBAR_BLOCK_TYPE_IMAGE:
		assert(block->image.path != NULL);
		json_object_object_add_ex(block_json, "path",
			json_object_new_string(block->image.path), jso_add_flags);
		if (block->image.type != SBAR_BLOCK_TYPE_DEFAULT) {
			json_object_object_add_ex(block_json, "image_type",
				json_object_new_uint64(block->image.type), jso_add_flags);
		}
		break;
	case SBAR_BLOCK_TYPE_COMPOSITE: {
		assert(block->composite.blocks.len > 0);
		json_object *blocks_array = json_object_new_array_ext((int)block->composite.blocks.len);
		for (size_t j = 0; j < block->composite.blocks.len; ++j) {
			struct sbar_json_block *b = block->composite.blocks.items[j];
			json_object *b_json = sbar_json_describe_block(b);
			json_object_array_add(blocks_array, b_json);
			if ((b->x >= 0) && (b->y >= 0)) {
				json_object_object_add_ex(b_json, "x",
					json_object_new_int64(b->x), jso_add_flags);
				json_object_object_add_ex(b_json, "y",
					json_object_new_int64(b->y), jso_add_flags);
			}
		}
		json_object_object_add_ex(block_json, "blocks", blocks_array, jso_add_flags);
		break;
	}
	default:
		assert(UNREACHABLE);
		break;
	}

	block->dirty = false;
	return block_json;
}

static void sbar_json_describe_surfaces(list_t *surfaces, json_object *dest_array,
		MAYBE_UNUSED enum sbar_json_surface_type expected_type) {
	struct sbar_json_surface *surface;
	list_for_each(surface, surfaces, link) {
		assert(surface->type == expected_type);
		json_object *surface_json = json_object_new_object();

		switch (surface->type) {
		case SBAR_JSON_SURFACE_TYPE_BAR: {
			struct sbar_json_surface_type_bar *bar = &surface->bar;
			if (bar->exclusive_zone >= 0) {
				json_object_object_add_ex(surface_json, "exclusive_zone",
					json_object_new_int64(bar->exclusive_zone), jso_add_flags);
			}
			if (bar->anchor != SBAR_BAR_ANCHOR_DEFAULT) {
				json_object_object_add_ex(surface_json, "anchor",
					json_object_new_uint64(bar->anchor), jso_add_flags);
			}
			if (bar->layer != SBAR_BAR_LAYER_DEFAULT) {
				json_object_object_add_ex(surface_json, "layer",
					json_object_new_uint64(bar->layer), jso_add_flags);
			}
			for (size_t i = 0; i < LENGTH(bar->margins); ++i) {
				if (bar->margins[i] >= 0) {
					static const char *keys[] = {
						"margin_top",
						"margin_right",
						"margin_bottom",
						"margin_left",
					};
					json_object_object_add_ex(surface_json, keys[i],
						json_object_new_int64(bar->margins[i]), jso_add_flags);
				}
			}
			break;
		}
		case SBAR_JSON_SURFACE_TYPE_POPUP: {
			struct sbar_json_surface_type_popup *popup = &surface->popup;
			json_object_object_add_ex(surface_json, "x",
				json_object_new_int64(popup->x), jso_add_flags);
			json_object_object_add_ex(surface_json, "y",
				json_object_new_int64(popup->y), jso_add_flags);
			if (!popup->vertical) {
				json_object_object_add_ex(surface_json, "vertical",
					json_object_new_boolean(popup->vertical), jso_add_flags);
			}
			if (popup->gravity != SBAR_POPUP_GRAVITY_DEFAULT) {
				json_object_object_add_ex(surface_json, "gravity",
					json_object_new_uint64(popup->gravity), jso_add_flags);
			}
			if (popup->constraint_adjustment != 0) {
				json_object_object_add_ex(surface_json, "constraint_adjustment",
					json_object_new_uint64(popup->constraint_adjustment), jso_add_flags);
			}
			if (popup->grab) {
				json_object_object_add_ex(surface_json, "grab",
					json_object_new_uint64(popup->grab_serial), jso_add_flags);
			}
			break;
		}
		default:
			assert(UNREACHABLE);
			break;
		}

		json_object_object_add_ex(surface_json, "userdata",
			json_object_new_uint64(surface->id), jso_add_flags);
		if (surface->wanted_width > 0) {
			json_object_object_add_ex(surface_json, "width",
				json_object_new_int64(surface->wanted_width), jso_add_flags);
		}
		if (surface->wanted_height > 0) {
			json_object_object_add_ex(surface_json, "height",
				json_object_new_int64(surface->wanted_height), jso_add_flags);
		}
		if (surface->cursor_shape != SBAR_SURFACE_CURSOR_SHAPE_DEFAULT_) {
			json_object_object_add_ex(surface_json, "cursor_shape",
				json_object_new_uint64(surface->cursor_shape), jso_add_flags);
		}
		if (surface->input_regions.len > 0) {
			json_object *input_regions_array =
				json_object_new_array_ext((int)surface->input_regions.len);
			json_object_object_add_ex(surface_json, "input_regions",
				input_regions_array, jso_add_flags);
			for (size_t i = 0; i < surface->input_regions.len; ++i) {
				struct sbar_json_box *region =
					&((struct sbar_json_box *)surface->input_regions.items)[i];
				json_object *region_json = json_object_new_object();
				if (region->x != 0) {
					json_object_object_add_ex(region_json, "x",
						json_object_new_int64(region->x), jso_add_flags);
				}
				if (region->y != 0) {
					json_object_object_add_ex(region_json, "y",
						json_object_new_int64(region->y), jso_add_flags);
				}
				if (region->width != 0) {
					json_object_object_add_ex(region_json, "width",
						json_object_new_int64(region->width), jso_add_flags);
				}
				if (region->height != 0) {
					json_object_object_add_ex(region_json, "height",
						json_object_new_int64(region->height), jso_add_flags);
				}
				json_object_array_add(input_regions_array, region_json);
			}
		}
		if (!surface->render) {
			json_object_object_add_ex(surface_json, "render",
				json_object_new_boolean(surface->render), jso_add_flags);
		}
		size_t popups_len = list_length(&surface->popups);
		if (popups_len > 0) {
			json_object *popups_array = json_object_new_array_ext((int)popups_len);
			sbar_json_describe_surfaces(&surface->popups, popups_array, SBAR_JSON_SURFACE_TYPE_POPUP);
			json_object_object_add_ex(surface_json, "popups", popups_array, jso_add_flags);
		}
		if (surface->blocks.len > 0) {
			json_object *blocks_array = json_object_new_array_ext((int)surface->blocks.len);
			for (size_t i = 0; i < surface->blocks.len; ++i) {
				json_object_array_add(blocks_array, sbar_json_describe_block(surface->blocks.items[i]));
			}
			json_object_object_add_ex(surface_json, "blocks", blocks_array, jso_add_flags);
		}

		json_object_array_add(dest_array, surface_json);
	}
}

static void sbar_json_describe_outputs(struct sbar_json_connection *connection,
		json_object *dest) {
	if (list_empty(&connection->outputs)) {
		return;
	}

	struct sbar_json_output *output;
	list_for_each(output, &connection->outputs, link) {
		size_t bars_len = list_length(&output->bars);
		if (bars_len > 0) {
			json_object *output_array_json = json_object_new_array_ext((int)bars_len);
			sbar_json_describe_surfaces(&output->bars,
				output_array_json, SBAR_JSON_SURFACE_TYPE_BAR);
			json_object_object_add_ex(dest, output->name,
				output_array_json, jso_add_flags);
		}
	}
}

static MAYBE_UNUSED int sbar_json_flush(struct sbar_json_connection *connection) {
	assert(connection != NULL);

	if (connection->dirty) {
		json_object *state = json_object_new_object();

		json_object_object_add_ex(state, "userdata",
			json_object_new_uint64(connection->sync), jso_add_flags);
		json_object_object_add_ex(state, "state_events",
			json_object_new_boolean(connection->state_events), jso_add_flags);

		sbar_json_describe_outputs(connection, state);

		size_t state_str_len;
		const char *state_str = json_object_to_json_string_length(
			state, JSON_C_TO_STRING_PLAIN, &state_str_len);

		struct sbar_json_connection_buffer *write_buffer = &connection->write_buffer;
		size_t required_size = state_str_len + write_buffer->index + 1;
		if (required_size > write_buffer->size) {
			write_buffer->size = required_size * 2;
			write_buffer->data = realloc(write_buffer->data, write_buffer->size);
		}

		memcpy(&write_buffer->data[write_buffer->index], state_str, state_str_len);
		write_buffer->index += state_str_len;
		write_buffer->data[write_buffer->index++] = '\n';

		json_object_put(state);
		connection->dirty = false;
	}

	while (connection->write_buffer.index > 0) {
		ssize_t written_bytes = write(connection->write_fd,
			connection->write_buffer.data, connection->write_buffer.index);
		if (written_bytes == -1) {
			if (errno == EAGAIN) {
				return 0;
			} else if (errno == EINTR) {
				continue;
			} else {
				return -1;
			}
		} else {
			connection->write_buffer.index -= (size_t)written_bytes;
			memmove(connection->write_buffer.data,
				&connection->write_buffer.data[written_bytes],
				connection->write_buffer.index);
		}
	}

	return 1;
}

static struct sbar_json_box sbar_json_parse_sbar_block(json_object *block) {
	json_object *x, *y, *width, *height;
	json_object_object_get_ex(block, "x", &x);
	json_object_object_get_ex(block, "y", &y);
	json_object_object_get_ex(block, "width", &width);
	json_object_object_get_ex(block, "height", &height);

	assert(json_object_is_type(x, json_type_int));
	assert(json_object_is_type(y, json_type_int));
	assert(json_object_is_type(width, json_type_int));
	assert(json_object_is_type(height, json_type_int));

	return (struct sbar_json_box) {
		.x = json_object_get_int(x),
		.y = json_object_get_int(y),
		.width = json_object_get_int(width),
		.height = json_object_get_int(height),
	};
}

static void sbar_json_parse_sbar_surfaces(struct sbar_json_connection *connection,
		json_object *source_array, list_t *dest) {
	assert(json_object_is_type(source_array, json_type_array));
	assert(list_length(dest) == json_object_array_length(source_array));

	size_t i = 0;
	struct sbar_json_surface *surface, *surface_tmp;
	list_for_each_safe(surface, surface_tmp, dest, link) {
		json_object *surface_json = json_object_array_get_idx(source_array, i++);
		if (surface_json == NULL) {
			list_pop(&surface->link);
			surface->destroy(surface);
			continue;
		}

		json_object *width, *height, *scale, *blocks_array, *popups;
		json_object_object_get_ex(surface_json, "width", &width);
		json_object_object_get_ex(surface_json, "height", &height);
		json_object_object_get_ex(surface_json, "scale", &scale);
		json_object_object_get_ex(surface_json, "blocks", &blocks_array);
		json_object_object_get_ex(surface_json, "popups", &popups);

		assert(json_object_is_type(width, json_type_int));
		assert(json_object_is_type(height, json_type_int));
		assert(json_object_is_type(scale, json_type_int));
		assert(json_object_is_type(blocks_array, json_type_array));

		surface->width = json_object_get_int(width);
		surface->height = json_object_get_int(height);
		surface->scale = json_object_get_int(scale);

		assert(surface->blocks.len == json_object_array_length(blocks_array));
		surface->block_hotspots.len = 0;
		for (size_t j = 0; j < surface->blocks.len; ++j) {
			json_object *block = json_object_array_get_idx(blocks_array, j);
			if (block) {
				struct sbar_json_box hotspot = sbar_json_parse_sbar_block(block);
				array_add(&surface->block_hotspots, &hotspot);
			} else {
				log_debug("WARNING: block is NULL");
				array_add(&surface->block_hotspots, &(struct sbar_json_box){ 0 });
			}
		}

		if (surface->updated_callback) {
			surface->updated_callback(surface);
		}

		sbar_json_parse_sbar_surfaces(connection, popups, &surface->popups);
	}
}

static void sbar_json_parse_sbar_outputs(struct sbar_json_connection *connection,
		json_object *outputs_array) {
	assert(json_object_is_type(outputs_array, json_type_array));

	list_t new_outputs;
	list_init(&new_outputs);

	size_t outputs_len = json_object_array_length(outputs_array);
	for (size_t i = 0; i < outputs_len; ++i) {
		json_object *output_json = json_object_array_get_idx(outputs_array, i);
		assert(json_object_is_type(output_json, json_type_object));

		json_object *name;
		json_object_object_get_ex(output_json, "name", &name);
		assert(json_object_is_type(name, json_type_string));

		const char *output_name = json_object_get_string(name);
		struct sbar_json_output *output = NULL;
		{
			struct sbar_json_output *output_;
			list_for_each(output_, &connection->outputs, link) {
				if (strcmp(output_->name, output_name) == 0) {
					list_pop(&output_->link);
					output = output_;
					break;
				}
			}
		}

		if (output == NULL) {
			output = connection->output_create(connection, output_name);
		}

		list_insert(new_outputs.prev, &output->link);
	}

	{
		struct sbar_json_output *output, *output_tmp;
		list_for_each_safe(output, output_tmp, &connection->outputs, link) {
			list_pop(&output->link);
			output->destroy(output);
		}
	}

	list_insert_list(&connection->outputs, &new_outputs);

	{
		size_t i = 0;
		struct sbar_json_output *output;
		list_for_each(output, &connection->outputs, link) {
			json_object *output_json = json_object_array_get_idx(outputs_array, i++);
			json_object *width, *height, *scale, *transform, *bars;
			json_object_object_get_ex(output_json, "width", &width);
			json_object_object_get_ex(output_json, "height", &height);
			json_object_object_get_ex(output_json, "scale", &scale);
			json_object_object_get_ex(output_json, "transform", &transform);
			json_object_object_get_ex(output_json, "bars", &bars);

			assert(json_object_is_type(width, json_type_int));
			assert(json_object_is_type(height, json_type_int));
			assert(json_object_is_type(scale, json_type_int));
			assert(json_object_is_type(transform, json_type_int));

			// ? TODO: upd callback
			output->width = json_object_get_int(width);
			output->height = json_object_get_int(height);
			output->scale = json_object_get_int(scale);
			output->transform = (uint32_t)json_object_get_int(transform);

			sbar_json_parse_sbar_surfaces(connection, bars, &output->bars);
		}
	}
}

static ATTRIB_PURE struct sbar_json_surface *sbar_json_find_surface_recursive(uint64_t id, list_t *source) {
	struct sbar_json_surface *surface, *surface_tmp;
	list_for_each_safe(surface, surface_tmp, source, link) {
		if (surface->id == id) {
			return surface;
		}
		surface = sbar_json_find_surface_recursive(id, &surface->popups);
		if (surface) {
			return surface;
		}
	}

	return NULL;
}


static ATTRIB_PURE struct sbar_json_surface *sbar_json_find_surface(struct sbar_json_connection *connection,
		uint64_t id) {
	struct sbar_json_output *output;
	list_for_each(output, &connection->outputs, link) {
		struct sbar_json_surface *surface = sbar_json_find_surface_recursive(
			id, &output->bars);
		if (surface) {
			return surface;
		}
	}

	return NULL;
}

static void sbar_json_parse_sbar_pointer(struct sbar_json_connection *connection,
		struct sbar_json_seat *seat, json_object *pointer_json) {
	if (pointer_json) {
		if (seat->pointer == NULL) {
			seat->pointer = malloc(sizeof(struct sbar_json_pointer));
			seat->pointer->focused_surface = NULL;
		}

		struct sbar_json_pointer *pointer = seat->pointer;
		json_object *focus;
		json_object_object_get_ex(pointer_json, "focus", &focus);
		if (focus) {
			json_object *surface_userdata, *x_json, *y_json;
			json_object_object_get_ex(focus, "surface_userdata", &surface_userdata);
			json_object_object_get_ex(focus, "x", &x_json);
			json_object_object_get_ex(focus, "y", &y_json);

			assert(json_object_is_type(surface_userdata, json_type_int));
			assert(json_object_is_type(x_json, json_type_double));
			assert(json_object_is_type(y_json, json_type_double));

			sbar_json_pointer_motion_callback_t motion_callback = NULL;
			uint64_t focused_surface_id = json_object_get_uint64(surface_userdata);
			if ((pointer->focused_surface == NULL) || (pointer->focused_surface->id != focused_surface_id)) {
				if (pointer->focused_surface && pointer->focused_surface->pointer_leave_callback) {
					pointer->focused_surface->pointer_leave_callback(pointer->focused_surface);
				}
				pointer->focused_surface = sbar_json_find_surface(connection, focused_surface_id);
				assert(pointer->focused_surface != NULL);
				assert(pointer->focused_surface->blocks.len == pointer->focused_surface->block_hotspots.len);
				if (pointer->focused_surface) {
					motion_callback = pointer->focused_surface->pointer_enter_callback;
				}
			} else {
				motion_callback = pointer->focused_surface->pointer_motion_callback;
			}

			double x = json_object_get_double(x_json) * pointer->focused_surface->scale;
			double y = json_object_get_double(y_json) * pointer->focused_surface->scale;
			if (motion_callback) { // ? TODO: store prev x, y
				motion_callback(pointer->focused_surface, x, y);
			}

			if (pointer->focused_surface) {
				json_object *button, *scroll;
				json_object_object_get_ex(pointer_json, "button", &button);
				json_object_object_get_ex(pointer_json, "scroll", &scroll);
				if (button && pointer->focused_surface->pointer_button_callback) {
					json_object *code, *state, *serial;
					json_object_object_get_ex(button, "code", &code);
					json_object_object_get_ex(button, "state", &state);
					json_object_object_get_ex(button, "serial", &serial);

					assert(json_object_is_type(code, json_type_int));
					assert(json_object_is_type(state, json_type_int));
					assert(json_object_is_type(serial, json_type_int));

					pointer->focused_surface->pointer_button_callback(pointer->focused_surface,
						(uint32_t)json_object_get_int(code),
						(uint32_t)json_object_get_int(state),
						(uint32_t)json_object_get_int(serial), x, y);
				} else if (scroll && pointer->focused_surface->pointer_scroll_callback) {
					json_object *axis, *vector_length;
					json_object_object_get_ex(scroll, "axis", &axis);
					json_object_object_get_ex(scroll, "vector_length", &vector_length);

					assert(json_object_is_type(axis, json_type_int));
					assert(json_object_is_type(vector_length, json_type_double));

					pointer->focused_surface->pointer_scroll_callback(pointer->focused_surface,
						(uint32_t)json_object_get_int(axis),
						json_object_get_double(vector_length),
						x, y);
				}
			}

		} else if (pointer->focused_surface) {
			if (pointer->focused_surface->pointer_leave_callback) {
				pointer->focused_surface->pointer_leave_callback(pointer->focused_surface);
			}
			pointer->focused_surface = NULL;
		}

	} else if (seat->pointer) {
		free(seat->pointer);
		seat->pointer = NULL;
	}
}

static void sbar_json_parse_sbar_seats(struct sbar_json_connection *connection,
		json_object *seats_array) {
	assert(json_object_is_type(seats_array, json_type_array));

	list_t new_seats;
	list_init(&new_seats);

	size_t seats_len = json_object_array_length(seats_array);
	for (size_t i = 0; i < seats_len; ++i) {
		json_object *seat_json = json_object_array_get_idx(seats_array, i);
		assert(json_object_is_type(seat_json, json_type_object));

		json_object *name;
		json_object_object_get_ex(seat_json, "name", &name);

		assert(json_object_is_type(name, json_type_string));

		const char *seat_name = json_object_get_string(name);
		struct sbar_json_seat *seat = NULL;
		{
			struct sbar_json_seat *seat_;
			list_for_each(seat_, &connection->seats, link) {
				if (strcmp(seat_->name, seat_name) == 0) {
					list_pop(&seat_->link);
					seat = seat_;
					break;
				}
			}
		}

		if (seat == NULL) {
			seat = connection->seat_create(connection, seat_name);
		}

		list_insert(new_seats.prev, &seat->link);
	}

	{
		struct sbar_json_seat *seat, *seat_tmp;
		list_for_each_safe(seat, seat_tmp, &connection->seats, link) {
			list_pop(&seat->link);
			seat->destroy(seat);
		}
	}

	list_insert_list(&connection->seats, &new_seats);

	{
		size_t i = 0;
		struct sbar_json_seat *seat;
		list_for_each(seat, &connection->seats, link) {
			json_object *seat_json = json_object_array_get_idx(seats_array, i++);
			json_object *pointer;
			json_object_object_get_ex(seat_json, "pointer", &pointer);
			sbar_json_parse_sbar_pointer(connection, seat, pointer);
		}
	}
}

static void sbar_json_parse_sbar_state(struct sbar_json_connection *connection, const char *json) {
	assert(json != NULL);
	json_object *state = json_tokener_parse(json);
	assert(json_object_is_type(state, json_type_object));

	json_object *userdata;
	json_object_object_get_ex(state, "userdata", &userdata);
	assert(json_object_is_type(userdata, json_type_int));
	if (json_object_get_uint64(userdata) != connection->sync) {
		goto cleanup;
	}

	json_object *outputs, *seats;
	json_object_object_get_ex(state, "outputs", &outputs);
	json_object_object_get_ex(state, "seats", &seats);

	sbar_json_parse_sbar_outputs(connection, outputs);
	sbar_json_parse_sbar_seats(connection, seats);

cleanup:
	json_object_put(state);
}

static MAYBE_UNUSED int sbar_json_process(struct sbar_json_connection *connection) {
	assert(connection != NULL);

	struct sbar_json_connection_buffer *read_buffer = &connection->read_buffer;
    for (;;) {
		ssize_t read_bytes = read(connection->read_fd,
			&read_buffer->data[read_buffer->index],
			read_buffer->size - read_buffer->index);
		if (read_bytes <= 0) {
			if (read_bytes == 0) {
				errno = EPIPE;
			}
			if (errno == EAGAIN) {
				break;
			} else if (errno == EINTR) {
				continue;
			} else {
				return -1;
			}
		} else {
			read_buffer->index += (size_t)read_bytes;
			if (read_buffer->index == read_buffer->size) {
				read_buffer->size *= 2;
				read_buffer->data = realloc(
					read_buffer->data, read_buffer->size);
			}
		}
    }

	if (read_buffer->index > 0) {
		for (size_t n = read_buffer->index - 1; n > 0; --n) {
			if (read_buffer->data[n] == '\n') {
				read_buffer->data[n] = '\0';
				char *tmp = NULL;
				const char *json = strtok_r(read_buffer->data, "\n", &tmp);
				while (json) {
					sbar_json_parse_sbar_state(connection, json);
					json = strtok_r(NULL, "\n", &tmp);
				}
				read_buffer->index -= ++n;
				memmove(read_buffer->data, &read_buffer->data[n], read_buffer->index);
				return 1;
			}
		}
	}

	return 0;
}

#endif // SBAR_JSON_H
