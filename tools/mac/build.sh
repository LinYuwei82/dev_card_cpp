#!/bin/bash
# tools/mac/build.sh —— macOS 侧新架构 Release 构建入口（检查 → configure → 构建 → 打印产物）
#
# 用法：
#   tools/mac/build.sh          # 一条龙：前置检查 → configure → Release 构建 → 打印产物绝对路径
#   tools/mac/build.sh --help   # 打印这份说明
#
# 退出码（与 tools/verify/*.sh 沿用同一套语义）：
#   0 通过
#   1 失败（configure/构建失败，或仓库里应有的文件缺失）
#   2 保留：本脚本不产出「待确认」状态
#   3 环境不具备（非 macOS、架构不认识、缺 cmake/ninja/clang/Xcode CLT、cmake 版本太旧）
#
# 与既有脚本的调用关系（不复制实现）：
#   configure 走 CMakePresets.json 的 macos-release preset（Ninja + DEVCARD_NEW_ARCH=ON
#   + CMAKE_BUILD_TYPE=Release），产物落在仓库下 build-new/，与 tools/package/make_dmg.sh
#   的默认 BUILD_DIR（<repo>/build-new）是同一目录。本脚本只负责「编出来」；DMG 打包
#   （依赖自检、ad-hoc 签名、hdiutil）仍由 make_dmg.sh 负责，构建完直接复用产物：
#       tools/package/make_dmg.sh --skip-build
#   本脚本不反过来调用 make_dmg.sh：构建入口与发布入口分开，避免每次构建都触发打包。
#
# 兼容性：macOS 自带 /bin/bash 是 3.2。本脚本不用 mapfile/readarray、关联数组、declare -A、
#   ${var,,}、readlink -f、stat -c、grep -P、date -d、sed -i 一类 bash4/GNU 专有写法；
#   所有路径引用都加引号，可在含中文与空格的目录下运行。
#
# 【未跑 shellcheck】编写本脚本的机器（Windows）没有安装 shellcheck，因此本脚本没有任何
#   shellcheck 结论，不要当成已通过。mac 上首次运行前请自行执行：shellcheck tools/mac/build.sh
#
# 【以下是「假设」，尚未在 macOS 上真实执行过】mac 上第一次运行请以实际输出为准修正，
# 不要把这里当成已验结论：
#   1. 假设产物布局是 build-new/shell/DevCard.app（目标名来自 shell/CMakeLists.txt 的
#      add_executable(DevCard MACOSX_BUNDLE ...)）与 build-new/server/devcard-server
#      （目标由 server/CMakeLists.txt 定义）；脚本跑完会逐个校验存在性，不符就失败。
#   2. 假设 preset 名 macos-release、generator Ninja 不再变化（来源：CMakePresets.json）。
#   3. 假设本机已装 Ninja：preset 指定了它，但仓库不负责安装；没装按「环境不具备」退出。
#   4. 假设 CMake >= 3.21 是 preset 机制的硬要求（来源：CMakePresets.json 的
#      cmakeMinimumRequired）；只在低于该版本时阻断。
#   5. 假设支持口径仍是 Apple Silicon + macOS 15+（README §2）：Intel 只提示、不阻断，
#      因为「Intel 未验证」是文档事实，不是本脚本能判定的环境缺陷。
#   6. 查分支状态一律用 origin/<branch>，不要用本地同名分支——本地分支不保证与远端同步。
#      踩过的坑：2026-11-15 用本地 win/m7-server（停在 cee94fe）判定「tools/verify 不存在」，
#      而远端此时早已有；git ls-tree <本地分支>:path 还会直接报 Not a valid object name。

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

PRESET="macos-release"
# preset 的 binaryDir 是 ${sourceDir}/build-new；这里写常量是为了在产物路径缺失时
# 能给出可读的报错。若将来改了 preset，这里要同步改（脚本会在缺失时报错，不会静默）。
BUILD_DIR="$REPO_ROOT/build-new"
CMAKE_MIN_VERSION="3.21"

say() { printf '%s\n' "$*"; }
note() { printf '  %s\n' "$*"; }

fail() {
  printf '\n[失败] %s\n' "$*" >&2
  exit 1
}

env_missing() {
  printf '\n[环境不具备] %s\n' "$*" >&2
  exit 3
}

usage() {
  sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
  -h | --help)
    usage
    exit 0
    ;;
  "") : ;;
  *) fail "未知参数：${1}（用 --help 查看用法）" ;;
esac

# ---------- 1. 前置检查 ----------
HOST_OS=$(uname -s)
if [ "$HOST_OS" != "Darwin" ]; then
  env_missing "build.sh 只支持 macOS（当前 uname -s = ${HOST_OS}）；Windows 侧请用 shell/win 下的脚本。"
fi

ARCH=$(uname -m)
case "$ARCH" in
  arm64) note "架构：arm64（Apple Silicon，本轮支持口径）" ;;
  x86_64) note "架构：x86_64（Intel）：README §2 只承诺 Apple Silicon，产物能否运行需自行实测" ;;
  *) env_missing "未知架构：${ARCH}（本脚本只认 arm64 / x86_64）" ;;
esac

