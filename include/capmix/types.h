#ifndef CAPMIX_TYPES_H
#define CAPMIX_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t cm_paddr_t;
typedef uint64_t cm_vaddr_t;
typedef uint64_t cm_id_t;
typedef uint64_t cm_tick_t;
typedef uint64_t cm_trace_t;
typedef uint32_t cm_rights_t;
typedef int32_t  cm_error_t;

#define CM_PAGE_SHIFT  12
#define CM_PAGE_SIZE   (1ULL << CM_PAGE_SHIFT)
#define CM_PAGE_MASK   (CM_PAGE_SIZE - 1)

#define CM_OK           0
#define CM_ERR_NOMEM   -1
#define CM_ERR_INVAL   -2
#define CM_ERR_DENIED  -3
#define CM_ERR_EXIST   -4
#define CM_ERR_AGAIN   -5
#define CM_ERR_NOSPC   -6
#define CM_ERR_NOTSUP  -7
#define CM_ERR_BUSY    -8
#define CM_ERR_DEAD    -9
#define CM_ERR_OVERCOMMIT -10
#define CM_ERR_IO      -11   /* checksum mismatch / media error */
#define CM_ERR_NOENT   -12
#define CM_ERR_NOTEMPTY -13
#define CM_ERR_ISDIR   -14
#define CM_ERR_NOTDIR  -15
#define CM_ERR_ROFS    -16

#define CM_ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define CM_ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define CM_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define CM_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define CM_ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))

#define CM_UNUSED(x)        ((void)(x))

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif
