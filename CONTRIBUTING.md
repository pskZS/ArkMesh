# 贡献指南

感谢你对 ArkMesh 的关注！我们欢迎各种形式的贡献，包括 Bug 报告、功能请求、代码提交和文档改进。

## 提交 Issue

### Bug 报告

提交 Bug 前请确认：

- 使用的是最新版本
- 已在 [现有 Issue](../../issues) 中搜索过相同问题

请提供以下信息：

- 设备型号与 HarmonyOS 版本
- ArkMesh 版本
- 复现步骤
- 预期行为 vs 实际行为
- 日志（注意脱敏，不要包含服务器地址、密钥等敏感信息）

### 功能请求

请描述：

- 使用场景和动机
- 期望的行为
- 可能的替代方案

## Pull Request 流程

1. Fork 本仓库
2. 创建功能分支：`git checkout -b feat/your-feature`
3. 提交变更：`git commit -m 'feat: add some feature'`
4. 推送到分支：`git push origin feat/your-feature`
5. 提交 Pull Request

## Commit 规范

采用 [Conventional Commits](https://www.conventionalcommits.org/) 格式：

```
feat: 新功能
fix: 修复 Bug
docs: 文档变更
style: 代码格式（不影响逻辑）
refactor: 重构
test: 测试相关
chore: 构建/工具变更
```

## 代码风格

- **ArkTS**: 遵循 HarmonyOS 编码规范
- **C++**: Google C++ Style Guide
- **Go**: 标准 gofmt + go vet

## 安全提醒

- **绝对不要**提交签名材料（证书、密钥文件）
- **绝对不要**提交包含密码、Auth Key 等敏感信息的文件
- 日志文件提交前请脱敏处理

## 构建验证

提交 PR 前请确保：

- [ ] Go 核心编译通过（`bash build.sh`）
- [ ] HAP 构建通过（DevEco Studio Build）
- [ ] 真机测试基本功能正常
- [ ] 无编译警告
