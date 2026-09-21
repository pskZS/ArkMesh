#!/usr/bin/env bash
# Build OpenSSL for HarmonyOS and install it into the path expected by CMake.
#
# Usage:
#   ./build_openssl.sh [arm64-v8a|x86_64]
#
# The output is written to:
#   <script_dir>/openssl_install/<arch>/{include,lib}
#
# CMakeLists.txt searches this location (among others).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENSSL_DIR="${SCRIPT_DIR}/openssl"
INSTALL_ROOT="${SCRIPT_DIR}/openssl_install"

ARCH="${1:-arm64-v8a}"
case "$ARCH" in
  arm64-v8a)
    OPENSSL_TARGET="linux-aarch64"
    CLANG_TARGET="aarch64-linux-ohos"
    ;;
  x86_64)
    OPENSSL_TARGET="linux-x86_64"
    CLANG_TARGET="x86_64-linux-ohos"
    ;;
  *)
    echo "Unsupported architecture: ${ARCH}" >&2
    echo "Supported: arm64-v8a, x86_64" >&2
    exit 1
    ;;
esac

# Resolve the OpenHarmony SDK. Prefer the WSL/Linux convention, then
# DevEco Studio's Windows path.
OHOS_SDK="${OHOS_SDK:-$DEVECO_SDK_HOME}"
if [ -z "$OHOS_SDK" ]; then
  OHOS_SDK="D:/Program Files/Huawei/DevEco Studio/sdk"
fi

NDK_ROOT=""
for candidate in \
  "${OHOS_SDK}/native" \
  "${OHOS_SDK}/default/openharmony/native" \
  "${OHOS_SDK}/openharmony/native"; do
  if [ -d "$candidate" ]; then
    NDK_ROOT="$candidate"
    break
  fi
done

if [ -z "$NDK_ROOT" ]; then
  echo "OpenHarmony native SDK not found under ${OHOS_SDK}" >&2
  echo "Set OHOS_SDK to your SDK root and try again." >&2
  exit 1
fi

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    EXE=".exe"
    ;;
  *)
    EXE=""
    ;;
esac

CC="${NDK_ROOT}/llvm/bin/clang${EXE}"
CXX="${NDK_ROOT}/llvm/bin/clang++${EXE}"
AR="${NDK_ROOT}/llvm/bin/llvm-ar${EXE}"
RANLIB="${NDK_ROOT}/llvm/bin/llvm-ranlib${EXE}"

if [ ! -x "$CC" ]; then
  echo "Compiler not found: ${CC}" >&2
  exit 1
fi

INSTALL_DIR="${INSTALL_ROOT}/${ARCH}"
mkdir -p "$INSTALL_DIR"

cd "$OPENSSL_DIR"

perl Configure "$OPENSSL_TARGET" \
  --prefix="$INSTALL_DIR" \
  --openssldir="$INSTALL_DIR" \
  no-shared \
  no-tests \
  CC="$CC" \
  CXX="$CXX" \
  AR="$AR" \
  RANLIB="$RANLIB" \
  CFLAGS="--target=${CLANG_TARGET} -fPIC" \
  CXXFLAGS="--target=${CLANG_TARGET} -fPIC"

make -j"$(nproc 2>/dev/null || echo 2)"
make install_sw

echo "OpenSSL ${ARCH} installed to ${INSTALL_DIR}"
