#!/usr/bin/env python3
"""check_version —— 安装包版本必须等于仓库根目录 VERSION.txt

用法：
    python3 tools/verify/check_version.py [--root PATH] [--setup PATH]

前置条件：无（--setup 只在 Windows 上读 PE 版本资源，其它平台跳过该项）。
    macOS/Linux 用 tools/verify/check_version.sh 调用
    Windows     用 tools/verify/check_version.ps1 调用

判据：
    1. packaging/windows/DevCard.iss 里不得出现 AppVersion 的字符串字面量，
       必须用 ISPP 的 FileRead 从 VERSION.txt 取；出现字面量即失败。
    2. 给了 --setup 时：该 exe 的版本属性（FileVersion / ProductVersion）
       必须与 VERSION.txt 逐字符相等，任一不等即失败。
    3. macOS 只看**真产物** build/release/shell/DevCard.app/Contents/Info.plist 的
       CFBundleShortVersionString；不一致即失败。
       packaging/mac/Info.plist 是旧 Qt 线的冻结文件，package_macos.sh 不引用它，
       只作为一行信息列出，不参与判定。

退出码：
    0  通过
    1  失败（版本不同源，或产物版本与 VERSION.txt 不一致）
    3  环境不具备（没有 app bundle：Windows 本机、或尚未构建 macOS 产物）。
       不是失败，也不是通过——不要把它当绿。

故意造坏输入的示例（应得到退出码 1）：
    python3 tools/verify/check_version.py --setup dist/DevCard-1.1.3-setup.exe
    （在 VERSION.txt 为 1.2.0 时）
"""

import argparse
import pathlib
import re
import subprocess
import sys

ISS = pathlib.Path("packaging/windows/DevCard.iss")
# 判据只指向真产物：package_macos.sh:123-125 读的就是这份，不一致直接 fail。
APP_PLIST = pathlib.Path("build/release/shell/DevCard.app/Contents/Info.plist")
# 旧 Qt 线的冻结文件，package_macos.sh 里一次都没出现；只列出，不判。
FROZEN_PLIST = pathlib.Path("packaging/mac/Info.plist")


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2]


def read_version(root: pathlib.Path) -> str:
    text = (root / "VERSION.txt").read_text(encoding="utf-8").strip()
    return text


def check_iss(root: pathlib.Path) -> list[str]:
    issues = []
    path = root / ISS
    text = path.read_text(encoding="utf-8", errors="replace")
    if "VERSION.txt" not in text or "FileRead" not in text:
        issues.append(f"{ISS}: 没有从 VERSION.txt 取版本（缺 FileRead）")
    # 允许 AppVersion 出现在 {#AppVersion} 与注释里，不允许出现字符串字面量。
    for number, line in enumerate(text.splitlines(), 1):
        if re.search(r'#define\s+AppVersion\s+"', line):
            issues.append(f"{ISS}:{number}: AppVersion 仍是字面量：{line.strip()}")
    return issues


PLIST_KEYS = ("CFBundleShortVersionString", "CFBundleVersion")


def check_app_plist(root: pathlib.Path, version: str) -> tuple[str, list[str], bool]:
    """读真产物的 Info.plist，双键都比。返回 (说明, 失败项, 是否缺产物)。"""
    path = root / APP_PLIST
    if not path.exists():
        return (f"（没有 app bundle：{APP_PLIST} 不存在）", [], True)
    text = path.read_text(encoding="utf-8", errors="replace")
    messages = []
    failures = []
    for key in PLIST_KEYS:
        # 缺失也判失败：package_macos.sh:123-134 是双键都比，缺一个就不算同源。
        match = re.search(rf"{key}</key>\s*<string>([^<]*)</string>", text)
        if not match:
            messages.append(f"{key} 缺失")
            failures.append(f"{APP_PLIST}: 缺少 {key}")
            continue
        found = match.group(1).strip()
        if found == version:
            messages.append(f"{key}={found}，一致")
        else:
            messages.append(f"{key}={found}，与 VERSION.txt 不一致")
            failures.append(f"{APP_PLIST}: {key} {found} != VERSION.txt {version}")
    return ("；".join(messages), failures, False)


def setup_version(path: pathlib.Path) -> tuple[str, str] | None:
    if sys.platform != "win32":
        return None
    script = (
        "$v=(Get-Item -LiteralPath '{}').VersionInfo;"
        "$v.FileVersion + '|' + $v.ProductVersion"
    ).format(str(path).replace("'", "''"))
    out = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True,
        text=True,
    )
    if out.returncode != 0:
        raise RuntimeError(out.stderr.strip() or "读取版本属性失败")
    file_version, _, product_version = out.stdout.strip().partition("|")
    return file_version.strip(), product_version.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description="安装包版本单一来源检查")
    parser.add_argument("--root", default=str(repo_root()))
    parser.add_argument("--setup", default=None, help="待检查的安装包 exe")
    args = parser.parse_args()
    root = pathlib.Path(args.root)

    version = read_version(root)
    print(f"VERSION.txt = {version}")

    failures = check_iss(root)
    print("安装脚本版本来源")
    if failures:
        for item in failures:
            print(f"  [失败] {item}")
    else:
        print(f"  {ISS}：从 VERSION.txt 取，无第二处字面量")

    print("macOS 产物 Info.plist（判据指向真产物，双键都比）")
    message, plist_failures, missing_bundle = check_app_plist(root, version)
    print(f"  {message}")
    failures.extend(plist_failures)
    frozen = root / FROZEN_PLIST
    if frozen.exists():
        print(f"  {FROZEN_PLIST}：旧 Qt 线冻结文件，不进产物、不参与判定")
    else:
        print(f"  {FROZEN_PLIST}：不存在（旧 Qt 线冻结文件，不参与判定）")

    if args.setup:
        setup = pathlib.Path(args.setup)
        print(f"安装包 {setup.name}")
        if not setup.exists():
            failures.append(f"安装包不存在：{setup}")
            print("  [失败] 文件不存在")
        else:
            got = setup_version(setup)
            if got is None:
                print("  （非 Windows，跳过 PE 版本读取）")
            else:
                file_version, product_version = got
                print(f"  FileVersion={file_version}  ProductVersion={product_version}")
                # Inno Setup 的 Setup.exe 可能不填 FileVersion（留空即跳过），
                # 但 ProductVersion 必须与 VERSION.txt 相等。
                if file_version and file_version != version:
                    failures.append(
                        f"FileVersion {file_version} != VERSION.txt {version}"
                    )
                if product_version != version:
                    failures.append(
                        f"ProductVersion {product_version} != VERSION.txt {version}"
                    )
                if not failures:
                    print("  与 VERSION.txt 一致")

    if failures:
        print()
        for item in failures:
            print(f"[失败] {item}")
        return 1
    if missing_bundle:
        print()
        print("环境不具备：没有 macOS app bundle，无法核对产物 Info.plist（退出码 3）")
        print("  这不是通过：macOS 上先构建出 build/release/shell/DevCard.app 再跑本脚本。")
        return 3
    print()
    print("通过：安装包版本与 VERSION.txt 同源")
    return 0


if __name__ == "__main__":
    sys.exit(main())
