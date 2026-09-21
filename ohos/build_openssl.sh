#!/bin/bash

# ============================================================================
# OpenSSL自动编译脚本 for HarmonyOS
# 
# 功能：
# 1. 自动检测并安装依赖
# 2. 克隆编译仓库
# 3. 配置编译环境
# 4. 编译OpenSSL
# 5. 集成到项目
# 
# 使用方法：
# chmod +x build_openssl.sh
# ./build_openssl.sh
# ============================================================================

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'  # No Color

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}========================================${NC}"
}

# 检测操作系统
detect_os() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        OS="linux"
    elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]]; then
        OS="windows"
    else
        log_error "不支持的操作系统: $OSTYPE"
        exit 1
    fi
    log_info "检测到操作系统: $OS"
}

# 检测包管理器
detect_package_manager() {
    if command -v apt-get &> /dev/null; then
        PKG_MANAGER="apt-get"
    elif command -v yum &> /dev/null; then
        PKG_MANAGER="yum"
    elif command -v dnf &> /dev/null; then
        PKG_MANAGER="dnf"
    else
        log_error "未检测到支持的包管理器"
        exit 1
    fi
    log_info "检测到包管理器: $PKG_MANAGER"
}

# 安装依赖
install_dependencies() {
    log_step "步骤1: 安装编译依赖"
    
    local deps="build-essential git cmake python3 python3-pip wget curl"
    
    log_info "正在安装依赖: $deps"
    
    if [[ "$PKG_MANAGER" == "apt-get" ]]; then
        sudo apt-get update
        sudo apt-get install -y $deps
    elif [[ "$PKG_MANAGER" == "yum" ]]; then
        sudo yum groupinstall -y "Development Tools"
        sudo yum install -y git cmake python3 python3-pip wget curl
    elif [[ "$PKG_MANAGER" == "dnf" ]]; then
        sudo dnf groupinstall -y "Development Tools"
        sudo dnf install -y git cmake python3 python3-pip wget curl
    fi
    
    log_success "依赖安装完成"
}

# 检查HarmonyOS SDK
check_sdk() {
    log_step "步骤2: 检查HarmonyOS SDK"
    
    if [[ -z "$OHOS_SDK" ]]; then
        log_warning "OHOS_SDK环境变量未设置"
        log_info "请输入HarmonyOS SDK路径（例如：/opt/harmonyos/sdk）:"
        read -r SDK_PATH
        
        if [[ ! -d "$SDK_PATH" ]]; then
            log_error "SDK路径不存在: $SDK_PATH"
            log_info "请先下载HarmonyOS SDK:"
            log_info "1. 访问 https://developer.huawei.com/consumer/cn/download/"
            log_info "2. 下载 HarmonyOS SDK (Linux版本)"
            log_info "3. 解压到任意目录"
            exit 1
        fi
        
        export OHOS_SDK="$SDK_PATH"
        
        # 添加到环境变量
        log_info "是否将SDK路径添加到 ~/.bashrc? (y/n)"
        read -r ADD_TO_BASHRC
        if [[ "$ADD_TO_BASHRC" == "y" ]]; then
            echo "export OHOS_SDK=$SDK_PATH" >> ~/.bashrc
            log_success "已添加到 ~/.bashrc"
        fi
    fi
    
    log_success "HarmonyOS SDK路径: $OHOS_SDK"
    
    # 验证SDK目录结构
    if [[ ! -d "$OHOS_SDK/native/llvm/bin" ]]; then
        log_error "SDK目录结构不正确，缺少: $OHOS_SDK/native/llvm/bin"
        exit 1
    fi
}

# 克隆编译仓库
clone_repo() {
    log_step "步骤3: 克隆编译仓库"
    
    WORK_DIR="$HOME/harmonyos-build"
    mkdir -p "$WORK_DIR"
    cd "$WORK_DIR"
    
    if [[ -d "tpc_c_cplusplus" ]]; then
        log_warning "tpc_c_cplusplus目录已存在"
        log_info "是否删除并重新克隆? (y/n)"
        read -r RECLONE
        if [[ "$RECLONE" == "y" ]]; then
            rm -rf tpc_c_cplusplus
        else
            cd tpc_c_cplusplus/lycium
            return
        fi
    fi
    
    log_info "正在克隆 tpc_c_cplusplus 仓库..."
    git clone https://gitee.com/openharmony/tpc_c_cplusplus.git
    
    cd tpc_c_cplusplus/lycium
    log_success "仓库克隆完成"
}

