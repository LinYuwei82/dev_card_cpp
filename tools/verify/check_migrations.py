#!/usr/bin/env python3
"""check_migrations —— 迁移号与归属核对（macOS / Linux / Windows 共用实现）

用法：
    python3 tools/verify/check_migrations.py [--file PATH]

前置条件：无（只读源码，不连数据库、不需要构建）。
    macOS/Linux 用 tools/verify/check_migrations.sh 调用
    Windows     用 tools/verify/check_migrations.ps1 调用

退出码：
    0  通过：版本号唯一、单调递增、不跳号，且已占用的 v2/v3 内容与 Windows 一致
    1  失败：版本号重复 / 不递增 / 跳号；或本分支出现 v2/v3 但建表建列与约定不符
    2  待确认：出现 v4 及以后（Windows 尚未认领的编号），需与 Windows 侧确认后再合流

故意造坏输入的示例（应得到退出码 1）：
    cp core/src/migrations.cpp /tmp/m.cpp
    # 把其中一处 {2, ...} 改成与 v1 重复的 {1, ...}，或把 v2 的 audit_log 删掉
    python3 tools/verify/check_migrations.py --file /tmp/m.cpp
"""

import argparse
import pathlib
import re
import sys

# Windows 侧已占用的编号与内容（写死比对，不随本分支变化）。
WINDOWS_OWNED = {
    2: {"device.version", "device.updated_by", "audit_log"},
    3: {"instance_heartbeat"},
}


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2]


def parse(text: str):
    """返回 (entries, sql_by_const)。entries 是 [(version, name, const)]。"""
    sql_by_const = {}
    for match in re.finditer(
        r'constexpr\s+const\s+char\*\s+(kMigration\w+)\s*=\s*R"sql\((.*?)\)sql";',
        text,
        re.S,
    ):
        sql_by_const[match.group(1)] = match.group(2)
    entries = []
    for match in re.finditer(
        r'\{\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*(kMigration\w+)\s*\}', text
    ):
        entries.append((int(match.group(1)), match.group(2), match.group(3)))
    return entries, sql_by_const


def objects_of(sql: str):
    """该版本建的表 / 加的列，统一成 'table' 或 'table.column'。"""
    found = set()
    for match in re.finditer(
        r"CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_]\w*)", sql, re.I
    ):
        found.add(match.group(1))
    for match in re.finditer(
        r"ALTER\s+TABLE\s+([A-Za-z_]\w*)\s+ADD\s+(?:COLUMN\s+)?([A-Za-z_]\w*)",
        sql,
        re.I,
    ):
        found.add(f"{match.group(1)}.{match.group(2)}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description="迁移号与归属核对")
    parser.add_argument(
        "--file",
        default=str(repo_root() / "core" / "src" / "migrations.cpp"),
        help="migrations.cpp 路径（默认按仓库位置推断）",
    )
    args = parser.parse_args()

    path = pathlib.Path(args.file)
    if not path.exists():
        print(f"[失败] 找不到 {path}")
        return 1
    text = path.read_text(encoding="utf-8")
    entries, sql_by_const = parse(text)
    if not entries:
        print(f"[失败] 在 {path} 里没有解析到任何迁移项")
        return 1

    failures = []

    # 1) 版本号唯一、单调递增、不跳号
    versions = [entry[0] for entry in entries]
    if len(set(versions)) != len(versions):
        failures.append(f"版本号重复：{versions}")
    if versions != sorted(versions):
        failures.append(f"版本号不是单调递增：{versions}")
    expected = list(range(1, max(versions) + 1))
    if versions != expected:
        failures.append(f"版本号跳号：实际 {versions}，应为 {expected}")

    # 2) 清单与 SQL 常量必须对得上
    print("版本 → 建的表 / 加的列")
    owned = {}
    for version, name, const in entries:
        if const not in sql_by_const:
            failures.append(f"v{version} 引用了不存在的常量 {const}")
            objects = set()
        else:
            objects = objects_of(sql_by_const[const])
        owned[version] = objects
        shown = "、".join(sorted(objects)) if objects else "（无建表建列）"
        print(f"  v{version:<2} {name:<28} {shown}")

    # 3) 已知归属：v2 / v3 若出现，内容必须与 Windows 一致
    for version, must in WINDOWS_OWNED.items():
        if version not in owned:
            continue
        actual = owned[version]
        if actual != must:
            missing = sorted(must - actual)
            extra = sorted(actual - must)
            detail = []
            if missing:
                detail.append(f"缺少 {missing}")
            if extra:
                detail.append(f"多出 {extra}")
            failures.append(f"v{version} 归 Windows，内容不符：{'；'.join(detail)}")

    # 4) v4 及以后：不判红，但必须与 Windows 确认
    reserved = [version for version in versions if version >= 4]

    if failures:
        print()
        for item in failures:
            print(f"[失败] {item}")
        return 1

    if reserved:
        print()
        print(
            f"[待确认] 已出现 v{reserved[0]} 及以后（{reserved}）："
            "Windows 尚未认领，合流前需与 Windows 侧确认编号与表名"
        )
        return 2

    print()
    print(f"通过：{len(versions)} 个迁移，编号连续且归属与 Windows 约定一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
