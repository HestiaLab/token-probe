# 传输层

传输层是 TokenProbe 里唯一接触操作系统和第三方库的地方。它被抽成一个接口
（`tokenprobe_transport_t`），因为这决定了整个项目的可测性和可嵌入性：
换掉传输层，就能让完整的栈——拼 URL、填认证头、判状态码、解 JSON、抽 metric——
对着录制的响应体跑一遍，不联网、不需要密钥、不花钱。

## 三个实现

| 实现 | 平台 | 外部依赖 | 说明 |
| --- | --- | --- | --- |
| `winhttp` | Windows | 无（OS 组件） | 默认选择。WinHTTP 不绑定用户配置或交互式会话，服务和计划任务里也能用；代理配置独立于进程内的 WinINet 状态。 |
| `openssl` | Linux/macOS | OpenSSL 1.1.1+ | 支持 `https://` 和 `http://`；明文 HTTP 仅用于回环测试，生产端点必须使用 HTTPS。 |
| `mock` | 全部 | 无 | 离线响应表。测试与 UI 回放使用。 |

`TOKENPROBE_HTTP_BACKEND` 选择后端，`auto` 在 Windows 取 `winhttp`、
其余取 `openssl`。`mock` 会构建一个没有平台传输的库：此时
`tokenprobe_transport_platform()` 返回 `NULL`，`tokenprobe_context_create()`
对未注入传输的调用失败，而注入了传输的调用完全不受影响。

## 证书校验不可关闭

探测请求会带上 API 密钥，因此中间人攻击等于凭据泄露。两个后端都强制校验：

- WinHTTP 使用系统信任库与吊销检查；
- OpenSSL 后端设置 `SSL_VERIFY_PEER`、加载系统默认信任路径、用
  `X509_VERIFY_PARAM_set1_host()` 做主机名校验，并设置 SNI。

没有任何选项可以绕过它。证书错误会返回 `TOKENPROBE_ERR_TLS`，
hint 会直接说明"校验永不关闭，请修信任库"。

## 两个后端的有意差异

WinHTTP 会透明处理分块传输和 gzip；OpenSSL 后端不会。与其掩盖这个差异，
不如把它显式化：

| 行为 | winhttp | openssl |
| --- | --- | --- |
| `Transfer-Encoding: chunked` | 由 WinHTTP 解码 | 不做解码，返回 `TOKENPROBE_ERR_ENCODING` |
| `Content-Encoding: gzip` | 由 WinHTTP 解压 | 不做解压，返回 `TOKENPROBE_ERR_ENCODING` |
| 连接复用 | WinHTTP 连接池 | 每次请求 `Connection: close`，读到 EOF |
| 响应大小上限 | 4 MiB | 4 MiB |

**为什么不解码，而是报错？** 静默返回被截断或被压缩的字节，会变成"解析失败"
或更糟——一个看起来合理的错误数值。明确报 `TOKENPROBE_ERR_ENCODING`
让调用方知道是编码问题，而不是数据问题。

驱动因此必须显式请求 `Accept-Encoding: identity`（DeepSeek 驱动就是这么做的）。
如果某个厂商坚持返回压缩响应，那就是需要引入 zlib 的信号——
届时应当同时给两个后端加解码，而不是只修一个。

## 超时与大小限制

- 默认每请求超时 `TOKENPROBE_DEFAULT_TIMEOUT_MS`（15 秒），
  由 `tokenprobe_context_options_t::timeout_ms` 覆盖。WinHTTP 后端通过
  `WinHttpSetTimeouts()` 应用到请求句柄，OpenSSL 后端通过
  `SO_RCVTIMEO`/`SO_SNDTIMEO` 落地；超时统一映射为 `TOKENPROBE_ERR_TIMEOUT`。
- 响应体超过 4 MiB 直接失败。探测结果的正常量级是几百字节，
  无上限缓冲只会让一个坏掉的端点拖垮进程。

## 自己实现一个传输

```c
typedef struct my_transport_impl {
    /* 宿主自己的 HTTP 客户端状态 */
} my_transport_impl;

static tokenprobe_status_t my_perform(void *impl,
                                      const tokenprobe_http_request_t *request,
                                      tokenprobe_http_response_t *response,
                                      tokenprobe_error_t *err)
{
    /* 填充 response->status / body / headers。
     * body 与 header 字符串必须用 malloc/realloc 分配：
     * tokenprobe_http_response_free() 用 free() 释放它们，
     * tokenprobe_strdup() 提供了一个合规的复制辅助函数。 */
    return TOKENPROBE_OK;
}

static void my_destroy(void *impl) { /* 释放 impl */ }
```

把 `perform`、`destroy`、`impl` 填进 `tokenprobe_transport_t`，
再赋给 `tokenprobe_context_options_t::transport` 即可。调用方注入的传输由调用方
负责销毁。平台传输层是进程级共享实例，context 只借用，不负责销毁；
`tokenprobe_context_destroy()` 只释放 context 自身的资源。

这一条也是"作为核心被别的项目集成"的落点——宿主不必接受本库的网络实现，
只要满足上面的契约就行。
