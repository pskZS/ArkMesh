# ============================================================================
# WSL2环境设置和OpenSSL编译脚本
# 
# 使用方法：
# 以管理员身份运行PowerShell，然后执行：
# .\setup_wsl_and_build.ps1
# ============================================================================

param(
    [string]$SdkPath = "",
    [switch]$SkipWslInstall = $false
)

$ErrorActionPreference = "Stop"

# 颜色输出函数
function Log-Info {
    Write-Host "[INFO] $args" -ForegroundColor Cyan
}

function Log-Success {
    Write-Host "[SUCCESS] $args" -ForegroundColor Green
}

function Log-Warning {
    Write-Host "[WARNING] $args" -ForegroundColor Yellow
}

function Log-Error {
    Write-Host "[ERROR] $args" -ForegroundColor Red
}

function Log-Step {
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "$args" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
}

# 检查是否以管理员身份运行
function Check-Admin {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentUser)
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    
    if (-not $isAdmin) {
        Log-Error "请以管理员身份运行此脚本！"
        Log-Info "右键点击PowerShell，选择'以管理员身份运行'"
        exit 1
    }
    
    Log-Success "已确认管理员权限"
}

# 检查WSL2是否已安装
function Check-WSL {
    Log-Step "步骤1: 检查WSL2环境"
    
    try {
        $wslList = wsl --list --verbose 2>&1
        if ($wslList -match "Ubuntu") {
            Log-Success "WSL2已安装"
            Log-Info $wslList
            return $true
        }
    }
    catch {
        Log-Warning "WSL2未安装"
        return $false
    }
    
    return $false
}

# 安装WSL2
function Install-WSL {
    Log-Step "步骤2: 安装WSL2"
    
    if ($SkipWslInstall) {
        Log-Warning "跳过WSL2安装（--SkipWslInstall参数）"
        return
    }
    
    Log-Info "正在安装WSL2和Ubuntu-22.04..."
    Log-Warning "此过程可能需要几分钟，请耐心等待"
    
    try {
        # 安装WSL2
        wsl --install -d Ubuntu-22.04
        
        Log-Success "WSL2安装完成"
        Log-Warning "需要重启计算机才能完成安装"
        
        $restart = Read-Host "是否立即重启计算机? (y/n)"
        if ($restart -eq "y") {
            Restart-Computer
        }
        else {
            Log-Info "请手动重启计算机后，重新运行此脚本"
            exit 0
        }
    }
    catch {
        Log-Error "WSL2安装失败: $_"
        Log-Info "请尝试手动安装:"
        Log-Info "1. 打开PowerShell（管理员）"
        Log-Info "2. 执行: wsl --install -d Ubuntu-22.04"
        Log-Info "3. 重启计算机"
        exit 1
    }
}

# 配置WSL2
function Configure-WSL {
    Log-Step "步骤3: 配置WSL2环境"
    
    Log-Info "正在配置WSL2..."
    
    # 设置WSL2为默认版本
    wsl --set-default-version 2
    
    # 设置Ubuntu-22.04为默认发行版
    try {
        wsl --set-default Ubuntu-22.04
    }
    catch {
        Log-Warning "设置默认发行版失败，继续..."
    }
    
    Log-Success "WSL2配置完成"
}

# 复制编译脚本到WSL2
function Copy-BuildScript {
    Log-Step "步骤4: 复制编译脚本到WSL2"
    
    $scriptDir = $PSScriptRoot
    if (-not $scriptDir) {
        $scriptDir = Get-Location
    }
    
    $buildScript = Join-Path $scriptDir "build_openssl.sh"
    
    if (-not (Test-Path $buildScript)) {
        Log-Error "未找到编译脚本: $buildScript"
        exit 1
    }
    
    # 复制脚本到WSL2
    Log-Info "复制编译脚本到WSL2..."
    
    # 转换Windows路径为WSL2路径
    $wslPath = $scriptDir.Replace('\', '/').Replace(':', '').ToLower()
    $wslScriptPath = "/mnt/$wslPath/build_openssl.sh"
    
    wsl cp "$wslScriptPath" ~/build_openssl.sh
    wsl chmod +x ~/build_openssl.sh
    
    Log-Success "编译脚本已复制"
}

# 执行编译
function Execute-Build {
    Log-Step "步骤5: 执行OpenSSL编译"
    
    Log-Info "开始在WSL2中编译OpenSSL..."
    Log-Warning "此过程需要20-30分钟，请耐心等待"
    Log-Info "如果需要输入SDK路径，请输入Linux格式的路径"
    Log-Info "例如: /mnt/d/path/to/harmonyos/sdk"
    
    # 执行编译脚本
    wsl bash -c "cd ~ && ./build_openssl.sh"
    
    Log-Success "OpenSSL编译完成"
}

# 验证编译结果
function Verify-Build {
    Log-Step "步骤6: 验证编译结果"
    
    $targetDir = "entry\src\main\cpp\thirdparty\openssl\arm64-v8a"
    
    if ((Test-Path "$targetDir\lib\libssl.a") -and (Test-Path "$targetDir\lib\libcrypto.a")) {
        Log-Success "OpenSSL库文件已正确集成"
        Log-Info "目录结构:"
        Get-ChildItem $targetDir -Recurse | Select-Object FullName
    }
    else {
        Log-Warning "未找到OpenSSL库文件"
        Log-Info "请检查编译日志，确认是否有错误"
    }
}

# 显示后续步骤
function Show-NextSteps {
    Log-Step "🎉 设置完成！"
    
    Write-Output ""
    Log-Info "后续步骤:"
    Write-Output ""
    Write-Output "1. 打开 DevEco Studio"
    Write-Output "2. 选择项目: $PWD"
    Write-Output "3. 点击 Build > Rebuild Project"
    Write-Output "4. 查看构建日志，确认OpenSSL被正确链接:"
    Write-Output "   ✅ Found OpenSSL:"
    Write-Output "      Headers: .../thirdparty/openssl/arm64-v8a/include"
    Write-Output "      SSL library: .../thirdparty/openssl/arm64-v8a/lib/libssl.a"
    Write-Output "      Crypto library: .../thirdparty/openssl/arm64-v8a/lib/libcrypto.a"
    Write-Output "   ✅ OpenSSL libraries linked successfully"
    Write-Output ""
    Write-Output "5. 使用真机测试VPN功能"
    Write-Output ""
    Log-Success "祝您开发顺利！"
}

# 主函数
function Main {
    Log-Step "WSL2环境设置和OpenSSL编译脚本"
    
    # 检查管理员权限
    Check-Admin
    
    # 检查并安装WSL2
    $wslInstalled = Check-WSL
    if (-not $wslInstalled) {
        Install-WSL
    }
    
    # 配置WSL2
    Configure-WSL
    
    # 复制编译脚本
    Copy-BuildScript
    
    # 执行编译
    Execute-Build
    
    # 验证编译结果
    Verify-Build
    
    # 显示后续步骤
    Show-NextSteps
}

# 运行主函数
Main
