# TokenProbe

用 C11 写的 AI 厂商 API 额度探测库与命令行工具。查询各厂商 AI API 的
**剩余额度**、**周期花费**与**配额**，并可作为核心库被桌面、网页或服务端项目集成。

- **C11 + CMake**，Windows、Linux 与 macOS 跨平台。
- **依赖尽可能少**：Windows 只用系统自带的 WinHTTP，无任何外部依赖；
  Linux 仅链接 OpenSSL（libc 不含 TLS 实现，无法避免）。
- **公共 ABI 不暴露任何第三方头文件**，可被 C、C++、C#、Rust、Python (FFI)
  直接嵌入。
- **可离线测试**：传输层是可替换接口，完整栈可以对着录制的响应体运行，
  不联网、不花钱、不需要密钥。

## 三个量不是同一个东西

这是本项目最容易踩的坑，因此 API 把它们分开建模：

| 量 | 含义 | 例子 |
| --- | --- | --- |
| **credit** | 某个时刻的剩余余额 | DeepSeek `GET /user/balance` |
| **spend** | 某段时间内已消耗的金额 | OpenAI organization costs |
| **quota** | 剩余限流额度，只能从真实推理请求的响应头读到 | `x-ratelimit-remaining-tokens` |

**厂商不支持的量不会产生 metric。**调用方因此总能区分"不支持"和"等于零"，
不会把缺失字段渲染成自信的 `0.00`。能力在
`tokenprobe_provider_t::supports_credit` 等字段上预先声明。

金额一律用精确十进制定点数 `tokenprobe_decimal_t` 表示，绝不用 `double`：
厂商返回的是 `"110.00"` 这样的字符串，二进制浮点无法可靠往返。

## 支持的厂商

| ID | 厂商 | 剩余额度 | 周期花费 | 密钥环境变量 |
| --- | --- | --- | --- | --- |
| `deepseek` | DeepSeek | 是 | 否（官方无此 API） | `DEEPSEEK_API_KEY` |

新增厂商需在 `src/tokenprobe/` 下加一个驱动文件、在 `src/tokenprobe/tp_ctx.c`
的注册表里登记、在 `cmake/Sources.cmake` 中登记源文件，并补一组离线测试，
详见 [docs/guides/providers.md](docs/guides/providers.md)。

## 快速开始

**所需工具：** CMake 3.21+、C11 编译器（MSVC、GCC 或 Clang）、Ninja（默认预设）、
Python 3（测试门禁脚本需要）。Linux 还需要 OpenSSL 开发包。

Windows PowerShell
```powershell
./scripts/bootstrap.ps1
```

Linux/macOS
```sh
./scripts/bootstrap.sh
```

日常检查（格式、构建、测试）：

Windows PowerShell
```powershell
./scripts/check.ps1
```

Linux/macOS
```sh
./scripts/check.sh
```

预设、生成器与常见错误见 [docs/guides/cmake.md](docs/guides/cmake.md)。

## 命令行用法

```sh
# 列出支持的厂商及其能力
tokenprobe --list

# 查询余额（密钥从 DEEPSEEK_API_KEY 读取）
tokenprobe --provider deepseek

# 显式传密钥，输出机器可读的 XML
tokenprobe --provider deepseek --api-key sk-... --xml

# 也可以输出 JSON；两种格式字段相同，差别只在编码
tokenprobe --provider deepseek --api-key sk-... --json

# 打印原始响应体以便排查字段变化
tokenprobe --provider deepseek --raw
```

周期花费、配额探测等需要额外参数的场景使用 `tokenprobe_probe_ex()`，
可传 `tokenprobe_probe_options_t`（时间范围、附加请求头、方法与请求体）。

退出码：`0` 成功，`1` 探测失败，`2` 用法错误。机器可读模式下失败也会输出结构化错误文档，
便于被别的程序消费。

### 机器可读输出（`--xml` / `--json`）

两种格式承载**同一组字段**，共用同一个 `schema` 号；`--xml` 与 `--json` 互斥，
同时给出会以退出码 `2` 失败，而不是静默采用最后一个。差异只来自编码本身：

| | XML | JSON |
| --- | --- | --- |
| 未上报的字段 | 省略元素/属性 | 写 `null`（键集合保持稳定） |
| 文本中的 C0 控制符 | 无法表示，替换为 U+FFFD | `\uXXXX` 转义，原样保留 |
| 数值 | 属性字符串 | 字符串（金额绝不做成 JSON number） |

```xml
<?xml version="1.0" encoding="UTF-8"?>
<tokenprobe schema="1" version="0.2.0">
  <result provider="deepseek" display-name="DeepSeek" ok="true" http-status="200" latency-ms="12" observed-at="1760790000000" available="true">
    <capabilities credit="true" spend="false" quota="false"/>
    <metrics>
      <metric kind="credit" id="total_balance" label="Total balance">
        <amount unit="currency" currency="CNY">110.00</amount>
      </metric>
    </metrics>
    <note>multiple currencies reported; each currency is listed separately and must not be summed</note>
  </result>
</tokenprobe>
```

