#if !defined(XDG_ICON_THEME_H)
#define XDG_ICON_THEME_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>

#include "macros.h"
#include "util.h"

enum xdg_icon_theme_icon_type {
	XDG_ICON_THEME_ICON_TYPE_XPM = 1, // deprecated, not implemented
	XDG_ICON_THEME_ICON_TYPE_PNG = 2,
	XDG_ICON_THEME_ICON_TYPE_SVG = 4,
};

struct xdg_icon_theme_icon {
	char *path;
	enum xdg_icon_theme_icon_type type;
};

struct xdg_icon_theme_icon_cache {
	char *icon_name;
	char *icon_theme_name;
	struct xdg_icon_theme_icon icon;
};

static MAYBE_UNUSED void xdg_icon_theme_get_basedirs(ptr_array_t *dest) {
	char *data_home = getenv("XDG_DATA_HOME");
	ptr_array_add(dest, (data_home && *data_home)
		? fstr_create("%s/icons", data_home)
		: fstr_create("%s/.local/share/icons", getenv("HOME")));

	char *data_dirs = getenv("XDG_DATA_DIRS");
	if (data_dirs && *data_dirs) {
		data_dirs = strdup(data_dirs);
		char *tmp = NULL, *dir = strtok_r(data_dirs, ":", &tmp);
		while (dir) {
			ptr_array_add(dest,
				fstr_create("%s/icons", dir));
			dir = strtok_r(NULL, ":", &tmp);
		}
		free(data_dirs);
	} else {
		ptr_array_add(dest, strdup("/usr/local/share/icons"));
		ptr_array_add(dest, strdup("/usr/share/icons"));
	}

	ptr_array_add(dest, strdup("/usr/share/pixmaps"));
}

static MAYBE_UNUSED void xdg_icon_theme_icon_cache_destroy(struct xdg_icon_theme_icon_cache *cache) {
	if (cache == NULL) {
		return;
	}

	free(cache->icon_name);
	free(cache->icon_theme_name);
	free(cache->icon.path);

	free(cache);
}

static int xdg_icon_theme_name_reverse_fts_private(const FTSENT **a, const FTSENT **b) {
    return strcmp((*b)->fts_name, (*a)->fts_name);
}

static struct xdg_icon_theme_icon xdg_icon_theme_find_icon_in_dir(const char *icon_name,
		const char *path, uint32_t icon_types) { // enum xdg_icon_theme_icon_type |
	FTS *fts = fts_open((char *[]){(char *)path, NULL}, FTS_LOGICAL | FTS_NOSTAT,
		xdg_icon_theme_name_reverse_fts_private);
	if (fts == NULL) {
        return (struct xdg_icon_theme_icon){ 0 };
	}

	int icon_name_len;
    char icon_name_svg[NAME_MAX];
	icon_name_len = snprintf(icon_name_svg, sizeof(icon_name_svg), "%s.svg", icon_name);
	char icon_name_png[NAME_MAX];
	icon_name_len = snprintf(icon_name_png, sizeof(icon_name_png), "%s.png", icon_name);

	struct xdg_icon_theme_icon icon = { 0 };
    FTSENT *f;
    while ((f = fts_read(fts))) {
        switch (f->fts_info) {
        case FTS_F:
			if (f->fts_namelen == icon_name_len) {
				// ? TODO: access R_OK
				// ? TODO: do not prioritize SVGs
				if ((icon_types & XDG_ICON_THEME_ICON_TYPE_SVG)
						&& (strcmp(icon_name_svg, f->fts_name) == 0)) {
					icon.path = strdup(f->fts_path);
					icon.type = XDG_ICON_THEME_ICON_TYPE_SVG;
					goto out;
				}
				if ((icon_types & XDG_ICON_THEME_ICON_TYPE_PNG) && (icon.path == NULL)
						&& (strcmp(icon_name_png, f->fts_name) == 0)) {
					icon.path = strdup(f->fts_path);
					icon.type = XDG_ICON_THEME_ICON_TYPE_PNG;
				}
			}
            break;
		default:
            break;
        }
    }

out:
	fts_close(fts);
    return icon;
}

static struct xdg_icon_theme_icon xdg_icon_theme_find_icon_in_theme(const char *icon_name,
		const char *icon_theme_name, ptr_array_t *basedirs, uint32_t icon_types) { // enum xdg_icon_theme_icon_type |
	struct xdg_icon_theme_icon icon = { 0 };
	for (size_t i = 0; i < basedirs->len; ++i) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s",
			(char *)basedirs->items[i], icon_theme_name);
		struct stat sb;
		if ((stat(path, &sb) == 0) && S_ISDIR(sb.st_mode)) {
            icon = xdg_icon_theme_find_icon_in_dir(icon_name, path, icon_types);
            if (icon.path) {
                break;
            }
		}
    }

    return icon;
}

static MAYBE_UNUSED struct xdg_icon_theme_icon xdg_icon_theme_find_icon(const char *icon_name,
		const char *icon_theme_name, ptr_array_t *basedirs, ptr_array_t *cache,
		uint32_t icon_types) { // enum xdg_icon_theme_icon_type |
	// TODO: https://specifications.freedesktop.org/icon-theme-spec/latest/

	if (cache) {
		// TODO: check mtime
		for (size_t i = 0; i < cache->len; ++i) {
			struct xdg_icon_theme_icon_cache *cached_icon = cache->items[i];
			if ((strcmp(cached_icon->icon_name, icon_name) == 0)
					&& (nstrcmp(cached_icon->icon_theme_name, icon_theme_name) == 0)
					&& (access(cached_icon->icon.path, R_OK) == 0)) {
				return cached_icon->icon;
			}
		}
	}

	struct xdg_icon_theme_icon icon = { 0 };
	if (icon_theme_name && *icon_theme_name) {
		icon = xdg_icon_theme_find_icon_in_theme(
			icon_name, icon_theme_name, basedirs, icon_types);
	}
	if ((icon.path == NULL) && !(icon_theme_name && (strcmp(icon_theme_name, "hicolor") == 0))) {
		icon = xdg_icon_theme_find_icon_in_theme(
			icon_name, "hicolor",basedirs, icon_types);
	}
	if (icon.path == NULL) {
		for (size_t i = basedirs->len - 1; i != SIZE_MAX; --i) {
			icon = xdg_icon_theme_find_icon_in_dir(
				icon_name, basedirs->items[i], icon_types);
			if (icon.path) {
				break;
			}
		}
	}

	if (cache && icon.path) {
		struct xdg_icon_theme_icon_cache *cache_entry = malloc(
			sizeof(struct xdg_icon_theme_icon_cache));
		cache_entry->icon_name = strdup(icon_name);
		cache_entry->icon_theme_name = icon_theme_name ? strdup(icon_theme_name) : NULL;
		cache_entry->icon = icon;
		ptr_array_add(cache, cache_entry);
	}
	return icon;
}

#endif // XDG_ICON_THEME_H
