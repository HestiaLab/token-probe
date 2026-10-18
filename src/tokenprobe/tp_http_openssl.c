/*
 * POSIX 传输层：BSD socket + OpenSSL。
 *
 * 这是 TokenProbe 唯一依赖第三方库的地方，且只在构建期依赖：没有任何 OpenSSL
 * 类型或头文件进入公共 ABI。
 *
 * 与 WinHTTP 后端的有意差异：
 *   - 请求总是发送 "Connection: close"，响应体读到 EOF，因此没有连接池，
 *     也不会有失效的 keep-alive 状态；
 *   - 不解码 Transfer-Encoding: chunked 和 Content-Encoding: gzip。
 *     我们请求 Accept-Encoding: identity，其他编码会明确返回
 *     TOKENPROBE_ERR_ENCODING，而不是返回一个被破坏的数字。
 * 这两点是有意保留的限制，不是疏漏。
 */
#if !defined(_WIN32) && defined(TOKENPROBE_USE_OPENSSL)

#include "tp_internal.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

typedef struct openssl_impl {
    SSL_CTX *ctx;
} openssl_impl;

static int g_openssl_init_done = 0;

static void ensure_openssl_init(void) {
    if (!g_openssl_init_done) {
        /*
         * OpenSSL 1.1.0 及以上会自动初始化；这里显式调用无害，
         * 同时兼容仍在 LTS 发行版中的旧版本。
         */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
#endif
        g_openssl_init_done = 1;
    }
}

static tokenprobe_status_t openssl_error(tokenprobe_status_t fallback, tokenprobe_error_t *err,
                                         const char *what) {
    unsigned long code = ERR_get_error();
    char buf[256];
    if (code) {
        ERR_error_string_n(code, buf, sizeof(buf));
        tp_error_set(err, fallback, "%s failed: %s", what, buf);
    } else {
        tp_error_set(err, fallback, "%s failed", what);
    }
    return fallback;
}

static tokenprobe_status_t connect_tcp(const tp_url *url, int timeout_ms, int *out_fd,
                                       tokenprobe_error_t *err) {
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    char port[16];
    int rc;
    int fd = -1;

    snprintf(port, sizeof(port), "%d", tp_url_effective_port(url));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(url->host, port, &hints, &list);
    if (rc != 0 || !list) {
        tp_error_set(err, TOKENPROBE_ERR_DNS, "cannot resolve '%s': %s", url->host,
                     gai_strerror(rc));
        tp_error_set_hint(err, "check the host name and DNS configuration");
        return TOKENPROBE_ERR_DNS;
    }

    {
        struct addrinfo *ai;
        for (ai = list; ai; ai = ai->ai_next) {
            fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                break;
            }
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(list);

    if (fd < 0) {
        tp_error_set(err, TOKENPROBE_ERR_CONNECT, "cannot connect to %s:%s", url->host, port);
        tp_error_set_hint(err, "check network connectivity, firewall and proxy settings");
        return TOKENPROBE_ERR_CONNECT;
    }

    {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    *out_fd = fd;
    return TOKENPROBE_OK;
}

static tokenprobe_status_t do_handshake(SSL *ssl, const tp_url *url, int fd,
                                        tokenprobe_error_t *err) {
    int rc;

    if (url->is_secure) {
        /* 现代服务端都要求 SNI；缺少它时，共享入口后面的主机会握手失败。 */
        if (!SSL_set_tlsext_host_name(ssl, url->host)) {
            return openssl_error(TOKENPROBE_ERR_TLS, err, "SSL_set_tlsext_host_name");
        }
        /* 主机名校验不可省略：请求中包含 API key。 */
        if (X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), url->host, 0) != 1) {
            return openssl_error(TOKENPROBE_ERR_TLS, err, "X509_VERIFY_PARAM_set1_host");
        }
    }

    if (SSL_set_fd(ssl, fd) != 1) {
        return openssl_error(TOKENPROBE_ERR_TLS, err, "SSL_set_fd");
    }

    rc = SSL_connect(ssl);
    if (rc != 1) {
        long verify = SSL_get_verify_result(ssl);
        if (verify != (long)X509_V_OK) {
            tp_error_set(err, TOKENPROBE_ERR_TLS, "certificate verification failed for %s: %s",
                         url->host, X509_verify_cert_error_string(verify));
            tp_error_set_hint(err, "verification is never disabled by tokenprobe; fix the trust "
                                   "store instead");
            return TOKENPROBE_ERR_TLS;
        }
        return openssl_error(TOKENPROBE_ERR_TLS, err, "SSL_connect");
    }
    return TOKENPROBE_OK;
}

