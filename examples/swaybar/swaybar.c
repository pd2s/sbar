#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <getopt.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <assert.h>
#include <ctype.h>

#include <linux/input-event-codes.h>
#define KEY_SCROLL_UP KEY_MAX + 1
#define KEY_SCROLL_DOWN KEY_MAX + 2
#define KEY_SCROLL_LEFT KEY_MAX + 3
#define KEY_SCROLL_RIGHT KEY_MAX + 4

#include <json_tokener.h>
#include <json_object.h>

#include "sbar-json.h"
#include "sway-ipc.h"
#include "sni-server.h"
#if HAVE_PNG || HAVE_PNG
#include "xdg-icon-theme.h"
#endif // HAVE_PNG || HAVE_SVG
#include "macros.h"
#include "util.h"

#include "config.h"

struct output {
	struct sbar_json_output _; // must be first
	list_t workspaces; // struct workspace::link
	bool focused;
};

struct popup;
typedef void (*popup_destroy_t)(struct popup *);

struct popup {
	struct sbar_json_surface _; // must be first
	popup_destroy_t destroy;
	void *data;

	struct block *focused_block;
	struct sbar_json_box *focused_block_hotspot;
};

struct bar {
	struct sbar_json_surface _; // must be first
	struct output *output;
	bool dirty;
	bool status_line_i3bar_short_text;
};

struct workspace {
	char *name;
	int32_t num;
	bool visible;
	bool focused;
	bool urgent;
	struct block *block;
	list_t link; // struct output::workspaces
};

struct block;
typedef void (*block_destroy_t)(struct block *);

struct block {
	struct sbar_json_block _; // must be first

	void *data;

	bool (*pointer_enter_callback)(struct sbar_json_surface *surface,
		struct block *block, struct sbar_json_box *hotspot);
	bool (*pointer_button_callback)(struct sbar_json_surface *surface,
		struct block *block, struct sbar_json_box *hotspot,
		uint32_t code, enum sbar_pointer_button_state state,
		uint32_t serial, double x, double y);
	void (*pointer_leave_callback)(struct sbar_json_surface *surface,
		struct block *block);

	block_destroy_t destroy;
	uint64_t ref_count;
};

struct binding {
	uint32_t event_code;
	bool release;
	char *command;
};

struct config_box_colors {
	uint32_t border;
	uint32_t background;
	uint32_t text;
};

struct tray {
	struct sni_server_state *sni;

#if HAVE_PNG || HAVE_PNG
	ptr_array_t icon_theme_basedirs; // char *
	ptr_array_t icon_cache; // struct xdg_icon_theme_icon_cache *
#endif // HAVE_PNG || HAVE_SVG
};

#if HAVE_PNG && HAVE_PNG
#define TRAY_ICON_TYPES XDG_ICON_THEME_ICON_TYPE_PNG | XDG_ICON_THEME_ICON_TYPE_SVG
#elif HAVE_PNG
#define TRAY_ICON_TYPES XDG_ICON_THEME_ICON_TYPE_SVG
#elif HAVE_PNG
#define TRAY_ICON_TYPES XDG_ICON_THEME_ICON_TYPE_PNG
#endif

struct tray_item {
	struct sni_item _; // must be first
	struct block *block;
};

enum tray_binding_command {
	TRAY_BINDING_COMMAND_NONE,
	TRAY_BINDING_COMMAND_CONTEXT_MENU,
	TRAY_BINDING_COMMAND_ACTIVATE,
	TRAY_BINDING_COMMAND_SECONDARY_ACTIVATE,
	TRAY_BINDING_COMMAND_SCROLL_DOWN,
	TRAY_BINDING_COMMAND_SCROLL_LEFT,
	TRAY_BINDING_COMMAND_SCROLL_RIGHT,
	TRAY_BINDING_COMMAND_SCROLL_UP,
	TRAY_BINDING_COMMAND_NOP,
};

struct tray_binding {
	uint32_t event_code;
	enum tray_binding_command command;
};

enum status_line_protocol {
	STATUS_LINE_PROTOCOL_UNDEF,
	STATUS_LINE_PROTOCOL_ERROR,
	STATUS_LINE_PROTOCOL_TEXT,
	STATUS_LINE_PROTOCOL_I3BAR,
};

enum status_line_i3bar_block_type {
	STATUS_LINE_I3BAR_BLOCK_TYPE_I3BAR,
	STATUS_LINE_I3BAR_BLOCK_TYPE_SBAR,
};

struct status_line_i3bar_block {
	char *name;
	char *instance;
	enum status_line_i3bar_block_type type;
	union {
		struct { // sbar
			json_object *json;
		};
		struct { // i3bar
			char *full_text;
			char *short_text;
			bool text_color_set;
			bool urgent;
			bool separator;
			bool border_color_set;
			uint32_t text_color; // ARGB32
			uint32_t background_color; // ARGB32
			uint32_t border_color;// ARGB32
			int32_t min_width;
			char *min_width_str;
			enum sbar_block_content_anchor content_anchor;
			int32_t separator_block_width;
			union {
				struct {
					int32_t border_widths[4]; // top, right, bottom, left
				};
				struct {
					int32_t border_top;
					int32_t border_right;
					int32_t border_bottom;
					int32_t border_left;
				};
			};
		};
	};

};

struct status_line {
	int read_fd, write_fd;
	pid_t pid;

	enum status_line_protocol protocol;
	bool click_events;
	bool float_event_coords;

	bool started;
	bool expecting_comma;
	bool clicked;

	json_tokener *tokener;
	ptr_array_t blocks;

	const char *text;

	int stop_signal;
	int cont_signal;

	char *read_buffer;
	size_t read_buffer_size, read_buffer_index;
	FILE *read;
};

enum config_hidden_state {
	CONFIG_HIDDEN_STATE_HIDE,
	CONFIG_HIDDEN_STATE_SHOW,
};

enum config_mode {
	CONFIG_MODE_DOCK,
	CONFIG_MODE_HIDE,
	CONFIG_MODE_INVISIBLE,
	CONFIG_MODE_OVERLAY,
};

enum config_position {
	CONFIG_POSITION_TOP,
	CONFIG_POSITION_BOTTOM,
};

static struct {
	bool binding_mode_indicator;
	ptr_array_t bindings; // struct binding *
	int32_t height;
	struct {
		uint32_t background;
		uint32_t statusline;
		uint32_t separator;
		uint32_t focused_background;
		uint32_t focused_statusline;
		uint32_t focused_separator;
		struct config_box_colors focused_workspace;
		struct config_box_colors active_workspace;
		struct config_box_colors inactive_workspace;
		struct config_box_colors urgent_workspace;
		struct config_box_colors binding_mode;
	} colors;
	const char *font;
	int32_t gaps[4]; // top, right, bottom, left
	enum config_hidden_state hidden_state;
	enum config_mode mode;
	ptr_array_t outputs; // char *
	enum config_position position;
	bool strip_workspace_name;
	bool strip_workspace_numbers;
	bool workspace_buttons;
	bool wrap_scroll;
	int32_t workspace_min_width;

	int32_t tray_padding;
	ptr_array_t tray_outputs; // char *
	array_t tray_bindings; // struct tray_binding
	char *icon_theme;

	char *status_command;
	int32_t status_padding;
	int32_t status_edge_padding;
	char *separator_symbol;
} config;

static char *bar_id;
static bool visible_by_urgency;
static bool visible_by_modifier;
static bool visible_by_mode;
static struct block *binding_mode_indicator;
static struct tray *tray;
static struct status_line *status;

static size_t bar_count = 0;
static struct popup *grabbing_popup;

static struct sbar_json_connection *sbar;
static int sway_ipc_fd;

static bool running = true;

static struct pollfd pollfds[] = {
	{ .fd = -1, .events = POLLIN, }, // sbar read
	{ .fd = -1, .events = POLLOUT, }, // sbar write
	{ .fd = -1, .events = POLLIN, }, // sway ipc read/write
	{ .fd = -1, .events = POLLIN, }, // status read
	{ .fd = -1, }, // dbus(tray) read/write
};

static struct block *block_ref(struct block *block) {
	if (block->ref_count == 1) {
		sbar_json_block_set_dirty(&block->_);
	}
	block->ref_count++;
	return block;
}

static void block_unref(struct block *block) {
	if ((block == NULL) || (--block->ref_count > 0)) {
		return;
	}

	block->destroy(block);
}

static void block_destroy_sbar_json(struct sbar_json_block *block) {
	block_unref((struct block *)block);
}

static struct block *block_create(block_destroy_t destroy) {
	struct block *block = malloc(sizeof(struct block));
	block->ref_count = 1;
	block->data = NULL;
	block->destroy = destroy;
	block->pointer_enter_callback = NULL;
	block->pointer_button_callback = NULL;
	block->pointer_leave_callback = NULL;

	sbar_json_block_init(sbar, &block->_, block_destroy_sbar_json);

	return block;
}

static void block_destroy(struct block *block) {
	if (block == NULL) {
		return;
	}

	sbar_json_block_fini(&block->_);

	free(block);
}

static bool workspace_pointer_button_callback(struct sbar_json_surface *bar,
		struct block *block, MAYBE_UNUSED struct sbar_json_box *hotspot,
		uint32_t code, enum sbar_pointer_button_state state,
		MAYBE_UNUSED uint32_t serial, MAYBE_UNUSED double x, MAYBE_UNUSED double y) {
	struct workspace *workspace = NULL;

	switch (code) {
	case BTN_LEFT:
		workspace = block->data;
		break;
	case KEY_SCROLL_DOWN:
	case KEY_SCROLL_RIGHT:
	case KEY_SCROLL_LEFT:
	case KEY_SCROLL_UP: {
		bool left = (code == KEY_SCROLL_UP) || (code == KEY_SCROLL_LEFT);
		struct output *output = ((struct bar *)bar)->output;
		struct workspace *first_ws =
			container_of(output->workspaces.next, first_ws, link);
		struct workspace *last_ws =
			container_of(output->workspaces.prev, last_ws, link);

		list_for_each(workspace, &output->workspaces, link) {
			if (workspace->visible) {
				break;
			}
		}

		if (workspace->visible) {
			if (left) {
				if (workspace == first_ws) {
					workspace = config.wrap_scroll ? last_ws : NULL;
				} else {
					workspace = container_of(workspace->link.prev, workspace, link);
				}
			} else {
				if (workspace == last_ws) {
					workspace = config.wrap_scroll ? first_ws : NULL;
				} else {
					workspace = container_of(workspace->link.next, workspace, link);
				}
			}
		}
		break;
	}
	default:
		return false;
	}

	if ((state == SBAR_POINTER_BUTTON_STATE_RELEASED) ||
			(workspace == NULL) || workspace->focused) {
		return true;
	}

	const char *name = workspace->name;
	size_t ws_len = strlen(name);
	size_t payload_length = strlen("workspace \"\"") + ws_len;
	for (size_t i = 0; i < ws_len; ++i) {
		if ((name[i] == '"') || (name[i] == '\\')) {
			payload_length++;
		}
	}

	char *payload = malloc(payload_length + 1);
	strcpy(payload, "workspace \"");
	strcpy(&payload[payload_length - 1], "\"");
	for (size_t i = 0, d = strlen("workspace \""); i < ws_len; ++i) {
		if ((name[i] == '"') || (name[i] == '\\')) {
			payload[d++] = '\\';
		}
		payload[d++] = name[i];
	}

	sway_ipc_send(sway_ipc_fd, SWAY_IPC_MESSAGE_TYPE_COMMAND, payload, (uint32_t)payload_length);
	free(payload);

	return true;
}

static struct workspace *workspace_create(json_object *workspace_json) {
	json_object *num, *name, *visible, *focused, *urgent;
	json_object_object_get_ex(workspace_json, "num", &num);
	json_object_object_get_ex(workspace_json, "name", &name);
	json_object_object_get_ex(workspace_json, "visible", &visible);
	json_object_object_get_ex(workspace_json, "focused", &focused);
	json_object_object_get_ex(workspace_json, "urgent", &urgent);

	struct workspace *workspace = malloc(sizeof(struct workspace));
	workspace->num = json_object_get_int(num);
	workspace->name = strdup(json_object_get_string(name));
	workspace->visible = json_object_get_boolean(visible);
	workspace->focused = json_object_get_boolean(focused);
	workspace->urgent = json_object_get_boolean(urgent);

	struct config_box_colors colors;
	if (workspace->urgent) {
		colors = config.colors.urgent_workspace;
	} else if (workspace->focused) {
		colors = config.colors.focused_workspace;
	} else if (workspace->visible) {
		colors = config.colors.active_workspace;
	} else {
		colors = config.colors.inactive_workspace;
	}

	struct block *block = block_create(block_destroy);
	block->_.type = SBAR_BLOCK_TYPE_COMPOSITE;
	ptr_array_init(&block->_.composite.blocks, 1);
	block->_.min_width = config.workspace_min_width;
	block->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
	block->_.color = colors.background;
	for (size_t i = 0; i < LENGTH(block->_.borders); ++i) {
		block->_.borders[i].width = WORKSPACE_BORDER_WIDTH;
		block->_.borders[i].color = colors.border;
	}
	block->pointer_button_callback = workspace_pointer_button_callback;
	block->data = workspace;

