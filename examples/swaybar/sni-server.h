#if !defined(SNI_SERVER_H)
#define SNI_SERVER_H

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>
#include <time.h>

#if defined(HAVE_LIBSYSTEMD)
#include <systemd/sd-bus.h>
#elif defined(HAVE_LIBELOGIND)
#include <elogind/sd-bus.h>
#elif defined(HAVE_BASU)
#include <basu/sd-bus.h>
#endif

#include "util.h"
#include "macros.h"

enum sni_dbusmenu_menu_item_type {
    SNI_DBUSMENU_MENU_ITEM_TYPE_STANDARD,
    SNI_DBUSMENU_MENU_ITEM_TYPE_SEPARATOR,
};

enum sni_dbusmenu_menu_item_toggle_type {
    SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_NONE,
    SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_CHECKMARK,
    SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_RADIO,
};

enum sni_dbusmenu_menu_item_disposition {
    SNI_DBUSMENU_MENU_ITEM_DISPOSITION_NORNAL,
    SNI_DBUSMENU_MENU_ITEM_DISPOSITION_INFORMATIVE,
    SNI_DBUSMENU_MENU_ITEM_DISPOSITION_WARNING,
    SNI_DBUSMENU_MENU_ITEM_DISPOSITION_ALERT,
};

enum sni_dbusmenu_menu_item_event_type {
    SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_CLICKED,
    SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_HOVERED,
    SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_OPENED,
    SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_CLOSED,
};

struct sni_dbusmenu_menu_item {
    struct sni_dbusmenu_menu *parent_menu;
    struct sni_dbusmenu_menu *submenu; // may be NULL
    int id;
    enum sni_dbusmenu_menu_item_type type;
    char *label; // may be NULL
    int enabled; // bool
    int visible; // bool
    char *icon_name; // may be NULL
    struct { // png data of the icon
        void *bytes; // may be NULL
        size_t nbytes;
    } icon_data;
    void *shortcut; // NOT IMPLEMENTED
    enum sni_dbusmenu_menu_item_toggle_type toggle_type;
    int toggle_state; // 0 - off, 1 - on, else - indeterminate
    enum sni_dbusmenu_menu_item_disposition disposition;
    int activation_requested; // bool  NOT IMPLEMENTED
};

struct sni_dbusmenu_menu {
    struct sni_dbusmenu *dbusmenu;
    struct sni_dbusmenu_menu_item *parent_menu_item; // NULL when root
	ptr_array_t menu_items; // struct sni_dbusmenu_menu_item *
	size_t depth;
};

enum sni_dbusmenu_status {
    SNI_DBUSMENU_STATUS_INVALID,
    SNI_DBUSMENU_STATUS_NORMAL,
    SNI_DBUSMENU_STATUS_NOTICE,
};

enum sni_dbusmenu_text_direction {
    SNI_DBUSMENU_TEXT_DIRECTION_INVALID,
    SNI_DBUSMENU_TEXT_DIRECTION_LEFT_TO_RIGHT,
    SNI_DBUSMENU_TEXT_DIRECTION_RIGHT_TO_LEFT,
};

struct sni_dbusmenu_properties {
    ptr_array_t icon_theme_path; // char *
    enum sni_dbusmenu_status status;
    enum sni_dbusmenu_text_direction text_direction;
};

struct sni_dbusmenu {
	struct sni_item *item;
    struct sni_dbusmenu_properties *properties; // may be NULL
    struct sni_dbusmenu_menu *menu; // may be NULL
};

struct sni_item_pixmap {
    int width;
    int height;
    uint32_t pixels[]; // ARGB32, native byte order
	// width * height * 4
};

enum sni_item_status {
    SNI_ITEM_STATUS_INVALID,
    SNI_ITEM_STATUS_PASSIVE,
    SNI_ITEM_STATUS_ACTIVE,
    SNI_ITEM_STATUS_NEEDS_ATTENTION,
};

enum sni_item_category {
    SNI_ITEM_CATEGORY_INVALID,
    SNI_ITEM_CATEGORY_APPLICATION_STATUS,
    SNI_ITEM_CATEGORY_COMMUNICATIONS,
    SNI_ITEM_CATEGORY_SYSTEM_SERVICES,
    SNI_ITEM_CATEGORY_HARDWARE,
};

struct sni_item_properties {
    // every field may be NULL
    char *icon_name;
    char *icon_theme_path;
    ptr_array_t icon_pixmap; // struct sni_item_pixmap *  qsorted size descending
    enum sni_item_status status;
    enum sni_item_category category;
    char *menu;
    char *attention_icon_name;
    ptr_array_t attention_icon_pixmap; // struct sni_item_pixmap *  qsorted size descending
    int item_is_menu; // bool
    int window_id;
    char *id;
    char *title;
    char *attention_movie_name;
    char *overlay_icon_name;
    ptr_array_t overlay_icon_pixmap; // struct sni_item_pixmap *  qsorted size descending
    struct {
        char *icon_name;
        ptr_array_t icon_pixmap; // struct sni_item_pixmap *  qsorted size descending
        char *title;
        char *text;
    } tooltip;
};

enum sni_item_scroll_orientation {
    SNI_ITEM_SCROLL_ORIENTATION_VERTICAL,
    SNI_ITEM_SCROLL_ORIENTATION_HORIZONTAL,
};

typedef void (*sni_item_destroy_t)(struct sni_item *);
typedef void (*sni_item_properties_updated_callback_t)(struct sni_item *, struct sni_item_properties *old);
typedef void (*sni_item_dbusmenu_menu_updated_callback_t)(struct sni_item *, struct sni_dbusmenu_menu *old);

struct sni_item {
    struct sni_item_properties *properties; // may be NULL
    struct sni_dbusmenu *dbusmenu; // may be NULL

    // dbus
    char *watcher_id;
    char *service;
    char *path;

	list_t slots; // struct sni_slot::link

	sni_item_destroy_t destroy;
	sni_item_properties_updated_callback_t properties_updated;
	sni_item_dbusmenu_menu_updated_callback_t dbusmenu_menu_updated;
};

struct sni_slot {
	union {
		struct sni_item *item;
		struct sni_dbusmenu *dbusmenu;
	};
	sd_bus_slot *slot;
	list_t link; // struct item/sni_dbusmenu::slots
};

