#if !defined(UTIL_H)
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdnoreturn.h>
#include <assert.h>
#include <time.h>

#include "macros.h"

typedef struct {
    size_t size;
    size_t len;
    void **items;
} ptr_array_t;

typedef struct {
    size_t size;
    size_t len;
    size_t elm_size;
    void *items;
} array_t;

typedef struct list list_t;
struct list {
	list_t *prev;
	list_t *next;
};

// TODO: use c23 typeof
#define container_of(ptr, sample, member)		\
	(__typeof__(sample))((char *)(ptr) -		\
		offsetof(__typeof__(*sample), member))

#define list_for_each(pos, head, member)						\
	for (pos = container_of((head)->next, pos, member);			\
		&pos->member != (head);									\
		pos = container_of(pos->member.next, pos, member))

#define list_for_each_safe(pos, tmp, head, member)              \
	for (pos = container_of((head)->next, pos, member),         \
		tmp = container_of((pos)->member.next, tmp, member);    \
		&pos->member != (head);                                 \
		pos = tmp,                                              \
		tmp = container_of(pos->member.next, tmp, member))

#define list_for_each_reverse(pos, head, member)				\
	for (pos = container_of((head)->prev, pos, member);			\
		&pos->member != (head);									\
		pos = container_of(pos->member.prev, pos, member))

static MAYBE_UNUSED void list_init(list_t *list) {
	list->prev = list;
	list->next = list;
}

static MAYBE_UNUSED void list_insert(list_t *list, list_t *elm) {
	elm->prev = list;
	elm->next = list->next;
	list->next = elm;
	elm->next->prev = elm;
}

static MAYBE_UNUSED void list_pop(list_t *elm) {
	elm->prev->next = elm->next;
	elm->next->prev = elm->prev;
	elm->next = NULL;
	elm->prev = NULL;
}

static MAYBE_UNUSED ATTRIB_PURE size_t list_length(list_t *list)
{
	list_t *e = list->next;
	size_t count = 0;

	while (e != list) {
		e = e->next;
		count++;
	}

	return count;
}

static MAYBE_UNUSED bool list_empty(list_t *list)
{
	return list->next == list;
}

static MAYBE_UNUSED void list_insert_list(list_t *list, list_t *other) {
	if (list_empty(other)) {
		return;
	}

	other->next->prev = list;
	other->prev->next = list->next;
	list->next->prev = other->prev;
	list->next = other->next;
}


static MAYBE_UNUSED void ptr_array_init(ptr_array_t *array, size_t initial_size) {
    assert(initial_size > 0);
    array->items = malloc(sizeof(void *) * initial_size);
	array->size = initial_size;
	array->len = 0;
}

static MAYBE_UNUSED void ptr_array_fini(ptr_array_t *array) {
    if (array) {
        free(array->items);
    }
}

//static MAYBE_UNUSED ptr_array_t *ptr_array_create(size_t initial_size) {
//	assert(initial_size > 0);
//	ptr_array_t *array = malloc(sizeof(ptr_array_t));
//	ptr_array_init(array, initial_size);
//
//	return array;
//}

//static MAYBE_UNUSED void ptr_array_destroy(ptr_array_t *array) {
//	if (array == NULL) {
//		return;
//	}
//
//	ptr_array_fini(array);
//
//	free(array);
//}

static MAYBE_UNUSED void ptr_array_resize(ptr_array_t *array, size_t new_size) {
    assert(new_size > 0);
    array->items = realloc(array->items, sizeof(void *) * new_size);
    array->size = new_size;
}

static MAYBE_UNUSED void ptr_array_add(ptr_array_t *array, void *item) {
    if (array->size == array->len) {
        ptr_array_resize(array, array->size * 2);
    }
    array->items[array->len++] = item;
}

static MAYBE_UNUSED void ptr_array_pop(ptr_array_t *array, size_t idx) {
    assert(idx < array->len);
    array->len--;
	memmove(&array->items[idx], &array->items[idx + 1],
            sizeof(void *) * (array->len - idx));
}

static MAYBE_UNUSED void ptr_array_put(ptr_array_t *array, size_t idx, void *item) {
    assert(idx <= array->len);
    if (idx == array->len) {
        if (array->size == array->len) {
            ptr_array_resize(array, array->size * 2);
        }
        array->len++;
    }
    array->items[idx] = item;
}

static MAYBE_UNUSED void ptr_array_insert(ptr_array_t *array, size_t idx, void *item) {
    assert(idx <= array->len);
    if (array->size == array->len) {
        ptr_array_resize(array, array->size * 2);
    }
	memmove(&array->items[idx + 1], &array->items[idx],
            sizeof(void*) * (array->len - idx));
    array->len++;
	array->items[idx] = item;
}

static MAYBE_UNUSED void ptr_array_swap(ptr_array_t *array, size_t src_index, size_t dest_index) {
	assert(src_index < array->len);
	assert(dest_index < array->len);
	void *tmp = array->items[src_index];
	array->items[src_index] = array->items[dest_index];
	array->items[dest_index] = tmp;
}

static MAYBE_UNUSED void ptr_array_qsort(ptr_array_t *array, int compare(const void *, const void *)) {
    qsort(array->items, array->len, sizeof(void *), compare);
}

static MAYBE_UNUSED void array_init(array_t *array, size_t initial_size, size_t elm_size) {
    assert((initial_size > 0) && (elm_size > 0));
    array->items = malloc(elm_size * initial_size);
	array->size = initial_size;
	array->len = 0;
    array->elm_size = elm_size;
}