	struct block *text = block_create(block_destroy);
	text->_.type = SBAR_BLOCK_TYPE_TEXT;
	ptr_array_init(&text->_.text.font_names, 1);
	ptr_array_add(&text->_.text.font_names, strdup(config.font));
	text->_.text.color = colors.text;
	text->_.borders[0].width = WORKSPACE_MARGIN_LEFT;
	text->_.borders[1].width = WORKSPACE_MARGIN_RIGHT;
	text->_.borders[2].width = WORKSPACE_MARGIN_BOTTOM;
	text->_.borders[3].width = WORKSPACE_MARGIN_TOP;
	if (workspace->num != -1) {
		if (config.strip_workspace_name) {
			text->_.text.text = fstr_create("%d", workspace->num);
		} else if (config.strip_workspace_numbers) {
			int num_len = snprintf(NULL, 0, "%d", workspace->num);
			num_len += workspace->name[num_len] == ':';
			if (workspace->name[num_len] != '\0') {
				text->_.text.text = strdup(workspace->name + num_len);
			}
		}
	}
	if (text->_.text.text == NULL) {
		text->_.text.text = strdup(workspace->name);
	}

	ptr_array_add(&block->_.composite.blocks, text);

	workspace->block = block;
	return workspace;
}

static void workspace_destroy(struct workspace *workspace) {
	if (workspace == NULL) {
		return;
	}

	block_unref(workspace->block);
	free(workspace->name);

	free(workspace);
}

static void bar_destroy(struct bar *bar) {
	if (bar == NULL) {
		return;
	}

	sbar_json_surface_fini(&bar->_);

	if ((--bar_count == 0) && status) {
		kill(-status->pid, status->stop_signal);
	}

	free(bar);
}

static void bar_destroy_sbar_json(struct sbar_json_surface *bar) {
	bar_destroy((struct bar *)bar);
}

static bool bar_process_button_event(struct bar *bar,
		uint32_t code, enum sbar_pointer_button_state state,
		uint32_t serial, double x, double y) {
	if (grabbing_popup) {
		if (state == SBAR_POINTER_BUTTON_STATE_PRESSED) {
			list_pop(&grabbing_popup->_.link);
			grabbing_popup->destroy(grabbing_popup);
		}
		return true;
	}

	for (size_t i = 0; i < bar->_.blocks.len; ++i) {
		struct block *block = bar->_.blocks.items[i];
		struct sbar_json_box *hotspot =
			&((struct sbar_json_box *)bar->_.block_hotspots.items)[i];
		if ((block->pointer_button_callback)
				&& (x >= hotspot->x) && (y >= hotspot->y)
				&& (x < (hotspot->x + hotspot->width))
				&& (y < (hotspot->y + hotspot->height))) {
			if (block->pointer_button_callback(&bar->_, block, hotspot,
					code, state, serial, x, y)) {
				return true;
			}
		}
	}

	bool released = state == SBAR_POINTER_BUTTON_STATE_RELEASED;
	for (size_t i = 0; i < config.bindings.len; ++i) {
		struct binding *binding = config.bindings.items[i];
		if ((binding->event_code == code) && (binding->release == released)) {
			sway_ipc_send(sway_ipc_fd, SWAY_IPC_MESSAGE_TYPE_COMMAND,
				binding->command, (uint32_t)strlen(binding->command));
			return true;
		}
	}

	return false;
}

static void bar_pointer_button_callback(struct sbar_json_surface *bar_,
		uint32_t code, enum sbar_pointer_button_state state,
		uint32_t serial, double x, double y) {
	bar_process_button_event((struct bar *)bar_, code, state, serial, x, y);
}

static void bar_pointer_scroll_callback(struct sbar_json_surface *bar_,
		enum sbar_pointer_axis axis, double vec_len, double x, double y) {
	uint32_t button_code;
	bool negative = vec_len < 0;
	switch (axis) {
	case SBAR_POINTER_AXIS_VERTICAL_SCROLL:
		button_code = negative ? KEY_SCROLL_UP : KEY_SCROLL_DOWN;
		break;
	case SBAR_POINTER_AXIS_HORIZONTAL_SCROLL:
		button_code = negative ? KEY_SCROLL_LEFT : KEY_SCROLL_RIGHT;
		break;
	default:
		assert(UNREACHABLE);
		return;
	}

	struct bar *bar = (struct bar *)bar_;
	if (!bar_process_button_event(bar, button_code, SBAR_POINTER_BUTTON_STATE_PRESSED, 0, x, y)) {
		bar_process_button_event(bar, button_code, SBAR_POINTER_BUTTON_STATE_RELEASED, 0, x, y);
	}
}

static struct bar *bar_create(struct output *output) {
	struct bar *bar = malloc(sizeof(struct bar));
	bar->output = output;
	bar->dirty = false;
	bar->status_line_i3bar_short_text = false;

	sbar_json_bar_init(sbar, &bar->_, bar_destroy_sbar_json);
	bar->_.pointer_button_callback = bar_pointer_button_callback;
	bar->_.pointer_scroll_callback = bar_pointer_scroll_callback;

	if ((bar_count++ == 0) && status) {
		kill(-status->pid, status->cont_signal);
	}
	return bar;
}

static void bars_set_dirty(void) {
	struct output *output;
	list_for_each(output, &sbar->outputs, _.link) {
		struct bar *bar;
		list_for_each(bar, &output->_.bars, _.link) {
			bar->dirty = true;
		}
	}
}

static void popup_destroy(struct popup *popup) {
	if (popup == NULL) {
		return;
	}

	sbar_json_surface_fini(&popup->_);

	if (popup == grabbing_popup) {
		grabbing_popup = NULL;
	}

	free(popup);
}

static void popup_destroy_sbar_json(struct sbar_json_surface *popup_) {
	struct popup *popup = (struct popup *)popup_;
	popup->destroy(popup);
}

static struct popup *popup_create(int32_t x, int32_t y, popup_destroy_t destroy) {
	struct popup *popup = malloc(sizeof(struct popup));
	popup->data = NULL;
	popup->focused_block = NULL;
	popup->focused_block_hotspot = NULL;
	popup->destroy = destroy;

	sbar_json_popup_init(sbar, &popup->_, x, y, popup_destroy_sbar_json);

	return popup;
}

static void output_destroy(struct output *output) {
	if (output == NULL) {
		return;
	}

	sbar_json_output_fini(&output->_);

	struct workspace *workspace, *workspace_tmp;
	list_for_each_safe(workspace, workspace_tmp, &output->workspaces, link) {
		list_pop(&workspace->link);
		workspace_destroy(workspace);
	}

	free(output);
}

static void output_destroy_sbar_json(struct sbar_json_output *output) {
	output_destroy((struct output *)output);
}

static struct output *output_create(struct sbar_json_connection *connection,
		const char *name) {
	struct output *output = malloc(sizeof(struct output));

	sbar_json_output_init(&output->_, connection, name, output_destroy_sbar_json);

	output->focused = false;
	list_init(&output->workspaces);

	return output;
}

static struct sbar_json_output *output_create_sbar_json(struct sbar_json_connection *connection,
		const char *name) {
	sway_ipc_send(sway_ipc_fd, SWAY_IPC_MESSAGE_TYPE_GET_WORKSPACES, NULL, 0);

	struct output *output = output_create(connection, name);
	return &output->_;
}

static void seat_destroy_sbar_json(struct sbar_json_seat *seat) {
	if (seat == NULL) {
		return;
	}

	sbar_json_seat_fini(seat);

	free(seat);
}

static struct sbar_json_seat *seat_create_sbar_json(struct sbar_json_connection *connection,
		const char *name) {
	struct sbar_json_seat *seat = malloc(sizeof(struct sbar_json_seat));
	sbar_json_seat_init(seat, connection, name, seat_destroy_sbar_json);

	return seat;
}

static char *bytes_to_shm(void *bytes, size_t nbytes) {
	char *path = strdup("/dev/shm/sbar-XXXXXX");
	int fd = mkstemp(path);
	if (fd == -1) {
		goto error_1;
	}

	size_t total = 0;
	while (total < nbytes) {
		ssize_t written_bytes = write(fd, (char *)bytes + total, nbytes - total);
		if (written_bytes == -1) {
			goto error_2;
		}
		total += (size_t)written_bytes;
	}

	return path;
error_2:
	close(fd);
error_1:
	free(path);
	return NULL;
}

static struct block *tray_dbusmenu_menu_item_get_text_block(
		struct sni_dbusmenu_menu_item *menu_item, const char *text) {
	struct block *block = block_create(block_destroy);
	block->_.type = SBAR_BLOCK_TYPE_TEXT;
	block->_.text.text = strdup(text);
	ptr_array_init(&block->_.text.font_names, 1);
	ptr_array_add(&block->_.text.font_names, strdup(config.font));
	if (menu_item->enabled) {
		block->_.text.color = config.colors.focused_statusline;
	} else {
		uint32_t c = config.colors.focused_statusline;
		uint32_t a = (c >> 24) & 0xFF;
		block->_.text.color = ((a >>= 1) << 24) | (c & 0x00FFFFFF);
	}
	block->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
	for (size_t j = 0; j < LENGTH(block->_.borders); ++j) {
		block->_.borders[j].width = config.tray_padding;
	}
	block->_.min_width = SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS;
	block->_.max_width = SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS;
	block->_.min_height = SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS;
	block->_.max_height = SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS;

	return block;
}

static void block_remove_image_path_and_destroy(struct block *block) {
	if (block == NULL) {
		return;
	}

	remove(block->_.image.path);

	block_destroy(block);
}

static struct popup *tray_dbusmenu_menu_popup_create(struct sni_dbusmenu_menu *menu,
		int32_t x, int32_t y, uint32_t grab_serial);

static bool tray_dbusmenu_menu_item_enter_callback(struct sbar_json_surface *popup,
		struct block *block, struct sbar_json_box *hotspot) {
	struct sni_dbusmenu_menu_item *menu_item = block->data;

	sni_dbusmenu_menu_item_event(menu_item,
		SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_HOVERED, true);

	struct popup *child_popup, *child_popup_tmp;
	list_for_each_safe(child_popup, child_popup_tmp, &popup->popups, _.link) {
		list_pop(&child_popup->_.link);
		child_popup->destroy(child_popup);
	}

	if (menu_item->enabled && (menu_item->type != SNI_DBUSMENU_MENU_ITEM_TYPE_SEPARATOR)) {
		if (menu_item->submenu && (menu_item->submenu->menu_items.len > 0)) {
			struct popup *submenu_popup = tray_dbusmenu_menu_popup_create(menu_item->submenu,
				hotspot->x, hotspot->y + hotspot->height,
				0);
			list_insert(&popup->popups, &submenu_popup->_.link);
		}

		block->_.color = config.colors.focused_separator;

		sbar_json_block_set_dirty(&block->_);
	}

	return true;
}

static bool tray_dbusmenu_menu_item_button_callback(MAYBE_UNUSED struct sbar_json_surface *popup_,
		struct block *block, MAYBE_UNUSED struct sbar_json_box *hotspot,
		uint32_t code, enum sbar_pointer_button_state state,
		MAYBE_UNUSED uint32_t serial, MAYBE_UNUSED double x, MAYBE_UNUSED double y) {
	if ((code != BTN_LEFT) || (state == SBAR_POINTER_BUTTON_STATE_RELEASED)) {
		return false;
	}

	struct sni_dbusmenu_menu_item *menu_item = block->data;
	if ((menu_item->type != SNI_DBUSMENU_MENU_ITEM_TYPE_SEPARATOR)
			&& menu_item->enabled) {
		sni_dbusmenu_menu_item_event(menu_item,
			SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_CLICKED, true);

		list_pop(&grabbing_popup->_.link);
		grabbing_popup->destroy(grabbing_popup);
	}

	return true;
}

static void tray_dbusmenu_menu_item_leave_callback(MAYBE_UNUSED struct sbar_json_surface *popup,
		struct block *block) {
	struct sni_dbusmenu_menu_item *menu_item = block->data;
	if ((menu_item->type != SNI_DBUSMENU_MENU_ITEM_TYPE_SEPARATOR) && menu_item->enabled) {
		block->_.color = config.colors.focused_background;
		sbar_json_block_set_dirty(&block->_);
	}
}