static tokenprobe_status_t build_request_text(const tokenprobe_http_request_t *req,
                                              const tp_url *url, tp_strbuf *sb,
                                              tokenprobe_error_t *err) {
    size_t i;
    int has_content_length = 0;

    if (tp_strbuf_printf(sb, "%s %s", req->method ? req->method : "GET", url->path) != 0) {
        goto oom;
    }
    if (url->query && tp_strbuf_printf(sb, "?%s", url->query) != 0) {
        goto oom;
    }
    if (tp_strbuf_puts(sb, " HTTP/1.1\r\nHost: ") != 0) {
        goto oom;
    }
    if (tp_strbuf_puts(sb, url->host) != 0) {
        goto oom;
    }
    if (url->port != (url->is_secure ? 443 : 80)) {
        if (tp_strbuf_printf(sb, ":%d", url->port) != 0) {
            goto oom;
        }
    }
    if (tp_strbuf_puts(sb, "\r\n") != 0) {
        goto oom;
    }

    for (i = 0; i < req->header_count; i++) {
        if (!req->headers[i].name) {
            continue;
        }
        /* 调用方自带的 Host 会与请求行不一致，因此忽略。 */
        if (tp_str_ieq(req->headers[i].name, "Host")) {
            continue;
        }
        if (tp_str_ieq(req->headers[i].name, "Content-Length")) {
            has_content_length = 1;
        }
        if (tp_strbuf_printf(sb, "%s: %s\r\n", req->headers[i].name,
                             req->headers[i].value ? req->headers[i].value : "") != 0) {
            goto oom;
        }
    }
    if (req->body_len > 0 && !has_content_length) {
        if (tp_strbuf_printf(sb, "Content-Length: %zu\r\n", req->body_len) != 0) {
            goto oom;
        }
    }
    /* 每次探测使用一个连接，参见文件头注释。 */
    if (tp_strbuf_puts(sb, "Connection: close\r\n\r\n") != 0) {
        goto oom;
    }
    return TOKENPROBE_OK;

oom:
    tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory building the HTTP request");
    return TOKENPROBE_ERR_OOM;
}

static tokenprobe_status_t write_all(SSL *ssl, const char *data, size_t len,
                                     tokenprobe_error_t *err) {
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(ssl, data + off, (int)(len - off));
        if (n <= 0) {
            int ssl_err = SSL_get_error(ssl, n);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            if (ssl_err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                tp_error_set(err, TOKENPROBE_ERR_TIMEOUT, "timed out writing the HTTP request");
                return TOKENPROBE_ERR_TIMEOUT;
            }
            return openssl_error(TOKENPROBE_ERR_TLS, err, "SSL_write");
        }
        off += (size_t)n;
    }
    return TOKENPROBE_OK;
}

