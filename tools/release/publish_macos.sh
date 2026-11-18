#!/bin/bash
# DevCard macOS 一条命令发布：出包 → DMG 签名 → appcast + manifest.json（v2）→ 上传 OSS。
#
# 清单为统一 Schema v2（见 mote/docs/更新清单Schema-v2.md）：manifest.json
# （platforms.macos，Ed25519 签名）；appcast.xml 是 Sparkle 专用，不在 v2 范围。
# 本脚本不再生成 latest.json，旧内测客户端按切换方案重装或经 Sparkle 升级。
#
# 用法：
#   tools/release/publish_macos.sh                    # 构建并发布
#   tools/release/publish_macos.sh --skip-build       # 复用已有 build-new 产物
#   tools/release/publish_macos.sh --notes "更新说明"  # 写进 appcast 与 notes_url
#   tools/release/publish_macos.sh --dry-run          # 只出包与清单，不上传
#
# 前置条件：
#   - 公证凭证在 Keychain profile devcard-notary（make_dmg.sh 负责签名与公证）
#   - Sparkle 私钥在 ~/.config/devcard/sparkle-keys/private_key.pem（appcast 用）
#   - v2 清单签名种子在 ~/.config/devcard/manifest-keys/ed25519_seed.b64
#   - OSS 凭据在 ~/.config/devcard/release.env（OSS_ACCESS_KEY_ID 等）
#
# 版本只认仓库根目录 VERSION.txt。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION.txt")"
DIST="${DEVCARD_DIST_DIR:-$REPO_ROOT/dist}"
BASE_URL="https://dl.xiaotu.online/devcard"
CHANNEL="stable"
ARCH="$(uname -m)"
NOTES=""
DRY_RUN=0
DMG_ARGS=()

