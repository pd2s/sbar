#if !defined(MACROS_H)
#define MACROS_H

#if !defined(__has_attribute)
#define __has_attribute(attr) 0
#endif

#if __has_attribute(format)
#define ATTRIB_FORMAT_PRINTF(start, end) __attribute__((__format__(__printf__, (start), (end))))
#else
#define ATTRIB_FORMAT_PRINTF(start, end)
#endif

#if __has_attribute(const)
#define ATTRIB_CONST __attribute__((__const__))
#else
#define ATTRIB_CONST
#endif

#if __has_attribute(unused)
#define ATTRIB_UNUSED __attribute__((__unused__))
#else
#define ATTRIB_UNUSED
#endif

#if __has_attribute(fallthrough)
#define ATTRIB_FALLTHROUGH __attribute__((__fallthrough__))
#else
#define ATTRIB_FALLTHROUGH
#endif

#if __has_attribute(pure)
#define ATTRIB_PURE __attribute__((__pure__))
#else
#define ATTRIB_PURE
#endif

#define MAYBE_UNUSED ATTRIB_UNUSED

#define LENGTH(arr) (sizeof((arr)) / sizeof((arr)[0]))
#define MIN(a, b) (((a) > (b)) ? (b) : (a))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define UNREACHABLE 0

//#define FILENAME (strrchr("/" __FILE__, '/') + 1)

#endif // MACROS_H
