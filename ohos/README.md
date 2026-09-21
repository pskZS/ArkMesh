# ArkMesh 鸿蒙版客户端

一个基于 HarmonyOS NEXT 的 ArkMesh 客户端实现，支持连接到自定义 Headscale 服务器。

## 功能特性

- 支持自定义 Headscale 服务器地址（非 Tailscale 官方域）
- 预认证密钥（Auth Key）登录
- WireGuard 隧道支持
- Noise 协议加密通信
- STUN / NAT 穿透
- VPN 扩展能力集成
- 节点列表查看
- 连接状态监听
- 持久化配置存储

## 快速开始

### 环境要求

- **DevEco Studio**: 5.0+
- **HarmonyOS SDK**: API 12+ (HarmonyOS NEXT)
- **NDK**: 已安装
- **CMake**: 3.10+
- **OpenSSL**: 开发库

### 构建步骤

1. **克隆项目**

   ```bash
   git clone <your-repo-url>
   cd ArkMesh/ohos
   ```

2. **在 DevEco Studio 中打开**
   - 打开 DevEco Studio
   - 选择 `File` -> `Open`
   - 选择 `ohos` 目录
   - 等待 Gradle 同步完成

3. **编译运行**
   - 点击菜单栏 `Build` -> `Build Hap(s)`
   - 或点击 `Run` 按钮直接运行到模拟器/真机

### 使用流程

1. **首次启动**
   - 在首页输入你的 Headscale 服务器地址（如 `https://headscale.example.com`）
   - （可选）输入预认证密钥
   - 点击"连接"按钮

2. **授予 VPN 权限**
   - 系统会弹出权限请求对话框
   - 点击"允许"授予 VPN 权限

3. **查看状态**
   - 连接成功后，首页会显示连接状态和分配的 IP 地址
   - 点击"网络节点"可以查看当前网络中的所有设备

4. **保存配置**
   - 在"设置"页面可以修改服务器地址、设备名称等配置
   - 配置会自动保存到本地

## 项目架构

```
ohos/entry/src/main/
├── cpp/                          # C++ 原生核心层
│   ├── napi_init.cpp             # NAPI 模块注册与接口导出
│   ├── arkmesh_core.cpp          # 核心控制逻辑
│   └── src/
│       ├── api/headscale_client.{h,cpp}   # Headscale HTTP API 通信
│       ├── key/key_manager.{h,cpp}        # Curve25519 密钥管理
│       ├── noise/noise_protocol.{h,cpp}   # Noise XX 握手协议
│       ├── wireguard/wireguard_tunnel.{h,cpp}  # WireGuard 隧道
│       ├── stun/stun_client.{h,cpp}       # STUN / NAT 穿透
│       └── utils/crypto_utils.{h,cpp}     # Base64/SHA256 工具
├── ets/
│   ├── core/
│   │   ├── ArkMeshCore.ts        # ArkTS 业务核心
│   │   ├── ArkMeshNative.ts      # NAPI 封装层
│   │   ├── ArkMeshStatus.ts      # 状态枚举
│   │   ├── StorageUtils.ts       # 持久化存储
│   │   ├── PowerManager.ets      # 电源管理与后台保活
│   │   └── NetworkMonitor.ets    # 网络状态监听
│   ├── entryability/EntryAbility.ets
│   ├── pages/
│   │   ├── Index.ets             # 主界面
│   │   ├── Settings.ets          # 设置页面
│   │   └── Nodes.ets             # 节点列表
│   └── vpn/ArkMeshVpnAbility.ets  # VPN 扩展能力
└── resources/                     # 资源文件
```

## 技术栈

- **UI 层**: ArkTS + 声明式 UI
- **原生层**: C++17 + NAPI
- **加密**: OpenSSL (Curve25519, ChaCha20-Poly1305, SHA256)
- **协议**: WireGuard, Noise Protocol, STUN
- **网络**: TCP/UDP Socket, HTTP Client (OpenSSL TLS)
- **VPN**: VpnExtensionAbility

## Headscale 服务器配置

确保你的 Headscale 服务器满足以下要求：

1. **版本**: Headscale 0.24.0+
2. **HTTPS**: 必须使用 HTTPS（可通过 nginx 反向代理 + Let's Encrypt 实现）
3. **预认证密钥**: 在 Headscale 后台生成 Auth Key

   ```bash
   headscale preauthkeys create --reusable --expiration 90d
   ```

## 注意事项

### TLS/HTTPS 支持

当前版本 HTTP 客户端已完整支持基于 OpenSSL 的 TLS/HTTPS：

- 使用 `TLS_client_method()` 建立 TLS 连接
- 支持 SNI（Server Name Indication）
- 支持系统 CA 证书验证
- 线程安全的全局 SSL 初始化（`std::call_once`）

### 已知限制

- WireGuard 数据平面尚未完全打通（TUN fd 读写为 TODO）
- DERP 中继尚未实现

### 调试技巧

- 查看日志：在 DevEco Studio 的 Logcat 窗口查看应用日志
- C++ 调试：在 `.cpp` 文件中设置断点，使用 LLDB 调试
- ArkTS 调试：在 `.ets` 文件中设置断点

## 常见问题

### Q: 编译时报错 "getContext is not defined"
A: 确保已导入 `import { getContext } from '@kit.UIAbilityKit'`

### Q: 编译时报错 "Cannot find module 'libarkmesh.so'"
A: 确保 NDK 已正确安装，CMake 编译成功

### Q: 运行时提示 VPN 权限被拒绝
A: 在系统设置中手动授予 VPN 权限

### Q: 连接失败，提示 "Failed to connect to server"
A:
1. 检查服务器地址是否正确
2. 确认服务器可访问（尝试在浏览器中打开）
3. 如果使用 HTTPS，确保证书有效

## 贡献指南

欢迎提交 Issue 和 Pull Request。

### 代码风格

- C++ 代码遵循 Google C++ Style Guide
- ArkTS 代码遵循 HarmonyOS 编码规范

## 许可

MIT License

---

**注意**: 本项目为开源实现，仅供学习和研究使用。生产环境请自行评估安全性。