XCODE_CLT_PATH=$(xcode-select -p 2>/dev/null || true)
if [ -z "$XCODE_CLT_PATH" ]; then
  env_missing "找不到 Xcode Command Line Tools（xcode-select -p 无输出）。先执行：xcode-select --install"
fi
note "Xcode CLT：${XCODE_CLT_PATH}"
if ! command -v clang >/dev/null 2>&1; then
  env_missing "PATH 里没有 clang（Xcode CLT 没装好或 xcode-select 指错了目录）"
fi

if ! command -v cmake >/dev/null 2>&1; then
  env_missing "缺少 cmake。preset 机制要求 CMake >= ${CMAKE_MIN_VERSION}。"
fi

# 版本号逐段比较：a >= b 返回 0。不用 sort -V（macOS 的 sort 没有这个选项）。
version_ge() {
  awk -v a="$1" -v b="$2" 'BEGIN {
    na = split(a, A, "[.]")
    nb = split(b, B, "[.]")
    n = (na > nb) ? na : nb
    for (i = 1; i <= n; i++) {
      x = (i <= na) ? A[i] + 0 : 0
      y = (i <= nb) ? B[i] + 0 : 0
      if (x > y) exit 0
      if (x < y) exit 1
    }
    exit 0
  }'
}

CMAKE_VERSION=$(cmake --version 2>/dev/null | head -n 1 | sed -n 's/^cmake version //p')
if [ -z "$CMAKE_VERSION" ]; then
  env_missing "读不出 cmake 版本（cmake --version 输出异常）"
fi
if ! version_ge "$CMAKE_VERSION" "$CMAKE_MIN_VERSION"; then
  env_missing "cmake ${CMAKE_VERSION} 太旧，preset 需要 >= ${CMAKE_MIN_VERSION}"
fi
note "cmake：${CMAKE_VERSION}"

if ! command -v ninja >/dev/null 2>&1; then
  env_missing "缺少 ninja（preset ${PRESET} 的 generator 是 Ninja；可 brew install ninja）"
fi
note "ninja：$(command -v ninja)"

# 版本号唯一来源：仓库根 VERSION.txt。这里不出现任何版本字面量。
VERSION_FILE="$REPO_ROOT/VERSION.txt"
[ -f "$VERSION_FILE" ] || fail "找不到版本文件：$VERSION_FILE（仓库不完整？）"
VERSION=$(tr -d '[:space:]' < "$VERSION_FILE")
[ -n "$VERSION" ] || fail "VERSION.txt 为空：$VERSION_FILE"
note "版本：${VERSION}（来源 VERSION.txt）"

if ! command -v sysctl >/dev/null 2>&1; then
  JOBS=4
else
  JOBS=$(sysctl -n hw.ncpu 2>/dev/null || printf '4')
  case "$JOBS" in
    '' | *[!0-9]*) JOBS=4 ;;
  esac
fi
[ "$JOBS" -ge 1 ] || JOBS=4

say ""
say "前置检查：通过（Darwin / ${ARCH} / cmake ${CMAKE_VERSION} / ninja / 版本 ${VERSION}）"

# ---------- 2. configure ----------
# DEVCARD_NEW_ARCH=ON 由 preset 携带，不再在命令行重复一遍，避免两处真值。
say ""
say "configure：cmake --preset ${PRESET}（build-new，Release，DEVCARD_NEW_ARCH=ON）"
if ! (cd "$REPO_ROOT" && cmake --preset "$PRESET"); then
  fail "configure 未通过（preset ${PRESET}）。看上面的 CMake 输出；常见是缺 Xcode CLT 或字体子集文件。"
fi

# ---------- 3. Release 构建 ----------
say ""
say "构建：cmake --build --preset ${PRESET} --parallel ${JOBS}"
if ! (cd "$REPO_ROOT" && cmake --build --preset "$PRESET" --parallel "$JOBS"); then
  fail "构建失败（preset ${PRESET}）。完整错误见上面的编译输出。"
fi

# ---------- 4. 打印产物绝对路径 ----------
APP="$BUILD_DIR/shell/DevCard.app"
APP_BIN="$APP/Contents/MacOS/DevCard"
APP_PLIST="$APP/Contents/Info.plist"
SERVER_BIN="$BUILD_DIR/server/devcard-server"

[ -d "$APP" ] || fail "构建结束但找不到应用包：${APP}（本脚本假设的产物布局见文件头；若 preset 的 binaryDir 变了请同步 BUILD_DIR）"
[ -x "$APP_BIN" ] || fail "应用包里没有可执行文件：${APP_BIN}"
[ -f "$APP_PLIST" ] || fail "应用包里缺少 Info.plist：${APP_PLIST}"
[ -x "$SERVER_BIN" ] || fail "找不到服务端产物：${SERVER_BIN}"

say ""
say "构建完成（版本 ${VERSION}，来源 VERSION.txt）"
say "产物（绝对路径）："
note "应用包：${APP}"
note "可执行：${APP_BIN}"
note "服务端：${SERVER_BIN}"
say ""
say "打包（仍走既有脚本，本脚本不复制 DMG 实现，产物目录已对齐）："
note "\"${REPO_ROOT}/tools/package/make_dmg.sh\" --skip-build"
