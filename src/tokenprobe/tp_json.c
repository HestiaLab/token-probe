/*
 * 基于内存池的严格 UTF-8 JSON 解析器（RFC 8259）。宁可严格：
 * 解析失败必须返回 TOKENPROBE_ERR_JSON，绝不能悄悄给出错误的数字。
 *
 * 有意拒绝以下输入：
 *   - 顶层值之后的多余内容
 *   - 未闭合的字符串、字符串中的原始控制字符（< 0x20）
 *   - 不成对的代理项或越界的 \u 转义
 *   - 溢出 int64 的数字或格式错误的指数
 *   - 重复的对象键（后一个会悄悄覆盖前一个）
 *   - 嵌套深度超过 TOKENPROBE_JSON_MAX_DEPTH（防止栈耗尽）
 */
#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct parser {
    const char *s;
    size_t len;
    size_t pos;
    tp_arena *arena;
    int depth;
    tokenprobe_error_t *err;
    int failed;
} parser;

struct tp_json_doc {
    tp_arena *arena;
    tp_json *root;
    int line;
};

static void fail(parser *p, tokenprobe_status_t code, int err_line, const char *msg) {
    if (!p->failed) {
        p->failed = 1;
        tp_error_set(p->err, code, "JSON error at line %d: %s", err_line, msg);
    }
}

static int line_at(const parser *p, size_t pos) {
    size_t i;
    int line = 1;
    for (i = 0; i < pos && i < p->len; i++) {
        if (p->s[i] == '\n') {
            line++;
        }
    }
    return line;
}

static void skip_ws(parser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static tp_json *node_new(parser *p) {
    tp_json *v = (tp_json *)tp_arena_alloc(p->arena, sizeof(tp_json));
    if (!v) {
        fail(p, TOKENPROBE_ERR_OOM, line_at(p, p->pos), "out of memory");
        return NULL;
    }
    memset(v, 0, sizeof(*v));
    return v;
}

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned)(c - 'a' + 10);
    }
    return (unsigned)(c - 'A' + 10);
}

/* 把 `cp` 编码为 UTF-8 写入 dst（至少 4 字节），返回写入的字节数。 */
static size_t utf8_encode(unsigned cp, char *dst) {
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    dst[0] = (char)(0xF0u | (cp >> 18));
    dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/*
 * 读取一个 \uXXXX 转义，`p->pos` 位于 'u' 之后。处理代理对，
 * 保证增补平面字符正确往返。
 */
static int read_unicode_escape(parser *p, unsigned *out_cp) {
    unsigned cp;
    size_t i;

    if (p->pos + 4 > p->len) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        if (!is_hex(p->s[p->pos + i])) {
            return -1;
        }
    }
    cp = (hex_val(p->s[p->pos + 0]) << 12) | (hex_val(p->s[p->pos + 1]) << 8) |
         (hex_val(p->s[p->pos + 2]) << 4) | hex_val(p->s[p->pos + 3]);
    p->pos += 4;

    if (cp >= 0xD800u && cp <= 0xDBFFu) {
        unsigned lo;
        if (p->pos + 6 > p->len || p->s[p->pos] != '\\' || p->s[p->pos + 1] != 'u') {
            return -1; /* 高代理项后缺少低代理项 */
        }
        for (i = 0; i < 4; i++) {
            if (!is_hex(p->s[p->pos + 2 + i])) {
                return -1;
            }
        }
        lo = (hex_val(p->s[p->pos + 2]) << 12) | (hex_val(p->s[p->pos + 3]) << 8) |
             (hex_val(p->s[p->pos + 4]) << 4) | hex_val(p->s[p->pos + 5]);
        if (lo < 0xDC00u || lo > 0xDFFFu) {
            return -1;
        }
        p->pos += 6;
        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
        return -1; /* 孤立的低代理项 */
    }
    *out_cp = cp;
    return 0;
}