```json
{"schema":1,"version":"0.2.0","result":{"provider":"deepseek","display_name":"DeepSeek","ok":true,"http_status":200,"latency_ms":12,"observed_at":1760790000000,"available":true,"capabilities":{"credit":true,"spend":false,"quota":false},"metrics":[{"kind":"credit","id":"total_balance","label":"Total balance","period_start":null,"period_end":null,"amount":"110.00","unit":"currency","currency":"CNY"}],"note":"…"}}
```

- **金额是字符串**，保留厂商返回的精确十进制，不经 `double`。
- 读取方应忽略不认识的字段；只有重命名、改类型或删除字段才提升 `schema`。
- `--raw` 输出 `<raw-response>` 元素 / `raw_response` 键，不会追加在文档之后。
- 只有这份出站文档是机器可读格式；厂商响应一律按 JSON 解析，
  见 [src/tokenprobe/tp_json.c](src/tokenprobe/tp_json.c)。

## 作为库使用

```cmake
find_package(TokenProbe CONFIG REQUIRED)
target_link_libraries(app PRIVATE TokenProbe::tokenprobe_core)
```

```c
#include <tokenprobe/probe.h>

tokenprobe_context_options_t options;
tokenprobe_context_options_init(&options);

tokenprobe_context_t *ctx = tokenprobe_context_create(&options);
tokenprobe_error_t    err;
tokenprobe_result_t  *result = NULL;

tokenprobe_error_init(&err);
if (tokenprobe_probe(tokenprobe_provider_find("deepseek"), ctx, NULL, NULL,
                    &result, &err) == TOKENPROBE_OK) {
    int i;
    for (i = 0; i < tokenprobe_result_metric_count(result); i++) {
        const tokenprobe_metric_t *m = tokenprobe_result_metric(result, i);
        char amount[40];
        tokenprobe_decimal_format(m->amount, amount, sizeof(amount));
        printf("%s: %s %s\n", m->id, amount, m->currency ? m->currency : "");
    }
}

tokenprobe_result_free(result);
tokenprobe_context_destroy(ctx);
```

结果通过 `tokenprobe_result_observed_at_ms()` 提供观测时间（Unix 毫秒），
便于 UI 判断数据新鲜度。需要时间范围、附加请求头或请求体时，使用
`tokenprobe_probe_ex()`。

**集成方的两个关键点：**

1. **无全局可变业务状态。**所有操作经 `tokenprobe_context_t` 句柄，线程间互不干扰，
   宿主可以一线程一 context 并发探测。平台传输层是初始化一次后只读的进程级共享实例。
2. **可注入传输层。**`tp_ctx_options.transport` 可以指向自定义传输，
   包括内置的离线 mock 传输。宿主可以拿它渲染 UI、跑回放测试，
   或接入自己的 HTTP 栈。

## 目录结构

```text
include/tokenprobe/   公共头文件（唯一对外 ABI）
src/tokenprobe/       实现：core、传输后端、厂商驱动
src/main.c            命令行工具
tests/                CTest 测试（vendored Unity）
docs/guides/          主题指南
docs/adr/             架构决策记录
scripts/              引导、检查、格式化与发布脚本
```

## 文档

本仓库文档只维护一种语言：中文。

### 需求

| 文档 | 说明 |
|---|---|
| [docs/requirements.md](docs/requirements.md) | 范围、三个量的语义、平台矩阵、ABI 与扩展契约 |

### 指南

| 文档 | 说明 |
|---|---|
| [docs/guides/environment.md](docs/guides/environment.md) | 平台环境与工具安装 |
| [docs/guides/cmake.md](docs/guides/cmake.md) | CMake 预设、选项与故障排除 |
| [docs/guides/coding-style.md](docs/guides/coding-style.md) | 命名、头文件、格式与 API 文档约定 |
| [docs/guides/testing.md](docs/guides/testing.md) | 测试分层、静态分析与冒烟测试 |
| [docs/guides/transport.md](docs/guides/transport.md) | 传输层后端、证书校验与已知限制 |
| [docs/guides/template.md](docs/guides/template.md) | 本项目相对标准的来源与裁剪 |
| [docs/guides/providers.md](docs/guides/providers.md) | 新增厂商驱动的步骤与契约 |
| [docs/guides/release.md](docs/guides/release.md) | 发布准备与打包 |

### 架构决策

| 文档 | 说明 |
|---|---|
| [docs/adr/README.md](docs/adr/README.md) | 架构决策记录（ADR）说明 |
| [docs/adr/0001-c11-cmake-toolchain-policy.md](docs/adr/0001-c11-cmake-toolchain-policy.md) | C11 与 CMake 工具链策略 |

### 根级文档

| 文档 | 说明 |
|---|---|
| [CONTRIBUTING.md](CONTRIBUTING.md) | 分支、提交与 PR 工作流 |
| [SECURITY.md](SECURITY.md) | 漏洞报告与依赖处理 |
| [C_PROJECT_STANDARD.md](C_PROJECT_STANDARD.md) | 本仓库遵循的 C 工程标准 |
| [CHANGELOG.md](CHANGELOG.md) | 版本变更记录 |

## 许可证

MIT