static tokenprobe_status_t read_all(SSL *ssl, tp_strbuf *sb, tokenprobe_error_t *err) {
    char buf[8192];
    for (;;) {
        int n = SSL_read(ssl, buf, (int)sizeof(buf));
        if (n > 0) {
            if (tp_strbuf_put(sb, buf, (size_t)n) != 0) {
                tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory reading the response");
                return TOKENPROBE_ERR_OOM;
            }
            if (sb->len > (size_t)4 * 1024 * 1024) {
                tp_error_set(err, TOKENPROBE_ERR_HTTP,
                             "response body exceeds the 4 MiB safety limit");
                return TOKENPROBE_ERR_HTTP;
            }
            continue;
        }
        {
            int ssl_err = SSL_get_error(ssl, n);
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                break; /* clean TLS shutdown */
            }
            if (ssl_err == SSL_ERROR_SYSCALL) {
                if (n == 0) {
                    break; /* EOF without close_notify; common, treat as end */
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    tp_error_set(err, TOKENPROBE_ERR_TIMEOUT, "timed out reading the response");
                    return TOKENPROBE_ERR_TIMEOUT;
                }
                tp_error_set(err, TOKENPROBE_ERR_CONNECT, "connection error while reading: %s",
                             strerror(errno));
                return TOKENPROBE_ERR_CONNECT;
            }
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return openssl_error(TOKENPROBE_ERR_TLS, err, "SSL_read");
        }
    }
    return TOKENPROBE_OK;
}

static tokenprobe_status_t write_all_plain(int fd, const char *data, size_t len,
                                           tokenprobe_error_t *err) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                tp_error_set(err, TOKENPROBE_ERR_TIMEOUT, "timed out writing the HTTP request");
                return TOKENPROBE_ERR_TIMEOUT;
            }
            tp_error_set(err, TOKENPROBE_ERR_CONNECT, "connection error while writing: %s",
                         strerror(errno));
            return TOKENPROBE_ERR_CONNECT;
        }
        if (n == 0) {
            tp_error_set(err, TOKENPROBE_ERR_CONNECT, "connection closed while writing");
            return TOKENPROBE_ERR_CONNECT;
        }
        off += (size_t)n;
    }
    return TOKENPROBE_OK;
}

static tokenprobe_status_t read_all_plain(int fd, tp_strbuf *sb, tokenprobe_error_t *err) {
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (tp_strbuf_put(sb, buf, (size_t)n) != 0) {
                tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory reading the response");
                return TOKENPROBE_ERR_OOM;
            }
            if (sb->len > (size_t)4 * 1024 * 1024) {
                tp_error_set(err, TOKENPROBE_ERR_HTTP,
                             "response body exceeds the 4 MiB safety limit");
                return TOKENPROBE_ERR_HTTP;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            tp_error_set(err, TOKENPROBE_ERR_TIMEOUT, "timed out reading the response");
            return TOKENPROBE_ERR_TIMEOUT;
        }
        tp_error_set(err, TOKENPROBE_ERR_CONNECT, "connection error while reading: %s",
                     strerror(errno));
        return TOKENPROBE_ERR_CONNECT;
    }
    return TOKENPROBE_OK;
}

static void append_header(tokenprobe_http_response_t *resp, const char *name, size_t name_len,
                          const char *value, size_t value_len) {
    tokenprobe_http_header_t *grown;
    tokenprobe_http_header_t *h;
    char *name_copy;
    char *value_copy;

    if (name_len == 0) {
        return;
    }
    grown = (tokenprobe_http_header_t *)realloc(
        resp->headers, (resp->header_count + 1) * sizeof(tokenprobe_http_header_t));
    if (!grown) {
        return;
    }
    resp->headers = grown;

    name_copy = tp_dup_range(name, name_len);
    value_copy = tp_dup_range(value, value_len);
    if (!name_copy || !value_copy) {
        free(name_copy);
        free(value_copy);
        return;
    }
    h = &resp->headers[resp->header_count];
    h->name = name_copy;
    h->value = value_copy;
    resp->header_count++;
}