typedef struct sni_item *(*sni_item_create_t)(const char *id);

struct sni_server_state {
    sd_bus *bus;
	sni_item_create_t item_create;
    struct {
        char *interface;
        ptr_array_t items; // struct sni_item *
    } host;
    struct {
        ptr_array_t items; // char *
        ptr_array_t hosts; // char *
    } watcher;
};

static struct sni_server_state sni_server_state;

static const char sni_watcher_interface[] = "org.kde.StatusNotifierWatcher";
static const char sni_watcher_obj_path[] = "/StatusNotifierWatcher";
static int sni_watcher_protocol_version = 0;

static const char sni_item_interface[] = "org.kde.StatusNotifierItem";

static const char sni_dbusmenu_interface[] = "com.canonical.dbusmenu";

static int sni_item_pixmap_size_descending_qsort_private(const void *a, const void *b) {
	struct sni_item_pixmap *p1 = *(struct sni_item_pixmap **)a;
	struct sni_item_pixmap *p2 = *(struct sni_item_pixmap **)b;
	return (p2->width * p2->height) - (p1->width * p1->height);
}

static void sni_item_read_pixmap_private(sd_bus_message *msg, ptr_array_t *dest) {
	ptr_array_init(dest, 10);

	sd_bus_message_enter_container(msg, 'a', "(iiay)");
	while (sd_bus_message_enter_container(msg, 'r', "iiay") == 1) {
		int width, height;
		const void *bytes;
		size_t nbytes;
		sd_bus_message_read_basic(msg, 'i', &width);
		sd_bus_message_read_basic(msg, 'i', &height);
		sd_bus_message_read_array(msg, 'y', &bytes, &nbytes);
		if (((size_t)width * (size_t)height * 4) == nbytes) {
			struct sni_item_pixmap *pixmap = malloc(
				sizeof(struct sni_item_pixmap) + nbytes);
			pixmap->width = width;
			pixmap->height = height;
			for (int i = 0; i < (width * height); ++i) {
				pixmap->pixels[i] = ntohl(((const uint32_t *)bytes)[i]);
			}
			ptr_array_add(dest, pixmap);
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);

	ptr_array_qsort(dest, sni_item_pixmap_size_descending_qsort_private);
}

static void sni_item_properties_destroy_private(struct sni_item_properties *properties) {
	if (properties == NULL) {
		return;
	}

	free(properties->icon_name);
	free(properties->icon_theme_path);
	for (size_t i = 0; i < properties->icon_pixmap.len; ++i) {
		free(properties->icon_pixmap.items[i]);
	}
	ptr_array_fini(&properties->icon_pixmap);
	free(properties->menu);
	free(properties->attention_icon_name);
	for (size_t i = 0; i < properties->attention_icon_pixmap.len; ++i) {
		free(properties->attention_icon_pixmap.items[i]);
	}
	ptr_array_fini(&properties->attention_icon_pixmap);
	free(properties->id);
	free(properties->title);
	free(properties->attention_movie_name);
	free(properties->overlay_icon_name);
	for (size_t i = 0; i < properties->overlay_icon_pixmap.len; ++i) {
		free(properties->overlay_icon_pixmap.items[i]);
	}
	ptr_array_fini(&properties->overlay_icon_pixmap);
	for (size_t i = 0; i < properties->tooltip.icon_pixmap.len; ++i) {
		free(properties->tooltip.icon_pixmap.items[i]);
	}
	ptr_array_fini(&properties->tooltip.icon_pixmap);
	free(properties->tooltip.icon_name);
	free(properties->tooltip.title);
	free(properties->tooltip.text);

	free(properties);
}

static void sni_slot_free_private(struct sni_slot *slot) {
	sd_bus_slot_unref(slot->slot);
	list_pop(&slot->link);
	free(slot);
}

static void sni_dbusmenu_menu_item_destroy_private(struct sni_dbusmenu_menu_item *menu_item);

static void sni_dbusmenu_menu_destroy_private(struct sni_dbusmenu_menu *menu) {
	if (menu == NULL) {
		return;
	}

	for (size_t i = 0; i < menu->menu_items.len; ++i) {
		sni_dbusmenu_menu_item_destroy_private(menu->menu_items.items[i]);
	}
	ptr_array_fini(&menu->menu_items);

	free(menu);
}

static void sni_dbusmenu_menu_item_destroy_private(struct sni_dbusmenu_menu_item *menu_item) {
	if (menu_item == NULL) {
		return;
	}

	sni_dbusmenu_menu_destroy_private(menu_item->submenu);

	free(menu_item->label);
	free(menu_item->icon_name);
	free(menu_item->icon_data.bytes);

	free(menu_item);
}

static void sni_dbusmenu_properties_destroy_private(struct sni_dbusmenu_properties *properties) {
	if (properties == NULL) {
		return;
	}

	for (size_t i = 0; i < properties->icon_theme_path.len; ++i) {
		free(properties->icon_theme_path.items[i]);
	}
	ptr_array_fini(&properties->icon_theme_path);

	free(properties);
}

static void sni_dbusmenu_destroy(struct sni_dbusmenu *dbusmenu) {
	if (dbusmenu == NULL) {
		return;
	}

	sni_dbusmenu_menu_destroy_private(dbusmenu->menu);
	sni_dbusmenu_properties_destroy_private(dbusmenu->properties);

	free(dbusmenu);
}

static struct sni_dbusmenu_menu *sni_dbusmenu_menu_create_private(sd_bus_message *msg,
		struct sni_dbusmenu *dbusmenu, struct sni_dbusmenu_menu_item *parent_menu_item) {
	struct sni_dbusmenu_menu *menu = calloc(1, sizeof(struct sni_dbusmenu_menu));
	menu->dbusmenu = dbusmenu;
	menu->parent_menu_item = parent_menu_item;
	if (parent_menu_item) {
		menu->depth = parent_menu_item->parent_menu->depth + 1;
	}

	ptr_array_init(&menu->menu_items, 50);

	while (sd_bus_message_at_end(msg, 0) == 0) {
		sd_bus_message_enter_container(msg, 'v', "(ia{sv}av)");
		sd_bus_message_enter_container(msg, 'r', "ia{sv}av");

		struct sni_dbusmenu_menu_item *menu_item = calloc(1,
				sizeof(struct sni_dbusmenu_menu_item));
		menu_item->parent_menu = menu;
		//menu_item->type = SNI_DBUSMENU_MENU_ITEM_TYPE_STANDARD;
		menu_item->enabled = 1;
		menu_item->visible = 1;
		//menu_item->toggle_type = SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_NONE;
		menu_item->toggle_state = -1;
		//menu_item->disposition = SNI_DBUSMENU_MENU_ITEM_DISPOSITION_NORNAL;
		sd_bus_message_read_basic(msg, 'i', &menu_item->id);

		bool children = false;
		sd_bus_message_enter_container(msg, 'a', "{sv}");
		while (sd_bus_message_enter_container(msg, 'e', "sv") == 1) {
			char *key;
			sd_bus_message_read_basic(msg, 's', &key);
			sd_bus_message_enter_container(msg, 'v', NULL);
			if (strcmp(key, "type") == 0) {
				char *type;
				sd_bus_message_read_basic(msg, 's', &type);
				if (strcmp(type, "separator") == 0) {
					menu_item->type = SNI_DBUSMENU_MENU_ITEM_TYPE_SEPARATOR;
				}
			} else if (strcmp(key, "label") == 0) {
				char *label;
				sd_bus_message_read_basic(msg, 's', &label);
				menu_item->label = malloc(strlen(label) + 1);
				int i = 0;
				for (char *c = label; *c; ++c) {
					if ((*c == '_') && (!*++c)) {
						break;
					}
					menu_item->label[i++] = *c;
				}
				menu_item->label[i] = '\0';
				// TODO: handle '_', '__' properly
			} else if (strcmp(key, "enabled") == 0) {
				sd_bus_message_read_basic(msg, 'b', &menu_item->enabled);
			} else if (strcmp(key, "visible") == 0) {
				sd_bus_message_read_basic(msg, 'b', &menu_item->visible);
			} else if (strcmp(key, "icon-name") == 0) {
				sd_bus_message_read_basic(msg, 's', &menu_item->icon_name);
				menu_item->icon_name = strdup(menu_item->icon_name);
			} else if (strcmp(key, "icon-data") == 0) {
				const void *bytes;
				sd_bus_message_read_array(msg, 'y', &bytes, &menu_item->icon_data.nbytes);
				menu_item->icon_data.bytes = malloc(menu_item->icon_data.nbytes);
				memcpy(menu_item->icon_data.bytes, bytes, menu_item->icon_data.nbytes);
			//} else if (strcmp(key, "shortcut") == 0) {
			} else if (strcmp(key, "toggle-type") == 0) {
				char *toggle_type;
				sd_bus_message_read_basic(msg, 's', &toggle_type);
				if (strcmp(toggle_type, "checkmark") == 0) {
					menu_item->toggle_type = SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_CHECKMARK;
				} else if (strcmp(toggle_type, "radio") == 0) {
					menu_item->toggle_type = SNI_DBUSMENU_MENU_ITEM_TOGGLE_TYPE_RADIO;
				}
			} else if (strcmp(key, "toggle-state") == 0) {
				sd_bus_message_read_basic(msg, 'i', &menu_item->toggle_state);
			} else if (strcmp(key, "children-display") == 0) {
				char *children_display;
				sd_bus_message_read_basic(msg, 's', &children_display);
				if (strcmp(children_display, "submenu") == 0) {
					children = true;
				}
			} else if (strcmp(key, "disposition") == 0) {
				char *disposition;
				sd_bus_message_read_basic(msg, 's', &disposition);
				if (strcmp(disposition, "normal") == 0) {
					menu_item->disposition = SNI_DBUSMENU_MENU_ITEM_DISPOSITION_NORNAL;
				} else if (strcmp(disposition, "informative") == 0) {
					menu_item->disposition = SNI_DBUSMENU_MENU_ITEM_DISPOSITION_INFORMATIVE;
				} else if (strcmp(disposition, "warning") == 0) {
					menu_item->disposition = SNI_DBUSMENU_MENU_ITEM_DISPOSITION_WARNING;
				} else if (strcmp(disposition, "alert") == 0) {
					menu_item->disposition = SNI_DBUSMENU_MENU_ITEM_DISPOSITION_ALERT;
				}
			} else {
				// TODO: "shortcut"
				sd_bus_message_skip(msg, NULL);
			}
			sd_bus_message_exit_container(msg);
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);

		ptr_array_add(&menu->menu_items, menu_item);

		sd_bus_message_enter_container(msg, 'a', "v");
		if (children) { // && (menu_item->id != 0)) {
			menu_item->submenu = sni_dbusmenu_menu_create_private(msg, dbusmenu, menu_item);
		}
		sd_bus_message_exit_container(msg);
		sd_bus_message_exit_container(msg);
		sd_bus_message_exit_container(msg);
	}

	return menu;
}

static int sni_dbusmenu_get_layout_callback_private(sd_bus_message *msg, void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	struct sni_slot *slot = data;
	struct sni_dbusmenu *dbusmenu = slot->dbusmenu;

	sni_slot_free_private(slot);

	struct sni_dbusmenu_menu *old_menu = dbusmenu->menu;

	int ret = sd_bus_message_skip(msg, "u");
	if (ret < 0) {
		dbusmenu->menu = NULL;
	} else {
		dbusmenu->menu = sni_dbusmenu_menu_create_private(msg, dbusmenu, NULL);
		ret = 1;
	}

	struct sni_item *item = dbusmenu->item;
	if (item->dbusmenu_menu_updated) {
		item->dbusmenu_menu_updated(item, old_menu);
	}

	sni_dbusmenu_menu_destroy_private(old_menu);

	return ret;
}

static int sni_dbusmenu_get_layout_private(struct sni_dbusmenu *dbusmenu) {
	struct sni_slot *slot = malloc(sizeof(struct sni_slot));
	int ret = sd_bus_call_method_async(sni_server_state.bus, &slot->slot, dbusmenu->item->service,
			dbusmenu->item->properties->menu, sni_dbusmenu_interface, "GetLayout",
			sni_dbusmenu_get_layout_callback_private, slot, "iias", 0, -1, NULL);
	if (ret < 0) {
		free(slot);
		return ret;
	}

	slot->dbusmenu = dbusmenu;
	list_insert(&dbusmenu->item->slots, &slot->link);

	return 1;
}

static int sni_dbusmenu_handle_signal_private(MAYBE_UNUSED sd_bus_message *msg, void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	// ? TODO: error check
	struct sni_slot *slot = data;
	struct sni_dbusmenu *dbusmenu = slot->dbusmenu;

	// TODO: ItemActivationRequested

	sni_dbusmenu_get_layout_private(dbusmenu);
	return 1;
}

static int sni_dbusmenu_get_properties_callback_private(sd_bus_message *msg, void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	struct sni_slot *slot = data;
	struct sni_dbusmenu *dbusmenu = slot->dbusmenu;

	sni_slot_free_private(slot);

	int ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	struct sni_dbusmenu_properties *props = calloc(1, sizeof(struct sni_dbusmenu_properties));
	while (sd_bus_message_enter_container(msg, 'e', "sv") == 1) {
		char *key;
		sd_bus_message_read_basic(msg, 's', &key);
		sd_bus_message_enter_container(msg, 'v', NULL);
		if (strcmp(key, "IconThemePath") == 0) {
			sd_bus_message_read_strv(msg, (char ***)&props->icon_theme_path.items);
			if (props->icon_theme_path.items) {
				while (props->icon_theme_path.items[props->icon_theme_path.len]) {
					props->icon_theme_path.len++;
				}
				props->icon_theme_path.size = props->icon_theme_path.len + 1;
			}
		} else if (strcmp(key, "Status") == 0) {
			char *status;
			sd_bus_message_read_basic(msg, 's', &status);
			if (strcmp(status, "normal") == 0) {
				props->status = SNI_DBUSMENU_STATUS_NORMAL;
			} else if (strcmp(status, "notice") == 0) {
				props->status = SNI_DBUSMENU_STATUS_NOTICE;
			}
		} else if (strcmp(key, "TextDirection") == 0) {
			char *text_direction;
			sd_bus_message_read_basic(msg, 's', &text_direction);
			if (strcmp(text_direction, "ltr") == 0) {
				props->text_direction = SNI_DBUSMENU_TEXT_DIRECTION_LEFT_TO_RIGHT;
			} else if (strcmp(text_direction, "rtl") == 0) {
				props->text_direction = SNI_DBUSMENU_TEXT_DIRECTION_RIGHT_TO_LEFT;
			}
		} else {
			// ignored: Version
			sd_bus_message_skip(msg, NULL);
		}
		sd_bus_message_exit_container(msg);
		sd_bus_message_exit_container(msg);
	}
	//sd_bus_message_exit_container(msg);

	dbusmenu->properties = props;
	return 1;
}


static struct sni_dbusmenu *sni_dbusmenu_create_private(struct sni_item *item) {
	if (item->properties->menu == NULL) {
		return NULL;
	}

	struct sni_dbusmenu *dbusmenu = calloc(1, sizeof(struct sni_dbusmenu));

	struct sni_slot *slot1 = malloc(sizeof(struct sni_slot));
	int ret = sd_bus_call_method_async(sni_server_state.bus, &slot1->slot, item->service,
			item->properties->menu, "org.freedesktop.DBus.Properties", "GetAll",
			sni_dbusmenu_get_properties_callback_private, slot1, "s", sni_dbusmenu_interface);
	if (ret < 0) {
		goto error_1;
	}

	struct sni_slot *slot2 = malloc(sizeof(struct sni_slot));
	ret = sd_bus_match_signal_async(sni_server_state.bus, &slot2->slot, item->service,
			item->properties->menu, sni_dbusmenu_interface, NULL,
			sni_dbusmenu_handle_signal_private, sni_dbusmenu_handle_signal_private, slot2);
	if (ret < 0) {
		goto error_2;
	}

	slot1->dbusmenu = dbusmenu;
	list_insert(&item->slots, &slot1->link);
	slot2->dbusmenu = dbusmenu;
	list_insert(&item->slots, &slot2->link);

	dbusmenu->item = item;
	return dbusmenu;
error_2:
	sd_bus_slot_unref(slot1->slot);
	free(slot2);
error_1:
	free(slot1);
	free(dbusmenu);
	return NULL;
}

static int sni_item_get_properties_callback_private(sd_bus_message *msg, void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	struct sni_slot *slot = data;
	struct sni_item *item = slot->item;

	sni_slot_free_private(slot);
	struct sni_item_properties *old_properties = item->properties;

	int ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		item->properties = NULL;
		goto out;
	} else {
		item->properties = calloc(1, sizeof(struct sni_item_properties));
		ret = 1;
	}

	struct sni_item_properties *props = item->properties;
	while (sd_bus_message_enter_container(msg, 'e', "sv") == 1) {
		char *key;
		sd_bus_message_read_basic(msg, 's', &key);
		sd_bus_message_enter_container(msg, 'v', NULL);
		if (strcmp(key, "IconName") == 0) {
			sd_bus_message_read_basic(msg, 's', &props->icon_name);
			props->icon_name = strdup(props->icon_name);
		} else if (strcmp(key, "IconThemePath") == 0) {
			sd_bus_message_read_basic(msg, 's', &props->icon_theme_path);
			props->icon_theme_path = strdup(props->icon_theme_path);
		} else if (strcmp(key, "IconPixmap") == 0) {
			sni_item_read_pixmap_private(msg, &props->icon_pixmap);
		} else if (strcmp(key, "Status") == 0) {
			char *status;
			sd_bus_message_read_basic(msg, 's', &status);
			if (strcmp(status, "Active") == 0) {
				props->status = SNI_ITEM_STATUS_ACTIVE;
			} else if (strcmp(status, "Passive") == 0) {
				props->status = SNI_ITEM_STATUS_PASSIVE;
			} else if (strcmp(status, "NeedsAttention") == 0) {
				props->status = SNI_ITEM_STATUS_NEEDS_ATTENTION;
			}
		} else if (strcmp(key, "Category") == 0) {
			char *category;
			sd_bus_message_read_basic(msg, 's', &category);
			if (strcmp(category, "ApplicationStatus") == 0) {
				props->category = SNI_ITEM_CATEGORY_APPLICATION_STATUS;
			} else if (strcmp(category, "Communications") == 0) {
				props->category = SNI_ITEM_CATEGORY_COMMUNICATIONS;
			} else if (strcmp(category, "SystemServices") == 0) {
				props->category = SNI_ITEM_CATEGORY_SYSTEM_SERVICES;
			} else if (strcmp(category, "Hardware") == 0) {
				props->category = SNI_ITEM_CATEGORY_HARDWARE;
			}
		} else if (strcmp(key, "Menu") == 0) {
			sd_bus_message_read_basic(msg, 'o', &props->menu);
			props->menu = strdup(props->menu);
		} else if (strcmp(key, "AttentionIconName") == 0) {
			sd_bus_message_read_basic(msg, 's', &props->attention_icon_name);
			props->attention_icon_name = strdup(props->attention_icon_name);
		} else if (strcmp(key, "AttentionIconPixmap") == 0) {
			sni_item_read_pixmap_private(msg, &props->attention_icon_pixmap);
		} else if (strcmp(key, "ItemIsMenu") == 0) {
			sd_bus_message_read_basic(msg, 'b', &props->item_is_menu);
		} else if (strcmp(key, "WindowId") == 0) {
			sd_bus_message_read_basic(msg, 'i', &props->window_id);
		} else if (strcmp(key, "Id") == 0) {
			sd_bus_message_read_basic(msg, 's', &props->id);
			props->id = strdup(props->id);
		} else if (strcmp(key, "Title") == 0) {
			sd_bus_message_read_basic(msg, 's', &props->title);
			props->title = strdup(props->title);
		} else if (strcmp(key, "AttentionMovieName") == 0) {
			sd_bus_message_read_basic(msg, 's', &props->attention_movie_name);
			props->attention_movie_name = strdup(props->attention_movie_name);
		} else if (strcmp(key, "OverlayIconName") == 0) {
			sd_bus_message_read_basic(msg, 's', &props->overlay_icon_name);
			props->overlay_icon_name = strdup(props->overlay_icon_name);
		} else if (strcmp(key, "OverlayIconPixmap") == 0) {
			sni_item_read_pixmap_private(msg, &props->overlay_icon_pixmap);
		} else if (strcmp(key, "ToolTip") == 0) {
			sd_bus_message_enter_container(msg, 'r', "sa(iiay)ss");
			sd_bus_message_read_basic(msg, 's', &props->tooltip.icon_name);
			props->tooltip.icon_name = strdup(props->tooltip.icon_name);
			sni_item_read_pixmap_private(msg, &props->tooltip.icon_pixmap);
			sd_bus_message_read_basic(msg, 's', &props->tooltip.title);
			props->tooltip.title = strdup(props->tooltip.title);
			sd_bus_message_read_basic(msg, 's', &props->tooltip.text);
			props->tooltip.text = strdup(props->tooltip.text);
			sd_bus_message_exit_container(msg);
		} else {
			sd_bus_message_skip(msg, NULL);
		}
		sd_bus_message_exit_container(msg);
		sd_bus_message_exit_container(msg);
	}
	//sd_bus_message_exit_container(msg);

	if (item->dbusmenu == NULL) {
		item->dbusmenu = sni_dbusmenu_create_private(item);
	}

out:
	if (item->properties_updated) {
		item->properties_updated(item, old_properties);
	}

	sni_item_properties_destroy_private(old_properties);

	return ret;
}

