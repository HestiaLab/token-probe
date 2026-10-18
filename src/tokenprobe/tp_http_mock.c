/*
 * 离线传输实现。
 *
 * 它让供应商驱动可以完整测试：URL 构造、认证头、状态处理、JSON 解析和指标提取
 * 都针对预置响应体运行，无需网络和 API key。与具体传输无关的驱动缺陷都能在 CI 中发现。
 */
#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct mock_entry_copy {
    char *url_prefix;
    long status;
    char *body;
    char **header_names;
    char **header_values;
    size_t header_count;
} mock_entry_copy;

typedef struct mock_impl {
    tp_arena *arena;
    mock_entry_copy *entries;
    size_t entry_count;

    /* 请求记录 */
    char **urls;
    char ***req_header_names;
    char ***req_header_values;
    size_t *req_header_counts;
    size_t req_count;
    size_t req_cap;
} mock_impl;

static char *split_header_name(const char *line, size_t *out_name_len) {
    const char *colon = strchr(line, ':');
    size_t n;
    if (!colon) {
        *out_name_len = 0;
        return NULL;
    }
    n = (size_t)(colon - line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) {
        n--;
    }
    *out_name_len = n;
    return tp_dup_range(line, n);
}

static const char *skip_header_ws(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static int grow_request_log(mock_impl *m) {
    size_t new_cap;
    char **u;
    char ***hn;
    char ***hv;
    size_t *hc;

    if (m->req_count < m->req_cap) {
        return 0;
    }
    new_cap = m->req_cap ? m->req_cap * 2 : 8;
    u = (char **)realloc(m->urls, new_cap * sizeof(*u));
    if (!u) {
        return -1;
    }
    m->urls = u;
    hn = (char ***)realloc(m->req_header_names, new_cap * sizeof(*hn));
    if (!hn) {
        return -1;
    }
    m->req_header_names = hn;
    hv = (char ***)realloc(m->req_header_values, new_cap * sizeof(*hv));
    if (!hv) {
        return -1;
    }
    m->req_header_values = hv;
    hc = (size_t *)realloc(m->req_header_counts, new_cap * sizeof(*hc));
    if (!hc) {
        return -1;
    }
    m->req_header_counts = hc;
    m->req_cap = new_cap;
    return 0;
}

static void free_request_log(mock_impl *m) {
    size_t i;
    for (i = 0; i < m->req_count; i++) {
        size_t h;
        free(m->urls[i]);
        for (h = 0; h < m->req_header_counts[i]; h++) {
            free(m->req_header_names[i][h]);
            free(m->req_header_values[i][h]);
        }
        free(m->req_header_names[i]);
        free(m->req_header_values[i]);
    }
    free(m->urls);
    free(m->req_header_names);
    free(m->req_header_values);
    free(m->req_header_counts);
    m->urls = NULL;
    m->req_header_names = NULL;
    m->req_header_values = NULL;
    m->req_header_counts = NULL;
    m->req_count = 0;
    m->req_cap = 0;
}

static void mock_destroy(void *impl) {
    mock_impl *m = (mock_impl *)impl;
    if (!m) {
        return;
    }
    free_request_log(m);
    /* 条目及其字符串都由内存池持有 */
    tp_arena_destroy(m->arena);
    free(m);
}

static const mock_entry_copy *find_entry(const mock_impl *m, const char *url) {
    const mock_entry_copy *best = NULL;
    size_t best_len = 0;
    size_t i;

    for (i = 0; i < m->entry_count; i++) {
        const mock_entry_copy *e = &m->entries[i];
        size_t n;
        if (!e->url_prefix) {
            continue;
        }
        n = strlen(e->url_prefix);
        if (strncmp(url, e->url_prefix, n) == 0 && n >= best_len) {
            best = e;
            best_len = n;
        }
    }
    return best;
}

static tokenprobe_status_t mock_perform(void *impl, const tokenprobe_http_request_t *req,
                                        tokenprobe_http_response_t *resp, tokenprobe_error_t *err) {
    mock_impl *m = (mock_impl *)impl;
    const mock_entry_copy *entry;
    size_t i;

    if (!m || !req || !resp) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG, "mock transport: invalid argument");
        return TOKENPROBE_ERR_INVALID_ARG;
    }

    if (grow_request_log(m) != 0) {
        tp_error_set(err, TOKENPROBE_ERR_OOM, "mock transport: out of memory");
        return TOKENPROBE_ERR_OOM;
    }
    {
        size_t idx = m->req_count++;
        m->urls[idx] = tp_dup(req->url ? req->url : "");
        m->req_header_counts[idx] = req->header_count;
        m->req_header_names[idx] =
            (char **)calloc(req->header_count ? req->header_count : 1, sizeof(char *));
        m->req_header_values[idx] =
            (char **)calloc(req->header_count ? req->header_count : 1, sizeof(char *));
        if (!m->urls[idx] || !m->req_header_names[idx] || !m->req_header_values[idx]) {
            tp_error_set(err, TOKENPROBE_ERR_OOM, "mock transport: out of memory");
            return TOKENPROBE_ERR_OOM;
        }
        for (i = 0; i < req->header_count; i++) {
            m->req_header_names[idx][i] = tp_dup(req->headers[i].name ? req->headers[i].name : "");
            m->req_header_values[idx][i] =
                tp_dup(req->headers[i].value ? req->headers[i].value : "");
            if (!m->req_header_names[idx][i] || !m->req_header_values[idx][i]) {
                tp_error_set(err, TOKENPROBE_ERR_OOM, "mock transport: out of memory");
                return TOKENPROBE_ERR_OOM;
            }
        }
    }

    entry = find_entry(m, req->url ? req->url : "");
    if (!entry) {
        tp_error_set(err, TOKENPROBE_ERR_HTTP, "mock transport: no entry matches '%s'", req->url);
        tp_error_set_hint(err, "add a tokenprobe_mock_entry_t whose url_prefix matches this URL");
        return TOKENPROBE_ERR_HTTP;
    }

    resp->status = entry->status;
    if (entry->body) {
        resp->body = tp_dup(entry->body);
        if (!resp->body) {
            tp_error_set(err, TOKENPROBE_ERR_OOM, "mock transport: out of memory");
            return TOKENPROBE_ERR_OOM;
        }
        resp->body_len = strlen(entry->body);
    }

    if (entry->header_count) {
        resp->headers = (tokenprobe_http_header_t *)calloc(entry->header_count,
                                                           sizeof(tokenprobe_http_header_t));
        if (!resp->headers) {
            tp_error_set(err, TOKENPROBE_ERR_OOM, "mock transport: out of memory");
            return TOKENPROBE_ERR_OOM;
        }
        for (i = 0; i < entry->header_count; i++) {
            resp->headers[i].name = entry->header_names[i];
            resp->headers[i].value = entry->header_values[i];
        }
        resp->header_count = entry->header_count;
    }
    return TOKENPROBE_OK;
}