static MAYBE_UNUSED void array_fini(array_t *array) {
    if (array) {
        free(array->items);
    }
}

static MAYBE_UNUSED void array_resize(array_t *array, size_t new_size) {
    assert(new_size > 0);
    array->items = realloc(array->items, array->elm_size * new_size);
    array->size = new_size;
}

static MAYBE_UNUSED void *array_add(array_t *array, void *item) {
    if (array->size == array->len) {
        array_resize(array, array->size * 2);
    }
    return memmove((char *)array->items + array->elm_size * array->len++,
			item, array->elm_size);
}

static MAYBE_UNUSED void *array_put(array_t *array, size_t idx, void *item) {
    assert(idx <= array->len);
    if (idx == array->len) {
        if (array->size == array->len) {
            array_resize(array, array->size * 2);
        }
        array->len++;
    }
    return memmove((char *)array->items + array->elm_size * idx, item, array->elm_size);
}

//static MAYBE_UNUSED void array_insert(array_t *array, size_t idx, void *item) {
//    assert(idx <= array->len);
//    if (array->size == array->len) {
//        array_resize(array, array->size * 2);
//    }
//    char *p = (char *)array->items + idx * array->elm_size;
//    memmove(p + array->elm_size, p, array->elm_size * (array->len - idx));
//    memmove(p, item, array->elm_size);
//    array->len++;
//}

//static MAYBE_UNUSED void array_pop(array_t *array, size_t idx) {
//    assert(idx < array->len);
//    array->len--;
//    char *p = (char *)array->items + idx * array->elm_size;
//    memmove(p, p + array->elm_size, array->elm_size * (array->len - idx));
//}

static MAYBE_UNUSED ATTRIB_FORMAT_PRINTF(1, 2) char *fstr_create(const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);

    size_t len = (size_t)vsnprintf(NULL, 0, fmt, ap) + 1;
    char *str = malloc(len);
    vsnprintf(str, len, fmt, aq);

    va_end(ap);
    va_end(aq);
    return str;
}

static MAYBE_UNUSED int nstrcmp(const char *s1, const char *s2) {
    if (!s1) {
        return -(s1 != s2);
    }
    if (!s2) {
        return s1 != s2;
    }

    return strcmp(s1, s2);
}

//static MAYBE_UNUSED uint64_t sdbm_hash(const char *s) {
//    uint64_t hash = 0;
//    for (; *s != '\0'; s++) {
//        hash = (uint64_t)*s + (hash << 6) + (hash << 16) - hash;
//    }
//
//    return hash;
//}

static MAYBE_UNUSED void premultiply_alpha_argb32(uint32_t *p) {
	uint8_t a = (uint8_t)(*p >> 24) & 0xFF;
	if (a == 0xFF) {
		return;
	} else if (a == 0) {
		*p = 0;
	} else {
		uint8_t r = (*p >> 16) & 0xFF;
		uint8_t g = (*p >> 8) & 0xFF;
		uint8_t b = (*p >> 0) & 0xFF;

		r = (uint8_t)(r * a / 0xFF);
		g = (uint8_t)(g * a / 0xFF);
		b = (uint8_t)(b * a / 0xFF);

		*p = (uint32_t)a << 24 | (uint32_t)r << 16
                | (uint32_t)g << 8 | (uint32_t)b << 0;
	}
}

//static MAYBE_UNUSED const uint8_t base64_reverse_lookup[] = {
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
//     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,  0,255,255,
//    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
//     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
//    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
//     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
//};

//static MAYBE_UNUSED void *base64_decode(const char *text, size_t text_len) {
//    if (text_len % 4 != 0) {
//        return NULL;
//    }
//
//    uint8_t *ret = malloc(text_len / 4 * 3);
//
//    for (size_t i = 0, o = 0; i < text_len; i += 4, o += 3) {
//        uint32_t a = base64_reverse_lookup[(uint8_t)text[i + 0]];
//        uint32_t b = base64_reverse_lookup[(uint8_t)text[i + 1]];
//        uint32_t c = base64_reverse_lookup[(uint8_t)text[i + 2]];
//        uint32_t d = base64_reverse_lookup[(uint8_t)text[i + 3]];
//
//        uint32_t u = a | b | c | d;
//        if (u & 128) {
//            goto error;
//        }
//
//        if (u & 64) {
//            if (i + 4 != text_len || (a | b) & 64 || (c & 64 && !(d & 64))) {
//                goto error;
//            }
//            c &= 63;
//            d &= 63;
//        }
//
//        uint32_t v = a << 18 | b << 12 | c << 6 | d << 0;
//        ret[o + 0] = (uint8_t)((v >> 16) & 0xFF);
//        ret[o + 1] = (uint8_t)((v >> 8) & 0xFF);
//        ret[o + 2] = (uint8_t)((v >> 0) & 0xFF);
//    }
//
//    return ret;
//error:
//    free(ret);
//    return NULL;
//}

static MAYBE_UNUSED ATTRIB_FORMAT_PRINTF(1, 0) void log_stderr_va(const char *fmt, va_list args) {
    fputs(LOG_PREFIX, stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

static MAYBE_UNUSED ATTRIB_FORMAT_PRINTF(1, 2) void log_stderr(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
    log_stderr_va(fmt, args);
	va_end(args);
}

static noreturn MAYBE_UNUSED ATTRIB_FORMAT_PRINTF(2, 3) void abort_(int code, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
    log_stderr_va(fmt, args);
	va_end(args);
    exit(code);
}

#if DEBUG
#define log_debug log_stderr
#else
#define log_debug(...)
#endif // DEBUG

#endif // UTIL_H
