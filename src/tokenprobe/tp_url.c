/*
 * 最小化的绝对 URL 解析器，只覆盖传输层需要的子集：
 * scheme://host[:port]/path[?query][#fragment]
 *
 * 有意不支持：authority 中的用户信息、非层级 scheme 和 IDN。无法完全理解的 URL
 * 一律拒绝，不做猜测——猜错可能把 API key 发到错误的主机。
 */
#include "tp_internal.h"

#include <string.h>

static int scheme_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' ||
           c == '-' || c == '.';
}

tokenprobe_status_t tp_url_parse_into(tp_arena *a, const char *url, tp_url *out,
                                      tokenprobe_error_t *err) {
    const char *p;
    const char *scheme_end;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    size_t scheme_len;
    int port_explicit = 0;

    if (!url || !out) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL is empty");
        return TOKENPROBE_ERR_INVALID_URL;
    }
    memset(out, 0, sizeof(*out));

    scheme_end = strstr(url, "://");
    if (!scheme_end) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL '%s' has no scheme (expected https://)",
                     url);
        tp_error_set_hint(err, "Base URLs must be absolute, e.g. https://api.deepseek.com");
        return TOKENPROBE_ERR_INVALID_URL;
    }
    scheme_len = (size_t)(scheme_end - url);
    if (scheme_len == 0 || scheme_len >= sizeof(out->scheme)) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL scheme is missing or too long");
        return TOKENPROBE_ERR_INVALID_URL;
    }
    {
        size_t i;
        if (!((url[0] >= 'a' && url[0] <= 'z') || (url[0] >= 'A' && url[0] <= 'Z'))) {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL scheme must start with a letter");
            return TOKENPROBE_ERR_INVALID_URL;
        }
        for (i = 0; i < scheme_len; i++) {
            if (!scheme_char((unsigned char)url[i])) {
                tp_error_set(err, TOKENPROBE_ERR_INVALID_URL,
                             "URL scheme contains invalid characters");
                return TOKENPROBE_ERR_INVALID_URL;
            }
            out->scheme[i] = (char)tp_ascii_lower((unsigned char)url[i]);
        }
        out->scheme[i] = '\0';
    }

    if (!tp_str_ieq(out->scheme, "https") && !tp_str_ieq(out->scheme, "http")) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL,
                     "unsupported URL scheme '%s' (only https and http are supported)",
                     out->scheme);
        return TOKENPROBE_ERR_INVALID_URL;
    }
    out->is_secure = tp_str_ieq(out->scheme, "https") ? 1 : 0;

    host_start = scheme_end + 3;
    if (*host_start == '\0') {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL has no host");
        return TOKENPROBE_ERR_INVALID_URL;
    }
    if (*host_start == '[') {
        /* IPv6 字面量 */
        const char *close = strchr(host_start, ']');
        if (!close) {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "unterminated IPv6 literal in URL");
            return TOKENPROBE_ERR_INVALID_URL;
        }
        host_end = close + 1;
    } else {
        host_end = host_start;
        while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?' &&
               *host_end != '#') {
            host_end++;
        }
    }
    if (host_end == host_start) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL has an empty host");
        return TOKENPROBE_ERR_INVALID_URL;
    }
    if (memchr(host_start, '@', (size_t)(host_end - host_start)) != NULL) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL,
                     "userinfo in URLs is not supported; put credentials in the API key "
                     "or an Authorization header instead");
        return TOKENPROBE_ERR_INVALID_URL;
    }

    out->host = tp_arena_strndup(a, host_start, (size_t)(host_end - host_start));
    if (!out->host) {
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory parsing URL");
        return TOKENPROBE_ERR_OOM;
    }

    p = host_end;
    if (*p == ':') {
        const char *port_start = ++p;
        long parsed = 0;
        while (*p >= '0' && *p <= '9') {
            p++;
        }
        if (p == port_start) {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL has ':' but no port number");
            return TOKENPROBE_ERR_INVALID_URL;
        }
        if (tp_parse_int(port_start, (size_t)(p - port_start), &parsed) != 0 || parsed <= 0 ||
            parsed > 65535) {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "URL port is out of range");
            return TOKENPROBE_ERR_INVALID_URL;
        }
        out->port = (int)parsed;
        port_explicit = 1;
    }

    if (*p == '\0' || *p == '/' || *p == '?' || *p == '#') {
        const char *hash;
        size_t path_len;

        path_start = (*p == '/') ? p : NULL;
        if (path_start) {
            const char *path_end = path_start;
            while (*path_end && *path_end != '?' && *path_end != '#') {
                path_end++;
            }
            path_len = (size_t)(path_end - path_start);
            out->path = tp_arena_strndup(a, path_start, path_len);
            if (!out->path) {
                tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory parsing URL");
                return TOKENPROBE_ERR_OOM;
            }
            p = path_end;
        }

        if (*p == '?') {
            const char *q_end = ++p;
            while (*q_end && *q_end != '#') {
                q_end++;
            }
            if (q_end != p) {
                out->query = tp_arena_strndup(a, p, (size_t)(q_end - p));
                if (!out->query) {
                    tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory parsing URL");
                    return TOKENPROBE_ERR_OOM;
                }
            }
            p = q_end;
        }

        hash = strchr(p, '#');
        if (hash == p) {
            p = hash + 1;
            while (*p) {
                p++;
            }
        }

        if (*p != '\0') {
            tp_error_set(err, TOKENPROBE_ERR_INVALID_URL, "unexpected characters after URL query");
            return TOKENPROBE_ERR_INVALID_URL;
        }
    } else {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_URL,
                     "URL has unexpected characters after the host");
        return TOKENPROBE_ERR_INVALID_URL;
    }

    if (!out->path) {
        out->path = tp_arena_strdup(a, "/");
        if (!out->path) {
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory parsing URL");
            return TOKENPROBE_ERR_OOM;
        }
    }

    if (!port_explicit) {
        out->port = out->is_secure ? 443 : 80;
    }
    return TOKENPROBE_OK;
}

int tp_url_effective_port(const tp_url *u) {
    if (!u) {
        return 0;
    }
    if (u->port > 0) {
        return u->port;
    }
    return u->is_secure ? 443 : 80;
}
