# 需求与兼容性基线

本文是 TokenProbe 的需求基线：说明项目承诺什么、不承诺什么，以及修改公共 API、
传输层或 provider 时必须遵守的约束。实现细节见 [指南](guides/cmake.md) 与
[ADR](adr/0001-c11-cmake-toolchain-policy.md)。

## 范围

- 用 C11 提供可嵌入的 AI API 额度探测库，以及一个薄命令行工具。
- 支持三类数据：`credit`（余额）、`spend`（周期支出）、`quota`（限流额度）。
- 传输层可替换；库在无网络、无密钥的条件下可以完整测试。
- 公共 ABI 不暴露第三方头文件。

## 非目标

- 不做汇率换算、账单分析或长期账务存储。
- 不保存 API key，也不提供凭据管理。
- 不自动重试失败请求；重试策略由宿主决定。
- 不实现流式响应、分页聚合或供应商特定的批量接口。
- 不保证 `0.x` 系列的二进制兼容（见「ABI 与版本」）。

## 三个量的语义

| 量 | 语义 | 产生条件 |
|---|---|---|
| `credit` | 某一时刻的剩余余额 | 供应商提供余额接口 |
| `spend` | 指定时段内的消耗金额 | 调用方通过 `tokenprobe_probe_ex()` 提供 `period_start` / `period_end`，且供应商支持 |
| `quota` | 剩余限流额度 | 调用方通过 `tokenprobe_probe_ex()` 提供探测所需的方法、请求头或请求体；供应商在响应头中暴露额度 |

规则：

- 供应商无法提供某个量时，不产生对应 metric；调用方用 `supports_*` 预判。
- 缺失字段不能渲染为 `0`；`0` 只表示供应商明确报告为零。
- 金额使用 `tokenprobe_decimal_t`，绝不经过 `double`。
- 多币种分别列出，不求和；结果 `note` 会说明这一点。

## 新鲜度

每个成功结果记录 `tokenprobe_result_observed_at_ms()`：从 Unix 纪元起的毫秒数，
在探测完成时写入。宿主缓存或异步展示结果时，应同时保留该时间戳。

## 平台矩阵

| 平台 | 传输后端 | 状态 |
|---|---|---|
| Windows 10/11（x64） | WinHTTP | 支持 |
| Linux（x64） | OpenSSL 1.1.1+ | 支持 |
| macOS | OpenSSL | CI 验证 |
| 任意平台（测试） | mock | 支持 |

Windows 构建不链接平台 SDK 之外的库；Linux/macOS 仅链接 OpenSSL。
平台后端必须保持相同的外部行为；已知差异见 [传输层指南](guides/transport.md)。

## 请求、超时与失败

- 每次探测使用一个请求；默认超时 `TOKENPROBE_DEFAULT_TIMEOUT_MS`，可由
  `tokenprobe_context_options_t::timeout_ms` 覆盖。
- WinHTTP 通过 `WinHttpSetTimeouts()` 应用超时，OpenSSL 通过 socket 超时应用；
  两者都必须返回 `TOKENPROBE_ERR_TIMEOUT`。
- 不自动重试；失败以 `tokenprobe_status_t` 和 `tokenprobe_error_t` 返回。
- TLS 证书校验不可关闭；证书错误返回 `TOKENPROBE_ERR_TLS`。
- 响应体超过 4 MiB 直接失败，避免无界缓冲。

## 凭据与安全

- API key 优先使用调用方参数，否则读取 provider 的 `env_var`。
- key 只进入 Authorization 头，不写日志、不写入结果。
- 探测请求不得在关闭证书校验的情况下发送。
- 生产端点必须使用 HTTPS；`http://` 仅用于回环测试和本地夹具。
- `--raw` / `raw_response` 只用于诊断，可能包含供应商返回的敏感文本，宿主需自行处理。

## ABI 与版本

- 公共头文件为 `include/tokenprobe/`；实现细节不进入公共 ABI。
- 版本遵循语义化版本：`0.x` 允许不兼容变更；`1.0` 后主版本变更才允许破坏 ABI。
- 新增字段优先追加到结构体末尾；调用方初始化的可扩展结构体必须带
  `struct_size`（例如 `tokenprobe_probe_options_t`）。
- 机器可读文档的 `schema` 只在字段重命名、类型变化或删除时递增；
  新增字段不递增，读取方必须忽略未知字段。
- 任何公共 API 变更都必须同步更新本文件、公共头文件注释和测试。

## 扩展契约

- 新增 provider：见 [provider 指南](guides/providers.md)。至少修改驱动、注册表、
  `cmake/Sources.cmake` 和离线测试。
- 新增传输层：实现 `tokenprobe_transport_t` 并注入
  `tokenprobe_context_options_t::transport`；调用方负责销毁注入的传输层。
- 平台传输层是进程级共享实例，context 只借用，不销毁。
- provider 不得包含操作系统或第三方传输库头文件；公共头不得引入第三方依赖。
  该约束由 `scripts/check_layers.py` 检查。

## 验收

- `ctest` 必须全部通过；测试不得访问网络或消耗 API 额度。
- 平台传输层至少要有编译验证；生命周期和超时行为由集成测试覆盖。
- 文档门禁必须通过；README 的文档索引必须覆盖 `docs/` 下的每个 Markdown 文件。
