#ifndef HELPA_H_
#define HELPA_H_

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define HELPA_ASSERT(v) assert(v)

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

/* DYNAMIC ARRAY STUFF */

#define HELPA_DA_INIT { .dt = NULL, .sz = 0, .cp = 0 }
#define helpa_da_empty(da) ((da).sz == 0)
#define helpa_da_size(da)  ((da).sz)
#define helpa_da_cap(da)   ((da).cp)
#define helpa_da_data(da)  ((da).dt)

#define helpa_da_clone(dst, src)                         \
    do {                                                 \
        (dst).sz = (src).sz;                             \
        (dst).cp = (src).sz;                             \
        (dst).dt = malloc(sizeof(*(src).dt) * (src).sz); \
        HELPA_ASSERT((dst).dt != NULL);                  \
        memcpy((dst).dt, (src).dt,                       \
               sizeof(*(src).dt) * (src).sz);            \
    } while (0)

#define helpa_da_init(da, cap)                         \
    do {                                               \
        (da).sz = 0;                                   \
        (da).cp = (cap);                               \
        (da).dt = malloc(sizeof(*(da).dt) * (da).cp);  \
        HELPA_ASSERT((da).dt != NULL);                 \
    } while (0)

#define helpa_da_reserve(da, n)                           \
    do {                                                  \
        if ((da).cp < (n)) {                              \
            (da).cp = (n);                                \
            void *tmp = realloc(                          \
                (da).dt,                                  \
                sizeof(*(da).dt) * (da).cp                \
            );                                            \
            HELPA_ASSERT(tmp != NULL);                    \
            (da).dt = (typeof((da).dt))tmp;               \
        }                                                 \
    } while (0)

#define helpa_da_shrink_to_fit(da)                         \
    do {                                                   \
        if ((da).sz < (da).cp) {                           \
            (da).cp = (da).sz;                             \
            void *tmp = realloc(                           \
                (da).dt,                                   \
                sizeof(*(da).dt) * (da).cp                 \
            );                                             \
            HELPA_ASSERT(tmp != NULL || (da).cp == 0);     \
            (da).dt = (typeof((da).dt))tmp;                \
        }                                                  \
    } while (0)

#define helpa_da_append(da, value)                     \
    do {                                               \
        if ((da).sz + 1 >= (da).cp) {                  \
            (da).cp = ((da).cp + 1) * 2;               \
            void *tmp = realloc(                       \
                (da).dt,                               \
                sizeof(*(da).dt) * (da).cp             \
            );                                         \
            HELPA_ASSERT(tmp != NULL &&                \
                "Failed to do realloc");               \
            (da).dt = tmp;                             \
        }                                              \
        (da).dt[(da).sz++] = (value);                  \
    } while (0)

#define helpa_da_first(da) da.dt[0]
#define helpa_da_last(da) da.dt[da.sz - 1]

#define helpa_da_for(da, i) \
    for (u64 i = 0; i < ((da).sz); i++)

#define helpa_da_foreach(da, v)       \
    for (typeof((da).dt) v = (da).dt; \
         v < (da).dt + (da).sz;       \
         ++v)

#define helpa_da_set(da, i, v)            \
    do {                                  \
        HELPA_ASSERT((i) < (da).sz);      \
        (da).dt[(i)] = (v);               \
    } while (0)

#define helpa_da_get(da, i)               \
    (HELPA_ASSERT((i) < (da).sz), (da).dt[(i)])

#define helpa_da_pop(da)                \
    (HELPA_ASSERT((da).sz > 0), (da).dt[--(da).sz])

#define helpa_da_remove_at(da, i)                  \
    do {                                           \
        HELPA_ASSERT((i) < (da).sz);               \
        for (u64 _j = (i); _j + 1 < (da).sz; _j++) \
            (da).dt[_j] = (da).dt[_j + 1];         \
        --(da).sz;                                 \
    } while (0)

#define helpa_da_remove_swap(da, i)             \
    do {                                        \
        HELPA_ASSERT((i) < (da).sz);            \
        (da).dt[(i)] = (da).dt[(da).sz - 1];    \
        --(da).sz;                              \
    } while (0)

#define helpa_da_free(da)   \
    do {                    \
        free((da).dt);      \
        (da).dt = NULL;     \
        (da).sz = 0;        \
        (da).cp = 0;        \
    } while (0)

/* ARENA STUFF */

typedef struct {
    u8  *base;
    u64  cap;
    u64  off;
} HArena;

static inline HArena arena_make(void *mem, u64 cap) {
    return (HArena){
        .base = (u8 *)mem,
        .cap  = cap,
        .off  = 0,
    };
}

static inline u64 arena_align(u64 off, u64 align) {
    return (off + align - 1) & ~(align - 1);
}

static inline void *arena_alloc_align(HArena *a, u64 size, u64 align) {
    u64 off = arena_align(a->off, align);
    if (off + size > a->cap) return NULL;
    void *ptr = a->base + off;
    a->off = off + size;
    return ptr;
}

#define arena_alloc(a, size) \
    arena_alloc_align((a), (size), 8)

static inline void arena_reset(HArena *a) {
    a->off = 0;
}

