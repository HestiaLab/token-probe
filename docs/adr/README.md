# 架构决策记录

对影响未来贡献者的稳定决策使用 ADR。

当一个决策改变项目结构、公共 API 方向、依赖策略、打包策略，或长期的测试与
CI 预期时，创建一条 ADR。不要为常规实现细节或一次性 bug 修复创建 ADR。

每条 ADR 应包含：

- 背景（Context）
- 决策（Decision）
- 后果（Consequences）

保持 ADR 简短。详细设计笔记只在解释局部复杂度时写进源码注释。

文件名使用如下形式：

```text
0001-use-cmake-presets.md
0002-support-shared-library-builds.md
```

## 记录

- [0001 C11 与 CMake 工具链策略](0001-c11-cmake-toolchain-policy.md)
