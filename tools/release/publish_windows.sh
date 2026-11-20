#!/usr/bin/env bash
# DevCard · Windows 在线更新发布（Mac 侧，一条命令）
#
# 与 publish_windows.ps1 的分工：
#   ps1 在 Windows 上「构建 → 出包 → 发布」一条龙；
#   本脚本在 Mac 上只做「发布」：拿现成的绿色包 / 安装包，生成 v2 清单、过门禁、传 OSS。
#   参数口径与另外两仓的 publish_windows.sh 一致，方便发布机统一操作。
#
# 输入：绿色包 zip（必需）、安装包 setup.exe（可选；缺省时只发绿色包）
# 动作：Ed25519 签名 manifest.json（v2）→ guard 门禁 → 上传 dl.xiaotu.online → 线上校验
#
# 构建号：显式 BUILD → 绿色包内 DevCard.exe 的 DEVCARD_BUILD_STAMP 标记 →
#   gen_build.py 兜底；显式值与包内标记不一致直接失败（避免清单比二进制「新」）。
#
# 用法：
#   tools/release/publish_windows.sh                       # 自动找 dist/ 产物 + 自动识别构建号
#   BUILD=202611180930 tools/release/publish_windows.sh
#   ZIP_PATH=... SETUP_PATH=... tools/release/publish_windows.sh
#   DRY_RUN=1 tools/release/publish_windows.sh             # 只生成清单 + 门禁，不上传
#   ROLLOUT_PERCENT=10 MANDATORY=1 MANDATORY_REASON=SECURITY \
#     MANDATORY_DEADLINE=2026-12-01T00:00:00Z tools/release/publish_windows.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PRODUCT_DIR="${PRODUCT_DIR:-devcard}"
CDN_DOMAIN="${CDN_DOMAIN:-https://dl.xiaotu.online}"
CHANNEL="${CHANNEL:-stable}"
case "$CHANNEL" in stable|beta) ;; *) echo "CHANNEL 必须是 stable/beta" >&2; exit 2 ;; esac

VERSION="${VERSION:-$(tr -d '[:space:]' < VERSION.txt)}"
[ -n "$VERSION" ] || { echo "无法确定 VERSION（VERSION.txt 为空）" >&2; exit 1; }

MIN_OS="${MIN_OS:-10.0}"
KEY_ID="${KEY_ID:-2026-11}"
DRY_RUN="${DRY_RUN:-false}"
SKIP_UPLOAD="${SKIP_UPLOAD:-false}"
OSS_ENV_FILE="${OSS_ENV_FILE:-$HOME/.config/devcard/release.env}"
SIGN_KEY="${SIGN_KEY:-$HOME/.config/devcard/manifest-keys/ed25519_seed.b64}"

# CI 用环境变量注入签名种子：落成临时文件，退出时清理
SIGN_KEY_TEMP=""
if [ -n "${DEVCARD_UPDATE_SIGNING_KEY:-}" ]; then
  SIGN_KEY_TEMP="$(mktemp "${TMPDIR:-/tmp}/devcard-signkey.XXXXXX")"
  printf '%s\n' "$DEVCARD_UPDATE_SIGNING_KEY" | tr -d '[:space:]' > "$SIGN_KEY_TEMP"
  SIGN_KEY="$SIGN_KEY_TEMP"
fi
cleanup() { [ -n "$SIGN_KEY_TEMP" ] && rm -f "$SIGN_KEY_TEMP"; }
trap cleanup EXIT
[ -f "$SIGN_KEY" ] || { echo "v2 清单签名种子不存在：$SIGN_KEY（或设置 DEVCARD_UPDATE_SIGNING_KEY）" >&2; exit 1; }

NOTES_URL="${NOTES_URL:-}"
if [ -n "$NOTES_URL" ]; then
  case "$NOTES_URL" in
    http://*|https://*) ;;
    *) echo "NOTES_URL 必须是绝对 http(s) 地址：$NOTES_URL" >&2; exit 2 ;;
  esac
