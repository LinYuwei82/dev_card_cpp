#!/bin/bash
# DevCard 图标生成：从 docs/design/icon 的 PNG 母版合成多尺寸 .ico。
#
#   shell/win/DevCard.ico  Windows exe / 安装器 / 快捷方式（16–256 七档）
#   web/favicon.ico        浏览器标签页（16 / 32 / 48）
#
# 图形源只有 docs/design/icon/icon.svg 与 png/icon-1024.png：这里不重画、
# 不改色，只按尺寸打包，保证 Windows 与网页用的是同一套图标。
# macOS 的 .icns 已在 shell/mac/DevCard.icns，不由本脚本生成。
#
# 依赖：python3（或 python）+ Pillow。macOS 自带 python3 没有 Pillow，
# 先 `python3 -m pip install --user Pillow`；也可用 PYTHON=... 指定解释器。
#
# 用法：tools/package/make_icons.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MASTER="$REPO_ROOT/docs/design/icon/png/icon-1024.png"
WIN_ICO="$REPO_ROOT/shell/win/DevCard.ico"
WEB_ICO="$REPO_ROOT/web/favicon.ico"

fail() {
  echo "[失败] $*" >&2
  exit 1
}

[ -f "$MASTER" ] || fail "找不到母版：$MASTER"

PYTHON="${PYTHON:-}"
if [ -z "$PYTHON" ]; then
  # 逐个试：某些环境里 python3 存在但没有 Pillow，而 python 才有。
  for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c 'import PIL' >/dev/null 2>&1; then
      PYTHON="$candidate"
      break
    fi
  done
  [ -n "$PYTHON" ] || fail "找不到带 Pillow 的 python3/python（python3 -m pip install --user Pillow）"
fi
"$PYTHON" -c 'import PIL' >/dev/null 2>&1 || fail "缺少 Pillow：$PYTHON -m pip install --user Pillow"

"$PYTHON" - "$MASTER" "$WIN_ICO" "$WEB_ICO" <<'PY'
import sys
from PIL import Image

master, win_ico, web_ico = sys.argv[1], sys.argv[2], sys.argv[3]
image = Image.open(master).convert("RGBA")

# 单文件多尺寸：Windows 在有 256px 时优先用它，小尺寸再取对应的档。
image.save(
    win_ico,
    format="ICO",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
)
# favicon 不需要 256：浏览器标签最大按 48 取。
image.save(web_ico, format="ICO", sizes=[(16, 16), (32, 32), (48, 48)])
PY

for file in "$WIN_ICO" "$WEB_ICO"; do
  bytes="$(wc -c < "$file" | tr -d '[:space:]')"
  [ "$bytes" -gt 0 ] || fail "生成的文件为空：$file"
  echo "生成：$file（${bytes} bytes）"
done