# 配置编译工具链
setup_toolchain() {
    log_step "步骤4: 配置编译工具链"
    
    cd "$WORK_DIR/tpc_c_cplusplus/lycium/Buildtools"
    
    if [[ ! -f "toolchain.tar.gz" ]]; then
        log_error "未找到 toolchain.tar.gz"
        log_info "请检查Buildtools目录"
        exit 1
    fi
    
    # 校验工具包
    log_info "校验工具包..."
    if sha512sum -c SHA512SUM 2>/dev/null; then
        log_success "工具包校验通过"
    else
        log_warning "工具包校验失败，继续尝试编译"
    fi
    
    # 解压工具包
    log_info "解压工具包..."
    tar -zxvf toolchain.tar.gz
    
    # 复制到SDK目录
    log_info "复制编译工具到SDK目录..."
    cp toolchain/* "$OHOS_SDK/native/llvm/bin/"
    
    log_success "编译工具链配置完成"
}

# 编译OpenSSL
build_openssl() {
    log_step "步骤5: 编译OpenSSL"
    
    cd "$WORK_DIR/tpc_c_cplusplus/lycium"
    
    log_info "开始编译OpenSSL（预计需要20-30分钟）..."
    log_info "请耐心等待..."
    
    # 编译OpenSSL
    ./build.sh openssl
    
    # 验证编译结果
    if [[ -f "usr/openssl/lib/libssl.a" ]] && [[ -f "usr/openssl/lib/libcrypto.a" ]]; then
        log_success "OpenSSL编译成功！"
        log_info "编译结果:"
        ls -lh usr/openssl/lib/
    else
        log_error "OpenSSL编译失败，未找到库文件"
        exit 1
    fi
}

# 集成到项目
integrate_to_project() {
    log_step "步骤6: 集成到项目"
    
    # 自动检测项目路径
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [[ -d "$SCRIPT_DIR/entry/src/main/cpp" ]]; then
        PROJECT_DIR="$SCRIPT_DIR"
    elif [[ -d "$HOME/ArkMesh/ohos" ]]; then
        PROJECT_DIR="$HOME/ArkMesh/ohos"
    else
        log_warning "未自动检测到项目路径"
        log_info "请输入项目路径（例如：/home/user/ArkMesh/ohos）:"
        read -r PROJECT_DIR

        if [[ ! -d "$PROJECT_DIR" ]]; then
            log_error "项目路径不存在: $PROJECT_DIR"
            exit 1
        fi
    fi
    
    log_info "项目路径: $PROJECT_DIR"
    
    # 创建目标目录
    TARGET_DIR="$PROJECT_DIR/entry/src/main/cpp/thirdparty/openssl/arm64-v8a"
    mkdir -p "$TARGET_DIR"
    
    # 复制头文件和库文件
    log_info "复制OpenSSL头文件和库文件..."
    cp -r "$WORK_DIR/tpc_c_cplusplus/lycium/usr/openssl/include" "$TARGET_DIR/"
    cp -r "$WORK_DIR/tpc_c_cplusplus/lycium/usr/openssl/lib" "$TARGET_DIR/"
    
    # 验证复制结果
    if [[ -f "$TARGET_DIR/lib/libssl.a" ]] && [[ -f "$TARGET_DIR/lib/libcrypto.a" ]]; then
        log_success "OpenSSL集成成功！"
        log_info "目标目录结构:"
        tree -L 3 "$TARGET_DIR" 2>/dev/null || ls -lh "$TARGET_DIR"
    else
        log_error "OpenSSL集成失败"
        exit 1
    fi
}

# 显示后续步骤
show_next_steps() {
    log_step "🎉 编译完成！"
    
    echo ""
    log_info "后续步骤:"
    echo ""
    echo "1. 打开 DevEco Studio"
    echo "2. 选择项目: $PROJECT_DIR"
    echo "3. 点击 Build > Rebuild Project"
    echo "4. 查看构建日志，确认OpenSSL被正确链接:"
    echo "   ✅ Found OpenSSL:"
    echo "      Headers: .../thirdparty/openssl/arm64-v8a/include"
    echo "      SSL library: .../thirdparty/openssl/arm64-v8a/lib/libssl.a"
    echo "      Crypto library: .../thirdparty/openssl/arm64-v8a/lib/libcrypto.a"
    echo "   ✅ OpenSSL libraries linked successfully"
    echo ""
    echo "5. 使用真机测试VPN功能"
    echo ""
    log_success "祝您开发顺利！"
}

# 主函数
main() {
    log_step "OpenSSL自动编译脚本 for HarmonyOS"
    
    # 检测环境
    detect_os
    detect_package_manager
    
    # 执行编译步骤
    install_dependencies
    check_sdk
    clone_repo
    setup_toolchain
    build_openssl
    integrate_to_project
    show_next_steps
}

# 运行主函数
main "$@"
