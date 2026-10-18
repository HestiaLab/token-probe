/* 内部实现头文件。不安装，也不属于公共 ABI。 */
#ifndef TP_INTERNAL_H
#define TP_INTERNAL_H

#include "tokenprobe/probe.h"

#include <stdarg.h>
#include <stdio.h>

/* 由 tp_provider_deepseek.c 定义，tp_ctx.c 的注册表引用。 */
extern const tokenprobe_provider_t tp_provider_deepseek;

/* 成功探测完成后记录观测时间（Unix 毫秒）。 */
void tp_result_set_observed_at(tokenprobe_result_t *r, int64_t ms);

/* --------------------------------------------------------------- 内存池 */

typedef struct tp_arena tp_arena;

tp_arena *tp_arena_create(size_t block_size);
void tp_arena_destroy(tp_arena *a);
void *tp_arena_alloc(tp_arena *a, size_t n);
char *tp_arena_strndup(tp_arena *a, const char *s, size_t n);
char *tp_arena_strdup(tp_arena *a, const char *s);
char *tp_arena_vprintf(tp_arena *a, const char *fmt, va_list ap);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
char *tp_arena_printf(tp_arena *a, const char *fmt, ...);

/* ------------------------------------------------------------- 字符串缓冲 */

typedef struct tp_strbuf {
    char *data;
    size_t len;
    size_t cap;
} tp_strbuf;

void tp_strbuf_init(tp_strbuf *b);
void tp_strbuf_free(tp_strbuf *b);
int tp_strbuf_reserve(tp_strbuf *b, size_t extra); /* 0 ok, -1 oom */
int tp_strbuf_put(tp_strbuf *b, const char *s, size_t n);
int tp_strbuf_putc(tp_strbuf *b, char c);
int tp_strbuf_puts(tp_strbuf *b, const char *s);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
int tp_strbuf_printf(tp_strbuf *b, const char *fmt, ...);

/* ----------------------------------------------------------------- 工具 */

int tp_ascii_lower(int c);
int tp_ascii_upper(int c);
int tp_str_ieq(const char *a, const char *b);
int tp_str_icmp_n(const char *a, const char *b, size_t n);
char *tp_dup(const char *s);
char *tp_dup_range(const char *s, size_t n);
int tp_parse_int(const char *text, size_t len, long *out);
int64_t tp_now_ms(void);
void tp_error_set(tokenprobe_error_t *err, tokenprobe_status_t code, const char *fmt, ...);
void tp_error_set_hint(tokenprobe_error_t *err, const char *fmt, ...);
void tp_error_set_http(tokenprobe_error_t *err, tokenprobe_status_t code, int http_status,
                       const char *fmt, ...);
/* 把 HTTP 状态码映射为语义化状态（200 -> TOKENPROBE_OK）。 */
tokenprobe_status_t tp_status_from_http(int http_status);

/* ----------------------------------------------------------------- json */

typedef enum tp_json_type {
    TP_JSON_NULL,
    TP_JSON_BOOL,
    TP_JSON_NUMBER,
    TP_JSON_STRING,
    TP_JSON_ARRAY,
    TP_JSON_OBJECT
} tp_json_type;

typedef struct tp_json tp_json;

struct tp_json {
    tp_json_type type;
    union {
        int boolean;
        struct {
            const char *text;
            size_t len;
        } number; /* 原始文本，不做修改 */
        struct {
            const char *text;
            size_t len;
        } string;
        struct {
            tp_json **items;
            size_t count;
        } array;
        struct {
            char **keys;
            size_t *key_lens;
            tp_json **values;
            size_t count;
        } object;
    } u;
    int line; /* token 起始行号（从 1 开始），用于错误信息 */
};

typedef struct tp_json_doc tp_json_doc;

/* 解析 `len` 字节的 UTF-8 JSON。允许结尾空白，不允许多余内容。
 * 拒绝重复的对象键、超过 TOKENPROBE_JSON_MAX_DEPTH 的嵌套深度，
 * 以及字符串内的原始控制字符。 */
tokenprobe_status_t tp_json_parse(const char *text, size_t len, tp_json_doc **out,
                                  tokenprobe_error_t *err);
void tp_json_free(tp_json_doc *doc);
const tp_json *tp_json_root(const tp_json_doc *doc);

#define TOKENPROBE_JSON_MAX_DEPTH 64

int tp_json_is_object(const tp_json *v);
int tp_json_is_array(const tp_json *v);
int tp_json_is_string(const tp_json *v);
int tp_json_is_number(const tp_json *v);
int tp_json_is_bool(const tp_json *v);
int tp_json_is_null(const tp_json *v);
const char *tp_json_string(const tp_json *v, size_t *len_out); /* 非字符串时返回 NULL */
int tp_json_bool(const tp_json *v, int *out);                  /* 成功返回 0 */
const tp_json *tp_json_get(const tp_json *obj, const char *key);
const tp_json *tp_json_at(const tp_json *arr, size_t index);
size_t tp_json_size(const tp_json *v); /* 对象或数组的元素个数 */
/* 接受 JSON 数字或形似小数的字符串。 */
tokenprobe_status_t tp_json_decimal(const tp_json *v, tokenprobe_decimal_t *out);
/* 便捷接口：对象取字段并转换为小数。字段缺失或类型不符时返回
 * TOKENPROBE_ERR_SCHEMA。 */
tokenprobe_status_t tp_json_field_decimal(const tp_json *obj, const char *key,
                                          tokenprobe_decimal_t *out);
/* 把字符串字段复制到内存池。字段缺失或不是字符串时返回 NULL。 */
const char *tp_json_field_string(tp_arena *a, const tp_json *obj, const char *key, size_t *len_out);

/* ------------------------------------------------------------------ url */

typedef struct tp_url {
    char scheme[8]; /* 例如 "https" */
    char *host;     /* 由 tp_url_parse_into() 的内存池持有 */
    int port;
    char *path;  /* 总以 '/' 开头，且不为空 */
    char *query; /* 不含 '?'，可能为 NULL */
    int is_secure;
} tp_url;

tokenprobe_status_t tp_url_parse_into(tp_arena *a, const char *url, tp_url *out,
                                      tokenprobe_error_t *err);
/* 返回实际连接的端口：显式指定优先，否则 443/80。 */
int tp_url_effective_port(const tp_url *u);

/* --------------------------------------------------------------- HTTP 探测 */

/* 供应商驱动与传输层之间的统一入口。 */
tokenprobe_status_t tp_probe_http(tokenprobe_context_t *ctx, const tp_url *url,
                                  const tokenprobe_http_header_t *headers, size_t header_count,
                                  tokenprobe_http_response_t *resp, tokenprobe_error_t *err);

/* 同 tp_probe_http()，但允许指定方法、请求体和长度。method 为 NULL 表示 GET。 */
tokenprobe_status_t tp_probe_http_ex(tokenprobe_context_t *ctx, const char *method,
                                     const tp_url *url, const tokenprobe_http_header_t *headers,
                                     size_t header_count, const char *body, size_t body_len,
                                     tokenprobe_http_response_t *resp, tokenprobe_error_t *err);

/* ---------------------------------------------------------------- win32 */

#if defined(_WIN32)
/* UTF-8 转 UTF-16LE，结果用 malloc 分配。失败返回 NULL。 */
wchar_t *tp_win32_utf8_to_wide(const char *s);
char *tp_win32_wide_to_utf8(const wchar_t *s);
#endif

#endif /* TP_INTERNAL_H */