/* 从起始引号处解析字符串字面量并写入 `buf`。 */
static int parse_string_into(parser *p, tp_strbuf *buf, int *start_line) {
    size_t start = p->pos;

    if (start_line) {
        *start_line = line_at(p, start);
    }
    p->pos++; /* 跳过起始引号 */
    for (;;) {
        char c;
        if (p->pos >= p->len) {
            return -1;
        }
        c = p->s[p->pos];
        if (c == '"') {
            p->pos++;
            return 0;
        }
        if ((unsigned char)c < 0x20u) {
            return -1; /* 原始控制字符 */
        }
        if (c != '\\') {
            if (tp_strbuf_putc(buf, c) != 0) {
                return -2;
            }
            p->pos++;
            continue;
        }
        p->pos++;
        if (p->pos >= p->len) {
            return -1;
        }
        switch (p->s[p->pos]) {
        case '"':
            tp_strbuf_putc(buf, '"');
            p->pos++;
            break;
        case '\\':
            tp_strbuf_putc(buf, '\\');
            p->pos++;
            break;
        case '/':
            tp_strbuf_putc(buf, '/');
            p->pos++;
            break;
        case 'b':
            tp_strbuf_putc(buf, '\b');
            p->pos++;
            break;
        case 'f':
            tp_strbuf_putc(buf, '\f');
            p->pos++;
            break;
        case 'n':
            tp_strbuf_putc(buf, '\n');
            p->pos++;
            break;
        case 'r':
            tp_strbuf_putc(buf, '\r');
            p->pos++;
            break;
        case 't':
            tp_strbuf_putc(buf, '\t');
            p->pos++;
            break;
        case 'u': {
            unsigned cp = 0;
            char enc[4];
            size_t n;
            p->pos++;
            if (read_unicode_escape(p, &cp) != 0) {
                return -1;
            }
            if (cp == 0) {
                return -1; /* NUL 会截断后续所有 C 字符串 */
            }
            n = utf8_encode(cp, enc);
            if (tp_strbuf_put(buf, enc, n) != 0) {
                return -2;
            }
            break;
        }
        default:
            return -1;
        }
    }
}

static tp_json *parse_value(parser *p);

static tp_json *parse_array(parser *p) {
    tp_json *v = node_new(p);
    size_t cap = 0;
    tp_json **items = NULL;
    size_t count = 0;

    if (!v) {
        return NULL;
    }
    v->type = TP_JSON_ARRAY;
    v->line = line_at(p, p->pos);
    p->pos++; /* 跳过 '[' */
    skip_ws(p);

    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        v->u.array.items = NULL;
        v->u.array.count = 0;
        return v;
    }

    for (;;) {
        tp_json *item;
        skip_ws(p);
        item = parse_value(p);
        if (!item) {
            return NULL;
        }
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            tp_json **grown = (tp_json **)tp_arena_alloc(p->arena, new_cap * sizeof(tp_json *));
            if (!grown) {
                fail(p, TOKENPROBE_ERR_OOM, line_at(p, p->pos), "out of memory");
                return NULL;
            }
            if (items && count) {
                memcpy(grown, items, count * sizeof(tp_json *));
            }
            items = grown;
            cap = new_cap;
        }
        items[count++] = item;
        skip_ws(p);
        if (p->pos >= p->len) {
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "unterminated array");
            return NULL;
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == ']') {
            p->pos++;
            break;
        }
        fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "expected ',' or ']' in array");
        return NULL;
    }

    v->u.array.items = items;
    v->u.array.count = count;
    return v;
}

static tp_json *parse_object(parser *p) {
    tp_json *v = node_new(p);
    size_t cap = 0;
    size_t count = 0;
    char **keys = NULL;
    size_t *key_lens = NULL;
    tp_json **values = NULL;

    if (!v) {
        return NULL;
    }
    v->type = TP_JSON_OBJECT;
    v->line = line_at(p, p->pos);
    p->pos++; /* 跳过 '{' */
    skip_ws(p);

    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        v->u.object.keys = NULL;
        v->u.object.key_lens = NULL;
        v->u.object.values = NULL;
        v->u.object.count = 0;
        return v;
    }

    for (;;) {
        tp_strbuf key;
        tp_json *value;
        size_t i;

        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != '"') {
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "expected object key string");
            return NULL;
        }
        tp_strbuf_init(&key);
        if (parse_string_into(p, &key, NULL) != 0) {
            tp_strbuf_free(&key);
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "malformed object key");
            return NULL;
        }

        for (i = 0; i < count; i++) {
            if (key_lens[i] == key.len && memcmp(keys[i], key.data, key.len) == 0) {
                fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "duplicate object key");
                tp_strbuf_free(&key);
                return NULL;
            }
        }

        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "expected ':' after object key");
            tp_strbuf_free(&key);
            return NULL;
        }
        p->pos++;
        skip_ws(p);

        value = parse_value(p);
        if (!value) {
            tp_strbuf_free(&key);
            return NULL;
        }

        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            char **nk = (char **)tp_arena_alloc(p->arena, new_cap * sizeof(*nk));
            size_t *nl = (size_t *)tp_arena_alloc(p->arena, new_cap * sizeof(*nl));
            tp_json **nv = (tp_json **)tp_arena_alloc(p->arena, new_cap * sizeof(tp_json *));
            if (!nk || !nl || !nv) {
                tp_strbuf_free(&key);
                fail(p, TOKENPROBE_ERR_OOM, line_at(p, p->pos), "out of memory");
                return NULL;
            }
            if (keys && count) {
                memcpy(nk, keys, count * sizeof(*nk));
                memcpy(nl, key_lens, count * sizeof(*nl));
                memcpy(nv, values, count * sizeof(tp_json *));
            }
            keys = nk;
            key_lens = nl;
            values = nv;
            cap = new_cap;
        }

        keys[count] = tp_arena_strndup(p->arena, key.data ? key.data : "", key.len);
        key_lens[count] = key.len;
        values[count] = value;
        count++;
        tp_strbuf_free(&key);

        if (!keys[count - 1]) {
            fail(p, TOKENPROBE_ERR_OOM, line_at(p, p->pos), "out of memory");
            return NULL;
        }

        skip_ws(p);
        if (p->pos >= p->len) {
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "unterminated object");
            return NULL;
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == '}') {
            p->pos++;
            break;
        }
        fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "expected ',' or '}' in object");
        return NULL;
    }

    v->u.object.keys = keys;
    v->u.object.key_lens = key_lens;
    v->u.object.values = values;
    v->u.object.count = count;
    return v;
}