fi
NOTES_FILE=""
if [ -z "$NOTES_URL" ] && [ -f "$REPO_ROOT/docs/release-notes-$VERSION.html" ]; then
  NOTES_FILE="$REPO_ROOT/docs/release-notes-$VERSION.html"
  NOTES_URL="$CDN_DOMAIN/$PRODUCT_DIR/release-notes-$VERSION.html"
fi

# ---- 灰度 / 强制更新参数：唯一实现在 publish_args.py（三仓共享）----
PUBLISH_ARGS_SH="$(python3 "$REPO_ROOT/tools/release/publish_args.py" validate \
  --product "$PRODUCT_DIR" \
  --rollout "${ROLLOUT_PERCENT:-100}" \
  --mandatory "${MANDATORY:-0}" \
  --mandatory-reason "${MANDATORY_REASON:-}" \
  --mandatory-deadline "${MANDATORY_DEADLINE:-}" \
  --emit sh)" || exit 2
eval "$PUBLISH_ARGS_SH"
if [ "$MANDATORY_ON" = "1" ]; then
  echo "更新策略：灰度 ${ROLLOUT_PERCENT}% · 强制更新（${MANDATORY_REASON}${MANDATORY_DEADLINE:+，截止 ${MANDATORY_DEADLINE}}）"
else
  echo "更新策略：灰度 ${ROLLOUT_PERCENT}% · 非强制"
fi

# ---- 定位产物（v2 命名：devcard-<ver>-win64-<type>.<ext>）----
if [ -n "${ZIP_PATH:-}" ]; then
  [ -f "$ZIP_PATH" ] || { echo "ZIP_PATH 不存在：$ZIP_PATH" >&2; exit 1; }
else
  ZIP_PATH="$REPO_ROOT/dist/DevCard-$VERSION-win64-portable.zip"
  [ -f "$ZIP_PATH" ] || \
    ZIP_PATH="$(find "$REPO_ROOT/dist" -maxdepth 1 -iname "DevCard*.zip" | head -1 || true)"
  [ -n "$ZIP_PATH" ] && [ -f "$ZIP_PATH" ] || { echo "绿色包 zip 不存在（ZIP_PATH 可显式指定）" >&2; exit 1; }
fi
if [ -n "${SETUP_PATH:-}" ]; then
  [ -f "$SETUP_PATH" ] || { echo "SETUP_PATH 不存在：$SETUP_PATH" >&2; exit 1; }
else
  SETUP_PATH="$REPO_ROOT/dist/DevCard-$VERSION-setup.exe"
  [ -f "$SETUP_PATH" ] || \
    SETUP_PATH="$(find "$REPO_ROOT/dist" -maxdepth 1 -iname "*setup*.exe" | head -1 || true)"
fi

# ---- 构建号：显式 → 包内标记 → gen_build ----
# 绿色包内的 DevCard.exe 带 "DEVCARD_BUILD_STAMP=<digits>" 静态串，用它反查编译时的构建号
EXE_STAMP="$(python3 - "$ZIP_PATH" <<'PY'
import re, sys, zipfile
try:
    with zipfile.ZipFile(sys.argv[1]) as zf:
        name = next((n for n in zf.namelist()
                     if n.lower().rstrip("/").endswith("devcard.exe")), None)
        if not name:
            sys.exit(0)
        data = zf.read(name)
except Exception:
    sys.exit(0)
m = re.search(rb"DEVCARD_BUILD_STAMP=(\d{4,})", data)
print(m.group(1).decode() if m else "")
PY
)"
if [ -n "${BUILD:-}" ]; then
  [[ "$BUILD" =~ ^[0-9]{4,}$ ]] || { echo "BUILD 非法：${BUILD}" >&2; exit 1; }
  if [ -n "$EXE_STAMP" ] && [ "$EXE_STAMP" != "$BUILD" ]; then
    echo "BUILD=$BUILD 与绿色包内构建号 $EXE_STAMP 不一致；请用编译时的构建号" >&2
    exit 1
  fi
  echo "build：${BUILD}（来源 显式 BUILD）"
