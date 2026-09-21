# Download precompiled OpenSSL for HarmonyOS
# This script downloads precompiled OpenSSL libraries from a trusted source

param(
    [string]$TargetArch = "arm64-v8a"
)

$ErrorActionPreference = "Stop"

Write-Host "=== OpenSSL Download Script for HarmonyOS ===" -ForegroundColor Cyan
Write-Host ""

# Check if we're in the project root
if (-not (Test-Path "entry\src\main\cpp\CMakeLists.txt")) {
    Write-Error "Please run this script from the project root directory!"
    exit 1
}

# Create thirdparty directory
$opensslDir = "entry\src\main\cpp\thirdparty\openssl\$TargetArch"
if (-not (Test-Path $opensslDir)) {
    New-Item -ItemType Directory -Path $opensslDir -Force | Out-Null
    Write-Host "✅ Created directory: $opensslDir" -ForegroundColor Green
}

# Note: Precompiled OpenSSL libraries need to be obtained from:
# 1. Compile using lycium (recommended)
# 2. Download from a trusted third-party source
# 3. Use system-provided OpenSSL if available

Write-Host ""
Write-Host "⚠️  Precompiled OpenSSL libraries are not available for download." -ForegroundColor Yellow
Write-Host "    You need to compile OpenSSL using lycium:" -ForegroundColor Yellow
Write-Host ""
Write-Host "    1. Prepare Linux environment (Ubuntu 22.04)" -ForegroundColor White
Write-Host "    2. git clone https://gitee.com/openharmony/tpc_c_cplusplus.git" -ForegroundColor White
Write-Host "    3. cd tpc_c_cplusplus/lycium && ./build.sh openssl" -ForegroundColor White
Write-Host "    4. Copy lycium/usr/openssl to entry/src/main/cpp/thirdparty/openssl/" -ForegroundColor White
Write-Host ""
Write-Host "    After copying, the directory structure should be:" -ForegroundColor Yellow
Write-Host "    entry/src/main/cpp/thirdparty/openssl/$TargetArch/" -ForegroundColor White
Write-Host "    ├── include/" -ForegroundColor White
Write-Host "    │   └── openssl/" -ForegroundColor White
Write-Host "    │       ├── ssl.h" -ForegroundColor White
Write-Host "    │       ├── evp.h" -ForegroundColor White
Write-Host "    │       └── ..." -ForegroundColor White
Write-Host "    └── lib/" -ForegroundColor White
Write-Host "        ├── libssl.a" -ForegroundColor White
Write-Host "        └── libcrypto.a" -ForegroundColor White
Write-Host ""

exit 0