/*
 * 数字 token 校验。token 保留原始文本：金额必须交给精确的
 * tokenprobe_decimal_parse()，在这里转成 double 只会引入精度损失。
 */
static tp_json *parse_number(parser *p) {
    tp_json *v;
    size_t start = p->pos;
    int is_int = 0;

    if (p->pos < p->len && p->s[p->pos] == '-') {
        p->pos++;
    }
    if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
        fail(p, TOKENPROBE_ERR_JSON, line_at(p, start), "invalid number");
        return NULL;
    }
    if (p->s[p->pos] == '0') {
        p->pos++;
        if (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, start), "leading zero in number");
            return NULL;
        }
    } else {
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            p->pos++;
        }
    }
    if (p->pos < p->len && p->s[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, start), "digits required after '.'");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            p->pos++;
        }
    } else {
        is_int = 1;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) {
            p->pos++;
        }
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, start), "digits required in exponent");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            p->pos++;
        }
        is_int = 0;
    }
    (void)is_int;

    v = node_new(p);
    if (!v) {
        return NULL;
    }
    v->type = TP_JSON_NUMBER;
    v->line = line_at(p, start);
    v->u.number.text = p->s + start;
    v->u.number.len = p->pos - start;
    return v;
}

static tp_json *parse_literal(parser *p, const char *word, size_t n, int is_bool, int bool_value) {
    tp_json *v;
    if (p->len - p->pos < n || memcmp(p->s + p->pos, word, n) != 0) {
        fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "invalid literal");
        return NULL;
    }
    p->pos += n;
    v = node_new(p);
    if (!v) {
        return NULL;
    }
    v->type = is_bool ? TP_JSON_BOOL : TP_JSON_NULL;
    v->line = line_at(p, p->pos - n);
    if (is_bool) {
        v->u.boolean = bool_value;
    }
    return v;
}

static tp_json *parse_value(parser *p) {
    tp_strbuf buf;
    tp_json *v;
    int start_line = 0;

    if (p->failed) {
        return NULL;
    }
    if (p->depth >= TOKENPROBE_JSON_MAX_DEPTH) {
        fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "nesting too deep");
        return NULL;
    }
    if (p->pos >= p->len) {
        fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos), "unexpected end of input");
        return NULL;
    }

    switch (p->s[p->pos]) {
    case '{':
        p->depth++;
        v = parse_object(p);
        p->depth--;
        return v;
    case '[':
        p->depth++;
        v = parse_array(p);
        p->depth--;
        return v;
    case '"':
        tp_strbuf_init(&buf);
        if (parse_string_into(p, &buf, &start_line) != 0) {
            tp_strbuf_free(&buf);
            fail(p, TOKENPROBE_ERR_JSON, line_at(p, p->pos),
                 p->pos < p->len ? "malformed string" : "unterminated string");
            return NULL;
        }
        v = node_new(p);
        if (!v) {
            tp_strbuf_free(&buf);
            return NULL;
        }
        v->type = TP_JSON_STRING;
        v->line = start_line;
        v->u.string.text = tp_arena_strndup(p->arena, buf.data ? buf.data : "", buf.len);
        v->u.string.len = buf.len;
        tp_strbuf_free(&buf);
        if (!v->u.string.text) {
            fail(p, TOKENPROBE_ERR_OOM, start_line, "out of memory");
            return NULL;
        }
        return v;
    case 't':
        return parse_literal(p, "true", 4, 1, 1);
    case 'f':
        return parse_literal(p, "false", 5, 1, 0);
    case 'n':
        return parse_literal(p, "null", 4, 0, 0);
    default:
        return parse_number(p);
    }
}