elif [ -n "$EXE_STAMP" ]; then
  BUILD="$EXE_STAMP"
  echo "build：${BUILD}（来源 绿色包内 DevCard.exe）"
else
  BUILD="$(python3 "$REPO_ROOT/tools/release/gen_build.py" \
    --product "$PRODUCT_DIR" --repo-root "$REPO_ROOT")" || {
    echo "gen_build.py 失败，请用 BUILD= 显式指定编译时的构建号" >&2; exit 1; }
  echo "build：${BUILD}（来源 gen_build.py；未在包内找到标记，如与编译值不同请显式传 BUILD）"
fi

DIST="$REPO_ROOT/dist"
mkdir -p "$DIST"
V2_ZIP_NAME="devcard-$VERSION-win64-portable.zip"
V2_SETUP_NAME="devcard-$VERSION-win64-setup.exe"
V2_ZIP="$DIST/$V2_ZIP_NAME"
V2_SETUP=""
if [ "$(cd "$(dirname "$ZIP_PATH")" && pwd)/$(basename "$ZIP_PATH")" != "$V2_ZIP" ]; then
  cp -f "$ZIP_PATH" "$V2_ZIP"
else
  V2_ZIP="$ZIP_PATH"
fi
if [ -n "$SETUP_PATH" ]; then
  V2_SETUP="$DIST/$V2_SETUP_NAME"
  if [ "$(cd "$(dirname "$SETUP_PATH")" && pwd)/$(basename "$SETUP_PATH")" != "$V2_SETUP" ]; then
    cp -f "$SETUP_PATH" "$V2_SETUP"
  else
    V2_SETUP="$SETUP_PATH"
  fi
fi

ZIP_SIZE="$(stat -f %z "$V2_ZIP")"
ZIP_SHA256="$(shasum -a 256 "$V2_ZIP" | awk '{print $1}')"
ZIP_URL="$CDN_DOMAIN/$PRODUCT_DIR/$V2_ZIP_NAME"
MANIFEST_KEY="$PRODUCT_DIR/updates/$CHANNEL/manifest.json"
MANIFEST_URL="$CDN_DOMAIN/$MANIFEST_KEY"
MANIFEST_LOCAL="$DIST/manifest.json"

echo "===== DevCard Windows 发布 · $VERSION / build $BUILD / $CHANNEL ====="
echo "绿色包：    $V2_ZIP ($ZIP_SIZE B)"
echo "SHA256：    $ZIP_SHA256"
if [ -n "$V2_SETUP" ]; then
  SETUP_SIZE="$(stat -f %z "$V2_SETUP")"
  SETUP_SHA256="$(shasum -a 256 "$V2_SETUP" | awk '{print $1}')"
  SETUP_URL="$CDN_DOMAIN/$PRODUCT_DIR/$V2_SETUP_NAME"
  echo "安装包：    $V2_SETUP ($SETUP_SIZE B)"
else
  SETUP_SIZE=""; SETUP_SHA256=""; SETUP_URL=""
  echo "安装包：    （未提供，仅发布绿色包）"
fi
echo "清单地址：  $MANIFEST_URL"

MANIFEST_V2_PY="$REPO_ROOT/tools/release/manifest_v2.py"
if [ ! -s "$MANIFEST_LOCAL" ]; then
  curl -fsSL "$MANIFEST_URL" -o "$MANIFEST_LOCAL" 2>/dev/null || rm -f "$MANIFEST_LOCAL"
fi

echo ""
echo "===== ① 生成 manifest.json（v2）"
MANIFEST_ARGS=(
  generate
  --product "$PRODUCT_DIR" --channel "$CHANNEL" --platform windows
  --version "$VERSION" --build "$BUILD" --min-os "$MIN_OS" --key-id "$KEY_ID"
  --artifact "portable=$V2_ZIP"
  --key "$SIGN_KEY" --out "$MANIFEST_LOCAL"
  --rollout "$ROLLOUT_PERCENT"
)
[ -n "$V2_SETUP" ] && MANIFEST_ARGS+=(--artifact "setup=$V2_SETUP")
if [ "$MANDATORY_ON" = "1" ]; then
  MANIFEST_ARGS+=(--mandatory --mandatory-reason "$MANDATORY_REASON")
  [ -n "$MANDATORY_DEADLINE" ] && MANIFEST_ARGS+=(--mandatory-deadline "$MANDATORY_DEADLINE")
