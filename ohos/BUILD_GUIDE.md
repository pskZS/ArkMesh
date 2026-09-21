# ArkMesh 鸿蒙版客户端 - 构建指南

## 环境要求

- **操作系统**: Windows 10/11, macOS, 或 Linux
- **IDE**: DevEco Studio 5.0+
- **SDK**: HarmonyOS NEXT API 12+
- **JDK**: OpenJDK 11 或更高版本
- **CMake**: 3.10 或更高版本（用于编译 C++ 代码）
- **OpenSSL**: 需要 OpenSSL 开发库

## 项目结构

`
ohos/
├── build-profile.json5          # 项目构建配置
├── entry/                        # 应用主模块
│  ├── build-profile.json5       # 模块构建配置
│  ├── oh-package.json5          # 依赖配置
│  └── src/main/
│      ├── ets/                  # ArkTS 代码
│      │  ├── entryability/     # 入口 Ability
│      │  ├── pages/            # UI 页面
│      │  ├── core/             # 核心业务逻辑
│      │  └── vpn/              # VPN 扩展能力
│      ├── cpp/                  # Native C++ 核心代码
│      │  ├── CMakeLists.txt
│      │  ├── include/
│      │  └── src/
│      └── resources/            # 资源文件
└── README.md
`

## 构建步骤

### 1. 安装 DevEco Studio

1. 下载并安装 DevEco Studio 5.0+ 从华为开发者官网
2. 配置 HarmonyOS SDK（API 12+）
3. 安装 NDK（Native Development Kit）

### 2. 克隆项目

`ash
git clone <your-repo-url>
cd ohos
`

### 3. 在 DevEco Studio 中打开项目

1. 打开 DevEco Studio
2. 选择 File -> Open，选择 ohos 目录
3. 等待 Gradle 同步完成

### 4. 配置 NDK 路径

在 DevEco Studio 中：
1. File -> Settings -> Appearance & Behavior -> System Settings -> HarmonyOS SDK
2. 确保已安装 NDK
3. 如果未安装，点击 Edit 安装

### 5. 构建项目

在 DevEco Studio 中：
1. 点击菜单栏 Build -> Build Hap(s)/App(s) -> Build Hap(s)
2. 或者使用快捷键 Ctrl+F9

或者使用命令行：
`ash
# 进入项目目录
cd ohos

# 构建 Debug 版本
hpm build debug

# 构建 Release 版本
hpm build release
`

### 6. 运行项目

#### 使用模拟器

1. 点击菜单栏 Tools -> Device Manager
2. 创建一个 HarmonyOS 模拟器
3. 启动模拟器
4. 点击 Run 按钮运行项目

#### 使用真机

1. 连接鸿蒙设备到电脑
2. 在设备上开启开发者模式
3. 允许 USB 调试
4. 点击 Run 按钮运行项目

## 常见问题

### 1. NDK 未找到

**问题**: 构建时报错 NDK not found

**解决**: 
1. 打开 DevEco Studio 设置
2. 导航到 Appearance & Behavior -> System Settings -> HarmonyOS SDK
3. 安装 NDK

### 2. CMake 版本不兼容

**问题**: CMake 版本低于 3.10

**解决**:
1. 检查 CMake 版本: cmake --version
2. 如果版本过低，升级 CMake

### 3. OpenSSL 头文件找不到

**问题**: 编译 C++ 代码时找不到 OpenSSL 头文件

**解决**:
1. 确保已安装 OpenSSL 开发库
2. 在 CMakeLists.txt 中正确设置 OpenSSL 路径

### 4. NAPI 接口编译失败

**问题**: 找不到 napi/native_api.h

**解决**:
1. 确保 NDK 已正确安装
2. 检查 CMakeLists.txt 中的 NAPI_INCLUDE_DIR 是否正确设置

## 调试技巧

### C++ 代码调试

1. 在 DevEco Studio 中设置断点
2. 使用 LLDB 调试器
3. 查看 Logcat 输出

### ArkTS 代码调试

1. 在 DevEco Studio 中设置断点
2. 使用 Chrome DevTools 调试
3. 查看日志输出

## 发布构建

### 签名

1. 生成签名文件：
`ash
keytool -genkeypair -alias "TailscaleClient" -keyalg RSA -keysize 2048 -validity 36500 -keystore ArkMesh.jks
`

2. 在 DevEco Studio 中配置签名：
   - File -> Project Structure -> Project -> Signing Configs
   - 选择 Release 模式
   - 配置签名文件路径、别名和密码

### 构建 Release 版本

`ash
hpm build release
`

构建完成后，HAP 文件位于 entry/build/outputs/hap/release/ 目录下。

## 性能优化

### 1. 优化 C++ 代码

- 使用编译器优化选项 -O2 或 -O3
- 避免不必要的内存拷贝
- 使用异步 I/O

### 2. 优化 UI

- 使用懒加载（Lazy Loading）
- 避免在 UI 线程执行耗时操作
- 使用 @State 和 @Prop 合理管理状态

### 3. 优化网络通信

- 使用连接池
- 启用压缩
- 合理设置超时时间

## 安全注意事项

1. **密钥管理**: 私钥存储在设备安全区域，不要硬编码在代码中
2. **网络通信**: 使用 HTTPS 和 Noise 协议加密通信
3. **权限控制**: 仅请求必要的权限
4. **日志安全**: 避免在日志中输出敏感信息

## 贡献指南

欢迎提交 Issue 和 Pull Request。

### 提交规范

- 使用清晰的提交信息
- 遵循代码风格规范
- 添加必要的注释

### 代码风格

- C++ 代码遵循 Google C++ Style Guide
- ArkTS 代码遵循 HarmonyOS 编码规范

## 许可

MIT License