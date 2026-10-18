/*
 * 递增式分配器。一次探测会从同一份 JSON 中分配数百个小字符串，随后整体丢弃，
 * 因此按结果分配一个内存池，既省去逐个 free()，也避免驱动层出现重复释放。
 */
#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

#define TP_ARENA_DEFAULT_BLOCK 4096

typedef struct tp_arena_block {
    struct tp_arena_block *next;
    size_t used;
    size_t cap;
    /* 数据紧跟在本结构之后 */
} tp_arena_block;

struct tp_arena {
    tp_arena_block *head;
    size_t block_size;
    size_t total;
};

static void *block_data(tp_arena_block *b) {
    return (void *)((char *)b + sizeof(tp_arena_block));
}

tp_arena *tp_arena_create(size_t block_size) {
    tp_arena *a = (tp_arena *)calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->block_size = block_size ? block_size : TP_ARENA_DEFAULT_BLOCK;
    return a;
}

void tp_arena_destroy(tp_arena *a) {
    if (!a) {
        return;
    }
    tp_arena_block *b = a->head;
    while (b) {
        tp_arena_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

static tp_arena_block *arena_new_block(tp_arena *a, size_t need) {
    size_t cap = a->block_size;
    if (cap < need) {
        cap = need;
    }
    if (cap > (size_t)-1 - sizeof(tp_arena_block)) {
        return NULL;
    }
    tp_arena_block *b = (tp_arena_block *)malloc(sizeof(tp_arena_block) + cap);
    if (!b) {
        return NULL;
    }
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    a->total += cap;
    return b;
}

void *tp_arena_alloc(tp_arena *a, size_t n) {
    if (!a || n == 0) {
        return NULL;
    }
    /* 按 16 字节对齐，满足指针和 int64 等类型的存放要求 */
    n = (n + 15u) & ~(size_t)15u;
    tp_arena_block *b = a->head;
    if (!b || b->cap - b->used < n) {
        b = arena_new_block(a, n);
        if (!b) {
            return NULL;
        }
    }
    void *p = (char *)block_data(b) + b->used;
    b->used += n;
    return p;
}

char *tp_arena_strndup(tp_arena *a, const char *s, size_t n) {
    if (!a) {
        return NULL;
    }
    if (!s) {
        s = "";
        n = 0;
    }
    char *p = (char *)tp_arena_alloc(a, n + 1);
    if (!p) {
        return NULL;
    }
    if (n) {
        memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

char *tp_arena_strdup(tp_arena *a, const char *s) {
    return s ? tp_arena_strndup(a, s, strlen(s)) : NULL;
}

char *tp_arena_vprintf(tp_arena *a, const char *fmt, va_list ap) {
    if (!a || !fmt) {
        return NULL;
    }
    va_list probe;
    va_copy(probe, ap);
    int need = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (need < 0) {
        return NULL;
    }
    char *p = (char *)tp_arena_alloc(a, (size_t)need + 1);
    if (!p) {
        return NULL;
    }
    vsnprintf(p, (size_t)need + 1, fmt, ap);
    return p;
}

char *tp_arena_printf(tp_arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *p = tp_arena_vprintf(a, fmt, ap);
    va_end(ap);
    return p;
}