fi
[ -n "$NOTES_URL" ] && MANIFEST_ARGS+=(--notes-url "$NOTES_URL")
[ -s "$MANIFEST_LOCAL" ] && MANIFEST_ARGS+=(--prev "$MANIFEST_LOCAL")
python3 "$MANIFEST_V2_PY" "${MANIFEST_ARGS[@]}"
PUB_HEX="$(python3 "$MANIFEST_V2_PY" pubkey --key "$SIGN_KEY" | sed 's/.*：//')"

echo ""
echo "===== ② 发布门禁（guard，v2）"
GUARD_PY="${GUARD_PY:-$REPO_ROOT/tools/release/guard.py}"
GUARD_ARGS=(--product "$PRODUCT_DIR" --kind win --channel "$CHANNEL"
            --manifest "$MANIFEST_LOCAL" --pub-hex "$PUB_HEX" --build "$BUILD")
if [ -n "$V2_SETUP" ]; then
  GUARD_ARGS+=(--file "$V2_SETUP" --name "$V2_SETUP_NAME" --zip "$V2_ZIP" --zip-name "$V2_ZIP_NAME")
else
  GUARD_ARGS+=(--file "$V2_ZIP" --name "$V2_ZIP_NAME")
fi
if [ "$DRY_RUN" = "true" ] || [ "$SKIP_UPLOAD" = "true" ]; then
  GUARD_ARGS+=(--offline)
fi
python3 "$GUARD_PY" preflight "${GUARD_ARGS[@]}"

if [ "$DRY_RUN" = "true" ] || [ "$SKIP_UPLOAD" = "true" ]; then
  echo ""
  echo "（跳过上传）本地清单：$MANIFEST_LOCAL"
  exit 0
fi

echo ""
echo "===== ③ 上传 OSS"
[ -f "$OSS_ENV_FILE" ] || { echo "缺少 OSS 凭据文件：$OSS_ENV_FILE" >&2; exit 1; }
set -a; . "$OSS_ENV_FILE"; set +a
for v in OSS_ACCESS_KEY_ID OSS_ACCESS_KEY_SECRET OSS_ENDPOINT OSS_BUCKET; do
  eval "[ -n \"\${$v:-}\" ]" || { echo "缺少环境变量 $v" >&2; exit 1; }
done
UPLOAD_PY="$REPO_ROOT/tools/release/upload_oss.py"
if [ -n "$V2_SETUP" ]; then
  python3 "$UPLOAD_PY" "$V2_SETUP" "$V2_ZIP" --prefix "$PRODUCT_DIR/"
else
  python3 "$UPLOAD_PY" "$V2_ZIP" --prefix "$PRODUCT_DIR/"
fi
if [ -n "$NOTES_FILE" ]; then
  python3 "$UPLOAD_PY" "$NOTES_FILE" --prefix "$PRODUCT_DIR/"
fi
python3 "$UPLOAD_PY" "$MANIFEST_LOCAL" --prefix "$PRODUCT_DIR/updates/$CHANNEL/"

echo ""
echo "===== ④ 线上校验"
for url in "$ZIP_URL" ${SETUP_URL:+"$SETUP_URL"} "$MANIFEST_URL"; do
  code="$(curl -s -o /dev/null -w '%{http_code}' -I "$url")"
  echo "  GET $url → HTTP $code"
  [ "$code" = "200" ] || { echo "  ✗ 线上校验失败：$url" >&2; exit 1; }
done
python3 "$GUARD_PY" verify-live "${GUARD_ARGS[@]}"

echo ""
echo "================ 完成 ================"
echo "版本 / build： $VERSION / $BUILD"
echo "绿色包：    $ZIP_URL"
echo "安装包：    ${SETUP_URL:-（无）}"
echo "manifest.json：$MANIFEST_URL"
