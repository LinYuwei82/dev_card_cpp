#!/bin/sh
# check_migrations.sh —— 迁移号与归属核对（macOS / Linux 入口）
#
# 用法：    tools/verify/check_migrations.sh [--file PATH]
# 前置条件：无（只读源码，不连库、不构建）；需要 python3。
# 退出码： 0 通过 / 1 失败 / 2 出现 v4+ 待与 Windows 确认
# 详细判据与「故意造坏输入」示例见 check_migrations.py 顶部注释。
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$DIR/check_migrations.py" "$@"
