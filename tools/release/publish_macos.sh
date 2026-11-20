#!/bin/bash
# DevCard macOS 一条命令发布：出包 → DMG 签名 → appcast + manifest.json（v2）→ 上传 OSS。
#
# 清单为统一 Schema v2（见 mote/docs/更新清单Schema-v2.md）：manifest.json
# （platforms.macos，Ed25519 签名）；appcast.xml 是 Sparkle 专用，不在 v2 范围。
# v1 清单（latest.json / windows.json）已于 2026-11-18 全量删除；
# 本脚本只生成 manifest.json（v2）+ appcast.xml。
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
MIN_OS="${MIN_OS:-15.0}"   # 与 bin/DevCard.app 的 LSMinimumSystemVersion 保持一致

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
# 发布说明：优先 docs/release-notes-<版本>.html；没有 .html 但有 .md 时
# 自动用 Python markdown 转 .html（客户端浏览器需要 html 渲染）。
# 也可用 DEVCARD_NOTES_FILE 显式指定覆盖。
NOTES_KEY="release-notes-$VERSION.html"
UPLOAD_NOTES=0
if [ -n "${DEVCARD_NOTES_FILE:-}" ]; then
    NOTES_PAGE="$DEVCARD_NOTES_FILE"
else
    # 辅助函数：找 .html，没有则从同名 .md 自动转（html 比 md 旧也重转）
    resolve_notes_file() {
        local ver md html
        ver="$1"
        md="$REPO_ROOT/docs/release-notes-$ver.md"
        html="$REPO_ROOT/docs/release-notes-$ver.html"
        if [ ! -f "$html" ] || { [ -f "$md" ] && [ "$md" -nt "$html" ]; }; then
            if [ -f "$md" ]; then
                if ! python3 -c 'import markdown' 2>/dev/null; then
                    echo "⚠️  有 release-notes-$ver.md 但缺 Python markdown 库，跳过 md→html" >&2; return 1
                fi
                python3 - "$md" "$html" <<'PYEOF'
import markdown, sys
css = """body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;max-width:720px;margin:40px auto;padding:0 20px;line-height:1.7;color:#222}h1{border-bottom:2px solid #eee;padding-bottom:8px}h2{margin-top:1.5em;color:#111}table{border-collapse:collapse;width:100%;margin:1em 0}th,td{border:1px solid #ddd;padding:8px 12px;text-align:left}th{background:#f6f8fa}code{background:#f0f0f0;padding:1px 6px;border-radius:3px;font-size:.9em}pre{background:#f6f8fa;padding:12px 16px;border-radius:6px;overflow-x:auto}pre code{background:none;padding:0}blockquote{border-left:4px solid #ddd;margin:1em 0;padding:0 16px;color:#666}"""
body = markdown.markdown(open(sys.argv[1],encoding='utf-8').read(), extensions=['tables','fenced_code'])
open(sys.argv[2],'w',encoding='utf-8').write(f'<!DOCTYPE html><html><head><meta charset="utf-8"><style>{css}</style></head><body>{body}</body></html>')
PYEOF
                echo "  (md → html 自动生成 $html)"
            fi
        fi
        [ -f "$html" ] && echo "$html"
    }
    NOTES_PAGE="$(resolve_notes_file "$VERSION" || true)"
fi
if [ -z "$NOTES_URL" ] && [ -n "$NOTES_PAGE" ] && [ -f "$NOTES_PAGE" ]; then
    NOTES_URL="$BASE_URL/$NOTES_KEY"
    UPLOAD_NOTES=1
fi

echo "===== 4/5 生成 manifest.json（v2，Ed25519 签名）"
# 另一平台段可能已在线上：先取线上清单，--prev 合并后整体重签，避免覆盖另一平台段
if [ ! -s "$MANIFEST" ]; then
  curl -fsSL "$BASE_URL/updates/$CHANNEL/manifest.json" -o "$MANIFEST" 2>/dev/null || rm -f "$MANIFEST"
fi
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
[ -s "$MANIFEST" ] && MANIFEST_ARGS+=(--prev "$MANIFEST")
python3 "$REPO_ROOT/tools/release/manifest_v2.py" "${MANIFEST_ARGS[@]}"

PUB_HEX="$(python3 "$REPO_ROOT/tools/release/manifest_v2.py" pubkey --key "$SIGN_KEY" | sed 's/.*：//')"

echo "===== 4.5/5 发布门禁（guard，v2）"
GUARD_PY="$REPO_ROOT/tools/release/guard.py"
GUARD_ARGS=(
    --product devcard --kind mac --channel "$CHANNEL"
    --file "$DMG" --name "$V2_DMG_NAME"
    --manifest "$MANIFEST" --pub-hex "$PUB_HEX"
# 注：guard.py 已移除 --schema 参数（argparse 无此选项），强制 v2
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
    if [ "$UPLOAD_NOTES" = "1" ]; then
        echo "    $NOTES_PAGE → devcard/$NOTES_KEY"
    fi
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

if [ "$UPLOAD_NOTES" = "1" ]; then
    python3 "$REPO_ROOT/tools/release/upload_oss.py" "$NOTES_PAGE" --prefix devcard/
fi
python3 "$REPO_ROOT/tools/release/upload_oss.py" \
    "$DMG" "$DMG.edSignature" --prefix devcard/
python3 "$REPO_ROOT/tools/release/upload_oss.py" \
    "$APPCAST" "$MANIFEST" --prefix "devcard/updates/$CHANNEL/"

echo "===== 线上校验"
CHECK_URLS=(
    "$BASE_URL/$V2_DMG_NAME"
    "$BASE_URL/updates/$CHANNEL/appcast.xml"
    "$BASE_URL/updates/$CHANNEL/manifest.json"
)
if [ -n "$NOTES_URL" ]; then
    CHECK_URLS+=("$NOTES_URL")
fi
for url in "${CHECK_URLS[@]}"; do
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