typedef u64 HArenaMark;

static inline HArenaMark arena_mark(HArena *a) {
    return a->off;
}

static inline void arena_pop(HArena *a, HArenaMark m) {
    HELPA_ASSERT(m <= a->off);
    a->off = m;
}

/* STRING STUFF */

typedef struct {
    u8  *dt;
    u64 sz;
    u64 cp;
} HStr;

typedef struct {
    const u8 *dt;
    u64       sz;
} HStrView;

#define helpa_cstr_eq(a, b) (strcmp((a), (b)) == 0)

#define HSTRV_LIT(s) \
    ((HStrView){ (const u8 *)(s), sizeof(s) - 1 })

#define HSTRV_CSTR(s) \
    ((HStrView){ (const u8 *)(s), strlen(s) })

#define hstrv_eq(a, b) \
    ((a).sz == (b).sz && memcmp((a).dt, (b).dt, (a).sz) == 0)

#define hstrv_starts_with(s, p)                     \
    ((s).sz >= (p).sz &&                            \
     memcmp((s).dt, (p).dt, (p).sz) == 0)

#define hstrv_ends_with(s, p)                       \
    ((s).sz >= (p).sz &&                            \
     memcmp((s).dt + (s).sz - (p).sz, (p).dt, (p).sz) == 0)

#define hstrv_sub(s, off, len)                       \
    (HELPA_ASSERT((off) + (len) <= (s).sz),          \
     (HStrView){ (s).dt + (off), (len) })

#define hstr_init(s) \
    do { (s)->dt = NULL; (s)->sz = 0; (s)->cp = 0; } while (0)

#define hstr_push(s, c)                          \
    do {                                         \
        helpa_da_append(*(s), (u8)(c));          \
        (s)->dt[(s)->sz] = 0;                    \
    } while (0)

#define hstr_append_view(s, v)                           \
    do {                                                 \
        helpa_da_reserve(*(s), (s)->sz + (v).sz + 1);    \
        memcpy((s)->dt + (s)->sz, (v).dt, (v).sz);       \
        (s)->sz += (v).sz;                               \
        (s)->dt[(s)->sz] = 0;                            \
    } while (0)

#define hstr_append_cstr(s, cstr) \
    hstr_append_view((s), HSTRV_CSTR(cstr))

#define hstr_view(s) \
    ((HStrView){ (s)->dt, (s)->sz })

#define hstr_free(s) \
    do { free((s)->dt); hstr_init(s); } while (0)

#define hstr_eq(a, b) \
    ((a)->sz == (b)->sz && memcmp((a)->dt, (b)->dt, (a)->sz) == 0)

#define hstr_eq_view(s, v) \
    ((s)->sz == (v).sz && memcmp((s)->dt, (v).dt, (v).sz) == 0)

#define hstr_clear(s) \
    do { (s)->sz = 0; if ((s)->dt) (s)->dt[0] = 0; } while (0)

#define hstr_pop(s) \
    (HELPA_ASSERT((s)->sz > 0), (s)->dt[--(s)->sz] = 0)

static inline HStrView hstrv_trim(HStrView s) {
    u64 b = 0, e = s.sz;
    while (b < e && isspace(s.dt[b])) b++;
    while (e > b && isspace(s.dt[e - 1])) e--;
    return (HStrView){ s.dt + b, e - b };
}

static inline void _hstr_vprintf(HStr *s, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);

    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);

    HELPA_ASSERT(n >= 0);

    helpa_da_reserve(*s, s->sz + (u64)n + 1);

    vsnprintf((char *)s->dt + s->sz, (size_t)n + 1, fmt, ap);
    s->sz += (u64)n;
}
static inline void hstr_printf(HStr *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _hstr_vprintf(s, fmt, ap);
    va_end(ap);
}

static inline void hstr_setf(HStr *s, const char *fmt, ...)
{
    s->sz = 0;
    if (s->dt) s->dt[0] = 0;

    va_list ap;
    va_start(ap, fmt);
    _hstr_vprintf(s, fmt, ap);
    va_end(ap);
}

#define hstr_printfln(s, fmt, ...) \
    hstr_printf((s), fmt "\n", ##__VA_ARGS__)

static inline u8 *arena_sprintf(HArena *a, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);

    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len < 0) {
        va_end(args2);
        return NULL;
    }

    u8 *buf = (u8 *)arena_alloc(a, (u64)len + 1);
    if (!buf) {
        va_end(args2);
        return NULL;
    }

    vsnprintf((char *)buf, (u64)len + 1, fmt, args2);
    va_end(args2);
    return buf;
}

static inline HStr arena_hstrf(HArena *a, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);

    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len < 0) {
        va_end(args2);
        return (HStr){0};
    }

    u8 *buf = (u8 *)arena_alloc(a, (u64)len + 1);
    if (!buf) {
        va_end(args2);
        return (HStr){0};
    }

    vsnprintf((char *)buf, (u64)len + 1, fmt, args2);
    va_end(args2);

    return (HStr){
        .dt = buf,
        .sz = (u64)len,
    };
}

#define HSTR(s) #s

/* TODO(if needed): QUEUE, MAP, UTF8, PATH STUFF */

#endif