int tokenprobe_transport_mock_create(const tokenprobe_mock_entry_t *entries, size_t entry_count,
                                     tokenprobe_transport_t *out_transport) {
    mock_impl *m;
    size_t i;

    if (!out_transport) {
        return -1;
    }
    memset(out_transport, 0, sizeof(*out_transport));

    m = (mock_impl *)calloc(1, sizeof(*m));
    if (!m) {
        return -1;
    }
    m->arena = tp_arena_create(4096);
    if (!m->arena) {
        free(m);
        return -1;
    }

    m->entries = (mock_entry_copy *)tp_arena_alloc(m->arena, (entry_count ? entry_count : 1) *
                                                                 sizeof(mock_entry_copy));
    if (!m->entries) {
        tp_arena_destroy(m->arena);
        free(m);
        return -1;
    }
    memset(m->entries, 0, (entry_count ? entry_count : 1) * sizeof(mock_entry_copy));
    m->entry_count = entry_count;

    for (i = 0; i < entry_count; i++) {
        mock_entry_copy *dst = &m->entries[i];
        size_t h;

        dst->url_prefix = tp_arena_strdup(m->arena, entries[i].url_prefix);
        dst->status = entries[i].status ? entries[i].status : 200;
        dst->body = tp_arena_strdup(m->arena, entries[i].body);

        if (entries[i].header_count && entries[i].header_lines) {
            dst->header_names =
                (char **)tp_arena_alloc(m->arena, entries[i].header_count * sizeof(char *));
            dst->header_values =
                (char **)tp_arena_alloc(m->arena, entries[i].header_count * sizeof(char *));
            if (!dst->header_names || !dst->header_values) {
                tp_arena_destroy(m->arena);
                free(m);
                return -1;
            }
            for (h = 0; h < entries[i].header_count; h++) {
                size_t name_len = 0;
                char *name = split_header_name(entries[i].header_lines[h], &name_len);
                const char *value;
                if (!name) {
                    continue;
                }
                value = strchr(entries[i].header_lines[h], ':');
                value = value ? skip_header_ws(value + 1) : "";
                dst->header_names[dst->header_count] = name;
                dst->header_values[dst->header_count] = tp_arena_strdup(m->arena, value);
                if (!dst->header_values[dst->header_count]) {
                    free(name);
                    tp_arena_destroy(m->arena);
                    free(m);
                    return -1;
                }
                (void)name_len;
                dst->header_count++;
            }
        }
    }

    out_transport->name = "mock";
    out_transport->perform = mock_perform;
    out_transport->destroy = mock_destroy;
    out_transport->impl = m;
    return 0;
}

void tokenprobe_transport_mock_free(tokenprobe_transport_t *transport) {
    if (transport && transport->destroy) {
        transport->destroy(transport->impl);
    }
    if (transport) {
        memset(transport, 0, sizeof(*transport));
    }
}

size_t tokenprobe_mock_request_count(const tokenprobe_transport_t *transport) {
    const mock_impl *m = transport ? (const mock_impl *)transport->impl : NULL;
    return m ? m->req_count : 0;
}

const char *tokenprobe_mock_request_url(const tokenprobe_transport_t *transport, size_t index) {
    const mock_impl *m = transport ? (const mock_impl *)transport->impl : NULL;
    if (!m || index >= m->req_count) {
        return NULL;
    }
    return m->urls[index];
}

const char *tokenprobe_mock_request_header(const tokenprobe_transport_t *transport, size_t index,
                                           const char *name) {
    const mock_impl *m = transport ? (const mock_impl *)transport->impl : NULL;
    size_t i;
    if (!m || index >= m->req_count || !name) {
        return NULL;
    }
    for (i = 0; i < m->req_header_counts[index]; i++) {
        if (m->req_header_names[index][i] && tp_str_ieq(m->req_header_names[index][i], name)) {
            return m->req_header_values[index][i];
        }
    }
    return NULL;
}
