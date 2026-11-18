#!/bin/bash
# DevCard macOS 一条命令发布：出包 → DMG 签名 → appcast + latest.json → 上传 OSS。
#
# 用法：
#   tools/release/publish_macos.sh                    # 构建并发布
#   tools/release/publish_macos.sh --skip-build       # 复用已有 build-new 产物
#   tools/release/publish_macos.sh --notes "更新说明"  # 写进 appcast 与 latest.json
#   tools/release/publish_macos.sh --dry-run          # 只出包与清单，不上传
#
# 前置条件：
#   - 公证凭证在 Keychain profile devcard-notary（make_dmg.sh 负责签名与公证）
#   - Sparkle 私钥在 ~/.config/devcard/sparkle-keys/private_key.pem
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

DMG="$DIST/DevCard-$VERSION-macos-$ARCH.dmg"
MANIFEST="$DIST/latest.json"
APPCAST="$DIST/appcast.xml"

echo "===== 1/5 出包"
"$REPO_ROOT/tools/package/make_dmg.sh" ${DMG_ARGS[@]+"${DMG_ARGS[@]}"}
[ -f "$DMG" ] || fail "没有找到产物：$DMG"

echo "===== 2/5 生成 EdDSA 签名"
SIG="$(python3 "$REPO_ROOT/tools/release/sign_update.py" "$DMG" | tail -1)"
[ -n "$SIG" ] || fail "签名失败"
[ -f "$DMG.edSignature" ] || fail "没有生成 $DMG.edSignature"

echo "===== 3/5 生成 appcast.xml"
APPCAST_ARGS=(
    --version "$VERSION"
    --dmg "$DMG"
    --dmg-url "$BASE_URL/$(basename "$DMG")"
    --sig "$SIG"
    --output "$APPCAST"
)
[ -n "$NOTES" ] && APPCAST_ARGS+=(--release-notes "$NOTES")
python3 "$REPO_ROOT/tools/release/generate_appcast.py" "${APPCAST_ARGS[@]}"

echo "===== 4/5 生成 latest.json"
VERSION="$VERSION" NOTES="$NOTES" DMG="$DMG" SIG="$SIG" \
BASE_URL="$BASE_URL" CHANNEL="$CHANNEL" DIST="$DIST" ARCH="$ARCH" \
python3 - <<'PY'
import hashlib, json, os

version = os.environ["VERSION"]
dmg = os.environ["DMG"]
notes = os.environ["NOTES"] or f"DevCard {version} 版本发布。"
base = os.environ["BASE_URL"].rstrip("/")
channel = os.environ["CHANNEL"]
name = os.path.basename(dmg)

h = hashlib.sha256()
with open(dmg, "rb") as f:
    for chunk in iter(lambda: f.read(1 << 20), b""):
        h.update(chunk)
digest = h.hexdigest()

manifest = {
    "latest": version,
    "download_url": "https://dl.xiaotu.online/",
    "notes": notes,
    "check_enabled": True,
    "check_endpoint": f"{base}/updates/{channel}/latest.json",
    "check_interval_hours": 24,
    "assets": [{
        "name": name,
        "platform": "macos",
        "arch": os.environ["ARCH"],
        "version": version,
        "download_url": f"{base}/{name}",
        "digest": f"sha256:{digest}",
        "size": os.path.getsize(dmg),
        "signature": os.environ["SIG"],
        "notarized": True,
    }],
}

out = os.path.join(os.environ["DIST"], "latest.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(manifest, f, ensure_ascii=False, indent=2)
    f.write("\n")
print(f"  {out}")
PY

echo "===== 5/5 上传 OSS"
if [ "$DRY_RUN" = "1" ]; then
    echo "  (dry-run：跳过上传)"
    echo "  将上传："
    echo "    $DMG → devcard/"
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
    "$BASE_URL/$(basename "$DMG")" \
    "$BASE_URL/updates/$CHANNEL/appcast.xml" \
    "$BASE_URL/updates/$CHANNEL/latest.json"; do
    code="$(curl -s -o /dev/null -w '%{http_code}' -I --max-time 20 "$url")"
    if [ "$code" = "200" ]; then
        echo "  ✓ $url"
    else
        fail "$url 返回 HTTP $code"
    fi
done

echo
echo "发布完成：DevCard ${VERSION}（macOS ${ARCH}）"
