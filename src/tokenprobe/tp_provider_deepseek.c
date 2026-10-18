/*
 * DeepSeek 供应商驱动。
 *
 * 接口已按 https://api-docs.deepseek.com/api/get-user-balance 核对：
 *   GET https://api.deepseek.com/user/balance
 *   Authorization: Bearer <key>
 *   200 {"is_available":true,
 *        "balance_infos":[{"currency":"CNY","total_balance":"110.00",
 *                          "granted_balance":"10.00","topped_up_balance":"100.00"}]}
 *
 * 三个金额都是 JSON 字符串，这正是需要十进制层的原因。DeepSeek 没有用量/费用
 * 接口，因此 supports_spend == 0，调用方会得到 TOKENPROBE_ERR_UNSUPPORTED，
 * 而不是虚假的 0。
 */
#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

static tokenprobe_status_t deepseek_probe(const tokenprobe_provider_t *self,
                                          tokenprobe_context_t *ctx,
                                          const tokenprobe_provider_request_t *req,
                                          tokenprobe_result_t *out, tokenprobe_error_t *err) {
    tp_arena *arena = NULL;
    tp_url url;
    tp_strbuf auth;
    tokenprobe_http_response_t resp;
    tp_json_doc *doc = NULL;
    const tp_json *root;
    const tp_json *infos;
    tokenprobe_status_t st;
    size_t i;
    int64_t t0;
    int added = 0;

    (void)self;
    if (!out) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG, "result is NULL");
        return TOKENPROBE_ERR_INVALID_ARG;
    }

    tp_strbuf_init(&auth);
    tokenprobe_http_response_init(&resp);
    arena = tp_arena_create(2048);
    if (!arena) {
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory");
        return TOKENPROBE_ERR_OOM;
    }

    st = tp_url_parse_into(arena, req->base_url, &url, err);
    if (st != TOKENPROBE_OK) {
        goto done;
    }
    /* base_url 可能带版本前缀；余额接口位于主机根路径，因此只复用
     * scheme://host:port。 */
    {
        tp_strbuf full;
        tp_strbuf_init(&full);
        tp_strbuf_printf(&full, "%s://%s", url.scheme, url.host);
        if ((url.is_secure && url.port != 443) || (!url.is_secure && url.port != 80)) {
            tp_strbuf_printf(&full, ":%d", url.port);
        }
        tp_strbuf_puts(&full, "/user/balance");
        if (!full.data) {
            tp_strbuf_free(&full);
            st = TOKENPROBE_ERR_OOM;
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building endpoint URL");
            goto done;
        }
        memset(&url, 0, sizeof(url));
        st = tp_url_parse_into(arena, full.data, &url, err);
        tp_strbuf_free(&full);
        if (st != TOKENPROBE_OK) {
            goto done;
        }
    }

    if (tp_strbuf_printf(&auth, "Bearer %s", req->api_key) != 0) {
        st = TOKENPROBE_ERR_OOM;
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building Authorization header");
        goto done;
    }

    {
        size_t extra = req->header_count;
        tokenprobe_http_header_t *all;
        size_t h;

        if (extra > (size_t)-1 - 3) {
            st = TOKENPROBE_ERR_OOM;
            tp_error_set(err, TOKENPROBE_ERR_OOM, "too many extra request headers");
            goto done;
        }
        all = (tokenprobe_http_header_t *)calloc(3 + extra, sizeof(*all));
        if (!all) {
            st = TOKENPROBE_ERR_OOM;
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building request headers");
            goto done;
        }
        all[0].name = "Authorization";
        all[0].value = auth.data;
        all[1].name = "Accept";
        all[1].value = "application/json";
        all[2].name = "Accept-Encoding";
        all[2].value = "identity";
        for (h = 0; h < extra; h++) {
            all[3 + h] = req->headers[h];
        }

        t0 = tp_now_ms();
        st = tp_probe_http_ex(ctx, "GET", &url, all, 3 + extra, NULL, 0, &resp, err);
        free(all);
    }
    if (st != TOKENPROBE_OK) {
        goto done;
    }

    tokenprobe_result_set_provider(out, "deepseek");
    tokenprobe_result_set_timing(out, resp.status, tp_now_ms() - t0);
    /* 限制复制长度：错误页可能有数 MB，而这里只用于展示。 */
    if (resp.body) {
        size_t n = resp.body_len > 8192 ? 8192 : resp.body_len;
        tokenprobe_result_set_raw_body(out, tp_arena_strndup(arena, resp.body, n));
    }

    if (resp.status < 200 || resp.status >= 300) {
        st = tp_status_from_http((int)resp.status);
        tp_error_set_http(err, st, (int)resp.status, "%s returned HTTP %ld for %s",
                          self->display_name, resp.status, "/user/balance");
        if (resp.status == 401) {
            tp_error_set_hint(err, "the API key was rejected; check %s",
                              self->env_var ? self->env_var : "your key");
        } else if (resp.status == 404) {
            tp_error_set_hint(err, "unexpected 404 — check --base-url");
        } else if (resp.status >= 500) {
            tp_error_set_hint(err, "provider-side failure; retry later");
        }
        goto done;
    }

    if (!resp.body || resp.body_len == 0) {
        st = TOKENPROBE_ERR_SCHEMA;
        tp_error_set(err, TOKENPROBE_ERR_SCHEMA, "%s returned an empty body", self->display_name);
        goto done;
    }

    st = tp_json_parse(resp.body, resp.body_len, &doc, err);
    if (st != TOKENPROBE_OK) {
        goto done;
    }
    root = tp_json_root(doc);
    if (!tp_json_is_object(root)) {
        st = TOKENPROBE_ERR_SCHEMA;
        tp_error_set(err, TOKENPROBE_ERR_SCHEMA, "%s balance response is not a JSON object",
                     self->display_name);
        goto done;
    }

    {
        const tp_json *avail = tp_json_get(root, "is_available");
        int flag = 0;
        if (tp_json_bool(avail, &flag) == 0) {
            tokenprobe_result_set_available(out, flag);
        }
    }

    infos = tp_json_get(root, "balance_infos");
    if (!tp_json_is_array(infos)) {
        st = TOKENPROBE_ERR_SCHEMA;
        tp_error_set(err, TOKENPROBE_ERR_SCHEMA,
                     "%s response has no 'balance_infos' array; the API shape may have changed",
                     self->display_name);
        tp_error_set_hint(err, "see %s", self->doc_url ? self->doc_url : "the provider docs");
        goto done;
    }

    for (i = 0; i < tp_json_size(infos); i++) {
        const tp_json *entry = tp_json_at(infos, i);
        const char *ccy;
        size_t ccy_len = 0;
        tokenprobe_decimal_t amount;
        static const struct {
            const char *key;
            const char *id;
            const char *label;
        } fields[] = {
            {"total_balance", "total_balance", "Total balance"},
            {"granted_balance", "granted_balance", "Granted balance"},
            {"topped_up_balance", "topped_up_balance", "Topped-up balance"},
        };
        size_t f;

        if (!tp_json_is_object(entry)) {
            continue;
        }
        ccy = tp_json_field_string(arena, entry, "currency", &ccy_len);
        if (!ccy) {
            ccy = "UNKNOWN";
        }

        for (f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
            tokenprobe_metric_t m;
            st = tp_json_field_decimal(entry, fields[f].key, &amount);
            if (st == TOKENPROBE_ERR_SCHEMA) {
                continue; /* 该币种缺少此字段，不算错误 */
            }
            if (st != TOKENPROBE_OK) {
                tp_error_set(err, st, "%s.%s is present but not a number", fields[f].key,
                             fields[f].key);
                goto done;
            }
            memset(&m, 0, sizeof(m));
            m.kind = TOKENPROBE_METRIC_CREDIT;
            m.id = fields[f].id;
            m.label = fields[f].label;
            m.currency = ccy;
            m.unit = "currency";
            m.amount = amount;
            st = tokenprobe_result_add_metric(out, &m);
            if (st != TOKENPROBE_OK) {
                tp_error_set(err, st, "failed to store metric %s", fields[f].id);
                goto done;
            }
            added++;
        }
    }

    if (added == 0) {
        st = TOKENPROBE_ERR_SCHEMA;
        tp_error_set(err, TOKENPROBE_ERR_SCHEMA,
                     "%s returned a balance_infos array with no usable amounts",
                     self->display_name);
        goto done;
    }

    {
        size_t n = tp_json_size(infos);
        if (n > 1) {
            tokenprobe_result_set_note(out, "multiple currencies reported; each currency is listed "
                                            "separately and must not be summed");
        }
    }

    /* DeepSeek 没有公开的用量/费用接口。这里明确记录，避免让调用方误以为费用为 0。 */
    {
        tokenprobe_metric_t q;
        memset(&q, 0, sizeof(q));
        q.kind = TOKENPROBE_METRIC_SPEND;
        q.id = "spend";
        q.label = "Period spend";
        q.unit = "currency";
        (void)q;
        tokenprobe_context_log(ctx, TOKENPROBE_LOG_DEBUG,
                               "deepseek: no spend endpoint available; SPEND metric not emitted");
    }

    st = TOKENPROBE_OK;

done:
    tp_json_free(doc);
    tokenprobe_http_response_free(&resp);
    tp_arena_destroy(arena);
    tp_strbuf_free(&auth);
    return st;
}

const tokenprobe_provider_t tp_provider_deepseek = {
    /* id             */ "deepseek",
    /* display_name   */ "DeepSeek",
    /* base_url       */ "https://api.deepseek.com",
    /* env_var        */ "DEEPSEEK_API_KEY",
    /* doc_url        */ "https://api-docs.deepseek.com/api/get-user-balance",
    /* supports_credit*/ 1,
    /* supports_spend */ 0,
    /* supports_quota */ 0,
    /* notes          */
    "Balance only. DeepSeek exposes no usage/cost-report API, so "
    "period spend cannot be reported.",
    /* probe          */ deepseek_probe,
};