static void tray_dbusmenu_menu_popup_update(struct sni_dbusmenu_menu *menu, struct popup *popup) {
	struct block *bg = block_create(block_destroy);
	//bg->_.type = SBAR_BLOCK_TYPE_SPACER;
	bg->_.anchor = SBAR_BLOCK_ANCHOR_NONE;
	bg->_.color = config.colors.focused_background;
	ptr_array_add(&popup->_.blocks, bg);

	bool needs_spacer = false;

	for (size_t i = 0; i < menu->menu_items.len; ++i) {
		struct sni_dbusmenu_menu_item *menu_item = menu->menu_items.items[i];
		if (!menu_item->visible) {
			continue;
		}

		struct block *block = block_create(block_destroy);
		block->pointer_enter_callback = tray_dbusmenu_menu_item_enter_callback;
		block->pointer_button_callback = tray_dbusmenu_menu_item_button_callback;
		block->pointer_leave_callback = tray_dbusmenu_menu_item_leave_callback;
		block->data = menu_item;
		if (menu_item->type == SNI_DBUSMENU_MENU_ITEM_TYPE_SEPARATOR) {
			//block->type = SBAR_BLOCK_TYPE_SPACER;
			block->_.min_height = 2;
			block->_.color = config.colors.focused_separator;
		} else {
			block->_.type = SBAR_BLOCK_TYPE_COMPOSITE;
			ptr_array_init(&block->_.composite.blocks, 4);

			if (menu_item->label) {
				ptr_array_add(&block->_.composite.blocks,
					tray_dbusmenu_menu_item_get_text_block(
						menu_item, menu_item->label));
			}

#if HAVE_PNG || HAVE_PNG
			if (menu_item->icon_name && *menu_item->icon_name) {
				struct sni_dbusmenu *dbusmenu = menu_item->parent_menu->dbusmenu;
				if (dbusmenu->item->properties && dbusmenu->item->properties->icon_theme_path
						&& *dbusmenu->item->properties->icon_theme_path) {
					ptr_array_add(&tray->icon_theme_basedirs, dbusmenu->item->properties->icon_theme_path);
				}
				if (dbusmenu->properties) {
					for (size_t j = 0; j < dbusmenu->properties->icon_theme_path.len; ++j) {
						ptr_array_add(&tray->icon_theme_basedirs, dbusmenu->properties->icon_theme_path.items[j]);
					}
				}
				struct xdg_icon_theme_icon xdg_icon = xdg_icon_theme_find_icon(
					menu_item->icon_name, config.icon_theme, &tray->icon_theme_basedirs,
					&tray->icon_cache, TRAY_ICON_TYPES);
				if (dbusmenu->properties) {
					tray->icon_theme_basedirs.len -= dbusmenu->properties->icon_theme_path.len;
				}
				if (dbusmenu->item->properties && dbusmenu->item->properties->icon_theme_path
						&& *dbusmenu->item->properties->icon_theme_path) {
					tray->icon_theme_basedirs.len--;
				}
				if (xdg_icon.path) {
					struct block *icon = block_create(block_destroy);
					icon->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
					for (size_t j = 0; j < LENGTH(icon->_.borders); ++j) {
						icon->_.borders[j].width = config.tray_padding;
					}
					icon->_.type = SBAR_BLOCK_TYPE_IMAGE;
					icon->_.image.path = strdup(xdg_icon.path);
					icon->_.image.type = (enum sbar_block_type_image_image_type)xdg_icon.type;
					icon->_.content_width = SBAR_BLOCK_SIZE_OFFSET(SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_MINUS, config.tray_padding * 2);
					icon->_.content_height = SBAR_BLOCK_SIZE_OFFSET(SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_MINUS, config.tray_padding * 2);

					ptr_array_add(&block->_.composite.blocks, icon);
				}
			}
#endif // HAVE_PNG || HAVE_SVG

#if HAVE_PNG
			if (menu_item->icon_data.nbytes > 0) {
				char *path;
				if ((path = bytes_to_shm(menu_item->icon_data.bytes, menu_item->icon_data.nbytes))) {
					struct block *icon = block_create(block_remove_image_path_and_destroy);
					icon->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
					for (size_t j = 0; j < LENGTH(icon->_.borders); ++j) {
						icon->_.borders[j].width = config.tray_padding;
					}
					icon->_.type = SBAR_BLOCK_TYPE_IMAGE;
					icon->_.image.path = path;
					icon->_.image.type = SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_PNG;
					icon->_.content_width = SBAR_BLOCK_SIZE_OFFSET(SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_MINUS, config.tray_padding * 2);
					icon->_.content_height = SBAR_BLOCK_SIZE_OFFSET(SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_MINUS, config.tray_padding * 2);

					ptr_array_add(&block->_.composite.blocks, icon);
				}
			}
#endif // HAVE_PNG

			switch (menu_item->toggle_type) {
			case SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_CHECKMARK:
				ptr_array_add(&block->_.composite.blocks,
					tray_dbusmenu_menu_item_get_text_block(menu_item,
						(menu_item->toggle_state == 1) ? "󰄲" : "󰄮"));
				needs_spacer = true;
				break;
			case SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_RADIO:
				ptr_array_add(&block->_.composite.blocks,
					tray_dbusmenu_menu_item_get_text_block(menu_item,
						(menu_item->toggle_state == 1) ? "󰐾" : "󰄯"));
				needs_spacer = true;
				break;
			case SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_NONE:
			default:
				break;
			}

			if (menu_item->submenu) {
				ptr_array_add(&block->_.composite.blocks,
					tray_dbusmenu_menu_item_get_text_block(menu_item, "󰍞"));
				needs_spacer = true;
			}
		}

		ptr_array_add(&popup->_.blocks, block);
	}

	if (needs_spacer) {
		struct block *spacer = block_create(block_destroy);
		//spacer->_.type = SBAR_BLOCK_TYPE_SPACER;
		spacer->_.min_width = SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS;
		spacer->_.min_height = SBAR_BLOCK_SIZE_PREV_BLOCK_HEIGHT_PLUS;

		for (size_t i = 0; i < popup->_.blocks.len; ++i) {
			struct block *block = popup->_.blocks.items[i];
			struct sni_dbusmenu_menu_item *menu_item = block->data;
			if ((block->_.type == SBAR_BLOCK_TYPE_COMPOSITE)
					&& (menu_item->toggle_type == SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_NONE)
					&& (menu_item->submenu == NULL)) {
				ptr_array_add(&block->_.composite.blocks, block_ref(spacer));
			}
		}

		block_unref(spacer);
	}

	popup->data = menu;

	sbar_json_state_set_dirty(sbar);
}

static void tray_dbusmenu_menu_popup_destroy(struct popup *popup) {
	if (popup == NULL) {
		return;
	}

	struct sni_dbusmenu_menu *menu = popup->data;
	sni_dbusmenu_menu_item_event(menu->parent_menu_item,
		SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_CLOSED, true);

	popup_destroy(popup);
}

static void tray_dbusmenu_menu_popup_pointer_motion_callback(struct sbar_json_surface *popup_,
		double x, double y) {
	struct popup *popup = (struct popup *)popup_;
	for (size_t i = 0; i < popup->_.blocks.len; ++i) {
		struct block *block = popup->_.blocks.items[i];
		struct sbar_json_box *hotspot =
			&((struct sbar_json_box *)popup->_.block_hotspots.items)[i];
		if ((popup->focused_block != block) // TODO: handle ref_count > 1
				&& (x >= hotspot->x) && (y >= hotspot->y)
				&& (x < (hotspot->x + hotspot->width))
				&& (y < (hotspot->y + hotspot->height))) {
			if (block->pointer_enter_callback
					&& block->pointer_enter_callback(&popup->_, block, hotspot)) {
				if (popup->focused_block && popup->focused_block->pointer_leave_callback) {
					popup->focused_block->pointer_leave_callback(&popup->_, popup->focused_block);
				}
				popup->focused_block = block;
				popup->focused_block_hotspot = hotspot;
				return;
			}
		}
	}
}

static void tray_dbusmenu_menu_popup_pointer_leave_callback(struct sbar_json_surface *popup_) {
	struct popup *popup = (struct popup *)popup_;
	if (popup->focused_block) {
		if (popup->focused_block->pointer_leave_callback) {
			popup->focused_block->pointer_leave_callback(&popup->_, popup->focused_block);
		}
		popup->focused_block = NULL;
		popup->focused_block_hotspot = NULL;
	}
}

static void tray_dbusmenu_menu_popup_pointer_button_callback(struct sbar_json_surface *popup_,
		uint32_t code, enum sbar_pointer_button_state state,
		uint32_t serial, double x, double y) {
	struct popup *popup = (struct popup *)popup_;
	if (popup->focused_block && popup->focused_block->pointer_button_callback) {
		popup->focused_block->pointer_button_callback(&popup->_,
			popup->focused_block, popup->focused_block_hotspot,
			code, state, serial, x, y);
	}
}

static struct popup *tray_dbusmenu_menu_popup_create(struct sni_dbusmenu_menu *menu,
		int32_t x, int32_t y, uint32_t grab_serial) {
	struct popup *popup = popup_create(x, y, tray_dbusmenu_menu_popup_destroy);
	popup->_.popup.grab = true;
	popup->_.popup.grab_serial = grab_serial;
	popup->_.popup.gravity = SBAR_POPUP_GRAVITY_TOP_LEFT;
	popup->_.popup.constraint_adjustment =
		SBAR_POPUP_CONSTRAINT_ADJUSTMENT_FLIP_X
		| SBAR_POPUP_CONSTRAINT_ADJUSTMENT_FLIP_Y;
	popup->_.pointer_enter_callback = tray_dbusmenu_menu_popup_pointer_motion_callback;
	popup->_.pointer_motion_callback = tray_dbusmenu_menu_popup_pointer_motion_callback;
	popup->_.pointer_leave_callback = tray_dbusmenu_menu_popup_pointer_leave_callback;
	popup->_.pointer_button_callback = tray_dbusmenu_menu_popup_pointer_button_callback;

	sni_dbusmenu_menu_about_to_show(menu, true);

	tray_dbusmenu_menu_popup_update(menu, popup);

	sni_dbusmenu_menu_item_event(menu->parent_menu_item,
		SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_OPENED, true);

	return popup;
}

static void tray_item_update(struct tray_item *tray_item) {
	struct block *block = tray_item->block;
	if ((block->_.type == SBAR_BLOCK_TYPE_IMAGE)
			&& (block->_.image.type == SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_PIXMAP)) {
		remove(block->_.image.path);
	}
	free(block->_.image.path);
	memset(&block->_.image, 0, sizeof(struct sbar_json_block_type_image));

	block->_.type = SBAR_BLOCK_TYPE_SPACER;
	block->destroy = block_destroy;
	block->_.content_width = SBAR_BLOCK_SIZE_OFFSET(SBAR_BLOCK_SIZE_SURFACE_HEIGHT_MINUS, config.tray_padding * 2);
	block->_.content_height = SBAR_BLOCK_SIZE_OFFSET(SBAR_BLOCK_SIZE_SURFACE_HEIGHT_MINUS, config.tray_padding * 2);

	sbar_json_block_set_dirty(&block->_);

	struct sni_item_properties *props = tray_item->_.properties;
	if (props == NULL) {
		return;
	}

	MAYBE_UNUSED char *icon_name = NULL;
	struct sni_item_pixmap *icon_pixmap = NULL;
	switch(props->status) {
	case SNI_ITEM_STATUS_ACTIVE:
		icon_name = props->icon_name;
		if (props->icon_pixmap.len > 0) {
			icon_pixmap = props->icon_pixmap.items[0];
		}
		break;
	case SNI_ITEM_STATUS_NEEDS_ATTENTION:
		icon_name = props->attention_icon_name;
		if (props->attention_icon_pixmap.len > 0) {
			icon_pixmap = props->attention_icon_pixmap.items[0];
		}
		break;
	case SNI_ITEM_STATUS_PASSIVE:
	case SNI_ITEM_STATUS_INVALID:
	default:
		break;
	}

#if HAVE_PNG || HAVE_PNG
	if (icon_name && *icon_name) {
		if (props->icon_theme_path && *props->icon_theme_path) {
			ptr_array_add(&tray->icon_theme_basedirs, props->icon_theme_path);
		}
		struct xdg_icon_theme_icon xdg_icon = xdg_icon_theme_find_icon(
			icon_name, config.icon_theme, &tray->icon_theme_basedirs,
			&tray->icon_cache, TRAY_ICON_TYPES);
		if (xdg_icon.path) {
			block->_.image.path = strdup(xdg_icon.path);
			block->_.image.type = (enum sbar_block_type_image_image_type)xdg_icon.type;
		}
		if (props->icon_theme_path && *props->icon_theme_path) {
			tray->icon_theme_basedirs.len--;
		}
	}
#endif // HAVE_PNG || HAVE_SVG

	if ((block->_.image.path == NULL) && icon_pixmap) {
		char *path;
		if ((path = bytes_to_shm(icon_pixmap,
				(size_t)icon_pixmap->width * (size_t)icon_pixmap->height * 4
					+ sizeof(struct sni_item_pixmap)))) {
			block->_.image.path = path;
			block->_.image.type = SBAR_BLOCK_TYPE_IMAGE_IMAGE_TYPE_PIXMAP;
			block->destroy = block_remove_image_path_and_destroy;
		}
	}

	if (block->_.image.path) {
		block->_.type = SBAR_BLOCK_TYPE_IMAGE;
	}
}

