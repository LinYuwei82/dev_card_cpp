#!/bin/bash
# tools/mac/test.sh —— macOS 侧测试入口：先跑 ctest，再跑 tools/verify 下的 .sh 入口
#
# 用法：
#   tools/mac/test.sh          # ctest + tools/verify/*.sh，最后汇总并给出聚合退出码
#   tools/mac/test.sh --help   # 打印这份说明
#
# 退出码（沿用 tools/verify/*.sh 现有约定）：
#   0 全部通过
#   1 有失败（ctest 有失败用例，或某个 verify 入口返回 1 / 未知退出码）
#   2 无失败，但有「已占位待确认」项（verify 入口返回 2）
#   3 环境不具备（非 macOS、缺 cmake/ctest/python3、build-new 还没 configure）
# 聚合优先级：1 > 3 > 2 > 0。3 是「这条没测成」，1 是「测成了而且坏了」；已知的坏不该被
#   未知盖住，否则环境缺一半时真失败会被藏起来。环境真缺的检查各自返回 3，不会伪装成 1。
#   环境不具备时绝不报 0——跑不动不等于通过。
#
# 兼容性：macOS 自带 /bin/bash 是 3.2。本脚本不用 mapfile/readarray、关联数组、declare -A、
#   ${var,,}、readlink -f、stat -c、grep -P、date -d、sed -i 一类 bash4/GNU 专有写法；
#   所有路径引用都加引号，可在含中文与空格的目录下运行。
#
# 【未跑 shellcheck】编写本脚本的机器（Windows）没有安装 shellcheck，因此本脚本没有任何
#   shellcheck 结论，不要当成已通过。mac 上首次运行前请自行执行：shellcheck tools/mac/test.sh
#
# 【以下是「假设」，尚未在 macOS 上真实执行过】mac 上第一次运行请以实际输出为准修正：
#   1. 假设构建目录是 preset macos-release 产出的 build-new（与 tools/mac/build.sh、
#      tools/package/make_dmg.sh 同一目录）；缺 CMakeCache.txt 时按「环境不具备」报 3，
#      而不是当成 0 通过。
#   2. 假设 tools/verify 下每个 .sh 都是 POSIX sh 入口（现有 check_bom / check_bootstrap /
#      check_migrations / check_path_helpers / check_version 五个都是 #!/bin/sh 后转派
#      python3），因此统一用 /bin/sh 执行——不依赖 git 里的可执行位，Windows 侧 checkout
#      丢掉 +x 也不会假失败。若将来出现 bash 专用入口，需要改这里。
#   3. 假设 check_bootstrap.sh 的默认构建目录仍是 build-new/server/devcard-server（见该
#      脚本注释），所以这里不额外传 --build-dir；若用了其它构建目录，它会返回 3 而不是
#      给出假通过。
#   4. 假设目前只有 check_migrations.sh 返回 2（「出现 v4+ 待与 Windows 确认」），其它入口
#      不用 2；返回 3 的是 check_bootstrap.sh（服务没构建/起不来）。
#   5. 假设 ctest 用例清单由 tests/CMakeLists.txt 决定；本脚本不挑选用例、不跳过用例，
#      也不重复实现用例里的判据。
#   6. 查分支状态一律用 origin/<branch>，不要用本地同名分支——本地分支不保证与远端同步。
#      教训：本脚本写于本地分支落后于远端的检出状态，据此误判「tools/verify 不存在」（远端早已有）；此后查分支状态一律以 origin/<branch> 为准，git ls-tree <本地分支>:path 还会直接报 Not a valid object name。

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

BUILD_DIR="$REPO_ROOT/build-new"
VERIFY_DIR="$REPO_ROOT/tools/verify"

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
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
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
  env_missing "test.sh 是 macOS 入口（当前 uname -s = ${HOST_OS}）；其它平台请用各自的测试命令。"
fi

for tool in cmake ctest python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    env_missing "缺少命令：${tool}（tools/verify 下的入口都经 python3 转派）"
  fi