static tokenprobe_status_t parse_response(tp_strbuf *raw, tokenprobe_http_response_t *resp,
                                          tokenprobe_error_t *err) {
    char *buf = raw->data;
    char *line_end;
    size_t header_bytes;

    if (!buf) {
        tp_error_set(err, TOKENPROBE_ERR_HTTP, "empty response from server");
        return TOKENPROBE_ERR_HTTP;
    }

    /* 状态行，例如 "HTTP/1.1 200 OK" */
    line_end = strstr(buf, "\r\n");
    if (!line_end) {
        tp_error_set(err, TOKENPROBE_ERR_HTTP, "response has no complete status line");
        return TOKENPROBE_ERR_HTTP;
    }
    {
        char *sp = strchr(buf, ' ');
        long status = 0;
        if (!sp) {
            tp_error_set(err, TOKENPROBE_ERR_HTTP, "response status line is malformed");
            return TOKENPROBE_ERR_HTTP;
        }
        sp++;
        if (tp_parse_int(sp, 3, &status) != 0) {
            tp_error_set(err, TOKENPROBE_ERR_HTTP, "response status code is malformed");
            return TOKENPROBE_ERR_HTTP;
        }
        resp->status = status;
    }

    {
        char *cursor = line_end + 2;
        for (;;) {
            char *colon;
            char *next = strstr(cursor, "\r\n");
            if (!next) {
                tp_error_set(err, TOKENPROBE_ERR_HTTP, "response headers are truncated");
                return TOKENPROBE_ERR_HTTP;
            }
            if (next == cursor) {
                /* 空行表示响应头结束 */
                cursor = next + 2;
                break;
            }
            colon = memchr(cursor, ':', (size_t)(next - cursor));
            if (colon) {
                char *value = colon + 1;
                size_t name_len = (size_t)(colon - cursor);
                size_t value_len;

                while (name_len > 0 &&
                       (cursor[name_len - 1] == ' ' || cursor[name_len - 1] == '\t')) {
                    name_len--;
                }
                while (value < next && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                value_len = (size_t)(next - value);
                while (value_len > 0 &&
                       (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) {
                    value_len--;
                }
                append_header(resp, cursor, name_len, value, value_len);
            }
            cursor = next + 2;
        }
        header_bytes = (size_t)(cursor - buf);
    }

    /*
     * 报文边界。我们总是请求 identity + close，因此只可能出现
     * Content-Length 或读到 EOF 两种情况。
     */
    {
        const char *te = tokenprobe_http_header_get(resp, "Transfer-Encoding");
        const char *ce = tokenprobe_http_header_get(resp, "Content-Encoding");
        if (te && strstr(te, "chunked")) {
            tp_error_set(err, TOKENPROBE_ERR_ENCODING,
                         "server used Transfer-Encoding: chunked, which this build does not "
                         "decode");
            tp_error_set_hint(err, "rebuild against a transport that supports chunked framing, or "
                                   "report this provider/endpoint so a decoder can be added");
            return TOKENPROBE_ERR_ENCODING;
        }
        if (ce && !tp_str_ieq(ce, "identity")) {
            tp_error_set(err, TOKENPROBE_ERR_ENCODING,
                         "server used Content-Encoding: %s; only identity is decoded", ce);
            tp_error_set_hint(err, "the request asked for identity, so this is unexpected");
            return TOKENPROBE_ERR_ENCODING;
        }
    }

    resp->body_len = raw->len > header_bytes ? raw->len - header_bytes : 0;
    if (resp->body_len) {
        resp->body = tp_dup_range(buf + header_bytes, resp->body_len);
        if (!resp->body) {
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory storing the response body");
            return TOKENPROBE_ERR_OOM;
        }
    } else {
        resp->body = tp_dup("");
        if (!resp->body) {
            tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory");
            return TOKENPROBE_ERR_OOM;
        }
    }
    return TOKENPROBE_OK;
}

static tokenprobe_status_t openssl_perform(void *impl, const tokenprobe_http_request_t *req,
                                           tokenprobe_http_response_t *resp,
                                           tokenprobe_error_t *err) {
    openssl_impl *w = (openssl_impl *)impl;
    tp_arena *arena = NULL;
    tp_url url;
    int fd = -1;
    SSL *ssl = NULL;
    tp_strbuf raw;
    tp_strbuf head;
    tokenprobe_status_t st = TOKENPROBE_OK;

    if (!w || !req || !resp || !req->url) {
        tp_error_set(err, TOKENPROBE_ERR_INVALID_ARG, "openssl: invalid argument");
        return TOKENPROBE_ERR_INVALID_ARG;
    }

    tp_strbuf_init(&raw);
    tp_strbuf_init(&head);
    arena = tp_arena_create(1024);
    if (!arena) {
        tp_error_set(err, TOKENPROBE_ERR_OOM, "out of memory");
        return TOKENPROBE_ERR_OOM;
    }

    st = tp_url_parse_into(arena, req->url, &url, err);
    if (st != TOKENPROBE_OK) {
        goto done;
    }
    st = connect_tcp(&url, req->timeout_ms > 0 ? req->timeout_ms : TOKENPROBE_DEFAULT_TIMEOUT_MS,
                     &fd, err);
    if (st != TOKENPROBE_OK) {
        goto done;
    }

    if (url.is_secure) {
        ssl = SSL_new(w->ctx);
        if (!ssl) {
            st = openssl_error(TOKENPROBE_ERR_INTERNAL, err, "SSL_new");
            goto done;
        }
        st = do_handshake(ssl, &url, fd, err);
        if (st != TOKENPROBE_OK) {
            goto done;
        }
    }

    st = build_request_text(req, &url, &head, err);
    if (st != TOKENPROBE_OK) {
        goto done;
    }
    if (ssl) {
        st = write_all(ssl, head.data, head.len, err);
    } else {
        st = write_all_plain(fd, head.data, head.len, err);
    }
    if (st != TOKENPROBE_OK) {
        goto done;
    }
    if (req->body && req->body_len) {
        if (ssl) {
            st = write_all(ssl, req->body, req->body_len, err);
        } else {
            st = write_all_plain(fd, req->body, req->body_len, err);
        }
        if (st != TOKENPROBE_OK) {
            goto done;
        }
    }

    if (ssl) {
        st = read_all(ssl, &raw, err);
    } else {
        st = read_all_plain(fd, &raw, err);
    }
    if (st != TOKENPROBE_OK) {
        goto done;
    }
    st = parse_response(&raw, resp, err);

done:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (fd >= 0) {
        close(fd);
    }
    tp_strbuf_free(&raw);
    tp_strbuf_free(&head);
    tp_arena_destroy(arena);
    return st;
}

/*
 * 平台传输层是进程级共享实例：SSL_CTX 只创建一次，之后只读使用。
 * OpenSSL 1.1+ 允许同一个 SSL_CTX 被多个线程并发创建 SSL 对象；
 * context 只借用它，不负责销毁。
 */
static pthread_once_t g_openssl_once = PTHREAD_ONCE_INIT;
static openssl_impl *g_openssl_shared = NULL;
static tokenprobe_transport_t g_openssl_transport = {"openssl", openssl_perform, NULL, NULL};

static void openssl_init_once(void) {
    openssl_impl *shared;

    ensure_openssl_init();
    shared = (openssl_impl *)calloc(1, sizeof(*shared));
    if (!shared) {
        return;
    }
    shared->ctx = SSL_CTX_new(TLS_client_method());
    if (!shared->ctx) {
        free(shared);
        return;
    }
    SSL_CTX_set_min_proto_version(shared->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(shared->ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(shared->ctx) != 1) {
        /*
         * 容器中没有信任库是真实存在的部署情况。此时让传输层失败，
         * 而不是悄悄降低校验强度。
         */
        SSL_CTX_free(shared->ctx);
        free(shared);
        return;
    }

    g_openssl_shared = shared;
    g_openssl_transport.impl = shared;
}

const tokenprobe_transport_t *tokenprobe_transport_platform(void) {
    if (pthread_once(&g_openssl_once, openssl_init_once) != 0) {
        return NULL;
    }
    return g_openssl_shared ? &g_openssl_transport : NULL;
}

#endif /* !_WIN32 && TOKENPROBE_USE_OPENSSL */