static bool tray_item_pointer_button_callback(struct sbar_json_surface *bar,
		struct block *block, MAYBE_UNUSED struct sbar_json_box *hotspot,
		uint32_t code, enum sbar_pointer_button_state state,
		uint32_t serial, double x, double y) {
	if (state == SBAR_POINTER_BUTTON_STATE_RELEASED) {
		return true;
	}

	struct tray_item *tray_item = block->data;
	struct sni_item *item = &tray_item->_;

	enum tray_binding_command command = TRAY_BINDING_COMMAND_NONE;
	for (size_t i = 0; i < config.tray_bindings.len; ++i) {
		struct tray_binding binding = ((struct tray_binding *)config.tray_bindings.items)[i];
		if (binding.event_code == code) {
			command = binding.command;
			break;
		}
	}
	if (command == TRAY_BINDING_COMMAND_NONE) {
		switch (code) {
		case BTN_LEFT:
			command = TRAY_BINDING_COMMAND_ACTIVATE;
			break;
		case BTN_MIDDLE:
			command = TRAY_BINDING_COMMAND_SECONDARY_ACTIVATE;
			break;
		case BTN_RIGHT:
			command = TRAY_BINDING_COMMAND_CONTEXT_MENU;
			break;
		case KEY_SCROLL_UP:
			command = TRAY_BINDING_COMMAND_SCROLL_UP;
			break;
		case KEY_SCROLL_DOWN:
			command = TRAY_BINDING_COMMAND_SCROLL_DOWN;
			break;
		case KEY_SCROLL_LEFT:
			command = TRAY_BINDING_COMMAND_SCROLL_LEFT;
			break;
		case KEY_SCROLL_RIGHT:
			command = TRAY_BINDING_COMMAND_SCROLL_RIGHT;
			break;
		default:
			command = TRAY_BINDING_COMMAND_NOP;
		}
	}

	if ((command == TRAY_BINDING_COMMAND_ACTIVATE) && item->properties
			&& item->properties->item_is_menu) {
		command = TRAY_BINDING_COMMAND_CONTEXT_MENU;
	}

	switch (command) {
	case TRAY_BINDING_COMMAND_CONTEXT_MENU: {
		struct sni_dbusmenu *dbusmenu = item->dbusmenu;
		#define MENU ((struct sni_dbusmenu_menu_item **)dbusmenu->menu->menu_items.items)[0]->submenu
		if (dbusmenu && dbusmenu->menu && (dbusmenu->menu->menu_items.len > 0)
				&& MENU && (MENU->menu_items.len > 0)) {
			grabbing_popup = tray_dbusmenu_menu_popup_create(MENU, (int32_t)x, (int32_t)y, serial);
			list_insert(&bar->popups, &grabbing_popup->_.link);
		} else {
			sni_item_context_menu(item, 0, 0, true);
		}
		#undef MENU
		break;
	}
	case TRAY_BINDING_COMMAND_ACTIVATE:
		sni_item_activate(item, 0, 0, true);
		break;
	case TRAY_BINDING_COMMAND_SECONDARY_ACTIVATE:
		sni_item_secondary_activate(item, 0, 0, true);
		break;
	case TRAY_BINDING_COMMAND_SCROLL_DOWN:
		sni_item_scroll(item, 1, SNI_ITEM_SCROLL_ORIENTATION_VERTICAL, true);
		break;
	case TRAY_BINDING_COMMAND_SCROLL_LEFT:
		sni_item_scroll(item, -1, SNI_ITEM_SCROLL_ORIENTATION_HORIZONTAL, true);
		break;
	case TRAY_BINDING_COMMAND_SCROLL_RIGHT:
		sni_item_scroll(item, 1, SNI_ITEM_SCROLL_ORIENTATION_HORIZONTAL, true);
		break;
	case TRAY_BINDING_COMMAND_SCROLL_UP:
		sni_item_scroll(item, -1, SNI_ITEM_SCROLL_ORIENTATION_VERTICAL, true);
		break;
	case TRAY_BINDING_COMMAND_NONE:
	case TRAY_BINDING_COMMAND_NOP:
	default:
		break;
	}

	return true;
}

static void tray_item_destroy(struct tray_item *tray_item) {
	if (tray_item == NULL) {
		return;
	}

	if (grabbing_popup) {
		struct sni_dbusmenu_menu *menu = grabbing_popup->data;
		if (menu->dbusmenu->item == &tray_item->_) {
			list_pop(&grabbing_popup->_.link);
			grabbing_popup->destroy(grabbing_popup);
		}
	}

	sni_item_fini(&tray_item->_);

	block_unref(tray_item->block);

	free(tray_item);
}

static void destroy_sni_item(struct sni_item *item) {
	tray_item_destroy((struct tray_item *)item);

	bars_set_dirty();
}

static void properties_updated_sni_item(struct sni_item *item,
		MAYBE_UNUSED struct sni_item_properties *old_props) {
	tray_item_update((struct tray_item *)item);

	bars_set_dirty();
}

static void dbusmenu_menu_updated_sni_item(struct sni_item *item,
		MAYBE_UNUSED struct sni_dbusmenu_menu *old_menu) {
	if (grabbing_popup == NULL) {
		return;
	}

	struct sni_dbusmenu_menu *menu = grabbing_popup->data;
	if (menu->dbusmenu->item != item) {
		return;
	}

	menu = item->dbusmenu->menu;
	#define MENU ((struct sni_dbusmenu_menu_item **)menu->menu_items.items)[0]->submenu
	if (menu && (menu->menu_items.len > 0) && MENU && (MENU->menu_items.len > 0)) {
		struct popup *popup, *popup_tmp;
		list_for_each_safe(popup, popup_tmp, &grabbing_popup->_.popups, _.link) {
			list_pop(&popup->_.link);
			popup->destroy(popup);
		}
		for (size_t i = 0; i < grabbing_popup->_.blocks.len; ++i) {
			block_unref(grabbing_popup->_.blocks.items[i]);
		}
		grabbing_popup->_.blocks.len = 0;
		tray_dbusmenu_menu_popup_update(MENU, grabbing_popup);
	} else {
		list_pop(&grabbing_popup->_.link);
		grabbing_popup->destroy(grabbing_popup);
	}
	#undef MENU
}

static struct tray_item *tray_item_create(const char *id) {
	struct tray_item *tray_item = malloc(sizeof(struct tray_item));

	if (!sni_item_init(&tray_item->_, id, destroy_sni_item)) {
		free(tray_item);
		return NULL;
	}

	struct block *block = block_create(block_destroy);
	//block->_.type = SBAR_BLOCK_TYPE_SPACER
	block->_.anchor = SBAR_BLOCK_ANCHOR_RIGHT;
	block->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
	block->_.min_width = SBAR_BLOCK_SIZE_SURFACE_HEIGHT_PLUS;
	block->_.max_width = SBAR_BLOCK_SIZE_SURFACE_HEIGHT_PLUS;
	block->_.min_height = SBAR_BLOCK_SIZE_SURFACE_HEIGHT_PLUS;
	block->_.max_height = SBAR_BLOCK_SIZE_SURFACE_HEIGHT_PLUS;
	block->data = tray_item;
	block->pointer_button_callback = tray_item_pointer_button_callback;

	tray_item->block = block;

	tray_item->_.properties_updated = properties_updated_sni_item;
	tray_item->_.dbusmenu_menu_updated = dbusmenu_menu_updated_sni_item;
	return tray_item;
}

static struct sni_item *create_sni_item(const char *id) {
	struct tray_item *tray_item = tray_item_create(id);

	bars_set_dirty();
	return &tray_item->_;
}

static void tray_init(void) {
	int ret = sni_server_init(create_sni_item);
	if (ret < 0) {
		abort_(-ret, "sni_server_init: %s", strerror(-ret));
	}

	tray = malloc(sizeof(struct tray));
	tray->sni = &sni_server_state;

#if HAVE_PNG || HAVE_PNG
	ptr_array_init(&tray->icon_theme_basedirs, 10);
	xdg_icon_theme_get_basedirs(&tray->icon_theme_basedirs);
	ptr_array_init(&tray->icon_cache, 20);
#endif // HAVE_PNG || HAVE_SVG
}

static void tray_fini(void) {
	sni_server_fini();

#if HAVE_PNG || HAVE_PNG
	for (size_t i = 0; i < tray->icon_cache.len; ++i) {
		xdg_icon_theme_icon_cache_destroy(tray->icon_cache.items[i]);
	}
	for (size_t i = 0; i < tray->icon_theme_basedirs.len; ++i) {
		free(tray->icon_theme_basedirs.items[i]);
	}
	ptr_array_fini(&tray->icon_cache);
	ptr_array_fini(&tray->icon_theme_basedirs);
#endif // HAVE_PNG || HAVE_SVG

	free(tray);

	pollfds[4].fd = -1;
	tray = NULL;
}

static void tray_update(void) {
	for (size_t i = 0; i < tray->sni->host.items.len; ++i) {
		tray_item_update(tray->sni->host.items.items[i]);
	}

#if HAVE_PNG || HAVE_PNG
	for (size_t i = 0; i < tray->icon_cache.len; ++i) {
		xdg_icon_theme_icon_cache_destroy(tray->icon_cache.items[i]);
	}
	for (size_t i = 0; i < tray->icon_theme_basedirs.len; ++i) {
		free(tray->icon_theme_basedirs.items[i]);
	}
	tray->icon_cache.len = 0;
	tray->icon_theme_basedirs.len = 0;

	xdg_icon_theme_get_basedirs(&tray->icon_theme_basedirs);
#endif // HAVE_PNG || HAVE_SVG
}

static void status_line_init(const char *cmd) {
	int pipe_read_fd[2];
	int pipe_write_fd[2];
	if ((pipe(pipe_read_fd) == -1) || (pipe(pipe_write_fd) == -1)) {
		abort_(errno, "pipe: %s", strerror(errno));
	}

	pid_t pid = fork();
	if (pid == -1) {
		abort_(errno, "fork: %s", strerror(errno));
	} else if (pid == 0) {
		setpgid(0, 0);

		dup2(pipe_read_fd[1], STDOUT_FILENO);
		close(pipe_read_fd[0]);
		close(pipe_read_fd[1]);

		dup2(pipe_write_fd[0], STDIN_FILENO);
		close(pipe_write_fd[0]);
		close(pipe_write_fd[1]);

		const char *_cmd[] = { "sh", "-c", cmd, NULL, };
		execvp(_cmd[0], (char * const *)_cmd);
		exit(1);
	}

	int read_fd = pipe_read_fd[0];
	int write_fd = pipe_write_fd[1];

	close(pipe_read_fd[1]);

	if (fcntl(read_fd, F_SETFL, O_NONBLOCK) == -1) {
		abort_(errno, "fcntl: %s", strerror(errno));
	}
	close(pipe_write_fd[0]);
	if (fcntl(write_fd, F_SETFL, O_NONBLOCK) == -1) {
		abort_(errno, "fcntl: %s", strerror(errno));
	}
	status = calloc(1, sizeof(struct status_line));
	status->read_buffer_size = 8192;
	status->read_buffer = malloc(status->read_buffer_size);

	status->read = fdopen(read_fd, "r");
	if (status->read == NULL) {
		abort_(errno, "fdopen: %s", strerror(errno));
	}

	status->stop_signal = SIGSTOP;
	status->cont_signal = SIGCONT;

	status->pid = pid;
	status->read_fd = read_fd;
	status->write_fd = write_fd;

	pollfds[3].fd = read_fd;
}

static void status_line_close_pipe(void) {
	if (status->read_fd != -1) {
		close(status->read_fd);
		status->read_fd = -1;

		pollfds[3].fd = -1;
	}
	if (status->write_fd != -1) {
		close(status->write_fd);
		status->write_fd = -1;
	}

	if (status->read) {
		fclose(status->read);
		status->read = NULL;
	}
}

static void status_line_i3bar_block_destroy(struct status_line_i3bar_block *block);

static void status_line_fini(void) {
	status_line_close_pipe();
	kill(-status->pid, status->cont_signal);
	kill(-status->pid, SIGTERM);
	waitpid(status->pid, NULL, 0);

	free(status->read_buffer);

	if (status->protocol == STATUS_LINE_PROTOCOL_I3BAR) {
		json_tokener_free(status->tokener);
		for (size_t i = 0; i < status->blocks.len; ++i) {
			status_line_i3bar_block_destroy(status->blocks.items[i]);
		}
		ptr_array_fini(&status->blocks);
	}

	free(status);
	status = NULL;
}

static void status_line_set_error(const char *text) {
	status_line_close_pipe();

	free(status->read_buffer);
	status->read_buffer = NULL;

	if (status->protocol == STATUS_LINE_PROTOCOL_I3BAR) {
		if (status->tokener) {
			json_tokener_free(status->tokener);
		}
		for (size_t i = 0; i < status->blocks.len; ++i) {
			status_line_i3bar_block_destroy(status->blocks.items[i]);
		}
		ptr_array_fini(&status->blocks);
	}

	status->protocol = STATUS_LINE_PROTOCOL_ERROR;
	status->text = text;
}

static uint32_t to_x11_button(uint32_t code) {
	switch (code) {
	case BTN_LEFT:
		return 1;
	case BTN_MIDDLE:
		return 2;
	case BTN_RIGHT:
		return 3;
	case KEY_SCROLL_UP:
		return 4;
	case KEY_SCROLL_DOWN:
		return 5;
	case KEY_SCROLL_LEFT:
		return 6;
	case KEY_SCROLL_RIGHT:
		return 7;
	case BTN_SIDE:
		return 8;
	case BTN_EXTRA:
		return 9;
	default:
		return 0;
	}
}

static bool status_line_i3bar_block_pointer_button_callback(struct sbar_json_surface *bar_,
		struct block *block, struct sbar_json_box *hotspot,
		uint32_t code, enum sbar_pointer_button_state state,
		MAYBE_UNUSED uint32_t serial, double x, double y) {
	if ((state == SBAR_POINTER_BUTTON_STATE_RELEASED) || (status->write_fd == -1)) {
		return true;
	}

	// TODO: scale

	struct bar *bar = (struct bar *)bar_;
	double rx = x - hotspot->x;
	double ry = y - hotspot->y;

	static const unsigned flags =
		JSON_C_OBJECT_ADD_KEY_IS_NEW | JSON_C_OBJECT_ADD_CONSTANT_KEY;

	struct status_line_i3bar_block *i3bar_block = block->data;

	struct json_object *event = json_object_new_object();
	json_object_object_add_ex(event, "name",
			json_object_new_string(i3bar_block->name), flags);
	if (i3bar_block->instance) {
		json_object_object_add_ex(event, "instance",
				json_object_new_string(i3bar_block->instance), flags);
	}

	json_object_object_add_ex(event, "button",
			json_object_new_int64((int32_t)to_x11_button(code)), flags);
	json_object_object_add_ex(event, "event", json_object_new_int64((int32_t)code), flags);
	if (status->float_event_coords) {
		json_object_object_add_ex(event, "x", json_object_new_double(x), flags);
		json_object_object_add_ex(event, "y", json_object_new_double(y), flags);
		json_object_object_add_ex(event, "relative_x", json_object_new_double(rx), flags);
		json_object_object_add_ex(event, "relative_y", json_object_new_double(ry), flags);
		json_object_object_add_ex(event, "width",
			json_object_new_double((double)hotspot->width), flags);
		json_object_object_add_ex(event, "height",
			json_object_new_double((double)hotspot->height), flags);
	} else {
		json_object_object_add_ex(event, "x", json_object_new_int64((int32_t)x), flags);
		json_object_object_add_ex(event, "y", json_object_new_int64((int32_t)y), flags);
		json_object_object_add_ex(event, "relative_x", json_object_new_int64((int32_t)rx), flags);
		json_object_object_add_ex(event, "relative_y", json_object_new_int64((int32_t)ry), flags);
		json_object_object_add_ex(event, "width", json_object_new_int64(hotspot->width), flags);
		json_object_object_add_ex(event, "height", json_object_new_int64(hotspot->height), flags);
	}
	json_object_object_add_ex(event, "scale", json_object_new_int64(bar->_.scale), flags);

	if (dprintf(status->write_fd, "%s%s\n", status->clicked ? "," : "",
				json_object_to_json_string_ext(event, JSON_C_TO_STRING_PLAIN)) <= 0) {
		status_line_set_error("[failed to write click event]");
		bars_set_dirty();
	}
	status->clicked = true;

	json_object_put(event);

	return true;
}

