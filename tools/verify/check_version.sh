#!/bin/sh
# 与 check_version.py 共用同一份判据实现。
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
exec python3 "$here/check_version.py" --root "$root" "$@"
