#!/bin/bash
# DevCard macOS DMG 打包：构建 → 依赖自检 → Developer ID 签名 → 公证 → DMG → 体积与 SHA256。
#
# 依赖清单不写死：从 otool -L 的实测输出推导。凡不是 /usr/lib 或
# /System/Library 的依赖都必须随包提供，否则直接失败——不产出一个
# 「能打出来但换台机器跑不起来」的 DMG。
#
# 用法：
#   tools/package/make_dmg.sh                # 构建 + 打包
#   tools/package/make_dmg.sh --skip-build   # 用已有 build-new 产物重打包
#   tools/package/make_dmg.sh --out dir      # 指定输出目录（默认 dist/）
#   tools/package/make_dmg.sh --feed-url URL # 指定 Sparkle feed（默认线上 appcast）
#
# Sparkle feed 默认取 https://dl.xiaotu.online/devcard/appcast.xml，也可用
# 环境变量 DEVCARD_SPARKLE_FEED_URL 覆盖；显式传空字符串表示不启用 Sparkle
# （仅用于自测，正式包不要这么做：没有 SUFeedURL 的包永远不会自更新）。
#
# 签名口径见 D13：Developer ID Application 逐层签名 + 公证（notarytool + stapler）；
# 公证凭证存 Keychain profile（默认 devcard-notary），不进脚本与仓库。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DEVCARD_BUILD_DIR:-$REPO_ROOT/build-new}"
OUT_DIR="${DEVCARD_DIST_DIR:-$REPO_ROOT/dist}"
SKIP_BUILD=0
FEED_URL="${DEVCARD_SPARKLE_FEED_URL:-https://dl.xiaotu.online/devcard/appcast.xml}"