static void status_line_i3bar_test_short_text_callback(struct sbar_json_surface *bar_) {
	struct sbar_json_box *valid = &((struct sbar_json_box *)bar_->block_hotspots.items)[0];
	if (!valid->width || !valid->height) {
		return;
	}

	struct bar *bar = (struct bar *)bar_;
	size_t last_status_block_idx = bar->_.blocks.len - 1
		- (binding_mode_indicator ? 1 : 0)
		- list_length(&bar->output->workspaces);
	int32_t reserved_width = 0;
	for (size_t i = bar->_.block_hotspots.len - 1; i > last_status_block_idx; --i) {
		struct sbar_json_box *hotspot = &((struct sbar_json_box *)bar->_.block_hotspots.items)[i];
		reserved_width += hotspot->width;
	}

	struct sbar_json_box *last_status_block_hotspot = &((struct sbar_json_box *)bar->
		_.block_hotspots.items)[last_status_block_idx];
	if (last_status_block_hotspot->x < reserved_width) {
		bar->dirty = true;
		bar->status_line_i3bar_short_text = true;
	}

	bar->_.updated_callback = NULL;
	bar->_.render = true;
	sbar_json_state_set_dirty(sbar);
}

static void status_line_get_blocks(struct bar *bar) {
	switch (status->protocol) {
	case STATUS_LINE_PROTOCOL_ERROR:
	case STATUS_LINE_PROTOCOL_TEXT: {
		struct block *block = block_create(block_destroy);
		block->_.anchor = SBAR_BLOCK_ANCHOR_RIGHT;
		block->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
		block->_.type = SBAR_BLOCK_TYPE_TEXT;
		block->_.text.text = strdup(status->text);
		ptr_array_init(&block->_.text.font_names, 1);
		ptr_array_add(&block->_.text.font_names, strdup(config.font));
		if (status->protocol == STATUS_LINE_PROTOCOL_TEXT) {
			block->_.text.color = bar->output->focused ?
				config.colors.focused_statusline : config.colors.statusline;
		} else {
			block->_.text.color = STATUS_ERROR_TEXT_COLOR;
		}
		block->_.borders[0].width = STATUS_MARGIN_LEFT;
		block->_.borders[1].width = STATUS_MARGIN_RIGHT;
		block->_.borders[2].width = config.status_padding;
		block->_.borders[3].width = config.status_padding;

		ptr_array_add(&bar->_.blocks, block);
		break;
	}
	case STATUS_LINE_PROTOCOL_I3BAR: {
		bool edge = bar->_.blocks.len == 1;

		for (size_t i = status->blocks.len - 1; i != SIZE_MAX; --i) {
			struct status_line_i3bar_block *i3bar_block = status->blocks.items[i];
			struct block *block = block_create(block_destroy);
			if (status->click_events && i3bar_block->name) {
				block->data = i3bar_block;
				block->pointer_button_callback = status_line_i3bar_block_pointer_button_callback;
			}

			switch (i3bar_block->type) {
			case STATUS_LINE_I3BAR_BLOCK_TYPE_I3BAR:
				if (edge && (config.status_edge_padding > 0)) {
					struct block *spacer = block_create(block_destroy);
					//spacer->_.type = SBAR_BLOCK_TYPE_SPACER;
					spacer->_.min_width = config.status_edge_padding;
					spacer->_.anchor = SBAR_BLOCK_ANCHOR_RIGHT;

					ptr_array_add(&bar->_.blocks, spacer);
				} else if (!edge && ((i3bar_block->separator_block_width > 0)
							|| i3bar_block->separator)) {
					struct block *separator = block_create(block_destroy);
					separator->_.anchor = SBAR_BLOCK_ANCHOR_RIGHT;
					separator->_.min_width = i3bar_block->separator_block_width;
					if (i3bar_block->separator) {
						if (config.separator_symbol) {
							separator->_.type = SBAR_BLOCK_TYPE_TEXT;
							separator->_.text.text = strdup(config.separator_symbol);
							ptr_array_init(&separator->_.text.font_names, 1);
							ptr_array_add(&separator->_.text.font_names, strdup(config.font));
							separator->_.text.color = bar->output->focused ?
								config.colors.focused_separator : config.colors.separator;
							separator->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
						} else {
							//separator->_.type = SBAR_BLOCK_TYPE_SPACER;
							int32_t width = MAX(i3bar_block->separator_block_width, STATUS_SEPARATOR_WIDTH);
							separator->_.min_width = width;
							separator->_.max_width = width;
							separator->_.color = bar->output->focused ?
								config.colors.focused_separator : config.colors.separator;
							int32_t border = (i3bar_block->separator_block_width - STATUS_SEPARATOR_WIDTH) / 2;
							separator->_.borders[0].width = border; // left
							if ((i3bar_block->separator_block_width - STATUS_SEPARATOR_WIDTH - border - border) == 1) {
								separator->_.borders[1].width = border + 1; // right
							} else {
								separator->_.borders[1].width = border; // right
							}
							separator->_.borders[0].color = 0xFFFF0000;
							separator->_.borders[1].color = 0xFFFF0000;
						}
					} else {
						//separator->_.type = SBAR_BLOCK_TYPE_SPACER;
					}

					ptr_array_add(&bar->_.blocks, separator);
				}

				block->_.type = SBAR_BLOCK_TYPE_COMPOSITE;
				ptr_array_init(&block->_.composite.blocks, 1);
				block->_.anchor = SBAR_BLOCK_ANCHOR_RIGHT;
				if (i3bar_block->min_width_str) {
					struct block *min_width = block_create(block_destroy);
					min_width->_.render = false;
					min_width->_.type = SBAR_BLOCK_TYPE_TEXT;
					min_width->_.text.text = strdup(i3bar_block->min_width_str);
					ptr_array_init(&min_width->_.text.font_names, 1);
					ptr_array_add(&min_width->_.text.font_names, strdup(config.font));

					ptr_array_add(&bar->_.blocks, min_width);

					block->_.min_width = SBAR_BLOCK_SIZE_PREV_BLOCK_WIDTH_PLUS;
				} else {
					block->_.min_width = i3bar_block->min_width;
				}
				block->_.color = i3bar_block->urgent ?
					config.colors.urgent_workspace.background : i3bar_block->background_color;
				block->_.content_anchor = i3bar_block->content_anchor;
				if (i3bar_block->border_color_set || i3bar_block->urgent) {
					for (size_t j = 0; j < LENGTH(block->_.borders); ++j) {
						block->_.borders[j].width = i3bar_block->border_widths[j];
						block->_.borders[j].color = i3bar_block->urgent ?
							config.colors.urgent_workspace.border : i3bar_block->border_color;
					}
				}

				struct block *text = block_create(block_destroy);
				text->_.type = SBAR_BLOCK_TYPE_TEXT;
				text->_.text.text =
					strdup(bar->status_line_i3bar_short_text && i3bar_block->short_text && *i3bar_block->short_text
						? i3bar_block->short_text : i3bar_block->full_text);
				ptr_array_init(&text->_.text.font_names, 1);
				ptr_array_add(&text->_.text.font_names, strdup(config.font));
				text->_.text.color = i3bar_block->urgent ? config.colors.urgent_workspace.text :
					(i3bar_block->text_color_set ? i3bar_block->text_color :
					(bar->output->focused ? config.colors.focused_statusline : config.colors.statusline));
				text->_.borders[0].width = STATUS_MARGIN_LEFT;
				text->_.borders[1].width = STATUS_MARGIN_RIGHT;
				text->_.borders[2].width = config.status_padding;
				text->_.borders[3].width = config.status_padding;

				ptr_array_add(&block->_.composite.blocks, text);

				ptr_array_add(&bar->_.blocks, block);

				if (!bar->status_line_i3bar_short_text && i3bar_block->short_text && *i3bar_block->short_text) {
					bar->_.updated_callback = status_line_i3bar_test_short_text_callback;
					bar->_.render = false;
				}
				break;
			case STATUS_LINE_I3BAR_BLOCK_TYPE_SBAR: {
				block->_.raw = true;
				block->_.json = json_object_get(i3bar_block->json);
				ptr_array_add(&bar->_.blocks, block);
				continue;
			}
			default:
				assert(UNREACHABLE);
				continue;
			}

			edge = false;
		}
		bar->status_line_i3bar_short_text = false;
		break;
	}
	case STATUS_LINE_PROTOCOL_UNDEF:
	default:
		break;
	}
}

