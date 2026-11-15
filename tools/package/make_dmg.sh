#!/bin/bash
# DevCard macOS DMG 打包：构建 → 依赖自检 → ad-hoc 签名 → DMG → 体积与 SHA256。
#
# 依赖清单不写死：从 otool -L 的实测输出推导。凡不是 /usr/lib 或
# /System/Library 的依赖都必须随包提供，否则直接失败——不产出一个
# 「能打出来但换台机器跑不起来」的 DMG。
#
# 用法：
#   tools/package/make_dmg.sh                # 构建 + 打包
#   tools/package/make_dmg.sh --skip-build   # 用已有 build-new 产物重打包
#   tools/package/make_dmg.sh --out dir      # 指定输出目录（默认 dist/）
#
# 签名口径见 D13：ad-hoc（codesign -s -），不公证；首次打开需 Gatekeeper 放行。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DEVCARD_BUILD_DIR:-$REPO_ROOT/build-new}"
OUT_DIR="${DEVCARD_DIST_DIR:-$REPO_ROOT/dist}"
SKIP_BUILD=0

usage() {
  sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

fail() {
  echo "" >&2
  echo "[失败] $*" >&2
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1 ;;
    --out)
      [ $# -ge 2 ] || fail "--out 需要一个目录参数"
      OUT_DIR="$2"
      shift
      ;;
    --build-dir)
      [ $# -ge 2 ] || fail "--build-dir 需要一个目录参数"
      BUILD_DIR="$2"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "未知参数：${1}（用 --help 查看用法）"
      ;;
  esac
  shift
done

[ "$(uname -s)" = "Darwin" ] || fail "make_dmg.sh 只能在 macOS 上运行"

for tool in cmake otool codesign hdiutil shasum; do
  command -v "$tool" >/dev/null 2>&1 || fail "缺少命令：$tool"
done

VERSION_FILE="$REPO_ROOT/VERSION.txt"
[ -f "$VERSION_FILE" ] || fail "找不到版本文件：$VERSION_FILE"
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || fail "VERSION.txt 为空"
echo "版本：${VERSION}（来源 VERSION.txt）"

APP="$BUILD_DIR/shell/DevCard.app"
BIN="$APP/Contents/MacOS/DevCard"
PLIST="$APP/Contents/Info.plist"

# ---------- 1. 构建 ----------
if [ "$SKIP_BUILD" -eq 0 ]; then
  echo "构建：${BUILD_DIR}（Release）"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR" --target DevCard --parallel "$(sysctl -n hw.ncpu)" \
    >/dev/null
else
  echo "构建：跳过（--skip-build）"
fi

[ -d "$APP" ] || fail "找不到应用包：${APP}（先不带 --skip-build 跑一次）"
[ -x "$BIN" ] || fail "应用包内没有可执行文件：$BIN"
[ -f "$PLIST" ] || fail "应用包内缺少 Info.plist"

# ---------- 2. 依赖自检（按 otool 实测清单，不写死） ----------
DEP_LIST="$(mktemp "${TMPDIR:-/tmp}/devcard-deps.XXXXXX")"
MISSING_LIST="$(mktemp "${TMPDIR:-/tmp}/devcard-missing.XXXXXX")"
trap 'rm -f "$DEP_LIST" "$MISSING_LIST"; [ -n "${STAGE:-}" ] && rm -rf "$STAGE"' EXIT

otool -L "$BIN" | tail -n +2 | awk '{print $1}' > "$DEP_LIST"
DEP_TOTAL="$(wc -l < "$DEP_LIST" | tr -d '[:space:]')"
[ "$DEP_TOTAL" -gt 0 ] || fail "otool 没有读到任何动态依赖：$BIN"

SYSTEM_COUNT="$(grep -c -E '^(/usr/lib/|/System/Library/)' "$DEP_LIST" || true)"
EXTERNAL_COUNT="$(grep -v -c -E '^(/usr/lib/|/System/Library/)' "$DEP_LIST" || true)"

# 系统库只核对路径前缀：macOS 11 起 /usr/lib 下的库在 dyld 共享缓存里，
# 磁盘上没有对应文件，用 -e 判断会误报。非系统依赖才检查存在性。
while IFS= read -r dep; do
  case "$dep" in
    /usr/lib/*|/System/Library/*) continue ;;
  esac
  [ -e "$dep" ] || printf '%s\n' "$dep" >> "$MISSING_LIST"
done < "$DEP_LIST"

if [ -s "$MISSING_LIST" ]; then
  echo "[失败] 以下依赖在本机不存在：" >&2
  sed 's/^/  - /' "$MISSING_LIST" >&2
  fail "依赖自检未通过"
fi

if [ "$EXTERNAL_COUNT" -gt 0 ]; then
  echo "[失败] 以下依赖不在系统路径，换台机器会找不到（打包前应收进 .app）：" >&2
  grep -v -E '^(/usr/lib/|/System/Library/)' "$DEP_LIST" | sed 's/^/  - /' >&2
  fail "依赖自检未通过"
fi

echo "依赖自检：通过（otool 实测 $DEP_TOTAL 项，系统 $SYSTEM_COUNT 项，包外 $EXTERNAL_COUNT 项）"

PLIST_SHORT="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST" 2>/dev/null || true)"
PLIST_BUILD="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$PLIST" 2>/dev/null || true)"
# 版本真值只有 VERSION.txt：新架构的 Info.plist 由 CMake 用
# MACOSX_BUNDLE_SHORT_VERSION_STRING / MACOSX_BUNDLE_BUNDLE_VERSION 生成，
# 这里只校验产物，不在这里回写——脚本写入再自己检查等于把门禁变成同义反复。
[ "$PLIST_SHORT" = "$VERSION" ] || \
  fail "Info.plist CFBundleShortVersionString（${PLIST_SHORT:-空}）与 VERSION.txt（${VERSION}）不一致"
[ "$PLIST_BUILD" = "$VERSION" ] || \
  fail "Info.plist CFBundleVersion（${PLIST_BUILD:-空}）与 VERSION.txt（${VERSION}）不一致"
if find "$APP" -type f \( -name '*.db' -o -name '*.db-wal' -o -name '*.db-shm' \) | grep -q .; then
  fail "应用包里混入了数据库文件"
fi
echo "包内容检查：通过（Info.plist 版本 ${PLIST_SHORT}/${PLIST_BUILD}，无运行期数据）"

# ---------- 2b. 子集字体随包自检 ----------
# 界面与打印共用 web/fonts 下的同一份字体；没进包就会回落到系统字体，
# 两个平台印出来的卡不一致——按 bug 处理，不能只警告。
FONT_DIR="$APP/Contents/Resources/fonts"
for font in NotoSansSC-Regular.subset.ttf NotoSansSC-Bold.subset.ttf; do
  SRC_FONT="$REPO_ROOT/web/fonts/$font"
  DST_FONT="$FONT_DIR/$font"
  [ -f "$SRC_FONT" ] || fail "仓库缺少子集字体：${SRC_FONT}（先跑 tools/fonts/make_subset.sh）"
  [ -f "$DST_FONT" ] || fail "字体没进包：${DST_FONT}（检查 shell/CMakeLists.txt 的 POST_BUILD）"
  cmp -s "$SRC_FONT" "$DST_FONT" || fail "包内字体与仓库不一致：${DST_FONT}"
  echo "  字体 ${font}  $(stat -f%z "$DST_FONT") bytes  $(shasum -a 256 "$DST_FONT" | awk '{print $1}')"
done
echo "字体自检：通过（Resources/fonts 与 web/fonts 逐字节一致）"

# ---------- 3. ad-hoc 签名 ----------
echo "签名：ad-hoc（codesign -s -）"
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"
echo "签名校验：通过（codesign --verify --deep --strict）"

# ---------- 4. DMG ----------
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/devcard-dmg.XXXXXX")"
cp -R "$APP" "$STAGE/DevCard.app"
ln -s /Applications "$STAGE/Applications"

mkdir -p "$OUT_DIR"
DMG="$OUT_DIR/DevCard-$VERSION-macos-$(uname -m).dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "DevCard $VERSION" -srcfolder "$STAGE" \
  -ov -format UDZO "$DMG"
hdiutil verify -quiet "$DMG"

BYTES="$(stat -f%z "$DMG")"
HUMAN="$(du -h "$DMG" | awk '{print $1}')"
SHA="$(shasum -a 256 "$DMG" | awk '{print $1}')"
# 身份看 App 内容层，不看 DMG 容器：hdiutil 每次新造 GUID，容器字节不可复现。
BIN_SHA="$(shasum -a 256 "$BIN" | awk '{print $1}')"
PLIST_SHA="$(shasum -a 256 "$PLIST" | awk '{print $1}')"

echo ""
echo "打包完成"
echo "  产物：$DMG"
echo "  版本：$VERSION"
echo "  体积：$BYTES bytes（${HUMAN}）"
echo "  SHA256（DMG，仅作传输校验，不作身份）：$SHA"
echo "  身份（App 层）："
echo "    可执行文件 $BIN"
echo "      SHA256：$BIN_SHA"
echo "    Info.plist $PLIST"
echo "      SHA256：$PLIST_SHA"

if [ "$SKIP_BUILD" -eq 1 ]; then
  echo ""
  echo "--skip-build 复用说明（缺任一项，该产物的体积与哈希都不可比）"
  COMMIT="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
  if [ -n "$COMMIT" ]; then
    echo "  复用的 $BUILD_DIR 对应 commit：$COMMIT"
  else
    echo "  [警告] 取不到 commit：该产物的体积与哈希不可比，不得写进基线"
  fi
  echo "  bundle 内可执行文件 SHA256：$BIN_SHA"
fi
