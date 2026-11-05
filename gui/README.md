# TokenProbe WPF

Windows 桌面界面，调用随程序一起分发的 `tokenprobe.exe`（`--json`）读取 AI 厂商余额。

当前只支持 DeepSeek（与 CLI 的厂商注册表一致），界面按"厂商可扩展"的结构编写。

## 页面

- **额度**：选择查询厂商，执行查询，查看余额、延迟与错误。未配置密钥时查询按钮禁用，并提供跳转到设置页的入口。
- **设置**：为厂商配置 API Key，只有点击"校验并保存"且探测成功才会写入本机；底部提供"报告问题（GitHub）"与"打开日志文件夹"。

左侧导航可折叠（展开 180 px / 收起 48 px），折叠状态会持久化。

## 需求

- Windows 10/11 x64。
- .NET 10 桌面运行时（框架依赖发布）。
- 仓库自身的 C CLI 构建产物：`build/ninja-release/bin/tokenprobe.exe`。

## 构建与运行

```powershell
dotnet build gui/TokenProbe.Wpf.slnx
dotnet run --project gui/TokenProbe.Wpf
```

`tokenprobe.exe` 会被复制到输出目录的 tools 子目录，运行时只从那里查找；没有界面可选项，也没有环境变量覆盖。缺失时主页面会提示"安装不完整"并禁用查询与校验。

- **Debug**：找不到 release 构建时回退到 `build/ninja-debug/bin/tokenprobe.exe`；两者都没有时只警告。
- **Release / publish**：必须存在 release 构建，否则构建直接失败，避免发布出缺少 CLI 的包。

发布：

```powershell
dotnet publish gui/TokenProbe.Wpf/TokenProbe.Wpf.csproj -c Release -r win-x64 --self-contained false
```

## 测试

```powershell
dotnet test gui/TokenProbe.Core.Tests/TokenProbe.Core.Tests.csproj
```

覆盖内容：

- `--json` 契约解析：成功、结构化错误、非法 JSON、空输出、未知字段、未来 schema、数字型金额、多币种。
- 命令行构造：断言密钥**不出现**在参数里，只进入子进程环境变量。
- 密钥门控：只有探测成功才落盘；`INVALID_KEY`/`AUTH`/`FORBIDDEN` 判为密钥无效；超时与网络错误判为"未能验证"，均不保存；失败不覆盖旧密钥。
- DPAPI 密钥存储：往返、覆盖、损坏文件、路径清洗。
- 日志：写入、`sk-` 脱敏、保留上限、不可写目录不影响运行。
- 进程集成：用 `TokenProbe.FakeProbe` 假 CLI 验证超时、取消、非零退出、stderr、UTF-8 中文。
- 真实 CLI：检出目录存在 C 构建时，对真实 `tokenprobe.exe` 跑一次"无密钥"错误路径与版本读取；未构建时跳过。

## 安全模型

- 密钥不进命令行：校验与查询时通过子进程环境变量注入。
- 只有校验成功（探测返回 `ok: true`）才用 DPAPI `CurrentUser` 范围加密写入 `%LOCALAPPDATA%\TokenProbe\keys`；解密仅对当前 Windows 账户与本机有效。
- 界面上没有删除入口；覆盖是唯一路径。厂商侧失效时，查询返回错误并提示"密钥无效"，可跳转到设置页重新校验。
- 输入框不回填已保存的密钥；空输入框显示"●●●●●● 已配置"作为纯视觉提示。未校验的草稿只存在于内存，关闭应用即丢失，且永不参与查询。
- 未保存密钥时，查询可回退到系统环境变量 `DEEPSEEK_API_KEY`，界面会明确提示。

## 日志与问题反馈

- 日志目录：`%LOCALAPPDATA%\TokenProbe\logs`，按天一个文件，最多保留 5 个、单文件上限 512 KB。
- 每次查询/校验记录：时间、厂商、退出码、耗时、CLI 版本、stderr、stdout 的 JSON 原文；启动时记录应用版本与操作系统版本。
- 写入前对形如 `sk-` 的字符串做脱敏；API Key 与环境变量值从不写入日志。
- 设置页"报告问题（GitHub）"打开项目预填的 issue 页面（应用版本、CLI 版本、操作系统、日志路径），日志内容不会进入 URL 或 issue 正文，需要用户手动附加最新日志文件。
- 界面不展示原始 JSON；解析失败时只显示"响应格式无法识别（退出码 N）"。

## 与 CLI 的契约

- 固定调用 `--provider <id> --json --timeout <ms>`，按 `schema: 1` 解析；未知字段忽略；`schema > 1` 仅提示，不阻断。
- 金额保持字符串原样显示，不转浮点、不跨币种求和。
- 退出码 0、1、2 与 JSON 同时出现时以 JSON 为准；JSON 无法解析时回退显示退出码与 stderr。
- 进程总超时 = CLI `--timeout`（15000 ms）+ 5 s 缓冲；超时或用户取消会终止整个进程树。
- 不解析 `--list`（定宽表格，面向人读），厂商元数据由 `TokenProbe.Core/ProviderCatalog.cs` 内置维护；新增厂商时需要同步。

## 目录

- `TokenProbe.Core/` — 进程调用、JSON 解析、DPAPI 密钥存储、日志、CLI 路径解析、issue 链接构造。
- `TokenProbe.Wpf/` — WPF 界面与 MVVM。
- `TokenProbe.FakeProbe/` — 测试用假 CLI。
- `TokenProbe.Core.Tests/` — xUnit 测试。