usage() {
    sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

fail() {
    echo "" >&2
    echo "[失败] $*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) DMG_ARGS+=(--skip-build) ;;
        --notes)
            [ $# -ge 2 ] || fail "--notes 需要一个参数"
            NOTES="$2"
            shift
            ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "未知参数：$1（见 --help）" ;;
    esac
    shift
done

[ -n "$VERSION" ] || fail "VERSION.txt 为空"
[ -f "$REPO_ROOT/VERSION.txt" ] || fail "找不到 VERSION.txt"
command -v python3 >/dev/null || fail "需要 python3"

# v2 清单签名种子（manifest.json 用；Sparkle 私钥只管 appcast，两者不能混用）
SIGN_KEY="${DEVCARD_SIGN_KEY:-$HOME/.config/devcard/manifest-keys/ed25519_seed.b64}"
[ -f "$SIGN_KEY" ] || fail "v2 清单签名种子不存在：$SIGN_KEY"
KEY_ID="${KEY_ID:-2026-11}"
MIN_OS="${MIN_OS:-11.0}"

# v2 安装包命名：<product>-<version>-<os>-<arch>-<type>.<ext>；dmg 固定 macos-arm64
if [ "$ARCH" != "arm64" ]; then
    fail "v2 命名固定 -macos-arm64，本机架构为 $ARCH；请在 Apple Silicon 上出包"
fi
DMG="$DIST/DevCard-$VERSION-macos-$ARCH.dmg"
V2_DMG_NAME="devcard-$VERSION-macos-arm64.dmg"
V2_DMG="$DIST/$V2_DMG_NAME"
MANIFEST="$DIST/manifest.json"
APPCAST="$DIST/appcast.xml"

echo "===== 1/5 出包"
"$REPO_ROOT/tools/package/make_dmg.sh" ${DMG_ARGS[@]+"${DMG_ARGS[@]}"}
[ -f "$DMG" ] || fail "没有找到产物：$DMG"
# 上传与清单一律按 v2 命名；本地产物改名后，Sparkle 签名与 appcast 同步跟随
mv -f "$DMG" "$V2_DMG"
DMG="$V2_DMG"

echo "===== 2/5 生成 EdDSA 签名"
SIG="$(python3 "$REPO_ROOT/tools/release/sign_update.py" "$DMG" | tail -1)"
[ -n "$SIG" ] || fail "签名失败"
[ -f "$DMG.edSignature" ] || fail "没有生成 $DMG.edSignature"

echo "===== 3/5 生成 appcast.xml"
APPCAST_ARGS=(
    --version "$VERSION"
    --dmg "$DMG"
    --dmg-url "$BASE_URL/$V2_DMG_NAME"
    --sig "$SIG"
    --output "$APPCAST"
)
[ -n "$NOTES" ] && APPCAST_ARGS+=(--release-notes "$NOTES")
python3 "$REPO_ROOT/tools/release/generate_appcast.py" "${APPCAST_ARGS[@]}"

# notes_url 可选：发布说明文件需自行上传后才写，避免客户端死链
NOTES_URL="${DEVCARD_NOTES_URL:-}"

echo "===== 4/5 生成 manifest.json（v2，Ed25519 签名）"
MANIFEST_ARGS=(
    generate
    --product devcard
    --channel "$CHANNEL"
    --platform macos
    --version "$VERSION"
    --min-os "$MIN_OS"
    --artifact "dmg=$DMG"
    --key-id "$KEY_ID"
    --key "$SIGN_KEY"
    --out "$MANIFEST"
)
[ -n "$NOTES_URL" ] && MANIFEST_ARGS+=(--notes-url "$NOTES_URL")
python3 "$REPO_ROOT/tools/release/manifest_v2.py" "${MANIFEST_ARGS[@]}"

PUB_HEX="$(python3 "$REPO_ROOT/tools/release/manifest_v2.py" pubkey --key "$SIGN_KEY" | sed 's/.*：//')"

echo "===== 4.5/5 发布门禁（guard，v2）"
GUARD_PY="$REPO_ROOT/tools/release/guard.py"
GUARD_ARGS=(
    --product devcard --kind mac --channel "$CHANNEL"
    --file "$DMG" --name "$V2_DMG_NAME"
    --schema v2 --manifest "$MANIFEST" --pub-hex "$PUB_HEX"
)
[ "$DRY_RUN" = "1" ] && GUARD_ARGS+=(--offline)
python3 "$GUARD_PY" preflight "${GUARD_ARGS[@]}"

echo "===== 5/5 上传 OSS"
if [ "$DRY_RUN" = "1" ]; then
    echo "  (dry-run：跳过上传)"
    echo "  将上传："
    echo "    $DMG → devcard/$V2_DMG_NAME"
    echo "    $DMG.edSignature → devcard/"
    echo "    $APPCAST → devcard/updates/$CHANNEL/"
    echo "    $MANIFEST → devcard/updates/$CHANNEL/"
    exit 0
fi

if [ -f "$HOME/.config/devcard/release.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$HOME/.config/devcard/release.env"
    set +a
fi
for v in OSS_ACCESS_KEY_ID OSS_ACCESS_KEY_SECRET OSS_ENDPOINT OSS_BUCKET; do
    [ -n "${!v:-}" ] || fail "环境变量 $v 未设置（见 ~/.config/devcard/release.env）"
done

python3 "$REPO_ROOT/tools/release/upload_oss.py" \
    "$DMG" "$DMG.edSignature" --prefix devcard/
python3 "$REPO_ROOT/tools/release/upload_oss.py" \
    "$APPCAST" "$MANIFEST" --prefix "devcard/updates/$CHANNEL/"

echo "===== 线上校验"
for url in \
    "$BASE_URL/$V2_DMG_NAME" \
    "$BASE_URL/updates/$CHANNEL/appcast.xml" \
    "$BASE_URL/updates/$CHANNEL/manifest.json"; do
    code="$(curl -s -o /dev/null -w '%{http_code}' -I --max-time 20 "$url")"
    if [ "$code" = "200" ]; then
        echo "  ✓ $url"
    else
        fail "$url 返回 HTTP $code"
    fi
done

echo "===== 线上门禁"
python3 "$GUARD_PY" verify-live "${GUARD_ARGS[@]}"

echo
echo "发布完成：DevCard ${VERSION}（macOS ${ARCH}）"
echo "  manifest.json：$BASE_URL/updates/$CHANNEL/manifest.json"
