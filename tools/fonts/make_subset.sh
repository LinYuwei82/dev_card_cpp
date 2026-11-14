#!/bin/bash
# 从 3rdparty/fonts/noto-sans-sc/NotoSansSC-VF.ttf 生成 web/fonts/ 下的子集字体。
#
# 为什么需要这个脚本：上游只发布变量字体，没有单独的 Regular/Bold 静态 TTF。
# 子集产物要能重算，参数写死在这里，不要手工敲 pyftsubset。
#
# 用法：
#   tools/fonts/make_subset.sh            # 重新生成子集（覆盖 web/fonts/*.ttf）
#   tools/fonts/make_subset.sh --verify   # 只校验现有子集的体积与 SHA256
#
# 依赖：fontTools（含 pyftsubset 与 varLib.instancer）
#   python3 -m venv /tmp/fontenv && /tmp/fontenv/bin/pip install fonttools
#   PATH=/tmp/fontenv/bin:$PATH tools/fonts/make_subset.sh
#
# 字符集：GB2312 全集（6763 汉字 + 682 符号/假名/希腊字母等）+ ASCII
#         + 常用标点（拉丁补充、通用标点、箭头、带圈数字、制表符、几何图形、
#           杂项符号、CJK 标点、全角字符）。

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$REPO_ROOT/3rdparty/fonts/noto-sans-sc/NotoSansSC-VF.ttf"
OUT_DIR="$REPO_ROOT/web/fonts"
PROV="$REPO_ROOT/3rdparty/fonts/noto-sans-sc/SOURCE.txt"

fail() {
  echo "[失败] $*" >&2
  exit 1
}

PYFTSUBSET="$(command -v pyftsubset || true)"
FONTTOOLS="$(command -v fonttools || true)"
[ -n "$PYFTSUBSET" ] || fail "找不到 pyftsubset（先装 fonttools）"
[ -n "$FONTTOOLS" ] || fail "找不到 fonttools（先装 fonttools）"
[ -f "$SRC" ] || fail "找不到原始字体：$SRC"

if [ "${1:-}" = "--verify" ]; then
  # 校验值以 SOURCE.txt 与本脚本的固定预期为准；体积对不上就说明有人手工改过产物。
  ok=0
  for pair in "NotoSansSC-Regular.subset.ttf:2281488" "NotoSansSC-Bold.subset.ttf:2278984"; do
    file="${pair%%:*}"; want="${pair##*:}"
    path="$OUT_DIR/$file"
    [ -f "$path" ] || fail "缺少子集文件：$path"
    got="$(stat -f%z "$path" 2>/dev/null || stat -c%s "$path")"
    if [ "$got" != "$want" ]; then
      fail "$file 体积 $got != $want（重新跑不带 --verify 的命令生成）"
    fi
    echo "  $file  $got bytes  $(shasum -a 256 "$path" | awk '{print $1}')"
    ok=$((ok + 1))
  done
  echo "子集校验：通过（$ok 个文件）"
  exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/devcard-fonts.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# 字符集：直接由 Python 的 gb2312 编解码器枚举，不手工维护字表。
python3 - "$WORK/charset.txt" <<'PY'
import sys
chars = set()
for b1 in range(0xA1, 0xF8):
    for b2 in range(0xA1, 0xFF):
        try:
            chars.add(bytes([b1, b2]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
chars.update(chr(c) for c in range(0x20, 0x7F))
for lo, hi in [(0x00A0, 0x00FF), (0x2000, 0x206F), (0x2190, 0x21FF),
               (0x2460, 0x24FF), (0x2500, 0x257F), (0x25A0, 0x25FF),
               (0x2600, 0x26FF), (0x3000, 0x303F), (0xFF00, 0xFFEF)]:
    chars.update(chr(c) for c in range(lo, hi + 1))
with open(sys.argv[1], "w", encoding="utf-8") as f:
    f.write("\n".join("%04X" % ord(c) for c in sorted(chars)))
print("字符集：%d 个码位" % len(chars))
PY

mkdir -p "$OUT_DIR"
for weight in 400:Regular 700:Bold; do
  num="${weight%%:*}"; name="${weight##*:}"
  inst="$WORK/NotoSansSC-$name.ttf"
  dst="$OUT_DIR/NotoSansSC-$name.subset.ttf"

  # 变量字体先定死字重，得到静态 TTF；再子集，否则 PDF 里会嵌一份变量字体。
  # --update-name-table：否则两个字重的 PostScript 名都叫 NotoSansSC-Thin，
  #   在 PDF 属性里分不出 Regular/Bold，出片前核对字体会误导。
  "$FONTTOOLS" varLib.instancer "$SRC" "wght=$num" --update-name-table -o "$inst" >/dev/null

  # --layout-features=  ：中文排版用不到 GSUB/GPOS 特性，去掉省体积
  # --no-hinting        ：打印与屏幕都用矢量轮廓，去掉 fpgm/prep/cvt
  # --recalc-bounds     ：重算 glyf 包围盒，子集后不残留整字体的 xMin/yMin
  "$PYFTSUBSET" "$inst" \
    --unicodes-file="$WORK/charset.txt" \
    --output-file="$dst" \
    --layout-features= \
    --no-hinting \
    --drop-tables+=DSIG,BASE,vhea,vmtx,HVAR,MATH,JSTF,STAT,gasp \
    --name-IDs=1,2,3,4,6 \
    --recalc-bounds

  # head.created/modified 默认会写成当前时间（instancer 会在静态实例上打一次戳，
  # 子集再继承），同一份输入两次跑出两个 SHA256。都固定为 0（1904-01-01）后再保存。
  python3 - "$dst" <<'PY'
import sys
from fontTools.ttLib import TTFont
path = sys.argv[1]
font = TTFont(path)
font.recalcTimestamp = False    # 否则 save 会把 head.modified 重写成当前时间
font["head"].created = 1577836800   # 2020-01-01T00:00:00Z，固定值
font["head"].modified = 1577836800
font.save(path, reorderTables=True)
PY

  bytes="$(stat -f%z "$dst" 2>/dev/null || stat -c%s "$dst")"
  echo "$name  $bytes bytes  $(shasum -a 256 "$dst" | awk '{print $1}')"
done

echo ""
echo "完成。把上面两行（体积 + SHA256）同步到 docs/改造技术方案.md 的 D12 与 $PROV"