static int sni_item_handle_signal_private(MAYBE_UNUSED sd_bus_message *msg, void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	// ? TODO: error check
	struct sni_item *item = data;

	struct sni_slot *slot = malloc(sizeof(struct sni_slot));
	int ret = sd_bus_call_method_async(sni_server_state.bus, &slot->slot, item->service,
			item->path, "org.freedesktop.DBus.Properties", "GetAll",
			sni_item_get_properties_callback_private, slot, "s", sni_item_interface);
	if (ret >= 0) {
		slot->item = item;
		list_insert(&item->slots, &slot->link);
	} else {
		free(slot);
	}

	return 1;
}

static MAYBE_UNUSED bool sni_item_init(struct sni_item *item, const char *id, sni_item_destroy_t destroy) {
    char *path_ptr = strchr(id, '/');
    if (path_ptr == NULL) {
        return false;
    }

	char *service = strndup(id, (size_t)(path_ptr - id));

	sd_bus_slot *slot_;
	int ret = sd_bus_match_signal_async(sni_server_state.bus, &slot_, service,
			path_ptr, sni_item_interface, NULL,
			sni_item_handle_signal_private, sni_item_handle_signal_private, item);
	if (ret < 0) {
		free(service);
		return false;
	}

	memset(item, 0, sizeof(struct sni_item));

	list_init(&item->slots);

	struct sni_slot *slot = malloc(sizeof(struct sni_slot));
	slot->slot = slot_;
	list_insert(&item->slots, &slot->link);

	item->watcher_id = strdup(id);
	item->service = service;
	item->path = strdup(path_ptr);
	item->destroy = destroy;

    return true;
}