tokenprobe_status_t tp_json_parse(const char *text, size_t len, tp_json_doc **out,
                                  tokenprobe_error_t *err) {
    parser p;
    tp_json_doc *doc;

    if (!out) {
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!text) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG, "JSON input is NULL");
        return TOKENPROBE_ERR_INVALID_ARG;
    }

    doc = (tp_json_doc *)calloc(1, sizeof(*doc));
    if (!doc) {
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory allocating JSON document");
        return TOKENPROBE_ERR_OOM;
    }
    doc->arena = tp_arena_create(4096);
    if (!doc->arena) {
        free(doc);
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory allocating JSON arena");
        return TOKENPROBE_ERR_OOM;
    }

    memset(&p, 0, sizeof(p));
    p.s = text;
    p.len = len;
    p.arena = doc->arena;
    p.err = err;

    skip_ws(&p);
    doc->root = parse_value(&p);
    if (!doc->root) {
        tp_json_free(doc);
        return err && err->code != TOKENPROBE_OK ? err->code : TOKENPROBE_ERR_JSON;
    }
    doc->line = doc->root->line;

    skip_ws(&p);
    if (p.pos != p.len) {
        tp_error_set(err, TOKENPROBE_ERR_JSON, "JSON error at line %d: trailing data after value",
                     line_at(&p, p.pos));
        tp_json_free(doc);
        return TOKENPROBE_ERR_JSON;
    }

    *out = doc;
    return TOKENPROBE_OK;
}

void tp_json_free(tp_json_doc *doc) {
    if (!doc) {
        return;
    }
    tp_arena_destroy(doc->arena);
    free(doc);
}

const tp_json *tp_json_root(const tp_json_doc *doc) {
    return doc ? doc->root : NULL;
}

int tp_json_is_object(const tp_json *v) {
    return v && v->type == TP_JSON_OBJECT;
}
int tp_json_is_array(const tp_json *v) {
    return v && v->type == TP_JSON_ARRAY;
}
int tp_json_is_string(const tp_json *v) {
    return v && v->type == TP_JSON_STRING;
}
int tp_json_is_number(const tp_json *v) {
    return v && v->type == TP_JSON_NUMBER;
}
int tp_json_is_bool(const tp_json *v) {
    return v && v->type == TP_JSON_BOOL;
}
int tp_json_is_null(const tp_json *v) {
    return v && v->type == TP_JSON_NULL;
}

const char *tp_json_string(const tp_json *v, size_t *len_out) {
    if (!tp_json_is_string(v)) {
        return NULL;
    }
    if (len_out) {
        *len_out = v->u.string.len;
    }
    return v->u.string.text;
}

int tp_json_bool(const tp_json *v, int *out) {
    if (!tp_json_is_bool(v)) {
        return -1;
    }
    if (out) {
        *out = v->u.boolean;
    }
    return 0;
}

const tp_json *tp_json_get(const tp_json *obj, const char *key) {
    size_t i;
    size_t n;
    if (!tp_json_is_object(obj) || !key) {
        return NULL;
    }
    n = strlen(key);
    for (i = 0; i < obj->u.object.count; i++) {
        if (obj->u.object.key_lens[i] == n && memcmp(obj->u.object.keys[i], key, n) == 0) {
            return obj->u.object.values[i];
        }
    }
    return NULL;
}

const tp_json *tp_json_at(const tp_json *arr, size_t index) {
    if (!tp_json_is_array(arr) || index >= arr->u.array.count) {
        return NULL;
    }
    return arr->u.array.items[index];
}

size_t tp_json_size(const tp_json *v) {
    if (tp_json_is_array(v)) {
        return v->u.array.count;
    }
    if (tp_json_is_object(v)) {
        return v->u.object.count;
    }
    return 0;
}

tokenprobe_status_t tp_json_decimal(const tp_json *v, tokenprobe_decimal_t *out) {
    if (!v || !out) {
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    if (tp_json_is_number(v)) {
        return tokenprobe_decimal_parse(v->u.number.text, v->u.number.len, out);
    }
    if (tp_json_is_string(v)) {
        return tokenprobe_decimal_parse(v->u.string.text, v->u.string.len, out);
    }
    return TOKENPROBE_ERR_SCHEMA;
}

tokenprobe_status_t tp_json_field_decimal(const tp_json *obj, const char *key,
                                          tokenprobe_decimal_t *out) {
    const tp_json *v = tp_json_get(obj, key);
    if (!v) {
        return TOKENPROBE_ERR_SCHEMA;
    }
    return tp_json_decimal(v, out);
}

const char *tp_json_field_string(tp_arena *a, const tp_json *obj, const char *key,
                                 size_t *len_out) {
    const tp_json *v = tp_json_get(obj, key);
    size_t n = 0;
    const char *s = tp_json_string(v, &n);
    if (!s) {
        if (len_out) {
            *len_out = 0;
        }
        return NULL;
    }
    if (len_out) {
        *len_out = n;
    }
    return a ? tp_arena_strndup(a, s, n) : s;
}
