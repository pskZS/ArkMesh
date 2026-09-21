#!/bin/bash
# 构建 ArkMesh 的 Go 核心 (openharmony/arm64, c-shared) 并直接落到 OHOS 工程里
# 注意: CGO 标志必须用白名单允许的分参写法, 路径用 8.3 短名避免空格
set -e

# 用户需要通过环境变量配置:
#   GOROOT    - OHOS 定制 Go 工具链路径 (例如 /path/to/ohos-go126/goroot)
#   OHOS_NDK  - HarmonyOS NDK 路径 (例如 /path/to/DevEco-Studio/sdk/default/openharmony/native)
# 示例:
#   export GOROOT=/path/to/ohos-go126/goroot
#   export OHOS_NDK=/path/to/DevEco-Studio/sdk/default/openharmony/native
#   export PATH="$GOROOT/bin:$PATH"

: "${GOROOT:?请设置 GOROOT 环境变量 (需要 OHOS 定制 Go 工具链)}"
: "${OHOS_NDK:?请设置 OHOS_NDK 环境变量}"

export PATH="$GOROOT/bin:$PATH"

export GOOS=openharmony
export GOARCH=arm64
export CGO_ENABLED=1
export CC="$OHOS_NDK/llvm/bin/clang"
export CGO_CFLAGS="-target aarch64-linux-ohos --sysroot $OHOS_NDK/sysroot -D __MUSL__"
export CGO_LDFLAGS="-target aarch64-linux-ohos --sysroot $OHOS_NDK/sysroot"

# 必须用本地工具链: 自动下载的官方工具链没有 OHOS 补丁
export GOTOOLCHAIN=local
export GOPROXY=https://goproxy.cn,direct
export GOSUMDB=off

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$SRC_DIR/../../ohos/entry/src/main/cpp/third_party/go/arm64-v8a"
STRIP="$OHOS_NDK/llvm/bin/llvm-strip"

cd "$SRC_DIR"
echo "=== go version ==="
go version
echo "=== build ==="
go build -buildmode=c-shared -o libarkmesh_core.so .

echo "=== strip (去掉 DWARF, 保留 symtab) ==="
cp libarkmesh_core.so "$DEST_DIR/libarkmesh_core.so"
cp libarkmesh_core.h "$DEST_DIR/libarkmesh_core.h"
"$STRIP" --strip-debug "$DEST_DIR/libarkmesh_core.so"

echo "=== 校验: TLS 重定位必须为 0 ==="
TLS_COUNT=$("$OHOS_NDK/llvm/bin/llvm-readelf" -r "$DEST_DIR/libarkmesh_core.so" | grep -ci tls || true)
echo "TLS relocs = $TLS_COUNT"
if [ "$TLS_COUNT" != "0" ]; then
    echo "!! 存在 TLS 重定位, OHOS 加载器会拒绝 dlopen"
    exit 1
fi

echo "=== 产物 ==="
ls -lh "$DEST_DIR/libarkmesh_core.so"
