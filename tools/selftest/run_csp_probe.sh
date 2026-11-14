#!/bin/bash
# 复现并校验 §10.4.3 的 `Load failed`：无 CSP 通过、旧 CSP 被拦、当前 CSP 通过。
# 只依赖 macOS 自带的 clang / WebKit，不需要登录态与数据库。
#
# 用法：tools/selftest/run_csp_probe.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$REPO_ROOT/tools/selftest/csp_probe.mm"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/devcard-csp.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

[ "$(uname -s)" = "Darwin" ] || { echo "[失败] 只能在 macOS 上运行" >&2; exit 2; }
command -v clang++ >/dev/null 2>&1 || { echo "[失败] 缺少 clang++" >&2; exit 2; }

echo "编译：csp_probe"
clang++ -std=c++17 -fobjc-arc "$SRC" -framework Cocoa -framework WebKit \
  -o "$TMP/csp_probe"

run() {
  "$TMP/csp_probe" "$1"
}

NONE="$(run none)"
echo "$NONE"
OLD="$(run old)"
echo "$OLD"
NEW="$(run new)"
echo "$NEW"

fail=0
case "$NONE" in
  *'"ok":true'*) ;;
  *) echo "[失败] 无 CSP 也应该通过" >&2; fail=1 ;;
esac
case "$OLD" in
  *'"name":"TypeError"'*'"message":"Load failed"'*) ;;
  *) echo "[失败] 旧 CSP 应报 TypeError: Load failed" >&2; fail=1 ;;
esac
case "$NEW" in
  *'"ok":true'*) ;;
  *) echo "[失败] 当前 CSP 应放行 connect-src" >&2; fail=1 ;;
esac

if [ "$fail" -ne 0 ]; then
  echo "[失败] Load failed 复现判据未通过" >&2
  exit 1
fi
echo "[通过] 无 CSP=200；旧 CSP=TypeError: Load failed；当前 CSP=200"
