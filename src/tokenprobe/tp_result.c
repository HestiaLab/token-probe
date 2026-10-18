/*
 * tokenprobe_result_t：每次探测返回的结果对象。
 *
 * 写入结果的字符串都复制到私有内存池，调用方无需关心所有权，探测实现也能在
 * 构建结果后立即释放 JSON 文档。这样嵌入方可以跨帧持有结果，而不必保留解析器状态。
 */
#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

struct tokenprobe_result_t {
    tp_arena *arena;
    char *provider;
    char *account;
    char *note;
    char *raw_body;
    long http_status;
    int64_t latency_ms;
    int64_t observed_at_ms;
    int available; /* -1 未知，0 否，1 是 */
    tokenprobe_metric_t *metrics;
    size_t metric_count;
    size_t metric_cap;
};

static char *arena_dup_or_null(tp_arena *a, const char *s) {
    return s ? tp_arena_strdup(a, s) : NULL;
}

tokenprobe_result_t *tokenprobe_result_create(void) {
    tokenprobe_result_t *r = (tokenprobe_result_t *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->arena = tp_arena_create(2048);
    if (!r->arena) {
        free(r);
        return NULL;
    }
    r->available = -1;
    return r;
}

void tp_result_set_observed_at(tokenprobe_result_t *r, int64_t ms) {
    if (r) {
        r->observed_at_ms = ms;
    }
}

void tokenprobe_result_free(tokenprobe_result_t *r) {
    if (!r) {
        return;
    }
    if (r->arena) {
        tp_arena_destroy(r->arena);
    }
    free(r->metrics);
    free(r);
}

const char *tokenprobe_result_provider(const tokenprobe_result_t *r) {
    return r ? r->provider : NULL;
}
const char *tokenprobe_result_account(const tokenprobe_result_t *r) {
    return r ? r->account : NULL;
}
const char *tokenprobe_result_note(const tokenprobe_result_t *r) {
    return r ? r->note : NULL;
}
const char *tokenprobe_result_raw_body(const tokenprobe_result_t *r) {
    return r ? r->raw_body : NULL;
}
long tokenprobe_result_http_status(const tokenprobe_result_t *r) {
    return r ? r->http_status : 0;
}
int64_t tokenprobe_result_latency_ms(const tokenprobe_result_t *r) {
    return r ? r->latency_ms : 0;
}
int64_t tokenprobe_result_observed_at_ms(const tokenprobe_result_t *r) {
    return r ? r->observed_at_ms : 0;
}
int tokenprobe_result_available(const tokenprobe_result_t *r) {
    return r ? r->available : -1;
}
int tokenprobe_result_metric_count(const tokenprobe_result_t *r) {
    return r ? (int)r->metric_count : 0;
}

const tokenprobe_metric_t *tokenprobe_result_metric(const tokenprobe_result_t *r, int index) {
    if (!r || index < 0 || (size_t)index >= r->metric_count) {
        return NULL;
    }
    return &r->metrics[index];
}

void tokenprobe_result_set_provider(tokenprobe_result_t *r, const char *provider_id) {
    if (r) {
        r->provider = arena_dup_or_null(r->arena, provider_id);
    }
}

void tokenprobe_result_set_account(tokenprobe_result_t *r, const char *account) {
    if (r) {
        r->account = arena_dup_or_null(r->arena, account);
    }
}

void tokenprobe_result_set_note(tokenprobe_result_t *r, const char *note) {
    if (r) {
        r->note = arena_dup_or_null(r->arena, note);
    }
}

void tokenprobe_result_set_raw_body(tokenprobe_result_t *r, const char *body) {
    if (r) {
        r->raw_body = arena_dup_or_null(r->arena, body);
    }
}

void tokenprobe_result_set_available(tokenprobe_result_t *r, int available) {
    if (r) {
        r->available = available ? 1 : 0;
    }
}

void tokenprobe_result_set_timing(tokenprobe_result_t *r, long http_status, int64_t latency_ms) {
    if (r) {
        r->http_status = http_status;
        r->latency_ms = latency_ms;
    }
}

tokenprobe_status_t tokenprobe_result_add_metric(tokenprobe_result_t *r,
                                                 const tokenprobe_metric_t *m) {
    tokenprobe_metric_t *slot;

    if (!r || !m || !m->id) {
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    if (r->metric_count == r->metric_cap) {
        size_t new_cap = r->metric_cap ? r->metric_cap * 2 : 8;
        tokenprobe_metric_t *grown =
            (tokenprobe_metric_t *)realloc(r->metrics, new_cap * sizeof(*grown));
        if (!grown) {
            return TOKENPROBE_ERR_OOM;
        }
        r->metrics = grown;
        r->metric_cap = new_cap;
    }

    slot = &r->metrics[r->metric_count];
    memset(slot, 0, sizeof(*slot));
    slot->kind = m->kind;
    slot->amount = m->amount;
    slot->id = arena_dup_or_null(r->arena, m->id);
    slot->label = arena_dup_or_null(r->arena, m->label);
    slot->currency = arena_dup_or_null(r->arena, m->currency);
    slot->unit = arena_dup_or_null(r->arena, m->unit);
    slot->period_start = arena_dup_or_null(r->arena, m->period_start);
    slot->period_end = arena_dup_or_null(r->arena, m->period_end);

    if (!slot->id) {
        return TOKENPROBE_ERR_OOM;
    }
    r->metric_count++;
    return TOKENPROBE_OK;
}

/* ----------------------------------------------------------------- 传输层 */

void tokenprobe_http_response_init(tokenprobe_http_response_t *resp) {
    if (resp) {
        memset(resp, 0, sizeof(*resp));
    }
}

void tokenprobe_http_response_free(tokenprobe_http_response_t *resp) {
    size_t i;
    if (!resp) {
        return;
    }
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    for (i = 0; i < resp->header_count; i++) {
        free((void *)resp->headers[i].name);
        free((void *)resp->headers[i].value);
    }
    free(resp->headers);
    resp->headers = NULL;
    resp->header_count = 0;
}

const char *tokenprobe_http_header_get(const tokenprobe_http_response_t *resp, const char *name) {
    size_t i;
    size_t n;
    if (!resp || !name) {
        return NULL;
    }
    n = strlen(name);
    for (i = 0; i < resp->header_count; i++) {
        const char *h = resp->headers[i].name;
        if (h && strlen(h) == n && tp_str_icmp_n(h, name, n) == 0) {
            return resp->headers[i].value;
        }
    }
    return NULL;
}
