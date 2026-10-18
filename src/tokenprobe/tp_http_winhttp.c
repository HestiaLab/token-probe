/*
 * Windows 传输层：WinHTTP。
 *
 * 选择 WinHTTP 而非 WinINet 的原因：
 *   - 不依赖用户配置文件或交互式会话，因此可在服务或计划任务中运行；
 *   - 有独立的代理配置查询，不继承主机进程的 WinINet 状态。
 *
 * 有意接受的差异：
 *   - 证书校验和吊销检查交给操作系统；本库从不关闭校验——探测会发送 API key，
 *     MITM 就意味着凭据泄漏；
 *   - WinHTTP 会解码 chunked 传输编码，默认也会解 gzip，因此比 OpenSSL 后端
 *     更宽容。
 */
#if defined(_WIN32) && defined(TOKENPROBE_USE_WINHTTP)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "tp_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct winhttp_impl {
    HINTERNET session;
    char *user_agent;
} winhttp_impl;

static tokenprobe_status_t map_win32_error(DWORD code, tokenprobe_error_t *err, const char *what) {
    tokenprobe_status_t st;
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
        st = TOKENPROBE_ERR_TIMEOUT;
        break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        st = TOKENPROBE_ERR_DNS;
        break;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        st = TOKENPROBE_ERR_CONNECT;
        break;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        st = TOKENPROBE_ERR_TLS;
        break;
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        st = TOKENPROBE_ERR_INVALID_URL;
        break;
    default:
        st = TOKENPROBE_ERR_HTTP;
        break;
    }
    tp_error_set(err, st, "WinHTTP %s failed (error %lu)", what, (unsigned long)code);
    if (st == TOKENPROBE_ERR_TLS) {
        tp_error_set_hint(err, "certificate validation failed; a probe must not be run with "
                               "verification disabled");
    } else if (st == TOKENPROBE_ERR_DNS || st == TOKENPROBE_ERR_CONNECT) {
        tp_error_set_hint(err, "check network connectivity and any proxy configuration");
    }
    return st;
}

/* 读取完整响应体。WinHttpReadData 会自动处理 chunked 响应体。 */
static tokenprobe_status_t read_body(HINTERNET request, tokenprobe_http_response_t *resp,
                                     tokenprobe_error_t *err) {
    tp_strbuf sb;
    BYTE chunk[8192];

    tp_strbuf_init(&sb);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request, chunk, (DWORD)sizeof(chunk), &got)) {
            DWORD code = GetLastError();
            tokenprobe_status_t st = map_win32_error(code, err, "WinHttpReadData");
            tp_strbuf_free(&sb);
            return st;
        }
        if (got == 0) {
            break;
        }
        if (tp_strbuf_put(&sb, (const char *)chunk, got) != 0) {
            tp_strbuf_free(&sb);
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory reading response body");
            return TOKENPROBE_ERR_OOM;
        }
        /* 拒绝缓冲无上限的响应体：探测结果不会这么大。 */
        if (sb.len > (size_t)4 * 1024 * 1024) {
            tp_strbuf_free(&sb);
            tp_error_set(err, TOKENPROBE_ERR_HTTP, "response body exceeds the 4 MiB safety limit");
            return TOKENPROBE_ERR_HTTP;
        }
    }

    resp->body = sb.data;
    resp->body_len = sb.len;
    if (!resp->body) {
        resp->body = tp_dup("");
        if (!resp->body) {
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory");
            return TOKENPROBE_ERR_OOM;
        }
    }
    return TOKENPROBE_OK;
}

