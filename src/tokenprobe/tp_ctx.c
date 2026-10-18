/*
 * 上下文、供应商注册表以及 tokenprobe_probe() 的编排逻辑。
 *
 * 设计要点：
 *  - 没有全局可变状态。所有操作都经过 tokenprobe_context_t，因此库可以安全地
 *    嵌入到已有线程模型的主机中。
 *  - v1 顺序探测各供应商。并发由调用方负责：公共 API 已按上下文可重入，
 *    主机可以为每个线程创建一个上下文，库本身不维护线程池。
 */
#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

/* 由各供应商驱动实现，声明见 tp_internal.h。 */
static const tokenprobe_provider_t *const k_providers[] = {
    &tp_provider_deepseek,
};

size_t tokenprobe_provider_count(void) {
    return sizeof(k_providers) / sizeof(k_providers[0]);
}

const tokenprobe_provider_t *tokenprobe_provider_at(size_t index) {
    if (index >= tokenprobe_provider_count()) {
        return NULL;
    }
    return k_providers[index];
}

const tokenprobe_provider_t *tokenprobe_provider_find(const char *id) {
    size_t i;
    if (!id) {
        return NULL;
    }
    for (i = 0; i < tokenprobe_provider_count(); i++) {
        const tokenprobe_provider_t *p = k_providers[i];
        size_t j;
        if (tp_str_ieq(p->id, id)) {
            return p;
        }
        /* 也接受显示名，忽略空格、点、连字符和下划线。 */
        {
            size_t k = 0;
            char compact[64];
            for (j = 0; p->display_name[j] && k + 1 < sizeof(compact); j++) {
                char c = p->display_name[j];
                if (c == ' ' || c == '.' || c == '-' || c == '_') {
                    continue;
                }
                compact[k++] = (char)tp_ascii_lower((unsigned char)c);
            }
            compact[k] = '\0';
            if (k > 0 && tp_str_ieq(compact, id)) {
                return p;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ 状态文本 */

const char *tokenprobe_status_name(tokenprobe_status_t s) {
    switch (s) {
    case TOKENPROBE_OK:
        return "OK";
    case TOKENPROBE_ERR_INVALID_ARG:
        return "INVALID_ARG";
    case TOKENPROBE_ERR_INVALID_KEY:
        return "INVALID_KEY";
    case TOKENPROBE_ERR_INVALID_URL:
        return "INVALID_URL";
    case TOKENPROBE_ERR_HTTP:
        return "HTTP";
    case TOKENPROBE_ERR_TLS:
        return "TLS";
    case TOKENPROBE_ERR_TIMEOUT:
        return "TIMEOUT";
    case TOKENPROBE_ERR_DNS:
        return "DNS";
    case TOKENPROBE_ERR_CONNECT:
        return "CONNECT";
    case TOKENPROBE_ERR_JSON:
        return "JSON";
    case TOKENPROBE_ERR_PARSE:
        return "PARSE";
    case TOKENPROBE_ERR_SCHEMA:
        return "SCHEMA";
    case TOKENPROBE_ERR_ENCODING:
        return "ENCODING";
    case TOKENPROBE_ERR_AUTH:
        return "AUTH";
    case TOKENPROBE_ERR_FORBIDDEN:
        return "FORBIDDEN";
    case TOKENPROBE_ERR_NOT_FOUND:
        return "NOT_FOUND";
    case TOKENPROBE_ERR_RATE_LIMIT:
        return "RATE_LIMIT";
    case TOKENPROBE_ERR_SERVER:
        return "SERVER";
    case TOKENPROBE_ERR_UNSUPPORTED:
        return "UNSUPPORTED";
    case TOKENPROBE_ERR_OOM:
        return "OUT_OF_MEMORY";
    case TOKENPROBE_ERR_IO:
        return "IO";
    case TOKENPROBE_ERR_INTERNAL:
        return "INTERNAL";
    default:
        return "UNKNOWN";
    }
}

const char *tokenprobe_status_message(tokenprobe_status_t s) {
    switch (s) {
    case TOKENPROBE_OK:
        return "success";
    case TOKENPROBE_ERR_INVALID_ARG:
        return "an argument was NULL or out of range";
    case TOKENPROBE_ERR_INVALID_KEY:
        return "no API key was supplied and the provider's environment variable is unset";
    case TOKENPROBE_ERR_INVALID_URL:
        return "the base URL could not be parsed";
    case TOKENPROBE_ERR_HTTP:
        return "the server returned an unexpected HTTP status";
    case TOKENPROBE_ERR_TLS:
        return "the TLS handshake failed (certificate or protocol error)";
    case TOKENPROBE_ERR_TIMEOUT:
        return "the request timed out";
    case TOKENPROBE_ERR_DNS:
        return "the host name could not be resolved";
    case TOKENPROBE_ERR_CONNECT:
        return "the TCP connection could not be established";
    case TOKENPROBE_ERR_JSON:
        return "the response body is not valid JSON";
    case TOKENPROBE_ERR_PARSE:
        return "a numeric or textual field could not be parsed";
    case TOKENPROBE_ERR_SCHEMA:
        return "the response does not contain the fields this provider expects";
    case TOKENPROBE_ERR_ENCODING:
        return "the response uses a transfer or content encoding this build cannot decode";
    case TOKENPROBE_ERR_AUTH:
        return "the API key was rejected (401)";
    case TOKENPROBE_ERR_FORBIDDEN:
        return "the key is valid but lacks the required scope (403)";
    case TOKENPROBE_ERR_NOT_FOUND:
        return "the endpoint does not exist for this account (404)";
    case TOKENPROBE_ERR_RATE_LIMIT:
        return "the provider is rate limiting this key (429)";
    case TOKENPROBE_ERR_SERVER:
        return "the provider returned a server error (5xx)";
    case TOKENPROBE_ERR_UNSUPPORTED:
        return "this provider does not expose this metric through its public API";
    case TOKENPROBE_ERR_OOM:
        return "out of memory";
    case TOKENPROBE_ERR_IO:
        return "a local I/O operation failed";
    case TOKENPROBE_ERR_INTERNAL:
        return "internal error";
    default:
        return "unknown status";
    }
}

void tokenprobe_error_init(tokenprobe_error_t *err) {
    if (!err) {
        return;
    }
    err->code = TOKENPROBE_OK;
    err->http_status = 0;
    err->message[0] = '\0';
    err->hint[0] = '\0';
}

void tokenprobe_error_format(char *dst, size_t dst_len, const tokenprobe_error_t *err) {
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!err) {
        return;
    }
    snprintf(dst, dst_len, "%s: %s%s%s", tokenprobe_status_name(err->code),
             err->message[0] ? err->message : tokenprobe_status_message(err->code),
             err->hint[0] ? " — " : "", err->hint);
}

/* ----------------------------------------------------------------- 上下文 */

struct tokenprobe_context_t {
    tokenprobe_transport_t transport;
    int owned_transport; /* 1 表示 impl 由本 context 私有，销毁时释放 */
    int timeout_ms;
    char *user_agent;
    tokenprobe_log_fn log_fn;
    void *log_user;
};

void tokenprobe_context_options_init(tokenprobe_context_options_t *opts) {
    if (!opts) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->struct_size = sizeof(*opts);
    opts->timeout_ms = TOKENPROBE_DEFAULT_TIMEOUT_MS;
}

#if !defined(TOKENPROBE_USE_WINHTTP) && !defined(TOKENPROBE_USE_OPENSSL)
/*
 * 只含 mock 传输的构建（CI 矩阵、主机测试夹具）也必须能链接。返回 NULL 是
 * “本构建没有平台传输层”的约定行为：此时 tokenprobe_context_create() 失败，
 * 而自行注入传输层的调用方不受影响。
 */
const tokenprobe_transport_t *tokenprobe_transport_platform(void) {
    return NULL;
}
#endif

static char *default_user_agent(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "tokenprobe/%d.%d.%d", TOKENPROBE_VERSION_MAJOR,
             TOKENPROBE_VERSION_MINOR, TOKENPROBE_VERSION_PATCH);
    return tp_dup(buf);
}

tokenprobe_context_t *tokenprobe_context_create(const tokenprobe_context_options_t *opts) {
    tokenprobe_context_t *ctx = (tokenprobe_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->timeout_ms =
        (opts && opts->timeout_ms > 0) ? opts->timeout_ms : TOKENPROBE_DEFAULT_TIMEOUT_MS;
    ctx->log_fn = opts ? opts->log_fn : NULL;
    ctx->log_user = opts ? opts->log_user : NULL;
    ctx->user_agent = (opts && opts->user_agent) ? tp_dup(opts->user_agent) : default_user_agent();

    if (opts && opts->transport && opts->transport->perform) {
        ctx->transport = *opts->transport;
        ctx->owned_transport = 0;
    } else {
        const tokenprobe_transport_t *platform = tokenprobe_transport_platform();
        if (!platform) {
            free(ctx->user_agent);
            free(ctx);
            return NULL;
        }
        ctx->transport = *platform;
        /* 平台传输层是进程级共享实例，context 只借用，不负责销毁。 */
        ctx->owned_transport = 0;
    }
    return ctx;
}

void tokenprobe_context_destroy(tokenprobe_context_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->owned_transport && ctx->transport.destroy) {
        ctx->transport.destroy(ctx->transport.impl);
    }
    free(ctx->user_agent);
    free(ctx);
}

const tokenprobe_transport_t *tokenprobe_context_transport(const tokenprobe_context_t *ctx) {
    return ctx ? &ctx->transport : NULL;
}

void tokenprobe_context_log(tokenprobe_context_t *ctx, tokenprobe_log_level_t level,
                            const char *fmt, ...) {
    char buf[512];
    va_list ap;
    if (!ctx || !ctx->log_fn) {
        return;
    }
    va_start(ap, fmt);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized): va_start 已初始化 ap
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->log_fn(ctx->log_user, level, buf);
}

/* ------------------------------------------------------------------- 探测 */

/* 供应商驱动通过它构造请求，以保证请求头一致。 */
tokenprobe_status_t tp_probe_http_ex(tokenprobe_context_t *ctx, const char *method,
                                     const tp_url *url, const tokenprobe_http_header_t *headers,
                                     size_t header_count, const char *body, size_t body_len,
                                     tokenprobe_http_response_t *resp, tokenprobe_error_t *err) {
    tokenprobe_http_request_t req;
    tp_strbuf sb;
    tokenprobe_status_t st;
    size_t i;

    if (!ctx || !url || !resp) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG, "invalid argument to HTTP probe");
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    if (header_count > (size_t)-1 - 1) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG, "too many request headers");
        return TOKENPROBE_ERR_INVALID_ARG;
    }

    tp_strbuf_init(&sb);
    tp_strbuf_printf(&sb, "%s://%s", url->scheme, url->host);
    if ((url->is_secure && url->port != 443) || (!url->is_secure && url->port != 80)) {
        tp_strbuf_printf(&sb, ":%d", url->port);
    }
    tp_strbuf_puts(&sb, url->path);
    if (url->query) {
        tp_strbuf_putc(&sb, '?');
        tp_strbuf_puts(&sb, url->query);
    }
    if (!sb.data) {
        tp_strbuf_free(&sb);
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building request URL");
        return TOKENPROBE_ERR_OOM;
    }

    {
        tokenprobe_http_header_t *with_ua =
            (tokenprobe_http_header_t *)calloc(header_count + 1, sizeof(tokenprobe_http_header_t));
        if (!with_ua) {
            tp_strbuf_free(&sb);
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building request headers");
            return TOKENPROBE_ERR_OOM;
        }
        for (i = 0; i < header_count; i++) {
            with_ua[i] = headers[i];
        }
        with_ua[header_count].name = "User-Agent";
        with_ua[header_count].value = ctx->user_agent;

        memset(&req, 0, sizeof(req));
        req.method = (method && *method) ? method : "GET";
        req.url = sb.data;
        req.headers = with_ua;
        req.header_count = header_count + 1;
        req.body = body;
        req.body_len = body_len;
        req.timeout_ms = ctx->timeout_ms;

        tokenprobe_context_log(ctx, TOKENPROBE_LOG_DEBUG, "%s %s", req.method, req.url);

        tokenprobe_http_response_init(resp);
        st = ctx->transport.perform(ctx->transport.impl, &req, resp, err);
        free(with_ua);
    }

    tp_strbuf_free(&sb);

    if (st != TOKENPROBE_OK) {
        if (err && err->code == TOKENPROBE_OK) {
            tp_error_set(err, st, "transport failed: %s", tokenprobe_status_message(st));
        }
        return st;
    }
    return TOKENPROBE_OK;
}