static bool parse_json_color(json_object *color, uint32_t *dest) {
	if (color == NULL) {
		return false;
	}

	const char *color_str = json_object_get_string(color);
	if (color_str[0] == '#') {
		++color_str;
	}

	uint8_t r, g, b, a;
	switch (sscanf(color_str, "%2hhx%2hhx%2hhx%2hhx", &r, &g, &b, &a)) {
	case 3:
		*dest = ((uint32_t)0xFF << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | ((uint32_t)b << 0);
		return true;
	case 4:
		*dest = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | ((uint32_t)b << 0);
		return true;
	default:
		return false;
	}
}

static struct status_line_i3bar_block *status_line_i3bar_block_create(json_object *json) {
	json_object *full_text, *short_text, *color, *min_width, *align, *urgent;
	json_object *separator, *separator_block_width;
	json_object *background, *border, *border_top, *border_bottom;
	json_object *border_left, *border_right;//, *markup;
	json_object_object_get_ex(json, "full_text", &full_text);
	json_object_object_get_ex(json, "short_text", &short_text);
	json_object_object_get_ex(json, "color", &color);
	json_object_object_get_ex(json, "min_width", &min_width);
	json_object_object_get_ex(json, "align", &align);
	json_object_object_get_ex(json, "urgent", &urgent);
	//json_object_object_get_ex(json, "markup", &markup);
	json_object_object_get_ex(json, "separator", &separator);
	json_object_object_get_ex(json, "separator_block_width", &separator_block_width);
	json_object_object_get_ex(json, "background", &background);
	json_object_object_get_ex(json, "border", &border);
	json_object_object_get_ex(json, "border_top", &border_top);
	json_object_object_get_ex(json, "border_bottom", &border_bottom);
	json_object_object_get_ex(json, "border_left", &border_left);
	json_object_object_get_ex(json, "border_right", &border_right);

	json_object *sbar_block, *name, *instance;
	json_object_object_get_ex(json, "_sbar", &sbar_block);
	json_object_object_get_ex(json, "name", &name);
	json_object_object_get_ex(json, "instance", &instance);

	struct status_line_i3bar_block *block = malloc(sizeof(struct status_line_i3bar_block));
	block->name = name ? strdup(json_object_get_string(name)) : NULL;
	block->instance = instance ? strdup(json_object_get_string(instance)) : NULL;

	if (json_object_is_type(sbar_block, json_type_object)) {
		block->type = STATUS_LINE_I3BAR_BLOCK_TYPE_SBAR;
		block->json = json_object_get(sbar_block);
	} else { // i3bar
		const char *full_text_str = json_object_get_string(full_text);
		if (!full_text_str || !*full_text_str) {
			free(block);
			return NULL;
		}
		block->type = STATUS_LINE_I3BAR_BLOCK_TYPE_I3BAR;
		block->full_text = strdup(full_text_str);
		block->short_text = short_text ? strdup(json_object_get_string(short_text)) : NULL;
		if (!(block->text_color_set = parse_json_color(color, &block->text_color))) {
			block->text_color = 0;
		}
		block->min_width_str = NULL;
		block->min_width = 0;
		if (min_width) {
			json_type type = json_object_get_type(min_width);
			if (type == json_type_int) {
				block->min_width = (int32_t)json_object_get_uint64(min_width);
			} else if (type == json_type_string) {
				block->min_width_str = strdup(json_object_get_string(min_width));
			}
		}
		block->content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_LEFT_CENTER;
		if (align) {
			const char *align_str = json_object_get_string(align);
			if (strcmp(align_str, "left") == 0) {
				block->content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_LEFT_CENTER;
			} else if (strcmp(align_str, "center") == 0) {
				block->content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
			} else if (strcmp(align_str, "right") == 0) {
				block->content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_RIGHT_CENTER;
			}
		}
		block->urgent = urgent ? json_object_get_boolean(urgent) : false;
		// TODO: "markup"
		block->separator = separator ? json_object_get_boolean(separator) : true;
		block->separator_block_width = separator_block_width ?
			(int32_t)json_object_get_uint64(separator_block_width) : 9;
		if (!parse_json_color(background, &block->background_color)) {
			block->background_color = 0;
		}
		if (!(block->border_color_set = parse_json_color(border, &block->border_color))) {
			block->border_color = 0;
		}
		block->border_top = border_top ? json_object_get_int(border_top) : 1;
		block->border_right = border_right ? json_object_get_int(border_right) : 1;
		block->border_bottom = border_bottom ? json_object_get_int(border_bottom) : 1;
		block->border_left = border_left ? json_object_get_int(border_left) : 1;
	}

	return block;
}

static void status_line_i3bar_block_destroy(struct status_line_i3bar_block *block) {
	if (block == NULL) {
		return;
	}

	switch (block->type) {
	case STATUS_LINE_I3BAR_BLOCK_TYPE_I3BAR:
		free(block->full_text);
		free(block->short_text);
		free(block->min_width_str);
		break;
	case STATUS_LINE_I3BAR_BLOCK_TYPE_SBAR:
		json_object_put(block->json);
		break;
	default:
		assert(UNREACHABLE);
		break;
	}

	free(block->name);
	free(block->instance);

	free(block);
}

static void status_line_i3bar_parse_json(json_object *json_array) {
	for (size_t i = 0; i < status->blocks.len; ++i) {
		status_line_i3bar_block_destroy(status->blocks.items[i]);
	}
	status->blocks.len = 0;

	size_t json_array_len = json_object_array_length(json_array);
	for (size_t i = 0; i < json_array_len; ++i) {
		json_object *json = json_object_array_get_idx(json_array, i);
		if (json) {
			struct status_line_i3bar_block *i3bar_block =
				status_line_i3bar_block_create(json);
			if (i3bar_block) {
				ptr_array_add(&status->blocks, i3bar_block);
			}
		}
	}
}

static bool status_line_i3bar_process(void) {
	while (!status->started) {
		for (size_t c = 0; c < status->read_buffer_index; ++c) {
			if (status->read_buffer[c] == '[') {
				status->started = true;
				status->read_buffer_index -= ++c;
				memmove(status->read_buffer, &status->read_buffer[c],
						status->read_buffer_index);
				break;
			} else if (!isspace(status->read_buffer[c])) {
				status_line_set_error("[invalid i3bar json]");
				return true;
			}
		}
		if (status->started) {
			break;
		}

		errno = 0;
		ssize_t read_bytes = read(status->read_fd, status->read_buffer,
				status->read_buffer_size);
		if (read_bytes > 0) {
			status->read_buffer_index = (size_t)read_bytes;
		} else if (errno == EAGAIN) {
			return false;
		} else {
			status_line_set_error("[error reading from status command]");
			return true;
		}
	}

	json_object *last_object = NULL;
	json_object *test_object;
	size_t buffer_pos = 0;
	while (true) {
		if (status->expecting_comma) {
			for (; buffer_pos < status->read_buffer_index; ++buffer_pos) {
				if (status->read_buffer[buffer_pos] == ',') {
					status->expecting_comma = false;
					++buffer_pos;
					break;
				} else if (!isspace(status->read_buffer[buffer_pos])) {
					status_line_set_error("[invalid i3bar json]");
					return true;
				}
			}
			if (buffer_pos < status->read_buffer_index) {
				continue;
			}
			buffer_pos = status->read_buffer_index = 0;
		} else {
			test_object = json_tokener_parse_ex(status->tokener,
				&status->read_buffer[buffer_pos],
				(int)(status->read_buffer_index - buffer_pos));
			enum json_tokener_error err = json_tokener_get_error(status->tokener);
			if (err == json_tokener_success) {
				if (json_object_is_type(test_object, json_type_array)) {
					if (last_object) {
						json_object_put(last_object);
					}
					last_object = test_object;
				} else {
					json_object_put(test_object);
				}

				buffer_pos += (size_t)status->tokener->char_offset;
				status->expecting_comma = true;

				if (buffer_pos < status->read_buffer_index) {
					continue;
				}
				buffer_pos = status->read_buffer_index = 0;
			} else if (err == json_tokener_continue) {
				json_tokener_reset(status->tokener);
				if (status->read_buffer_index < status->read_buffer_size) {
					status->read_buffer_index -= buffer_pos;
					memmove(status->read_buffer, &status->read_buffer[buffer_pos],
							status->read_buffer_index);
					buffer_pos = 0;
				} else {
					status->read_buffer_size *= 2;
					status->read_buffer = realloc(status->read_buffer,
						status->read_buffer_size);
				}
			} else {
				status_line_set_error("[failed to parse i3bar json]");
				return true;
			}
		}

		errno = 0;
		ssize_t read_bytes = read(status->read_fd,
			&status->read_buffer[status->read_buffer_index],
			status->read_buffer_size - status->read_buffer_index);
		if (read_bytes > 0) {
			status->read_buffer_index += (size_t)read_bytes;
		} else if (errno == EAGAIN) {
			break;
		} else {
			status_line_set_error("[error reading from status command]");
			return true;
		}
	}

	if (last_object) {
		status_line_i3bar_parse_json(last_object);
		json_object_put(last_object);
		return true;
	} else {
		return false;
	}
}

static bool status_line_process(void) {
	ssize_t read_bytes = 1;
	switch (status->protocol) {
	case STATUS_LINE_PROTOCOL_UNDEF:
		errno = 0;
		int available_bytes;
		if (ioctl(status->read_fd, FIONREAD, &available_bytes) == -1) {
			status_line_set_error("[error reading from status command]");
			return true;
		}

		if (((size_t)available_bytes + 1) > status->read_buffer_size) {
			status->read_buffer_size = (size_t)available_bytes + 1;
			status->read_buffer = realloc(status->read_buffer, status->read_buffer_size);
		}

		read_bytes = read(status->read_fd, status->read_buffer, (size_t)available_bytes);
		if (read_bytes != available_bytes) {
			status_line_set_error("[error reading from status command]");
			return true;
		}
		status->read_buffer[available_bytes] = '\0';

		char *newline = strchr(status->read_buffer, '\n');
		json_object *header = NULL;
		json_object *version;
		if ((newline != NULL)
				&& (header = json_tokener_parse(status->read_buffer))
				&& json_object_object_get_ex(header, "version", &version)
				&& (json_object_get_int(version) == 1)) {

			json_object *click_events;
			if (json_object_object_get_ex(header, "click_events", &click_events)
					&& json_object_get_boolean(click_events)) {
				status->click_events = true;
				if (write(status->write_fd, "[\n", 2) != 2) {
					json_object_put(header);
					status_line_set_error("[failed to write to status command]");
					return true;
				}
			}

			json_object *float_event_coords;
			if (json_object_object_get_ex(header, "float_event_coords", &float_event_coords)
					&& json_object_get_boolean(float_event_coords)) {
				status->float_event_coords = true;
			}

			json_object *signal;
			if (json_object_object_get_ex(header, "stop_signal", &signal)) {
				status->stop_signal = json_object_get_int(signal);
			}
			if (json_object_object_get_ex(header, "cont_signal", &signal)) {
				status->cont_signal = json_object_get_int(signal);
			}

			json_object_put(header);

			status->protocol = STATUS_LINE_PROTOCOL_I3BAR;
			status->tokener = json_tokener_new();
			ptr_array_init(&status->blocks, 20);
			status->read_buffer_index = strlen(newline + 1);
			memmove(status->read_buffer, newline + 1, status->read_buffer_index + 1);
			return status_line_i3bar_process();
		} else {
			json_object_put(header);
		}

		status->protocol = STATUS_LINE_PROTOCOL_TEXT;
		status->text = status->read_buffer;
		ATTRIB_FALLTHROUGH;
	case STATUS_LINE_PROTOCOL_TEXT:
		while (true) {
			if ((read_bytes > 0) && (status->read_buffer[read_bytes - 1] == '\n')) {
				status->read_buffer[read_bytes - 1] = '\0';
			}
			errno = 0;
			read_bytes = getline(&status->read_buffer,
					&status->read_buffer_size, status->read);
			if (read_bytes == -1) {
				if (errno == EAGAIN) {
					clearerr(status->read);
				} else {
					status_line_set_error("[error reading from status command]");
				}
				return true;
			}
		}
	case STATUS_LINE_PROTOCOL_I3BAR:
		return status_line_i3bar_process();
	case STATUS_LINE_PROTOCOL_ERROR:
	default:
		assert(UNREACHABLE);
		break;
	}

	return false;
}

static void bar_update(struct bar *bar) {
	bar->_.wanted_height = config.height;
	memcpy(bar->_.bar.margins, config.gaps, sizeof(bar->_.bar.margins));
	if ((config.mode == CONFIG_MODE_OVERLAY) || (config.mode == CONFIG_MODE_HIDE)) {
		bar->_.bar.layer = SBAR_BAR_LAYER_OVERLAY;
		bar->_.bar.exclusive_zone = 0;
		if ((config.mode == CONFIG_MODE_OVERLAY) && (bar->_.input_regions.size > 0)) {
			array_put(&bar->_.input_regions, 0, &(struct sbar_json_box){ 0 });
		}
	} else {
		bar->_.bar.layer = SBAR_BAR_LAYER_BOTTOM;
		bar->_.bar.exclusive_zone = -1;
		bar->_.input_regions.len = 0;
	}
	bar->_.bar.anchor = (config.position == CONFIG_POSITION_BOTTOM)
		? SBAR_BAR_ANCHOR_BOTTOM : SBAR_BAR_ANCHOR_TOP;

	ptr_array_t old_blocks = bar->_.blocks;
	ptr_array_init(&bar->_.blocks, 50);

	struct block *bg = block_create(block_destroy);
	//bg->_.type = SBAR_BLOCK_TYPE_SPACER;
	bg->_.anchor = SBAR_BLOCK_ANCHOR_NONE;
	bg->_.color = bar->output->focused ? config.colors.focused_background : config.colors.background;
	ptr_array_add(&bar->_.blocks, bg);

	if (tray) {
		bool visible = (config.tray_outputs.len > 0) ? false : true;
		for (size_t i = 0; i < config.tray_outputs.len; ++i) {
			if (strcmp(bar->output->_.name, config.tray_outputs.items[i]) == 0) {
				visible = true;
				break;
			}
		}
		if (visible) {
			for (size_t i = 0; i < tray->sni->host.items.len; ++i) {
				struct tray_item *tray_item = tray->sni->host.items.items[i];
				ptr_array_add(&bar->_.blocks, block_ref(tray_item->block));
			}
		}
	}

	if (status) {
		status_line_get_blocks(bar);
	}

	if (config.workspace_buttons) {
		struct workspace *workspace;
		list_for_each(workspace, &bar->output->workspaces, link) {
			ptr_array_add(&bar->_.blocks, block_ref(workspace->block));
		}
	}

	if (config.binding_mode_indicator && binding_mode_indicator) {
		ptr_array_add(&bar->_.blocks, block_ref(binding_mode_indicator));
	}

	for (size_t i = 0; i < old_blocks.len; ++i) {
		block_unref(old_blocks.items[i]);
	}
	ptr_array_fini(&old_blocks);

	sbar_json_state_set_dirty(sbar);
}

static struct binding *binding_create(json_object *binding_json) {
	json_object *event_code, *command, *release;
	json_object_object_get_ex(binding_json, "event_code", &event_code);
	json_object_object_get_ex(binding_json, "command", &command);
	json_object_object_get_ex(binding_json, "release", &release);

	struct binding *binding = malloc(sizeof(struct binding));
	binding->event_code = (uint32_t)json_object_get_int(event_code);
	binding->command = strdup(json_object_get_string(command));
	binding->release = json_object_get_boolean(release);

	return binding;
}

static void binding_destroy(struct binding *binding) {
	if (binding == NULL) {
		return;
	}

	free(binding->command);

	free(binding);
}

static void update_config(const char *json) {
	json_object *config_json = json_tokener_parse(json);

	json_object *id;
	if (!json_object_object_get_ex(config_json, "id", &id)) {
		json_object *error;
		json_object_object_get_ex(config_json, "error", &error);
		abort_(1, "%s", json_object_get_string(error));
	}

	if (strcmp(bar_id, json_object_get_string(id)) != 0) {
		goto cleanup;
	}

	json_object *bar_height, *binding_mode_indicator_json, *bindings_array;
	json_object *colors_json/*, *font*/, *gaps, *hidden_state, *mode;
	json_object *outputs_array/*, *pango_markup*/, *position;
	json_object *separator_symbol, *status_command, *status_edge_padding;
	json_object *status_padding, *strip_workspace_name, *strip_workspace_numbers;
	json_object *workspace_buttons, *workspace_min_width, *wrap_scroll;
	json_object_object_get_ex(config_json, "bar_height", &bar_height);
	json_object_object_get_ex(config_json, "binding_mode_indicator", &binding_mode_indicator_json);
	json_object_object_get_ex(config_json, "bindings", &bindings_array);
	json_object_object_get_ex(config_json, "colors", &colors_json);
	//json_object_object_get_ex(config_json, "font", &font);
	json_object_object_get_ex(config_json, "gaps", &gaps);
	json_object_object_get_ex(config_json, "hidden_state", &hidden_state);
	json_object_object_get_ex(config_json, "mode", &mode);
	json_object_object_get_ex(config_json, "outputs", &outputs_array);
	//json_object_object_get_ex(config_json, "pango_markup", &pango_markup);
	json_object_object_get_ex(config_json, "position", &position);
	json_object_object_get_ex(config_json, "separator_symbol", &separator_symbol);
	json_object_object_get_ex(config_json, "status_command", &status_command);
	json_object_object_get_ex(config_json, "status_edge_padding", &status_edge_padding);
	json_object_object_get_ex(config_json, "status_padding", &status_padding);
	json_object_object_get_ex(config_json, "strip_workspace_name", &strip_workspace_name);
	json_object_object_get_ex(config_json, "strip_workspace_numbers", &strip_workspace_numbers);
	json_object_object_get_ex(config_json, "workspace_buttons", &workspace_buttons);
	json_object_object_get_ex(config_json, "workspace_min_width", &workspace_min_width);
	json_object_object_get_ex(config_json, "wrap_scroll", &wrap_scroll);

	json_object *tray_outputs_array, *tray_padding, *tray_bindings_array, *icon_theme;
	json_object_object_get_ex(config_json, "tray_outputs", &tray_outputs_array);
	json_object_object_get_ex(config_json, "tray_padding", &tray_padding);
	json_object_object_get_ex(config_json, "tray_bindings", &tray_bindings_array);
	json_object_object_get_ex(config_json, "icon_theme", &icon_theme);

	config.height = bar_height ? json_object_get_int(bar_height) : 0;

	config.binding_mode_indicator = binding_mode_indicator_json
		? json_object_get_boolean(binding_mode_indicator_json) : true;

	for (size_t i = 0; i < config.bindings.len; ++i) {
		binding_destroy(config.bindings.items[i]);
	}
	config.bindings.len = 0;
	if (bindings_array) {
		if (config.bindings.size == 0) {
			ptr_array_init(&config.bindings, 10);
		}
		size_t len = json_object_array_length(bindings_array);
		for (size_t i = 0; i < len; ++i) {
			struct binding *binding = binding_create(
				json_object_array_get_idx(bindings_array, i));
			ptr_array_add(&config.bindings, binding);
		}
	}

	static struct {
		const char *object;
		uint32_t *dest;
		uint32_t def;
	} colors[] = {
		{ "background", &config.colors.background, 0xFF000000 },
		{ "statusline", &config.colors.statusline, 0xFFFFFFFF },
		{ "separator", &config.colors.separator, 0xFF666666 },
		{ "focused_background", &config.colors.focused_background, 0xFF000000 },
		{ "focused_statusline", &config.colors.focused_statusline, 0xFFFFFFFF },
		{ "focused_separator", &config.colors.focused_separator, 0 }, // TODO: default
		{ "focused_workspace_border", &config.colors.focused_workspace.border, 0xFF4C7899 },
		{ "focused_workspace_bg", &config.colors.focused_workspace.background, 0xFF285577 },
		{ "focused_workspace_text", &config.colors.focused_workspace.text, 0xFFFFFFFF },
		{ "active_workspace_border", &config.colors.active_workspace.border, 0xFF333333 },
		{ "active_workspace_bg", &config.colors.active_workspace.background, 0xFF5F676A },
		{ "active_workspace_text", &config.colors.active_workspace.text, 0xFFFFFFFF },
		{ "inactive_workspace_border", &config.colors.inactive_workspace.border, 0xFF333333 },
		{ "inactive_workspace_bg", &config.colors.inactive_workspace.background, 0xFF222222 },
		{ "inactive_workspace_text", &config.colors.inactive_workspace.text, 0xFF888888 },
		{ "urgent_workspace_border", &config.colors.urgent_workspace.border, 0xFF2F343A },
		{ "urgent_workspace_bg", &config.colors.urgent_workspace.background, 0xFF900000 },
		{ "urgent_workspace_text", &config.colors.urgent_workspace.text, 0xFFFFFFFF },
		{ "binding_mode_border", &config.colors.binding_mode.border, 0xFF2F343A },
		{ "binding_mode_bg", &config.colors.binding_mode.background, 0xFF900000 },
		{ "binding_mode_text", &config.colors.binding_mode.text, 0xFFFFFFFF },
	};
	for (size_t i = 0; i < LENGTH(colors); ++i) {
		json_object *object;
		json_object_object_get_ex(colors_json, colors[i].object, &object);
		if (!parse_json_color(object, colors[i].dest)) {
			*colors[i].dest = colors[i].def;
		}
	}

	// TODO: "font"
	//free(config.font);
	config.font = FONT;

	if (gaps) {
		json_object *top, *right, *bottom, *left;
		json_object_object_get_ex(gaps, "top", &top);
		json_object_object_get_ex(gaps, "right", &right);
		json_object_object_get_ex(gaps, "bottom", &bottom);
		json_object_object_get_ex(gaps, "left", &left);
		config.gaps[0] = json_object_get_int(top);
		config.gaps[1] = json_object_get_int(right);
		config.gaps[2] = json_object_get_int(bottom);
		config.gaps[3] = json_object_get_int(left);
	} else {
		memset(config.gaps, 0, sizeof(config.gaps));
	}

	if (hidden_state) {
		const char *hidden_state_str = json_object_get_string(hidden_state);
		if ((strcmp(hidden_state_str, "show") == 0)) {
			config.hidden_state = CONFIG_HIDDEN_STATE_SHOW;
		} else {
			config.hidden_state = CONFIG_HIDDEN_STATE_HIDE;
		}
	} else {
		config.hidden_state = CONFIG_HIDDEN_STATE_HIDE;
	}

	if (mode) {
		const char *mode_str = json_object_get_string(mode);
		if (strcmp(mode_str, "hide") == 0){
			config.mode = CONFIG_MODE_HIDE;
		} else if (strcmp(mode_str, "invisible") == 0){
			config.mode = CONFIG_MODE_INVISIBLE;
		} else if (strcmp(mode_str, "overlay") == 0) {
			config.mode = CONFIG_MODE_OVERLAY;
		} else {
			config.mode = CONFIG_MODE_DOCK;
		}
	} else {
		config.mode = CONFIG_MODE_DOCK;
	}

	for (size_t i = 0; i < config.outputs.len; ++i) {
		free(config.outputs.items[i]);
	}
	config.outputs.len = 0;
	if (outputs_array) {
		if (config.outputs.size == 0) {
			ptr_array_init(&config.outputs, 4);
		}
		size_t len = json_object_array_length(outputs_array);
		for (size_t i = 0; i < len; ++i) {
			const char *output = json_object_get_string(
				json_object_array_get_idx(outputs_array, i));
			if (strcmp(output, "*") == 0) {
				for (size_t j = 0; j < config.outputs.len; ++j) {
					free(config.outputs.items[j]);
				}
				config.outputs.len = 0;
				break;
			}
			ptr_array_add(&config.outputs, strdup(output));
		}
	}

	// TODO: "pango_markup"

	if (position) {
		const char *position_str = json_object_get_string(position);
		if (strcmp(position_str, "top") == 0) {
			config.position = CONFIG_POSITION_TOP;
		} else {
			config.position = CONFIG_POSITION_BOTTOM;
		}
	} else {
		config.position = CONFIG_POSITION_BOTTOM;
	}

	free(config.separator_symbol);
	config.separator_symbol = separator_symbol
		? strdup(json_object_get_string(separator_symbol)) : NULL;

	char *new_status_command = status_command
		? strdup(json_object_get_string(status_command)) : NULL;

	config.status_edge_padding = status_edge_padding
		? json_object_get_int(status_edge_padding) : 3;

	config.status_padding = status_padding
		? json_object_get_int(status_padding) : 1;

	config.strip_workspace_name = strip_workspace_name
		? json_object_get_boolean(strip_workspace_name) : false;

	config.strip_workspace_numbers = strip_workspace_numbers
		? json_object_get_boolean(strip_workspace_numbers) : false;

	config.workspace_buttons = workspace_buttons
		? json_object_get_boolean(workspace_buttons) : true;

	config.workspace_min_width = workspace_min_width
		? json_object_get_int(workspace_min_width) : 0;

	config.wrap_scroll = wrap_scroll
		? json_object_get_boolean(wrap_scroll) : false;

	for (size_t i = 0; i < config.tray_outputs.len; ++i) {
		free(config.tray_outputs.items[i]);
	}
	config.tray_outputs.len = 0;
	bool tray_enabled = true;
	if (tray_outputs_array) {
		if (config.tray_outputs.size == 0) {
			ptr_array_init(&config.tray_outputs, 4);
		}
		size_t len = json_object_array_length(tray_outputs_array);
		for (size_t i = 0; i < len; ++i) {
			const char *tray_output = json_object_get_string(
				json_object_array_get_idx(tray_outputs_array, i));
			if (strcmp(tray_output, "none") == 0) {
				tray_enabled = false;
				for (size_t j = 0; j < config.tray_outputs.len; ++j) {
					free(config.tray_outputs.items[j]);
				}
				config.tray_outputs.len = 0;
				break;
			}
			ptr_array_add(&config.tray_outputs, strdup(tray_output));
		}
	}

	config.tray_padding = tray_padding ? json_object_get_int(tray_padding) : 2;

	config.tray_bindings.len = 0;
	if (tray_bindings_array) {
		if (config.tray_bindings.size == 0) {
			array_init(&config.tray_bindings, 10, sizeof(struct tray_binding));
		}
		size_t len = json_object_array_length(tray_bindings_array);
		for (size_t i = 0; i < len; ++i) {
			json_object *tray_binding_json = json_object_array_get_idx(
				tray_bindings_array, i);
			json_object *event_code_json, *command_json;
			json_object_object_get_ex(tray_binding_json, "event_code", &event_code_json);
			json_object_object_get_ex(tray_binding_json, "command", &command_json);

			enum tray_binding_command command;
			const char *command_str = json_object_get_string(command_json);
			if (strcmp(command_str, "ContextMenu") == 0) {
				command = TRAY_BINDING_COMMAND_CONTEXT_MENU;
			} else if (strcmp(command_str, "Activate") == 0) {
				command = TRAY_BINDING_COMMAND_ACTIVATE;
			} else if (strcmp(command_str, "SecondaryActivate") == 0) {
				command = TRAY_BINDING_COMMAND_SECONDARY_ACTIVATE;
			} else if (strcmp(command_str, "ScrollDown") == 0) {
				command = TRAY_BINDING_COMMAND_SCROLL_DOWN;
			} else if (strcmp(command_str, "ScrollLeft") == 0) {
				command = TRAY_BINDING_COMMAND_SCROLL_LEFT;
			} else if (strcmp(command_str, "ScrollRight") == 0) {
				command = TRAY_BINDING_COMMAND_SCROLL_RIGHT;
			} else if (strcmp(command_str, "ScrollUp") == 0) {
				command = TRAY_BINDING_COMMAND_SCROLL_UP;
			} else {
				command = TRAY_BINDING_COMMAND_NOP;
			}
			struct tray_binding tray_binding = {
				.event_code = (uint32_t)json_object_get_int(event_code_json),
				.command = command,
			};
			array_add(&config.tray_bindings, &tray_binding);
		}
	}

	free(config.icon_theme);
	config.icon_theme = icon_theme ? strdup(json_object_get_string(icon_theme)) : NULL;

	if (status && ((new_status_command == NULL) ||
				strcmp(new_status_command, config.status_command) != 0)) {
		status_line_fini();
	}
	if ((status == NULL) && new_status_command) {
		status_line_init(new_status_command);
	}
	free(config.status_command);
	config.status_command = new_status_command;

	if (tray_enabled && (tray == NULL)) {
		tray_init();
	} else if (!tray_enabled && tray) {
		tray_fini();
	} else if (tray) {
		tray_update();
	}

	sway_ipc_send(sway_ipc_fd, SWAY_IPC_MESSAGE_TYPE_GET_BINDING_STATE, NULL, 0);

cleanup:
	json_object_put(config_json);
}

static struct block *binding_mode_get_block(const char *mode) {
	struct block *block = block_create(block_destroy);
	block->_.type = SBAR_BLOCK_TYPE_COMPOSITE;
	ptr_array_init(&block->_.composite.blocks, 1);
	block->_.min_width = config.workspace_min_width;
	block->_.content_anchor = SBAR_BLOCK_CONTENT_ANCHOR_CENTER_CENTER;
	block->_.color = config.colors.binding_mode.background;
	for (size_t i = 0; i < LENGTH(block->_.borders); ++i) {
		block->_.borders[i].width = BINDING_MODE_INDICATOR_BORDER_WIDTH;
		block->_.borders[i].color = config.colors.binding_mode.border;
	}

	struct block *text = block_create(block_destroy);
	text->_.type = SBAR_BLOCK_TYPE_TEXT;
	text->_.text.text = strdup(mode);
	ptr_array_init(&text->_.text.font_names, 1);
	ptr_array_add(&text->_.text.font_names, strdup(config.font));
	text->_.text.color = config.colors.binding_mode.text;
	text->_.borders[0].width = BINDING_MODE_INDICATOR_MARGIN_LEFT;
	text->_.borders[1].width = BINDING_MODE_INDICATOR_MARGIN_RIGHT;
	text->_.borders[2].width = BINDING_MODE_INDICATOR_MARGIN_BOTTOM;
	text->_.borders[3].width = BINDING_MODE_INDICATOR_MARGIN_TOP;

	ptr_array_add(&block->_.composite.blocks, text);

	return block;
}

static bool bar_visible_on_output(struct output *output) {
	bool visible = !((config.mode == CONFIG_MODE_INVISIBLE)
		|| ((config.hidden_state == CONFIG_HIDDEN_STATE_HIDE) && (config.mode == CONFIG_MODE_HIDE)
		&& !visible_by_modifier && !visible_by_urgency && !visible_by_mode));
	if (visible && (config.outputs.len > 0)) {
		visible = false;
		for (size_t i = 0; i < config.outputs.len; ++i) {
			if (strcmp(output->_.name, config.outputs.items[i]) == 0) {
				visible = true;
				break;
			}
		}
	}

	return visible;
}

static int process_sway_ipc(void) {
	struct sway_ipc_response *response = sway_ipc_receive(sway_ipc_fd);
	if (response == NULL) {
		return -1;
	}

	bool update = false;

	switch (response->type) {
	case SWAY_IPC_MESSAGE_TYPE_EVENT_WORKSPACE:
		sway_ipc_send(sway_ipc_fd, SWAY_IPC_MESSAGE_TYPE_GET_WORKSPACES, NULL, 0);
		break;
	case SWAY_IPC_MESSAGE_TYPE_GET_WORKSPACES: {
		json_object *workspaces_array = json_tokener_parse(response->payload);

		{
			struct output *output;
			list_for_each(output, &sbar->outputs, _.link) {
				output->focused = false;
				struct workspace *workspace, *workspace_tmp;
				list_for_each_safe(workspace, workspace_tmp, &output->workspaces,
						link) {
					list_pop(&workspace->link);
					workspace_destroy(workspace);
				}
			}
		}

		visible_by_urgency = false;
		size_t workspaces_len = json_object_array_length(workspaces_array);
		for (size_t i = 0; i < workspaces_len; ++i) {
			json_object *workspace_json = json_object_array_get_idx(workspaces_array, i);
			json_object *output_json;
			json_object_object_get_ex(workspace_json, "output", &output_json);

			const char *output_name = json_object_get_string(output_json);
			struct output *output;
			list_for_each(output, &sbar->outputs, _.link) {
				if (strcmp(output->_.name, output_name) == 0) {
					struct workspace *workspace = workspace_create(workspace_json);
					if (workspace->focused) {
						output->focused = true;
					}
					if (workspace->urgent) {
						visible_by_urgency = true;
					}
					list_insert(output->workspaces.prev, &workspace->link);
				}
			}
		}

		json_object_put(workspaces_array);
		update = true;
		break;
	}
	case SWAY_IPC_MESSAGE_TYPE_EVENT_BAR_STATE_UPDATE: {
		json_object *json = json_tokener_parse(response->payload);

		json_object *id;
		json_object_object_get_ex(json, "id", &id);
		if (strcmp(bar_id, json_object_get_string(id)) == 0) {
			json_object *visible_by_modifier_json;
			json_object_object_get_ex(json, "visible_by_modifier",
				&visible_by_modifier_json);
			visible_by_modifier = json_object_get_boolean(visible_by_modifier_json);
			if (visible_by_modifier) {
				visible_by_mode = false;
				visible_by_urgency = false;
			}
			update = true;
		}

		json_object_put(json);
		break;
	}
	case SWAY_IPC_MESSAGE_TYPE_EVENT_MODE:
	case SWAY_IPC_MESSAGE_TYPE_GET_BINDING_STATE: {
		json_object *json = json_tokener_parse(response->payload);

		block_unref(binding_mode_indicator);

		json_object *mode_json;
		json_object_object_get_ex(json, response->type == SWAY_IPC_MESSAGE_TYPE_EVENT_MODE
			? "change" : "name",
				&mode_json);
		const char *mode = json_object_get_string(mode_json);
		if (strcmp(mode, "default") != 0) {
			binding_mode_indicator = binding_mode_get_block(mode);
			visible_by_mode = true;
		} else {
			binding_mode_indicator = NULL;
			visible_by_mode = false;
		}

		// TODO: "pango_markup"

		json_object_put(json);
		update = true;
		break;
	}
	case SWAY_IPC_MESSAGE_TYPE_EVENT_BARCONFIG_UPDATE:
		update_config(response->payload);
		update = true;
		break;
	case SWAY_IPC_MESSAGE_TYPE_SUBSCRIBE:
	case SWAY_IPC_MESSAGE_TYPE_COMMAND:
	case SWAY_IPC_MESSAGE_TYPE_GET_OUTPUTS:
	case SWAY_IPC_MESSAGE_TYPE_GET_TREE:
	case SWAY_IPC_MESSAGE_TYPE_GET_MARKS:
	case SWAY_IPC_MESSAGE_TYPE_GET_BAR_CONFIG:
	case SWAY_IPC_MESSAGE_TYPE_GET_VERSION:
	case SWAY_IPC_MESSAGE_TYPE_GET_BINDING_MODES:
	case SWAY_IPC_MESSAGE_TYPE_GET_CONFIG:
	case SWAY_IPC_MESSAGE_TYPE_SEND_TICK:
	case SWAY_IPC_MESSAGE_TYPE_SYNC:
	case SWAY_IPC_MESSAGE_TYPE_GET_INPUTS:
	case SWAY_IPC_MESSAGE_TYPE_GET_SEATS:
	case SWAY_IPC_MESSAGE_TYPE_EVENT_OUTPUT:
	case SWAY_IPC_MESSAGE_TYPE_EVENT_WINDOW:
	case SWAY_IPC_MESSAGE_TYPE_EVENT_BINDING:
	case SWAY_IPC_MESSAGE_TYPE_EVENT_SHUTDOWN:
	case SWAY_IPC_MESSAGE_TYPE_EVENT_TICK:
	case SWAY_IPC_MESSAGE_TYPE_EVENT_INPUT:
	default:
		break;
	}

	if (update) {
		struct output *output;
		list_for_each(output, &sbar->outputs, _.link) {
			bool visible = bar_visible_on_output(output);
			if (visible && list_empty(&output->_.bars)) {
				struct bar *bar = bar_create(output);
				list_insert(&output->_.bars, &bar->_.link);
			}
			struct bar *bar, *bar_tmp;
			list_for_each_safe(bar, bar_tmp, &output->_.bars, _.link) {
				if (visible) {
					bar->dirty = true;
				} else {
					list_pop(&bar->_.link);
					bar_destroy(bar);
				}
			}
		}
	}

	sway_ipc_response_free(response);
	return 1;
}

static bool init_sway_ipc(void) {
	if (sway_ipc_send(sway_ipc_fd, SWAY_IPC_MESSAGE_TYPE_GET_BAR_CONFIG,
			bar_id, (uint32_t)strlen(bar_id)) == -1) {
		return false;
	}
	struct sway_ipc_response *response = sway_ipc_receive(sway_ipc_fd);
	if (response == NULL) {
		return false;
	}
	assert(response->type == SWAY_IPC_MESSAGE_TYPE_GET_BAR_CONFIG);
	update_config(response->payload);
	sway_ipc_response_free(response);

	static const char subscribe[] =
		"[\"barconfig_update\",\"bar_state_update\",\"mode\",\"workspace\"]";
	if (sway_ipc_send(sway_ipc_fd, SWAY_IPC_MESSAGE_TYPE_SUBSCRIBE,
			subscribe, sizeof(subscribe)) == -1) {
		return false;
	}

	return true;
}

static void signal_handler(MAYBE_UNUSED int sig) {
	running = false;
}

static void setup(int argc, char **argv) {
	sbar = sbar_json_connect(NULL, output_create_sbar_json, seat_create_sbar_json);
	if (sbar == NULL) {
		abort_(errno, "Failed to initialize sbar: %s", strerror(errno));
	}
	sbar->state_events = true;
	sbar_json_state_set_dirty(sbar);
	pollfds[0].fd = sbar->read_fd;

	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'v'},
		{"socket", required_argument, NULL, 's'},
		{"bar_id", required_argument, NULL, 'b'},
		// ? TODO: sbar path cmdline opt
		{ 0 },
	};
	char *sway_ipc_socket_path = NULL;
	int c;
	while ((c = getopt_long(argc, argv, "hvs:b:", long_options, NULL)) != -1) {
		switch (c) {
		case 's':
			sway_ipc_socket_path = strdup(optarg);
			break;
		case 'b':
			bar_id = strdup(optarg);
			break;
		case 'v':
			abort_(0, VERSION);
		default:
			abort_(1, "Usage: sbar-swaybar [options...]\n"
				"\n"
				"  -h, --help             Show this help message and quit.\n"
				"  -v, --version          Show the version and quit.\n"
				"  -s, --socket <path>    Connect to sway via socket specified in <path>.\n"
				"  -b, --bar_id <id>      Bar ID for which to get the configuration.\n"
				"\n"
				" PLEASE NOTE that you can use swaybar_command field in your\n"
				" configuration file to let sway start sbar-swaybar automatically.\n"
				" You should never need to start it manually.\n");
		}
	}

	if (bar_id == NULL) {
		abort_(1, "No bar_id passed. Provide --bar_id or use swaybar_command in sway config file");
	}

	if (sway_ipc_socket_path == NULL) {
		sway_ipc_socket_path = sway_ipc_get_socket_path();
		if (sway_ipc_socket_path == NULL) {
			abort_(ESOCKTNOSUPPORT, "Failed to get sway ipc socket path");
		}
	}
	sway_ipc_fd = sway_ipc_connect(sway_ipc_socket_path);
	if (sway_ipc_fd == -1) {
		abort_(errno, "Failed to connect to sway ipc socket '%s': %s",
			sway_ipc_socket_path, strerror(errno));
	}
	if (!init_sway_ipc()) {
		abort_(errno, "Failed to initialize sway ipc: %s", strerror(errno));
	}
	pollfds[2].fd = sway_ipc_fd;
	free(sway_ipc_socket_path);

	static const struct sigaction sigact = {
		.sa_handler = signal_handler,
	};
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	//sigaction(SIGPIPE, &sigact, NULL);
}

