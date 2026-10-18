# 新增一个厂商驱动

一个厂商驱动只做一件事：把某家厂商某个端点的响应，翻译成
`tokenprobe_metric_t` 记录。它不碰 socket、不碰 TLS、不解析 URL 字符串拼接——
那些属于传输层。

## 步骤

1. 先**用 `curl` 核实端点**，再写代码。记录真实的 URL、认证头、状态码和响应体。
   凭印象写驱动是这一层最常见的缺陷来源。
2. 在 `src/tokenprobe/tp_provider_<id>.c` 中实现 `probe` 回调。
3. 在该文件底部定义 `const tokenprobe_provider_t tp_provider_<id>`。
4. 在 `src/tokenprobe/tp_ctx.c` 里 `extern` 声明并加入 `k_providers` 注册表。
5. 在 `cmake/Sources.cmake` 的 `TOKENPROBE_CORE_SOURCES` 中登记源文件。
6. 在 `tests/` 下加一组离线测试（见下）。

## 驱动骨架

```c
static tokenprobe_status_t acme_probe(const tokenprobe_provider_t *self,
                                      tokenprobe_context_t *ctx,
                                      const tokenprobe_provider_request_t *req,
                                      tokenprobe_result_t *out,
                                      tokenprobe_error_t *err)
{
    tp_arena *arena = tp_arena_create(1024);
    tp_url    url;
    /* ... 构造请求、调用 tp_probe_http()、解析 JSON ... */
}
```

`tp_probe_http()` / `tp_probe_http_ex()` 是驱动与传输层之间唯一的接触点，
签名在 `tp_internal.h`。需要 POST 或请求体时使用 `tp_probe_http_ex()`。

## 时间范围与附加请求

`tokenprobe_probe_ex()` 会把 `tokenprobe_probe_options_t` 的内容传给驱动：

- `period_start` / `period_end`：周期支出的时间范围；
- `headers` / `header_count`：附加请求头；
- `method` / `body` / `body_len`：需要真实推理请求才能观察 quota 时使用。

驱动应按自身端点决定是否使用这些字段：例如 DeepSeek 余额接口固定为 GET，
只合并附加请求头；支持支出的驱动应读取时间范围并拼入查询参数。

## 必须遵守的契约

- **声明的能力必须是真的。** `supports_credit` / `supports_spend` /
  `supports_quota` 为 0 时，驱动不得产生对应种类的 metric。调用方靠这个区分
  "不支持"和"等于零"。
- **失败时不要留下半成品结果。** 校验完响应再写 metric；`tokenprobe_probe()`
  负责在失败时释放结果，但驱动自己创建的 arena、JSON 文档、HTTP 响应必须自己释放。
- **金额走 `tokenprobe_decimal_t`。** 用 `tp_json_field_decimal()` 读取；
  它同时接受 JSON 数字和十进制字符串。不要经 `double`。
- **多处失败要映射到语义状态码。** 用 `tp_status_from_http()` 处理状态码，
  并为 401 / 403 / 404 / 5xx 给出可操作的 hint。
- **请求 `Accept-Encoding: identity`。** OpenSSL 后端不解压 gzip，
  收到其它编码会明确报 `TOKENPROBE_ERR_ENCODING`，而不是返回被截断的数据。
- **不要拼接 URL 字符串。** 用 `tp_url_parse_into()` 解析 base URL，
  再基于它的 scheme/host/port 构造端点。

## 测试

驱动必须能在**不联网、不需要密钥**的情况下被测完。用内置的离线传输：

```c
tokenprobe_mock_entry_t entry;
memset(&entry, 0, sizeof(entry));
entry.url_prefix = "https://api.acme.com/balance";
entry.status     = 200;
entry.body       = k_balance_body;

tokenprobe_transport_t transport;
tokenprobe_transport_mock_create(&entry, 1, &transport);

tokenprobe_context_options_t options;
tokenprobe_context_options_init(&options);
options.transport = &transport;
tokenprobe_context_t *ctx = tokenprobe_context_create(&options);
```

至少覆盖：正常响应、401、5xx、JSON 截断、缺少必需字段、字段类型错误。
另外断言请求真的打到了预期端点，并检查 `Authorization` 头：

```c
TEST_ASSERT_EQUAL_STRING("https://api.acme.com/balance",
                         tokenprobe_mock_request_url(&transport, 0));
```

参考 `tests/test_provider_deepseek.c`。

## 认证方式

透传凭据的方式由驱动决定，写在 `Authorization` 或 `x-api-key` 头里。
`req->api_key` 由 `tokenprobe_probe()` 保证非空：它优先使用调用方显式传入的密钥，
否则读取 `p->env_var` 指向的环境变量。驱动不需要自己处理这个回退。

## 无法支持的端点

如果厂商没有公开某个量的 API，**不要伪造**。把对应的 `supports_*` 置 0，
在 `notes` 里写清楚原因，让调用方拿到 `TOKENPROBE_ERR_UNSUPPORTED`。
DeepSeek 的 `supports_spend = 0` 就是这种情况：它只有余额端点，
没有任何用量或账单 API。