tokenprobe_status_t tp_probe_http(tokenprobe_context_t *ctx, const tp_url *url,
                                  const tokenprobe_http_header_t *headers, size_t header_count,
                                  tokenprobe_http_response_t *resp, tokenprobe_error_t *err) {
    return tp_probe_http_ex(ctx, "GET", url, headers, header_count, NULL, 0, resp, err);
}

void tokenprobe_probe_options_init(tokenprobe_probe_options_t *options) {
    if (!options) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
}

tokenprobe_status_t tokenprobe_probe(const tokenprobe_provider_t *p, tokenprobe_context_t *ctx,
                                     const char *api_key, const char *base_url,
                                     tokenprobe_result_t **out, tokenprobe_error_t *err) {
    return tokenprobe_probe_ex(p, ctx, api_key, base_url, NULL, out, err);
}

tokenprobe_status_t tokenprobe_probe_ex(const tokenprobe_provider_t *p, tokenprobe_context_t *ctx,
                                        const char *api_key, const char *base_url,
                                        const tokenprobe_probe_options_t *options,
                                        tokenprobe_result_t **out, tokenprobe_error_t *err) {
    tokenprobe_provider_request_t req;
    tokenprobe_provider_request_t *reqp = &req;
    tokenprobe_error_t scratch;
    const char *key;
    const char *from_env = NULL;
    char from_env_buf[512];
    tokenprobe_status_t st;

    if (!out) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG,
                     "tokenprobe_probe requires provider, context and out");
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!p || !ctx) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG,
                     "tokenprobe_probe requires provider, context and out");
        return TOKENPROBE_ERR_INVALID_ARG;
    }
    if (options) {
        if (options->struct_size < sizeof(tokenprobe_probe_options_t)) {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG,
                         "probe options were not initialized with "
                         "tokenprobe_probe_options_init()");
            return TOKENPROBE_ERR_INVALID_ARG;
        }
        if (options->header_count > 0 && !options->headers) {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG,
                         "probe options have header_count but no headers");
            return TOKENPROBE_ERR_INVALID_ARG;
        }
        if (options->body_len > 0 && !options->body) {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG,
                         "probe options have body_len but no body");
            return TOKENPROBE_ERR_INVALID_ARG;
        }
    }
    if (!p->probe) {
        tp_error_set(err, TOKENPROBE_ERR_UNSUPPORTED, "provider '%s' has no probe implementation",
                     p->id);
        return TOKENPROBE_ERR_UNSUPPORTED;
    }

    key = api_key;
    if ((!key || !*key) && p->env_var) {
        const char *env = getenv(p->env_var);
        if (env && *env) {
            /*
             * 复制一份环境变量内容：并发的 SetEnvironmentVariable 可能使
             * getenv 返回的指针失效。
             */
            size_t n = strlen(env);
            if (n >= sizeof(from_env_buf)) {
                n = sizeof(from_env_buf) - 1;
            }
            memcpy(from_env_buf, env, n);
            from_env_buf[n] = '\0';
            from_env = from_env_buf;
            key = from_env;
        }
    }
    if (!key || !*key) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_KEY, "no API key for %s", p->display_name);
        tp_error_set_hint(err, "set %s or pass --api-key", p->env_var ? p->env_var : "<API_KEY>");
        return TOKENPROBE_ERR_INVALID_KEY;
    }

    memset(&req, 0, sizeof(req));
    reqp->api_key = key;
    reqp->base_url = (base_url && *base_url) ? base_url : p->base_url;
    if (options) {
        reqp->period_start = options->period_start;
        reqp->period_end = options->period_end;
        reqp->headers = options->headers;
        reqp->header_count = options->headers ? options->header_count : 0;
        reqp->method = options->method;
        reqp->body = options->body;
        reqp->body_len = options->body ? options->body_len : 0;
    }

    /*
     * 结果由调用方持有，但在本函数分配：这样驱动失败时不会把填写了一半的
     * 结果泄漏给调用方。
     */
    *out = tokenprobe_result_create();
    if (!*out) {
        tp_error_set(err, TOKENPROBE_ERR_OOM, "cannot allocate a result for %s", p->display_name);
        return TOKENPROBE_ERR_OOM;
    }

    tokenprobe_error_init(&scratch);
    st = p->probe(p, ctx, reqp, *out, err ? err : &scratch);
    if (st == TOKENPROBE_OK) {
        tp_result_set_observed_at(*out, tp_now_ms());
    } else if (*out) {
        tokenprobe_result_free(*out);
        *out = NULL;
    }
    return st;
}
