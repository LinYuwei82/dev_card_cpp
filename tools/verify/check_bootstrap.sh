#!/bin/sh
# check_bootstrap.sh —— 中文数据目录下 /api/bootstrap 实跑（macOS / Linux 入口）
#
# 用法：    tools/verify/check_bootstrap.sh [--server PATH] [--data-dir PATH]
#                                       [--build-dir PATH]
# 前置条件：已构建 devcard-server（默认 build-new/server/devcard-server）；
#           需要 python3；ctest 与构建目录可选（缺了只跳过耗时那节）。
# 退出码： 0 通过 / 1 失败 / 3 环境不具备（未构建、服务起不来）
# 详细判据与「故意造坏输入」示例见 check_bootstrap.py 顶部注释。
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$DIR/check_bootstrap.py" "$@"