done

# python3 光有命令不算数：Windows 上是应用执行别名会以 9009 空转，mac 上也可能指向坏链接。
if ! python3 --version >/dev/null 2>&1; then
  env_missing "python3 无法执行：$(command -v python3)（--version 失败）"
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  env_missing "构建目录还没 configure：${BUILD_DIR}。先跑 tools/mac/build.sh。"
fi

[ -d "$VERIFY_DIR" ] || fail "找不到验证入口目录：${VERIFY_DIR}（仓库不完整？）"

say ""
say "构建目录：${BUILD_DIR}"
say "验证入口：${VERIFY_DIR}"

# ---------- 2. ctest ----------
say ""
say "== ctest --test-dir \"${BUILD_DIR}\" --output-on-failure =="
CTEST_RC=0
if ! ctest --test-dir "$BUILD_DIR" --output-on-failure; then
  CTEST_RC=1
fi
if [ "$CTEST_RC" -eq 0 ]; then
  say "[ctest 通过]"
else
  say "[ctest 失败] 见上面的失败用例输出"
fi

# ---------- 3. tools/verify/*.sh ----------
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_PENDING=0
TOTAL_ENV=0

say ""
say "== tools/verify/*.sh =="
for f in "$VERIFY_DIR"/*.sh; do
  [ -f "$f" ] || continue
  name=$(basename "$f")
  say ""
  say "--- ${name} ---"
  rc=0
  if /bin/sh "$f"; then
    rc=0
  else
    rc=$?
  fi
  case "$rc" in
    0)
      say "[通过] ${name}"
      TOTAL_PASS=$((TOTAL_PASS + 1))
      ;;
    1)
      say "[失败] ${name}（退出码 1）"
      TOTAL_FAIL=$((TOTAL_FAIL + 1))
      ;;
    2)
      say "[待确认] ${name}（退出码 2：已占位，需与 Windows 侧确认后再定）"
      TOTAL_PENDING=$((TOTAL_PENDING + 1))
      ;;
    3)
      say "[环境不具备] ${name}（退出码 3）"
      TOTAL_ENV=$((TOTAL_ENV + 1))
      ;;
    *)
      say "[失败] ${name}（未知退出码 ${rc}，按失败处理）"
      TOTAL_FAIL=$((TOTAL_FAIL + 1))
      ;;
  esac
done

# ---------- 4. 汇总 ----------
TOTAL_VERIFY=$((TOTAL_PASS + TOTAL_FAIL + TOTAL_PENDING + TOTAL_ENV))
if [ "$TOTAL_VERIFY" -eq 0 ]; then
  fail "在 ${VERIFY_DIR} 下没有找到任何 .sh 入口（预期至少三个）"
fi

if [ "$CTEST_RC" -eq 0 ]; then
  CTEST_STATE="通过"
else
  CTEST_STATE="失败"
fi

say ""
say "汇总：ctest ${CTEST_STATE}；verify ${TOTAL_VERIFY} 个（通过 ${TOTAL_PASS} / 失败 ${TOTAL_FAIL} / 待确认 ${TOTAL_PENDING} / 环境不具备 ${TOTAL_ENV}）"

# 优先级 1 > 3 > 2 > 0：已知的坏（1）不该被未知（3）盖住，先报失败。
if [ "$CTEST_RC" -ne 0 ] || [ "$TOTAL_FAIL" -gt 0 ]; then
  say "结果：失败（退出码 1）——已知失败优先于环境缺失，不被未知掩盖。"
  exit 1
fi
if [ "$TOTAL_ENV" -gt 0 ]; then
  say "结果：环境不具备（退出码 3）——没有已知失败，但有入口因环境缺失没跑成，不算通过。"
  exit 3
fi
if [ "$TOTAL_PENDING" -gt 0 ]; then
  say "结果：通过但有待确认项（退出码 2）"
  exit 2
fi
say "结果：全部通过（退出码 0）"
exit 0
