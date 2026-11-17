#!/bin/bash
# DevCard 图标生成：从 docs/design/icon/png/icon-1024.png 母版合成全套图标。
#
# 输出（全部从同一张 1024 母版派生，保证几何/色彩一致）：
#   shell/mac/DevCard.icns           macOS App 图标（ic10/ic11/ic12/ic13/ic14 五档 + @2x）
#   shell/win/DevCard.ico            Windows exe / 安装器（16–256 七档）
#   web/favicon.ico                  浏览器标签页（16 / 32 / 48）
#   web/favicon-16/32/48x16/32/48.png 浏览器 shortcut icon（manifest 引用）
#   web/apple-touch-icon.png         iOS Safari 主屏幕（180x180）
#   web/og-image.png                 社交分享预览（1200x630 深青底 + 居中 icon）
#   web/icons/pwa-{192,512}.png      PWA 常规图标
#   web/icons/pwa-maskable-{192,512}.png  PWA maskable（安全区 80%）
#
# 依赖：python3 + Pillow、macOS 自带 iconutil。
#   python3 -m pip install --user Pillow
#
# 用法：tools/package/make_icons.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MASTER="$REPO_ROOT/docs/design/icon/png/icon-1024.png"
MAC_ICNS="$REPO_ROOT/shell/mac/DevCard.icns"
WIN_ICO="$REPO_ROOT/shell/win/DevCard.ico"
WEB_ICO="$REPO_ROOT/web/favicon.ico"
ICONS_DIR="$REPO_ROOT/web/icons"

fail() {
  echo "[失败] $*" >&2
  exit 1
}

[ -f "$MASTER" ] || fail "找不到母版：$MASTER"
command -v iconutil >/dev/null 2>&1 || fail "缺少 macOS iconutil（系统自带，检查 PATH）"

PYTHON="${PYTHON:-}"
for candidate in python3 python; do
  if command -v "$candidate" >/dev/null 2>&1 &&
     "$candidate" -c 'import PIL' >/dev/null 2>&1; then
    PYTHON="$candidate"
    break
  fi
done
[ -n "$PYTHON" ] || fail "找不到带 Pillow 的 python3/python（python3 -m pip install --user Pillow）"

# ---------- 1. macOS .icns ----------
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

ICONSET="$TMPDIR/DevCard.iconset"
mkdir -p "$ICONSET"

# iconset 需要 10 张：5 个基础尺寸 + 5 个 @2x
# iconutil 不允许缺失，也不接受额外尺寸
for size in 16 32 128 256 512; do
  "$PYTHON" - "$MASTER" "$ICONSET/icon_${size}x${size}.png" "$size" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).convert("RGBA").resize(
    (int(sys.argv[3]), int(sys.argv[3])), Image.LANCZOS
).save(sys.argv[2])
PY
  base=$(( size * 2 ))
  "$PYTHON" - "$MASTER" "$ICONSET/icon_${size}x${size}@2x.png" "$base" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).convert("RGBA").resize(
    (int(sys.argv[3]), int(sys.argv[3])), Image.LANCZOS
).save(sys.argv[2])
PY
done

iconutil --convert icns --output "$MAC_ICNS" "$ICONSET"
echo "[icns] $MAC_ICNS ($(wc -c < "$MAC_ICNS" | tr -d ' ') bytes)"

# ---------- 2. Windows .ico + web favicon.ico ----------
"$PYTHON" - "$MASTER" "$WIN_ICO" "$WEB_ICO" <<'PY'
import sys
from PIL import Image
master, win_ico, web_ico = sys.argv[1], sys.argv[2], sys.argv[3]
im = Image.open(master).convert("RGBA")
im.save(win_ico, format="ICO",
        sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
im.save(web_ico, format="ICO", sizes=[(16,16),(32,32),(48,48)])
PY
echo "[ico]  $WIN_ICO ($(wc -c < "$WIN_ICO" | tr -d ' ') bytes)"
echo "[ico]  $WEB_ICO ($(wc -c < "$WEB_ICO" | tr -d ' ') bytes)"

# ---------- 3. Web PNG ----------
mkdir -p "$ICONS_DIR"

"$PYTHON" - "$MASTER" "$REPO_ROOT/web" "$ICONS_DIR" <<'PY'
import sys, os
from PIL import Image

master, web_dir, icons_dir = sys.argv[1], sys.argv[2], sys.argv[3]
im = Image.open(master).convert("RGBA")

# favicon 独立 PNG（manifest 引用，和 favicon.ico 同源但不同打包方式）
for s in [16, 32, 48]:
    out = os.path.join(web_dir, f"favicon-{s}x{s}.png")
    im.resize((s, s), Image.LANCZOS).save(out)
    print(f"[png]  {out}")

# Apple Touch：iOS Safari 180x180
out = os.path.join(web_dir, "apple-touch-icon.png")
im.resize((180, 180), Image.LANCZOS).save(out)
print(f"[png]  {out}")

# OG image：1200x630 深青底 + 居中 icon
TEAL = (14, 75, 82, 255)
og = Image.new("RGBA", (1200, 630), TEAL)
icon_512 = im.resize((512, 512), Image.LANCZOS)
og.paste(icon_512, (344, 59), icon_512)  # 水平居中：(1200-512)/2=344；垂直居中：(630-512)/2=59
out = os.path.join(web_dir, "og-image.png")
og.save(out)
print(f"[png]  {out}")

# PWA icons
for sz in [192, 512]:
    out = os.path.join(icons_dir, f"pwa-{sz}x{sz}.png")
    im.resize((sz, sz), Image.LANCZOS).save(out)
    print(f"[png]  {out}")

# PWA maskable：安全区 80%，给圆形 mask 留 padding
for sz in [192, 512]:
    base = Image.new("RGBA", (sz, sz), TEAL)
    pad = sz // 5
    scaled = im.resize((sz - pad*2, sz - pad*2), Image.LANCZOS)
    base.paste(scaled, (pad, pad), scaled)
    out = os.path.join(icons_dir, f"pwa-maskable-{sz}x{sz}.png")
    base.save(out)
    print(f"[png]  {out}")
PY

echo ""
echo "完成。全套图标已从 $MASTER 统一派生。"
