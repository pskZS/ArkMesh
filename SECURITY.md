# 安全策略

## 漏洞报告

如果您发现安全漏洞，请通过 GitHub Security Advisory 私下报告，不要创建公开 Issue。

## 签名材料安全

本项目涉及 HarmonyOS 应用签名材料，开发者请注意：

- `sign/` 目录包含证书和密钥文件，**已排除在版本控制之外**
- 签名配置通过 DevEco Studio 的 Project Structure 界面管理，不要在代码中硬编码
- 构建配置文件 `build-profile.json5` 中的签名信息已清空，需要开发者自行配置

## 密钥管理

- 运行时密钥通过 `KeyManager`（ArkTS）和 `key_manager`（C++）管理
- 加密操作统一使用 OpenSSL
- 不要在日志中输出完整密钥或证书内容
- 不要在代码或配置中硬编码密钥密码