static MAYBE_UNUSED void sni_item_fini(struct sni_item *item) {
	struct sni_slot *slot, *slot_tmp;
	list_for_each_safe(slot, slot_tmp, &item->slots, link) {
        sd_bus_slot_unref(slot->slot);
        free(slot);
	}

	sni_dbusmenu_destroy(item->dbusmenu);

	sni_item_properties_destroy_private(item->properties);

    free(item->watcher_id);
	free(item->service);
	free(item->path);
}

static int sni_watcher_handle_register_item_private(sd_bus_message *msg, MAYBE_UNUSED void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	const char *service_or_path;
	int ret = sd_bus_message_read_basic(msg, 's', &service_or_path);
	if (ret < 0) {
		return ret;
	}

	const char *service, *path;
	if (service_or_path[0] == '/') {
		service = sd_bus_message_get_sender(msg);
		path = service_or_path;
	} else {
		service = service_or_path;
		path = "/StatusNotifierItem";
	}

    char *id = fstr_create("%s%s", service, path);
    for (size_t i = 0; i < sni_server_state.watcher.items.len; ++i) {
        if (strcmp(id, sni_server_state.watcher.items.items[i]) == 0) {
            free(id);
            return -EEXIST;
        }
    }

    ret = sd_bus_emit_signal(sni_server_state.bus, sni_watcher_obj_path, sni_watcher_interface,
			"StatusNotifierItemRegistered", "s", id);
    if (ret < 0) {
        free(id);
        return ret;
    }

    ptr_array_add(&sni_server_state.watcher.items, id);

    ret = sd_bus_reply_method_return(msg, "");
    if (ret < 0) {
        return ret;
    }

	return 1;
}