static void parse_raw_headers(const wchar_t *raw, tokenprobe_http_response_t *resp) {
    char *utf8;
    char *cursor;
    char **names = NULL;
    char **values = NULL;
    size_t count = 0;
    size_t cap = 0;

    if (!raw) {
        return;
    }
    utf8 = tp_win32_wide_to_utf8(raw);
    if (!utf8) {
        return;
    }

    cursor = utf8;
    while (*cursor) {
        char *line_end = strstr(cursor, "\r\n");
        char *colon;
        if (!line_end) {
            break;
        }
        *line_end = '\0';
        /* 第一行是状态行，不能当作响应头处理。 */
        if (cursor == utf8) {
            cursor = line_end + 2;
            continue;
        }
        colon = strchr(cursor, ':');
        if (colon) {
            char *value = colon + 1;
            size_t name_len;
            *colon = '\0';
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            name_len = strlen(cursor);
            while (name_len > 0 && (cursor[name_len - 1] == ' ' || cursor[name_len - 1] == '\t')) {
                cursor[--name_len] = '\0';
            }
            if (name_len > 0) {
                if (count == cap) {
                    size_t new_cap = cap ? cap * 2 : 16;
                    char **n = (char **)realloc(names, new_cap * sizeof(*n));
                    char **v = (char **)realloc(values, new_cap * sizeof(*v));
                    if (!n || !v) {
                        free(n ? n : names);
                        free(v ? v : values);
                        free(utf8);
                        return;
                    }
                    names = n;
                    values = v;
                    cap = new_cap;
                }
                names[count] = tp_dup(cursor);
                values[count] = tp_dup(value);
                if (!names[count] || !values[count]) {
                    free(utf8);
                    goto fail_cleanup;
                }
                count++;
            }
        }
        cursor = line_end + 2;
    }

    free(utf8);
    if (count) {
        size_t i;
        resp->headers = (tokenprobe_http_header_t *)calloc(count, sizeof(tokenprobe_http_header_t));
        if (!resp->headers) {
            goto fail_cleanup;
        }
        for (i = 0; i < count; i++) {
            resp->headers[i].name = names[i];
            resp->headers[i].value = values[i];
        }
        resp->header_count = count;
    }
    free(names);
    free(values);
    return;

fail_cleanup: {
    size_t i;
    for (i = 0; i < count; i++) {
        free(names ? names[i] : NULL);
        free(values ? values[i] : NULL);
    }
    free(names);
    free(values);
}
}

static tokenprobe_status_t winhttp_perform(void *impl, const tokenprobe_http_request_t *req,
                                           tokenprobe_http_response_t *resp,
                                           tokenprobe_error_t *err) {
    winhttp_impl *w = (winhttp_impl *)impl;
    tp_arena *arena = NULL;
    tp_url url;
    wchar_t *wpath = NULL;
    wchar_t *whost = NULL;
    wchar_t *wverb = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    tokenprobe_status_t st = TOKENPROBE_OK;

    if (!w || !req || !resp || !req->url) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG, "winhttp: invalid argument");
        return TOKENPROBE_ERR_INVALID_ARG;
    }

    arena = tp_arena_create(1024);
    if (!arena) {
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory");
        return TOKENPROBE_ERR_OOM;
    }
    st = tp_url_parse_into(arena, req->url, &url, err);
    if (st != TOKENPROBE_OK) {
        goto done;
    }

    whost = tp_win32_utf8_to_wide(url.host);
    wverb = tp_win32_utf8_to_wide(req->method ? req->method : "GET");
    if (!whost || !wverb) {
        st = TOKENPROBE_ERR_OOM;
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory converting request to UTF-16");
        goto done;
    }
    {
        tp_strbuf path;
        tp_strbuf_init(&path);
        tp_strbuf_puts(&path, url.path);
        if (url.query) {
            tp_strbuf_putc(&path, '?');
            tp_strbuf_puts(&path, url.query);
        }
        if (!path.data) {
            tp_strbuf_free(&path);
            st = TOKENPROBE_ERR_OOM;
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building request path");
            goto done;
        }
        wpath = tp_win32_utf8_to_wide(path.data);
        tp_strbuf_free(&path);
        if (!wpath) {
            st = TOKENPROBE_ERR_OOM;
            tp_error_set(err, TOKENPROBE_ERR_OOM,
                         "out of memory converting request path to UTF-16");
            goto done;
        }
    }

    connect = WinHttpConnect(w->session, whost, (INTERNET_PORT)tp_url_effective_port(&url), 0);
    if (!connect) {
        st = map_win32_error(GetLastError(), err, "WinHttpConnect");
        goto done;
    }

    request =
        WinHttpOpenRequest(connect, wverb, wpath, NULL, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, url.is_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        st = map_win32_error(GetLastError(), err, "WinHttpOpenRequest");
        goto done;
    }

    {
        int timeout = req->timeout_ms > 0 ? req->timeout_ms : TOKENPROBE_DEFAULT_TIMEOUT_MS;
        /* WinHTTP 的超时按句柄生效；未设置时会一直等到系统默认上限。 */
        if (!WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout)) {
            st = map_win32_error(GetLastError(), err, "WinHttpSetTimeouts");
            goto done;
        }
    }

    if (req->header_count) {
        tp_strbuf hdrs;
        size_t i;
        int ok;
        tp_strbuf_init(&hdrs);
        for (i = 0; i < req->header_count; i++) {
            if (!req->headers[i].name) {
                continue;
            }
            tp_strbuf_printf(&hdrs, "%s: %s\r\n", req->headers[i].name,
                             req->headers[i].value ? req->headers[i].value : "");
        }
        if (!hdrs.data) {
            tp_strbuf_free(&hdrs);
            st = TOKENPROBE_ERR_OOM;
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building request headers");
            goto done;
        }
        {
            wchar_t *whdrs = tp_win32_utf8_to_wide(hdrs.data);
            tp_strbuf_free(&hdrs);
            if (!whdrs) {
                st = TOKENPROBE_ERR_OOM;
                tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory converting headers to UTF-16");
                goto done;
            }
            ok = WinHttpAddRequestHeaders(request, whdrs, (DWORD)-1L,
                                          WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            free(whdrs);
        }
        if (!ok) {
            st = map_win32_error(GetLastError(), err, "WinHttpAddRequestHeaders");
            goto done;
        }
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)(req->body ? req->body : ""), (DWORD)req->body_len,
                            (DWORD)req->body_len, 0)) {
        st = map_win32_error(GetLastError(), err, "WinHttpSendRequest");
        goto done;
    }

    if (!WinHttpReceiveResponse(request, NULL)) {
        st = map_win32_error(GetLastError(), err, "WinHttpReceiveResponse");
        goto done;
    }

    {
        DWORD status = 0;
        DWORD len = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                 WINHTTP_NO_HEADER_INDEX)) {
            st = map_win32_error(GetLastError(), err, "WinHttpQueryHeaders(status)");
            goto done;
        }
        resp->status = (long)status;
    }

    /*
     * 一次取回全部响应头。WinHTTP 也提供按名称查询限流头的标志，但那些头是
     * 供应商相关的；统一取原始响应头可以让驱动代码与供应商无关。
     */
    {
        DWORD needed = 0;
        WCHAR *raw = NULL;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            NULL, &needed, WINHTTP_NO_HEADER_INDEX);
        if (needed > 0) {
            raw = (WCHAR *)malloc(needed);
            if (raw) {
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                        WINHTTP_HEADER_NAME_BY_INDEX, raw, &needed,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    parse_raw_headers(raw, resp);
                }
                free(raw);
            }
        }
    }

    st = read_body(request, resp, err);

