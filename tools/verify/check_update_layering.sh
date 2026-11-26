#!/bin/sh
# check_update_layering.sh —— 自动更新分层守卫（macOS / Linux 入口）
#
# 用法：    tools/verify/check_update_layering.sh [--root PATH]
# 前置条件：无（只读源码）；需要 python3。
# 退出码： 0 通过 / 1 判据回流到前端或壳
# 详细判据与「故意造坏输入」示例见 check_update_layering.py 顶部注释。
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$DIR/check_update_layering.py" "$@"
