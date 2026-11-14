#!/usr/bin/env python3
"""check_bom —— tools/verify 下的 .ps1 必须带 UTF-8 BOM

用法：
    python3 tools/verify/check_bom.py [--root PATH]

前置条件：无（只读文件字节）。
    macOS/Linux 用 tools/verify/check_bom.sh 调用
    Windows     用 tools/verify/check_bom.ps1 调用

判据：
    tools/verify/ 下每个 .ps1 的前三字节必须是 EF BB BF。
    只看字节，不看脚本在某个环境里是否侥幸跑通：
    Windows PowerShell 5.1 按 ANSI 读无 BOM 文件，中文注释会吃掉引号，
    解析阶段就失败（`The string is missing the terminator`），脚本一行都不跑；
    PowerShell 7.x 默认按 UTF-8 读，看不出来——拿 7.x 通过当免检会把这条永久掩盖。

退出码：
    0  通过：每个 .ps1 都有 BOM
    1  失败：存在缺 BOM 的 .ps1
"""

import argparse
import pathlib
import sys

BOM = b"\xef\xbb\xbf"


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description="tools/verify 下 .ps1 的 BOM 检查")
    parser.add_argument("--root", default=str(repo_root()))
    args = parser.parse_args()
    root = pathlib.Path(args.root)
    verify_dir = root / "tools" / "verify"

    scripts = sorted(verify_dir.glob("*.ps1"))
    print(f"扫描 {verify_dir}")
    if not scripts:
        print("  （没有 .ps1）")
        return 0

    failures = []
    for path in scripts:
        head = path.read_bytes()[:3]
        has_bom = head == BOM
        shown = " ".join(f"{byte:02X}" for byte in head)
        print(f"  {'OK  ' if has_bom else '失败'} {path.name:<28} 前三字节 {shown}")
        if not has_bom:
            failures.append(path.name)

    if failures:
        print()
        for name in failures:
            print(f"[失败] {name} 缺 UTF-8 BOM：5.1 会按 ANSI 读，中文注释导致解析失败")
        return 1
    print()
    print("通过：tools/verify 下每个 .ps1 都带 BOM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