done:
    if (request) {
        WinHttpCloseHandle(request);
    }
    if (connect) {
        WinHttpCloseHandle(connect);
    }
    free(whost);
    free(wpath);
    free(wverb);
    tp_arena_destroy(arena);
    return st;
}

/*
 * 平台传输层是进程级共享实例：会话只创建一次，之后不再修改。
 * 按 WinHTTP 文档，会话句柄可被多个线程并发使用；每次请求使用独立的
 * connect/request 句柄。context 只借用它，不负责销毁。
 */
static INIT_ONCE g_winhttp_once = INIT_ONCE_STATIC_INIT;
static winhttp_impl *g_winhttp_shared = NULL;
static tokenprobe_transport_t g_winhttp_transport = {"winhttp", winhttp_perform, NULL, NULL};

static BOOL CALLBACK winhttp_init_once(PINIT_ONCE once, PVOID param, PVOID *context) {
    winhttp_impl *shared;
    DWORD access_type = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;

    (void)once;
    (void)param;
    (void)context;

    shared = (winhttp_impl *)calloc(1, sizeof(*shared));
    if (!shared) {
        return FALSE;
    }
    shared->user_agent = tp_dup("tokenprobe/" TOKENPROBE_VERSION_STRING);
#if defined(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY)
    access_type = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#endif
    {
        /* WinHttpOpen 需要 UTF-16 字符串；转换结果在本作用域内立即释放。 */
        wchar_t *wide_agent = shared->user_agent ? tp_win32_utf8_to_wide(shared->user_agent) : NULL;
        shared->session =
            WinHttpOpen(wide_agent, access_type, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        free(wide_agent);
    }
    if (!shared->session) {
        free(shared->user_agent);
        free(shared);
        return FALSE;
    }

    g_winhttp_shared = shared;
    g_winhttp_transport.impl = shared;
    return TRUE;
}

const tokenprobe_transport_t *tokenprobe_transport_platform(void) {
    if (!InitOnceExecuteOnce(&g_winhttp_once, winhttp_init_once, NULL, NULL)) {
        return NULL;
    }
    return g_winhttp_shared ? &g_winhttp_transport : NULL;
}

#endif /* _WIN32 && TOKENPROBE_USE_WINHTTP */
