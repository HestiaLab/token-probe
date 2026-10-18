# 更新日志

> 新增段落用中文书写；已发布段落保留发布时的原文，不重写。

## 0.2.0 — 2026-10-18

- 新增 `tokenprobe_probe_ex()` 与 `tokenprobe_probe_options_t`：支持统计时段、
  附加请求头、HTTP 方法与请求体，为 spend / quota 驱动提供输入模型。
- 结果新增 `tokenprobe_result_observed_at_ms()`；XML/JSON 输出新增
  `observed-at` / `observed_at`（新增字段不提升 schema）。
- 平台传输层改为进程级共享实例（`INIT_ONCE` / `pthread_once`），修复多个
  context 之间误释放 WinHTTP 会话 / OpenSSL `SSL_CTX` 的生命周期缺陷。
- WinHTTP 后端应用 `timeout_ms`（`WinHttpSetTimeouts`）。
- 新增分层门禁 `scripts/check_layers.py`；补充 context 生命周期与 CLI 超时测试。
- 修复 CMake `PARENT_SCOPE` 警告，清理未使用的内部函数。
- 源码注释与文档统一为中文。

## 0.1.0 — 2026-10-16

首个版本。

- 公共 C11 ABI（`include/tokenprobe/`），不暴露任何第三方头文件。
- 精确十进制定点数 `tokenprobe_decimal_t`：金额绝不经 `double`，
  解析采用十进制字符串与 JSON 数字两条路径，解析期一次性舍入到最多 9 位小数。
- 严格 JSON 解析器：拒绝重复键、尾部垃圾、孤立代理对、裸控制字符与超深嵌套。
- 传输层抽象：Windows 用 WinHTTP（零外部依赖），Linux 用 OpenSSL（仅构建期依赖），
  另有完全离线的 mock 传输，用于无网络、无密钥、无花费的端到端测试。
- 传输层可注入，宿主可替换为自己的 HTTP 栈；库内无全局可变状态，
  一线程一 context 即可并发。
- DeepSeek 驱动：`GET /user/balance`，报告 `total_balance` /
  `granted_balance` / `topped_up_balance`，按币种分行。
- `credit` / `spend` / `quota` 三类量分开建模；厂商不支持的量不产生 metric，
  调用方总能区分"不支持"与"等于零"。
- `tokenprobe` 命令行工具，支持 `--list`、`--xml`、`--json`、`--raw`，退出码 0/1/2。
  两种机器可读格式承载同一组字段、共用 `schema` 版本号，差异只来自编码
  （XML 无法表示 C0 控制符，JSON 用 `null` 表示未上报字段）；厂商响应一律按 JSON 解析。
- CMake 包配置与 `add_subdirectory()` 双消费路径，含 C 与 C++ 冒烟测试。
- CTest 套件：13 个定点数用例、16 个 JSON 用例、14 个 URL 用例、
  17 个 DeepSeek 驱动用例，外加 CLI 冒烟测试与仓库质量门禁。
- 修复 `scripts/check_decl_def.py`：函数指针声明会被误判为"已声明但未定义"的
  公共函数（返回类型被当成函数名）。现在先屏蔽 `(*name)` 声明符再扫描。