usage() {
  sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
    --feed-url)
      [ $# -ge 2 ] || fail "--feed-url 需要一个地址（传空字符串表示不启用 Sparkle）"
      FEED_URL="$2"
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
    -DCMAKE_BUILD_TYPE=Release \
    -DDEVCARD_SPARKLE_FEED_URL="$FEED_URL" >/dev/null
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
EXTERNAL_COUNT="$(grep -v -c -E '^(/usr/lib/|/System/Library/|@rpath/)' "$DEP_LIST" || true)"

# 系统库只核对路径前缀：macOS 11 起 /usr/lib 下的库在 dyld 共享缓存里，
# 磁盘上没有对应文件，用 -e 判断会误报。非系统依赖才检查存在性。
# @rpath 依赖不是绝对路径，直接 [ -e ] 必然失败；本工程的框架统一嵌在
# Contents/Frameworks（可执行文件的 LC_RPATH = @loader_path/../Frameworks），
# 按这个位置核对，缺了才算缺失。
BUNDLED_COUNT=0
while IFS= read -r dep; do
  case "$dep" in
    /usr/lib/*|/System/Library/*) continue ;;
    @rpath/*)
      if [ -e "$APP/Contents/Frameworks/${dep#@rpath/}" ]; then
        BUNDLED_COUNT=$((BUNDLED_COUNT + 1))
      else
        printf '%s\n' "$dep" >> "$MISSING_LIST"
      fi
      continue
      ;;
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
  grep -v -E '^(/usr/lib/|/System/Library/|@rpath/)' "$DEP_LIST" | sed 's/^/  - /' >&2
  fail "依赖自检未通过"
fi

echo "依赖自检：通过（otool 实测 $DEP_TOTAL 项，系统 $SYSTEM_COUNT 项，包内 $BUNDLED_COUNT 项，包外 $EXTERNAL_COUNT 项）"

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

# ---------- 2c. Sparkle feed 自检 ----------
# 没有 SUFeedURL 的包不会报错，只会永远「检查不到更新」——必须在打包时拦住。
if [ -n "$FEED_URL" ]; then
  PLIST_FEED="$(/usr/libexec/PlistBuddy -c 'Print :SUFeedURL' "$PLIST" 2>/dev/null || true)"
  [ "$PLIST_FEED" = "$FEED_URL" ] || \
    fail "Info.plist SUFeedURL（${PLIST_FEED:-空}）与 feed（${FEED_URL}）不一致；--skip-build 时请先重新构建"
  PLIST_PUBKEY="$(/usr/libexec/PlistBuddy -c 'Print :SUPublicEDKey' "$PLIST" 2>/dev/null || true)"
  [ -n "$PLIST_PUBKEY" ] || fail "Info.plist 缺少 SUPublicEDKey，Sparkle 不会启动"
  echo "Sparkle 自检：通过（feed=${PLIST_FEED}）"
else
  echo "[警告] 未指定 Sparkle feed：该包不会自更新（仅用于自测）"
fi

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

# ---------- 3. Developer ID 正式签名 ----------
# 从 Keychain 动态读取 Developer ID Application 身份：证书名称含 "Developer ID Application"
# 不硬编码团队名——续期时名称不变，万一换团队也自动适配。
IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
  | grep 'Developer ID Application' \
  | sed -E 's/^[[:space:]]*[0-9]+\) [A-F0-9]+ "(.+)"/\1/' \
  | head -1)"
[ -n "$IDENTITY" ] || fail "Keychain 里找不到 Developer ID Application 证书（security find-identity -v -p codesigning）"
echo "签名：Developer ID Application（${IDENTITY}）"
# 按 Sparkle 官方要求从内到外逐层签名，禁止 --deep：
# "please do not add --deep ... This is a common source of Sandboxing errors"
# Downloader.xpc 自 2.6 起必须 --preserve-metadata=entitlements，否则其
# entitlements 会被抹掉导致服务无法加载。
# 非沙盒构建已在 CMake POST_BUILD 移除 XPCServices，这里存在才签。
SPARKLE_FW="$APP/Contents/Frameworks/Sparkle.framework"
if [ -d "$SPARKLE_FW" ]; then
  for nested in \
    "$SPARKLE_FW/Versions/B/XPCServices/Installer.xpc" \
    "$SPARKLE_FW/Versions/B/XPCServices/Downloader.xpc" \
    "$SPARKLE_FW/Versions/B/Autoupdate" \
    "$SPARKLE_FW/Versions/B/Updater.app"; do
    [ -e "$nested" ] || continue
    codesign --force --sign "$IDENTITY" --timestamp --options runtime \
      --preserve-metadata=entitlements "$nested"
  done
  codesign --force --sign "$IDENTITY" --timestamp --options runtime "$SPARKLE_FW"
fi
codesign --force --sign "$IDENTITY" --timestamp --options runtime "$APP"
codesign --verify --deep --strict "$APP"
codesign --verify --deep --strict --verbose=2 "$APP" | tail -1 || true
echo "签名校验：通过（codesign --verify --deep --strict）"
spctl --assess --type execute --verbose=2 "$APP" 2>&1 | tail -1 || true

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

# DMG 容器本身也要签：Gatekeeper 对 .dmg 评估的是容器自己的签名，
# 只签 App 会得到 source=no usable signature 而被拒。
codesign --force --sign "$IDENTITY" --timestamp "$DMG"
codesign --verify --strict "$DMG"
echo "DMG 签名：通过（codesign --verify --strict）"

# ---------- 5. 公证 + staple ----------
NOTARY_PROFILE="${DEVCARD_NOTARY_PROFILE:-devcard-notary}"
NOTARY_WAIT_MIN="${DEVCARD_NOTARY_WAIT_MIN:-20}"

echo "公证：提交到 Apple（keychain profile=${NOTARY_PROFILE}，最长等待 ${NOTARY_WAIT_MIN} 分钟）"
# BSD mktemp 只认模板末尾的 X，带后缀（devcard-notary.XXXXXX.log）会直接失败。
NOTARY_LOG="$(mktemp "${TMPDIR:-/tmp}/devcard-notary.XXXXXX")"
xcrun notarytool submit "$DMG" \
  --keychain-profile "$NOTARY_PROFILE" \
  --wait \
  --timeout "$((NOTARY_WAIT_MIN * 60))s" \
  2>&1 | tee "$NOTARY_LOG"

# notarytool exit 0 + 输出含 "Accepted" = 公证通过
grep -q 'Accepted' "$NOTARY_LOG" || {
  echo "[失败] 公证未通过，完整日志：$NOTARY_LOG" >&2
  exit 1
}
echo "公证：通过（Accepted）"

echo "Stapling：给 DMG 贴公证票据"
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
echo "Stapling：通过"

# spctl 最终评估——Gatekeeper 应该报 "accepted" 而非 "rejected" / "unnotarized"
SPCTL_OUT="$(spctl --assess --type open --context context:primary-signature --verbose=4 "$DMG" 2>&1)"
echo "Gatekeeper 评估："
echo "$SPCTL_OUT" | sed 's/^/  /'
echo "$SPCTL_OUT" | grep -q 'accepted' || {
  echo "[失败] Gatekeeper 未 accepted，拒绝原因：" >&2
  echo "$SPCTL_OUT" >&2
  exit 1
}
echo "Gatekeeper：accepted"

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