static int sni_watcher_handle_register_host_private(sd_bus_message *msg, MAYBE_UNUSED void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	const char *service;
	int ret = sd_bus_message_read_basic(msg, 's', &service);
	if (ret < 0) {
		return ret;
	}

    for (size_t i = 0; i < sni_server_state.watcher.hosts.len; ++i) {
        if (strcmp(service, sni_server_state.watcher.hosts.items[i]) == 0) {
            return -EEXIST;
        }
    }

    ret = sd_bus_emit_signal(sni_server_state.bus, sni_watcher_obj_path, sni_watcher_interface,
			"StatusNotifierHostRegistered", "");
    if (ret < 0) {
        return ret;
    }

    ptr_array_add(&sni_server_state.watcher.hosts, strdup(service));

    ret = sd_bus_reply_method_return(msg, "");
    if (ret < 0) {
        return ret;
    }

	return 1;
}

static int sni_watcher_handle_get_registered_items_private(MAYBE_UNUSED sd_bus *b, MAYBE_UNUSED const char *path,
		MAYBE_UNUSED const char *iface, MAYBE_UNUSED const char *prop, sd_bus_message *reply,
		MAYBE_UNUSED void *data, MAYBE_UNUSED sd_bus_error *ret_error) {
    ptr_array_add(&sni_server_state.watcher.items, NULL);
    int ret = sd_bus_message_append_strv(reply, (char **)sni_server_state.watcher.items.items);
    sni_server_state.watcher.items.len--;
    if (ret < 0) {
		return ret;
    }

    return 1;
}