static void run(void) {
	while (running) {
        int timeout_ms = -1;
		if (tray) {
			uint64_t usec;
			int ret = sni_server_get_poll_info(&pollfds[4].fd, (int *)&pollfds[4].events, &usec);
			if (ret < 0) {
				abort_(-ret, "sni_server_get_poll_info: %s", strerror(-ret));
			}
			switch (usec) {
			case UINT64_MAX:
				timeout_ms = -1;
				break;
			case 0:
				timeout_ms = 0;
				break;
			default: {
				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				uint64_t now_usec = (uint64_t)((now.tv_sec * 1000000) + (now.tv_nsec / 1000));
				timeout_ms = (int)((usec - now_usec) / 1000);
				break;
			}
			}
		}

		bool timed_out = false;
		switch (poll(pollfds, LENGTH(pollfds), timeout_ms)) {
		case -1:
			if (errno != EINTR) {
				abort_(errno, "poll: %s", strerror(errno));
			}
			break;
		case 0:
			timed_out = true;
			break;
		default:
			break;
		}

		static short err = POLLHUP | POLLERR | POLLNVAL;

		short events = pollfds[0].events | err;
		if (pollfds[0].revents & events) {
			if (sbar_json_process(sbar) == -1) {
				abort_(errno, "sbar_json_process: %s", strerror(errno));
			}
		}

		events = pollfds[2].events | err;
		if (pollfds[2].revents & events) {
			if (process_sway_ipc() == -1) {
				abort_(errno, "process_sway_ipc: %s", strerror(errno));
			}
		}

		if (status) {
			events = pollfds[3].events | err;
			if (pollfds[3].revents & events) {
				if (status_line_process()) {
					bars_set_dirty();
				}
			}
		}

		if (tray) {
			events = pollfds[4].events | err;
			if (timed_out || (pollfds[4].revents & events)) {
				int ret = sni_server_process();
				if (ret < 0) {
					abort_(-ret, "sni_server_process: %s", strerror(-ret));
				}
			}
		}

		{
			struct output *output;
			list_for_each(output, &sbar->outputs, _.link) {
				struct bar *bar;
				list_for_each(bar, &output->_.bars, _.link) {
					if (bar->dirty) {
						bar_update(bar);
						bar->dirty = false;
					}
				}
			}
		}

		switch (sbar_json_flush(sbar)) {
		case -1:
			abort_(errno, "sbar_json_flush: %s", strerror(errno));
		case 0:
			pollfds[1].fd = sbar->write_fd;
			break;
		default:
			if (pollfds[1].fd != -1) {
				pollfds[1].fd = -1;
			}
		}
	}
}

#if DEBUG
static void cleanup(void) {
	if (status) {
		status_line_fini();
	}

	if (tray) {
		tray_fini();
	}

	close(sway_ipc_fd);

	sbar_json_disconnect(sbar);

	block_unref(binding_mode_indicator);

	for (size_t i = 0; i < config.bindings.len; ++i) {
		binding_destroy(config.bindings.items[i]);
	}
	ptr_array_fini(&config.bindings);
	//free(config.font);
	for (size_t i = 0; i < config.outputs.len; ++i) {
		free(config.outputs.items[i]);
	}
	ptr_array_fini(&config.outputs);

	for (size_t i = 0; i < config.tray_outputs.len; ++i) {
		free(config.tray_outputs.items[i]);
	}
	ptr_array_fini(&config.tray_outputs);
	array_fini(&config.tray_bindings);
	free(config.icon_theme);

	free(config.status_command);
	free(config.separator_symbol);

	free(bar_id);
}
#endif // DEBUG

int main(int argc, char **argv) {
	setup(argc, argv);
	run();
#if DEBUG
	cleanup();
#endif // DEBUG

	return EXIT_SUCCESS;
}
