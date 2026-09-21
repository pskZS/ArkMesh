# OpenSSL编译指南 for HarmonyOS

本文档详细说明如何为HarmonyOS编译OpenSSL库。

## 📋 前置要求

### 方案A：WSL2（推荐，Windows用户）

1. **安装WSL2**：
   ```powershell
   # 在PowerShell（管理员）中执行
   wsl --install -d Ubuntu-22.04
   ```
   
2. **配置WSL2**：
   ```bash
   # 进入WSL2
   wsl
   
   # 更新系统
   sudo apt-get update && sudo apt-get upgrade -y
   
   # 安装编译工具
   sudo apt-get install -y build-essential git cmake python3 python3-pip wget curl
   ```

### 方案B：Linux环境（Ubuntu 22.04）

```bash
# 更新系统
sudo apt-get update && sudo apt-get upgrade -y

# 安装编译工具
sudo apt-get install -y build-essential git cmake python3 python3-pip wget curl
```

## 🔧 编译步骤

### 步骤1：下载HarmonyOS SDK

1. 访问 [HarmonyOS SDK下载页面](https://developer.huawei.com/consumer/cn/download/)
2. 下载 **HarmonyOS SDK**（选择Linux版本）
3. 解压到 `/opt/harmonyos/sdk`（或任意目录）

### 步骤2：克隆编译仓库

```bash
# 创建工作目录
mkdir -p ~/harmonyos-build
cd ~/harmonyos-build

# 克隆tpc_c_cplusplus仓库
git clone https://gitee.com/openharmony/tpc_c_cplusplus.git
cd tpc_c_cplusplus/lycium
```

### 步骤3：配置环境变量

```bash
# 设置HarmonyOS SDK路径
export OHOS_SDK=/opt/harmonyos/sdk  # 修改为您的SDK路径

# 添加到环境变量（可选，永久生效）
echo "export OHOS_SDK=/opt/harmonyos/sdk" >> ~/.bashrc
source ~/.bashrc
```

### 步骤4：配置编译工具链

```bash
cd ~/harmonyos-build/tpc_c_cplusplus/lycium/Buildtools

# 校验工具包
sha512sum -c SHA512SUM

# 解压工具包
tar -zxvf toolchain.tar.gz

# 复制到SDK目录
cp toolchain/* ${OHOS_SDK}/native/llvm/bin/
```

### 步骤5：编译OpenSSL

```bash
cd ~/harmonyos-build/tpc_c_cplusplus/lycium

# 编译OpenSSL（自动编译arm64-v8a和x86_64）
./build.sh openssl
```

编译过程约需20-30分钟，请耐心等待。

### 步骤6：验证编译结果

```bash
# 检查编译输出
ls -lh usr/openssl/

# 应该看到以下目录结构：
# usr/openssl/
# ├── include/
# │   └── openssl/
# │       ├── ssl.h
# │       ├── evp.h
# │       └── ...
# └── lib/
#     ├── libssl.a
#     └── libcrypto.a
```

## 📦 集成到项目

### Windows项目路径

假设您的项目在 `D:\mytest\ArkMesh\ohos`，需要将编译好的OpenSSL复制到：

```
D:\mytest\ArkMesh\ohos\entry\src\main\cpp\third_party\openssl\
├── arm64-v8a\
│   ├── include\
│   │   └── openssl\
│   │       ├── ssl.h
│   │       ├── evp.h
│   │       └── ...
│   └── lib\
│       ├── libssl.a
│       └── libcrypto.a
└── x86_64\  （可选，用于模拟器）
    ├── include\
    └── lib\
```

### 复制命令（在WSL2中执行）

```bash
# 设置项目路径（Windows路径需要转换）
# WSL2中访问Windows文件：/mnt/d/mytest/ArkMesh/ohos

PROJECT_DIR="/mnt/d/mytest/ArkMesh/ohos"
OPENSSL_BUILD_DIR="$HOME/harmonyos-build/tpc_c_cplusplus/lycium/usr/openssl"

# 创建目标目录
mkdir -p "$PROJECT_DIR/entry/src/main/cpp/third_party/openssl/arm64-v8a"

# 复制头文件和库文件
cp -r "$OPENSSL_BUILD_DIR/include" "$PROJECT_DIR/entry/src/main/cpp/third_party/openssl/arm64-v8a/"
cp -r "$OPENSSL_BUILD_DIR/lib" "$PROJECT_DIR/entry/src/main/cpp/third_party/openssl/arm64-v8a/"

# 验证复制结果
ls -lh "$PROJECT_DIR/entry/src/main/cpp/third_party/openssl/arm64-v8a/"
```

## 🔄 重新编译应用

1. 打开 **DevEco Studio**
2. 选择项目 `D:\mytest\ArkMesh\ohos`
3. 点击 **Build > Rebuild Project**
4. 查看构建日志，确认OpenSSL被正确链接：
   ```
   ✅ Found OpenSSL:
      Headers: D:/mytest/ArkMesh/ohos/entry/src/main/cpp/third_party/openssl/arm64-v8a/include
      SSL library: D:/mytest/ArkMesh/ohos/entry/src/main/cpp/third_party/openssl/arm64-v8a/lib/libssl.a
      Crypto library: D:/mytest/ArkMesh/ohos/entry/src/main/cpp/third_party/openssl/arm64-v8a/lib/libcrypto.a
   ✅ OpenSSL libraries linked successfully
   ```

## ✅ 验证VPN功能

1. **使用真机测试**：
   - 连接HarmonyOS真机（系统版本4.0+）
   - 安装应用
   - 启动VPN功能
   - 查看日志确认加密操作正常

2. **查看日志**：
   ```
   # VPN Extension启动成功
   VPN extension started successfully
   
   # 加密操作正常
   OpenSSL initialized for TLS/HTTPS support
   Noise protocol initialized
   ```

## 🔍 常见问题

### Q1: 编译失败 "toolchain not found"

**解决方案**：
```bash
# 确保已复制编译工具
cd ~/harmonyos-build/tpc_c_cplusplus/lycium/Buildtools
tar -zxvf toolchain.tar.gz
cp toolchain/* ${OHOS_SDK}/native/llvm/bin/
```

### Q2: 编译失败 "OHOS_SDK not set"

**解决方案**：
```bash
# 设置环境变量
export OHOS_SDK=/opt/harmonyos/sdk
# 或添加到 ~/.bashrc
echo "export OHOS_SDK=/opt/harmonyos/sdk" >> ~/.bashrc
source ~/.bashrc
```

### Q3: 复制文件失败 "Permission denied"

**解决方案**：
```bash
# 修改目标目录权限
chmod -R 755 /mnt/d/mytest/ArkMesh/ohos/entry/src/main/cpp/third_party
```

### Q4: DevEco Studio找不到OpenSSL

**解决方案**：
1. 确认文件已正确复制到 `entry/src/main/cpp/third_party/openssl/arm64-v8a/`
2. 确认目录结构正确（include和lib目录）
3. 重新构建项目（Build > Rebuild Project）

## 📚 参考资料

- [HarmonyOS三方库编译指南](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-build-third-party)
- [OpenSSL官方文档](https://www.openssl.org/docs/)
- [lycium编译工具使用说明](https://gitee.com/openharmony/tpc_c_cplusplus)

## 🎉 完成！

恭喜！您已成功编译并集成OpenSSL for HarmonyOS。现在可以：

1. ✅ 使用完整的VPN加密功能
2. ✅ 支持TLS/HTTPS通信
3. ✅ 支持Noise协议加密
4. ✅ 支持X25519/ED25519密钥交换

享受您的HarmonyOS VPN应用吧！