static int sni_watcher_handle_is_host_registered_private(MAYBE_UNUSED sd_bus *b, MAYBE_UNUSED const char *path,
		MAYBE_UNUSED const char *iface, MAYBE_UNUSED const char *prop, sd_bus_message *reply,
		MAYBE_UNUSED void *data, MAYBE_UNUSED sd_bus_error *ret_error) {
	int registered = 1; // sni_server_state.watcher.hosts.len > 0
	int ret = sd_bus_message_append_basic(reply, 'b', &registered);
	if (ret < 0) {
		return ret;
	}

	return 1;
}

static const sd_bus_vtable sni_watcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", sni_watcher_handle_register_item_private,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", sni_watcher_handle_register_host_private,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", sni_watcher_handle_get_registered_items_private,
			0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", sni_watcher_handle_is_host_registered_private,
			0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ProtocolVersion", "i", NULL, 0,
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierHostRegistered", NULL, 0),
	SD_BUS_SIGNAL("StatusNotifierHostUnregistered", NULL, 0),
	SD_BUS_VTABLE_END,
};

static int sni_watcher_handle_lost_service_private(sd_bus_message *msg, MAYBE_UNUSED void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	char *service, *old_owner, *new_owner;
	int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
	if (ret < 0) {
		return ret;
	}
    if (*new_owner) {
        return 0;
    }

    size_t service_len = strlen(service);
    for (size_t i = 0; i < sni_server_state.watcher.items.len; ++i) {
        char *item = sni_server_state.watcher.items.items[i];
        if (strncmp(item, service, service_len) == 0) {
            ret = sd_bus_emit_signal(sni_server_state.bus, sni_watcher_obj_path, sni_watcher_interface,
					"StatusNotifierItemUnregistered", "s", item);
            if (ret < 0) {
                return ret;
            }
            free(item);
            ptr_array_pop(&sni_server_state.watcher.items, i);
            return 0;
        }
    }

    for (size_t i = 0; i < sni_server_state.watcher.hosts.len; ++i) {
        char *host = sni_server_state.watcher.hosts.items[i];
        if (strcmp(service, host) == 0) {
            ret = sd_bus_emit_signal(sni_server_state.bus, sni_watcher_obj_path, sni_watcher_interface,
					"StatusNotifierHostUnregistered", "");
			if (ret < 0) {
                return ret;
            }
            free(host);
            ptr_array_pop(&sni_server_state.watcher.hosts, i);
            return 0;
        }
    }

	return 0;
}

static void sni_host_add_item_private(const char *id) {
	for (size_t i = 0; i < sni_server_state.host.items.len; ++i) {
		struct sni_item *item = sni_server_state.host.items.items[i];
		if (strcmp(id, item->watcher_id) == 0) {
			return;
		}
	}

	struct sni_item *item = sni_server_state.item_create(id);
	if (item) {
		ptr_array_add(&sni_server_state.host.items, item);
	}
}

static int sni_host_get_registered_items_callback_private(sd_bus_message *msg, MAYBE_UNUSED void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	int ret = sd_bus_message_enter_container(msg, 'v', "as");
	if (ret < 0) {
		return ret;
	}

	char **ids;
	ret = sd_bus_message_read_strv(msg, &ids);
	if (ret < 0) {
		return ret;
	}

	if (ids) {
		for (char **id = ids; *id; ++id) {
			sni_host_add_item_private(*id);
			free(*id);
		}
	}

	free(ids);
	//sd_bus_message_exit_container(msg);
	return 1;
}

static int sni_host_register_to_watcher_private(void) {
	// ? TODO: slots
	int ret = sd_bus_call_method_async(sni_server_state.bus, NULL,
			sni_watcher_interface, sni_watcher_obj_path, sni_watcher_interface,
			"RegisterStatusNotifierHost", NULL, NULL,
			"s", sni_server_state.host.interface);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_call_method_async(sni_server_state.bus, NULL,
			sni_watcher_interface, sni_watcher_obj_path,
			"org.freedesktop.DBus.Properties", "Get",
			sni_host_get_registered_items_callback_private, NULL, "ss",
			sni_watcher_interface, "RegisteredStatusNotifierItems");
	if (ret < 0) {
		return ret;
	}

	return 1;
}

