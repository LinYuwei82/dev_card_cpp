#!/bin/sh
# check_path_helpers.sh —— 路径 helper 唯一性与调用点盘点（macOS / Linux 入口）
#
# 用法：    tools/verify/check_path_helpers.sh [--root PATH]
# 前置条件：无（只读源码）；需要 python3。
# 退出码： 0 通过 / 1 某个 helper 有 2 份及以上定义
# 详细判据与「故意造坏输入」示例见 check_path_helpers.py 顶部注释。
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$DIR/check_path_helpers.py" "$@"
