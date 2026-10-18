/* 核心库与 CLI 共用的小型分配和字符串辅助函数。 */
#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

int tp_ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int tp_ascii_upper(int c) {
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

int tp_str_ieq(const char *a, const char *b) {
    if (a == b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (tp_ascii_lower((unsigned char)*a) != tp_ascii_lower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int tp_str_icmp_n(const char *a, const char *b, size_t n) {
    size_t i;
    if (!a || !b) {
        return a ? 1 : (b ? -1 : 0);
    }
    for (i = 0; i < n; i++) {
        int ca = tp_ascii_lower((unsigned char)a[i]);
        int cb = tp_ascii_lower((unsigned char)b[i]);
        if (ca != cb) {
            return ca - cb;
        }
        if (ca == 0) {
            return 0;
        }
    }
    return 0;
}

char *tp_dup_range(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) {
        return NULL;
    }
    if (n) {
        memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

char *tp_dup(const char *s) {
    return s ? tp_dup_range(s, strlen(s)) : NULL;
}

char *tokenprobe_strdup(const char *s) {
    return tp_dup(s);
}

char *tokenprobe_strndup(const char *s, size_t n) {
    return s ? tp_dup_range(s, n) : NULL;
}

int tp_parse_int(const char *text, size_t len, long *out) {
    long value = 0;
    size_t i = 0;
    int neg = 0;
    if (!text || len == 0) {
        return -1;
    }
    if (text[0] == '+' || text[0] == '-') {
        neg = text[0] == '-';
        i = 1;
    }
    if (i == len) {
        return -1;
    }
    for (; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        value = value * 10 + (text[i] - '0');
        if (value > 1000000000L) {
            return -1;
        }
    }
    *out = neg ? -value : value;
    return 0;
}

int64_t tp_now_ms(void) {
#if defined(_WIN32)
    FILETIME ft;
    ULARGE_INTEGER u;
    ULONGLONG t;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    t = u.QuadPart; /* 自 1601-01-01 起的 100ns 计数 */
    return (int64_t)(t / 10000ULL) - 11644473600000LL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* -------------------------------------------------------------- 字符串缓冲 */

void tp_strbuf_init(tp_strbuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void tp_strbuf_free(tp_strbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int tp_strbuf_reserve(tp_strbuf *b, size_t extra) {
    size_t need;
    size_t cap;
    char *p;
    if (extra > (size_t)-1 - b->len - 1) {
        return -1;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) {
        return 0;
    }
    cap = b->cap ? b->cap : 128;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    p = (char *)realloc(b->data, cap);
    if (!p) {
        return -1;
    }
    b->data = p;
    b->cap = cap;
    if (b->len == 0) {
        b->data[0] = '\0';
    }
    return 0;
}

int tp_strbuf_put(tp_strbuf *b, const char *s, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (tp_strbuf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int tp_strbuf_puts(tp_strbuf *b, const char *s) {
    return s ? tp_strbuf_put(b, s, strlen(s)) : 0;
}

int tp_strbuf_putc(tp_strbuf *b, char c) {
    return tp_strbuf_put(b, &c, 1);
}

int tp_strbuf_printf(tp_strbuf *b, const char *fmt, ...) {
    va_list ap;
    int need;
    va_list probe;

    va_start(ap, fmt);
    va_copy(probe, ap);
    need = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (need < 0) {
        va_end(ap);
        return -1;
    }
    if (tp_strbuf_reserve(b, (size_t)need) != 0) {
        va_end(ap);
        return -1;
    }
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
    return 0;
}

/* ------------------------------------------------------------------ 错误 */

void tp_error_set(tokenprobe_error_t *err, tokenprobe_status_t code, const char *fmt, ...) {
    va_list ap;
    if (!err) {
        return;
    }
    err->code = code;
    err->http_status = 0;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt ? fmt : "", ap);
    va_end(ap);
    err->hint[0] = '\0';
}

void tp_error_set_hint(tokenprobe_error_t *err, const char *fmt, ...) {
    va_list ap;
    if (!err) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err->hint, sizeof(err->hint), fmt ? fmt : "", ap);
    va_end(ap);
}

void tp_error_set_http(tokenprobe_error_t *err, tokenprobe_status_t code, int http_status,
                       const char *fmt, ...) {
    va_list ap;
    if (!err) {
        return;
    }
    err->code = code;
    err->http_status = http_status;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt ? fmt : "", ap);
    va_end(ap);
    err->hint[0] = '\0';
}

tokenprobe_status_t tp_status_from_http(int http_status) {
    if (http_status >= 200 && http_status < 300) {
        return TOKENPROBE_OK;
    }
    if (http_status == 401) {
        return TOKENPROBE_ERR_AUTH;
    }
    if (http_status == 403) {
        return TOKENPROBE_ERR_FORBIDDEN;
    }
    if (http_status == 404) {
        return TOKENPROBE_ERR_NOT_FOUND;
    }
    if (http_status == 429) {
        return TOKENPROBE_ERR_RATE_LIMIT;
    }
    if (http_status >= 500) {
        return TOKENPROBE_ERR_SERVER;
    }
    return TOKENPROBE_ERR_HTTP;
}

/* ----------------------------------------------------------------- win32 */

#if defined(_WIN32)
wchar_t *tp_win32_utf8_to_wide(const char *s) {
    int need;
    wchar_t *out;
    if (!s) {
        return NULL;
    }
    need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    out = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!out) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, need) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

char *tp_win32_wide_to_utf8(const wchar_t *s) {
    int need;
    char *out;
    if (!s) {
        return NULL;
    }
    need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        return NULL;
    }
    out = (char *)malloc((size_t)need);
    if (!out) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, need, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}
#endif