static int sni_host_handle_item_registered_private(sd_bus_message *msg, MAYBE_UNUSED void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	const char *id;
	int ret = sd_bus_message_read_basic(msg, 's', &id);
	if (ret < 0) {
		return ret;
	}

	sni_host_add_item_private(id);
	return 1;
}

static int sni_host_handle_item_unregistered_private(sd_bus_message *msg, MAYBE_UNUSED void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	const char *id;
	int ret = sd_bus_message_read_basic(msg, 's', &id);
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0; i < sni_server_state.host.items.len; ++i) {
		struct sni_item *item = sni_server_state.host.items.items[i];
		if (strcmp(item->watcher_id, id) == 0) {
			item->destroy(item);
            ptr_array_pop(&sni_server_state.host.items, i);
			break;
		}
	}

	return 1;
}

static int sni_host_handle_new_watcher_private(sd_bus_message *msg, MAYBE_UNUSED void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	char *service, *old_owner, *new_owner;
	int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
	if (ret < 0) {
		return ret;
	}

	if (strcmp(service, sni_watcher_interface) == 0) {
        ret = sni_host_register_to_watcher_private();
		if (ret < 0) {
			return ret;
		}
        for (size_t i = 0; i < sni_server_state.host.items.len; ++i) {
			struct sni_item *item = sni_server_state.host.items.items[i];
            item->destroy(item);
        }
        sni_server_state.host.items.len = 0;
	}

	return 0;
}

static MAYBE_UNUSED void sni_server_fini(void) {
	sd_bus_flush_close_unref(sni_server_state.bus);

	for (size_t i = 0; i < sni_server_state.host.items.len; ++i) {
		struct sni_item *item = sni_server_state.host.items.items[i];
		item->destroy(item);
	}
	free(sni_server_state.host.interface);
	for (size_t i = 0; i < sni_server_state.watcher.items.len; ++i) {
		free(sni_server_state.watcher.items.items[i]);
	}
	for (size_t i = 0; i < sni_server_state.watcher.hosts.len; ++i) {
		free(sni_server_state.watcher.hosts.items[i]);
	}
	ptr_array_fini(&sni_server_state.host.items);
	ptr_array_fini(&sni_server_state.watcher.items);
	ptr_array_fini(&sni_server_state.watcher.hosts);

    memset(&sni_server_state, 0, sizeof(struct sni_server_state));
}

static MAYBE_UNUSED int sni_server_init(sni_item_create_t item_create) {
	assert(sni_server_state.bus == NULL);

    int ret = sd_bus_open_user(&sni_server_state.bus);
    if (ret < 0) {
		return ret;
    }

	ret = sd_bus_request_name(sni_server_state.bus, sni_watcher_interface, SD_BUS_NAME_QUEUE);
	if (ret < 0) {
        goto error;
    }

    ret = sd_bus_add_object_vtable(sni_server_state.bus, NULL, sni_watcher_obj_path,
            sni_watcher_interface, sni_watcher_vtable, &sni_watcher_protocol_version);
    if (ret < 0) {
        goto error;
    }

    ret = sd_bus_match_signal(sni_server_state.bus, NULL, "org.freedesktop.DBus",
			"/org/freedesktop/DBus", "org.freedesktop.DBus",
			"NameOwnerChanged", sni_watcher_handle_lost_service_private, NULL);
    if (ret < 0) {
        goto error;
    }

    ptr_array_init(&sni_server_state.watcher.items, 10);
    ptr_array_init(&sni_server_state.watcher.hosts, 4);


	sni_server_state.host.interface = fstr_create("org.kde.StatusNotifierHost-%d", getpid());
    ret = sd_bus_request_name(sni_server_state.bus, sni_server_state.host.interface, 0); // ? SD_BUS_NAME_QUEUE
    if (ret < 0) {
        goto error;
    }

    ret = sni_host_register_to_watcher_private();
    if (ret < 0) {
        goto error;
    }

    ret = sd_bus_match_signal(sni_server_state.bus, NULL, sni_watcher_interface,
			sni_watcher_obj_path, sni_watcher_interface,
			"StatusNotifierItemRegistered", sni_host_handle_item_registered_private, NULL);
	if (ret < 0) {
		goto error;
	}
	ret = sd_bus_match_signal(sni_server_state.bus, NULL, sni_watcher_interface,
			sni_watcher_obj_path, sni_watcher_interface,
			"StatusNotifierItemUnregistered", sni_host_handle_item_unregistered_private, NULL);
	if (ret < 0) {
		goto error;
	}

	ret = sd_bus_match_signal(sni_server_state.bus, NULL, "org.freedesktop.DBus",
			"/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
			sni_host_handle_new_watcher_private, NULL);
	if (ret < 0) {
		goto error;
	}

    ptr_array_init(&sni_server_state.host.items, 10);


	sni_server_state.item_create = item_create;
	return 1;
error:
	sni_server_fini();
	return ret;
}

static MAYBE_UNUSED int sni_server_get_poll_info(int *fd_, int *events_, uint64_t *timeout_) {
	assert(sni_server_state.bus != NULL);

	int fd = sd_bus_get_fd(sni_server_state.bus);
	if (fd < 0) {
		return fd;
	}

	int events = sd_bus_get_events(sni_server_state.bus);
	if (events < 0) {
		return events;
	}

	uint64_t timeout;
	int ret = sd_bus_get_timeout(sni_server_state.bus, &timeout);
	if (ret < 0) {
		return ret;
	}

	*fd_ = fd;
	*events_ = events;
	*timeout_ = timeout;
	return 1;
}

static MAYBE_UNUSED int sni_server_process(void) {
	assert(sni_server_state.bus != NULL);

	int ret;
    while ((ret = sd_bus_process(sni_server_state.bus, NULL)) > 0)
	{
	}
	if (ret < 0) {
		return ret;
	}

	return 1;
}

static int sni_item_method_callback_private(MAYBE_UNUSED sd_bus_message *msg, void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	struct sni_slot *slot = data;
	sni_slot_free_private(slot);
	return 1;
}

