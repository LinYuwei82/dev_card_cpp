#!/usr/bin/env python3
"""check_path_helpers —— 路径转换 helper 唯一性与调用点盘点

用法：
    python3 tools/verify/check_path_helpers.py [--root PATH]

前置条件：无（只读源码）。
    macOS/Linux 用 tools/verify/check_path_helpers.sh 调用
    Windows     用 tools/verify/check_path_helpers.ps1 调用

判据：
    1. path_utf8 / path_to_utf8 / path_from_utf8 每个名字最多只许一份**定义**；
       多一份即失败（防止有人在某个 TU 里又抄一份按 ACP 解释的实现）。
    2. 输出每个名字的调用点计数与所在文件。
    3. core/src/paths.cpp 里 POSIX 分支的 .string() 单列一节，标成「允许的例外」；
       其余 .string() 只列出待核，不判红。

退出码：
    0  通过：每个 helper 名字的定义数 ≤ 1
    1  失败：某个名字有 2 份及以上定义

故意造坏输入的示例（应得到退出码 1）：
    mkdir -p /tmp/dup/core/src && cp core/src/paths.cpp /tmp/dup/core/src/a.cpp
    python3 tools/verify/check_path_helpers.py --root /tmp/dup
"""

import argparse
import pathlib
import re
import sys

HELPERS = ["path_from_utf8", "path_to_utf8", "path_utf8"]
# 只跳过仓库顶层的目录：core/src 必须扫（helpers 就定义在那里）。
SKIP_TOP_DIRS = {"build", "build-new", "build-win", "third_party", "src", ".git", "node_modules"}
DEF_PATTERN = re.compile(
    r"^(?:std::string|std::filesystem::path|auto)\s+(" + "|".join(HELPERS) + r")\s*\("
)


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2]


def iter_sources(root: pathlib.Path):
    for path in sorted(root.rglob("*")):
        if path.suffix not in {".cpp", ".h", ".hpp", ".cc"}:
            continue
        relative = path.relative_to(root)
        if relative.parts and relative.parts[0] in SKIP_TOP_DIRS:
            continue
        yield path


def main() -> int:
    parser = argparse.ArgumentParser(description="路径 helper 唯一性检查")
    parser.add_argument("--root", default=str(repo_root()))
    args = parser.parse_args()
    root = pathlib.Path(args.root)

    definitions = {name: [] for name in HELPERS}
    calls = {name: [] for name in HELPERS}
    string_uses = []
    name_re = re.compile(r"\b(" + "|".join(HELPERS) + r")\b")
    # 宽命令行里把 UTF-8 字节直接拼进 wstring 的写法（Windows 上报过的那类错）。
    narrow_into_wide = re.compile(
        r"(?:\.append\([^;]*\.begin\(\),\s*[^;]*\.end\(\)\)"
        r"|wstring\([^;]*\.begin\(\),\s*[^;]*\.end\(\)\))"
    )
    # 产品代码里出现即红；tests/ 属于 Windows 线在改的文件，只列待核。
    WIDE_ARG_PRODUCT_DIRS = {"core", "server", "shell"}
    wide_arg_offenders = {"product": [], "tests": []}

    for path in iter_sources(root):
        relative = path.relative_to(root)
        top = relative.parts[0] if relative.parts else ""
        for number, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            if narrow_into_wide.search(line):
                bucket = "product" if top in WIDE_ARG_PRODUCT_DIRS else "tests"
                wide_arg_offenders[bucket].append((relative, number, line.strip()))
            match = DEF_PATTERN.match(line)
            if match:
                # 以 `;` 结尾的是头文件声明，不是定义。
                if not line.rstrip().endswith(";"):
                    definitions[match.group(1)].append(f"{relative}:{number}")
                continue
            for name in name_re.findall(line):
                calls[name].append(str(relative))
            if ".string()" in line:
                string_uses.append((relative, number, line.strip()))

    failures = []
    print("定义（每个名字最多一份）")
    for name in HELPERS:
        where = definitions[name]
        print(f"  {name:<15} {len(where)} 份" + (f"：{', '.join(where)}" if where else ""))
        if len(where) > 1:
            failures.append(f"{name} 有 {len(where)} 份定义：{', '.join(where)}")

    print()
    print("调用点")
    for name in HELPERS:
        per_file = {}
        for file in calls[name]:
            per_file[file] = per_file.get(file, 0) + 1
        total = sum(per_file.values())
        detail = "，".join(f"{file}×{count}" for file, count in sorted(per_file.items()))
        print(f"  {name:<15} {total} 处" + (f"（{detail}）" if detail else ""))

    print()
    print("允许的例外：core/src/paths.cpp 的 POSIX 分支")
    allowed = [
        (relative, number, text)
        for relative, number, text in string_uses
        if str(relative).replace("\\", "/") == "core/src/paths.cpp"
    ]
    if allowed:
        for relative, number, text in allowed:
            print(f"  {relative}:{number}  {text}")
        print("  说明：POSIX 的窄编码就是 UTF-8，这里的 .string() 正确且必要。")
    else:
        print("  （未发现——若 paths.cpp 已改用 u8string，也属正常）")

    others = [
        (relative, number, text)
        for relative, number, text in string_uses
        if str(relative).replace("\\", "/") != "core/src/paths.cpp"
    ]
    print()
    print("其余 .string()（仅列出待核，不判红）")
    if others:
        for relative, number, text in others:
            print(f"  {relative}:{number}  {text}")
    else:
        print("  （无）")

    print()
    print("宽命令行：禁止把 UTF-8 字节直接拼进 wstring（须逐参数 MultiByteToWideChar）")
    if wide_arg_offenders["product"]:
        for relative, number, text in wide_arg_offenders["product"]:
            print(f"  [失败] {relative}:{number}  {text}")
            failures.append(
                f"{relative}:{number} 把窄字节直接拼进宽命令行：应逐参数按 CP_UTF8 转换"
            )
    else:
        print("  产品代码（core/server/shell）：无")
    for relative, number, text in wide_arg_offenders["tests"]:
        print(f"  [待核] {relative}:{number}  {text}")

    if failures:
        print()
        for item in failures:
            print(f"[失败] {item}")
        return 1
    print()
    print("通过：路径 helper 各只有一份定义，产品代码无窄字节拼宽命令行")
    return 0


if __name__ == "__main__":
    sys.exit(main())