static int sni_item_call_method_async_private(struct sni_item *item, const char *method,
		const char *types, ...) {
    va_list args;
    va_start(args, types);

	struct sni_slot *slot = malloc(sizeof(struct sni_slot));
	int ret = sd_bus_call_method_asyncv(sni_server_state.bus, &slot->slot, item->service,
			item->path, sni_item_interface, method,
			sni_item_method_callback_private, slot, types, args);
	va_end(args);
	if (ret < 0) {
		free(slot);
		return ret;
	}

	list_insert(&item->slots, &slot->link);

	return 1;
}

static int sni_item_call_method_private(struct sni_item *item, const char *method,
		const char *types, ...) {
    va_list args;
    va_start(args, types);

	int ret = sd_bus_call_methodv(sni_server_state.bus, item->service,
			item->path, sni_item_interface, method,
			NULL, NULL, types, args);
	va_end(args);
	if (ret < 0) {
		return ret;
	}

	return 1;
}

static MAYBE_UNUSED int sni_item_context_menu(struct sni_item *item, int x, int y, bool async) {
	assert(sni_server_state.bus != NULL);

	return async ? sni_item_call_method_async_private(item, "ContextMenu", "ii", x, y)
		: sni_item_call_method_private(item, "ContextMenu", "ii", x, y);
}

static MAYBE_UNUSED int sni_item_activate(struct sni_item *item, int x, int y, bool async) {
	assert(sni_server_state.bus != NULL);

	return async ? sni_item_call_method_async_private(item, "Activate", "ii", x, y)
		: sni_item_call_method_private(item, "Activate", "ii", x, y);
}

static MAYBE_UNUSED int sni_item_secondary_activate(struct sni_item *item, int x, int y, bool async) {
	assert(sni_server_state.bus != NULL);

	return async ? sni_item_call_method_async_private(item, "SecondaryActivate", "ii", x, y)
		: sni_item_call_method_private(item, "SecondaryActivate", "ii", x, y);
}

static MAYBE_UNUSED int sni_item_scroll(struct sni_item *item, int delta,
		enum sni_item_scroll_orientation orientation, bool async) {
	assert(sni_server_state.bus != NULL);

	return async ? sni_item_call_method_async_private(item, "Scroll", "is", delta,
			(orientation == SNI_ITEM_SCROLL_ORIENTATION_VERTICAL) ? "vertical" : "horizontal")
		: sni_item_call_method_private(item, "Scroll", "is", delta,
			(orientation == SNI_ITEM_SCROLL_ORIENTATION_VERTICAL) ? "vertical" : "horizontal");
}

static MAYBE_UNUSED int sni_dbusmenu_menu_item_event(struct sni_dbusmenu_menu_item *menu_item,
		enum sni_dbusmenu_menu_item_event_type type, bool async) {
	assert(sni_server_state.bus != NULL);
	assert(((type == SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_OPENED)
				|| (type == SNI_DBUSMENU_MENU_ITEM_EVENT_TYPE_CLOSED))
			? (menu_item->submenu != NULL) : 1);

	struct sni_dbusmenu *dbusmenu = menu_item->parent_menu->dbusmenu;
	if ((dbusmenu->item->properties == NULL) || (dbusmenu->item->properties->menu == NULL)) {
		return -ENOENT;
	}

	static const char *event_types[] = {
		"clicked",
		"hovered",
		"opened",
		"closed",
	};

	if (async) {
		struct sni_slot *slot = malloc(sizeof(struct sni_slot));
		int ret = sd_bus_call_method_async(sni_server_state.bus, &slot->slot, dbusmenu->item->service,
				dbusmenu->item->properties->menu, sni_dbusmenu_interface, "Event",
				sni_item_method_callback_private, slot, "isvu",
				menu_item->id, event_types[type], "y", 0, time(NULL));
		if (ret < 0) {
			free(slot);
			return ret;
		} else {
			list_insert(&dbusmenu->item->slots, &slot->link);
		}
	} else {
		int ret = sd_bus_call_method(sni_server_state.bus, dbusmenu->item->service,
				dbusmenu->item->properties->menu, sni_dbusmenu_interface, "Event",
				NULL, NULL, "isvu", menu_item->id, event_types[type], "y", 0, time(NULL));
		if (ret < 0) {
			return ret;
		}
	}

	return 1;
}

static int sni_dbusmenu_menu_item_about_to_show_callback_private(sd_bus_message *msg, void *data,
        MAYBE_UNUSED sd_bus_error *ret_error) {
	struct sni_slot *slot = data;
	struct sni_dbusmenu *dbusmenu = slot->dbusmenu;

	sni_slot_free_private(slot);

	int need_update;
	int ret = sd_bus_message_read_basic(msg, 'b', &need_update);
	if (ret >= 0) {
		if (need_update) {
			sni_dbusmenu_get_layout_private(dbusmenu);
		}
		ret = 1;
	}

	return ret;
}

static MAYBE_UNUSED int sni_dbusmenu_menu_about_to_show(struct sni_dbusmenu_menu *menu, bool async) {
	assert(sni_server_state.bus != NULL);
	assert(menu->parent_menu_item);

	struct sni_dbusmenu *dbusmenu = menu->dbusmenu;
	if ((dbusmenu->item->properties == NULL) || (dbusmenu->item->properties->menu == NULL)) {
		return -ENOENT;
	}

	if (async) {
		struct sni_slot *slot = malloc(sizeof(struct sni_slot));
		int ret = sd_bus_call_method_async(sni_server_state.bus, &slot->slot, dbusmenu->item->service,
				dbusmenu->item->properties->menu, sni_dbusmenu_interface, "AboutToShow",
				sni_dbusmenu_menu_item_about_to_show_callback_private, slot, "i", menu->parent_menu_item->id);
		if (ret < 0) {
			free(slot);
			return ret;
		} else {
			slot->dbusmenu = dbusmenu;
			list_insert(&dbusmenu->item->slots, &slot->link);
		}
	} else {
		int ret = sd_bus_call_method(sni_server_state.bus, dbusmenu->item->service,
				dbusmenu->item->properties->menu, sni_dbusmenu_interface, "AboutToShow",
				NULL, NULL, "i", menu->parent_menu_item->id);
		if (ret < 0) {
			return ret;
		}
	}

	return 1;
}


#endif // SNI_SERVER_H
