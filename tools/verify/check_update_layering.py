#!/usr/bin/env python3
"""check_update_layering —— 自动更新分层守卫（双胞胎 bug 防回潮自检）

用法：
    python3 tools/verify/check_update_layering.py [--root PATH]

前置条件：无（只读源码）。
    macOS/Linux 用 tools/verify/check_update_layering.sh 调用
    Windows     用 tools/verify/check_update_layering.ps1 调用

背景：更新状态机历史上实现过多份——win 壳（updater_win，已删）、前端
update.js、server /api/version、mac Sparkle 后台自查——「修一处漏一处」的
双胞胎 bug 反复出现（强制更新不拦截、灰度 0% 误判全量、同版本 build 判定
不一致、installing 不复位、残留 pending…）。分层重构后的唯一归属：

    检测判据   → core evaluate_update（全仓唯一一份）
    编排状态机 → server UpdateService（全仓唯一一份）
    平台安装   → 壳（mac = Sparkle 安装执行器；win = apply_update 协助器）

判据（任一违例 → 退出码 1）：
    1. web/ 前端不允许出现检测判据 token（版本比较 / 清单解析 / 灰度 /
       期限 / 节流 / 旧更新桥）。
    2. shell/win 不允许出现 updater_win 桥残留（类名、旧文件名、公钥宏、
       旧 HTTP/哈希库链接、shim 注入）。
    3. build 双键比较与 rollout 调用只许出现在 core/。
    4. gen_info_plist.py 必须写 SUEnableAutomaticChecks=false——Sparkle
       只当安装执行器，不允许在 mac 上留第二个检测源。
    5. 清单验签公钥 hex 只许出现在 server/src/server.cpp（代码目录内）。

退出码：
    0  通过
    1  存在违例

故意造坏输入的示例（应得到退出码 1）：
    cp web/js/update.js /tmp/update.js.bak
    echo "function compareVersion(a,b){return 0}" >> web/js/update.js
    python3 tools/verify/check_update_layering.py; echo $?
    mv /tmp/update.js.bak web/js/update.js
"""

import argparse
import pathlib
import re
import sys

# 1) 前端禁止的检测判据 token。
FORBIDDEN_WEB = [
    r"\bcompareVersion\b",
    r"\bnormalizeVersion\b",
    r"\bparseRelease\b",
    r"\bparseRemoteManifest\b",
    r"\bfetchRemoteManifest\b",
    r"\bpickUpdateAsset\b",
    r"\bupdateThrottle\w*\b",
    r"\bparseDeadline\b",
    r"\bmandatoryActive\b",
    r"\b__devcardUpdateReplay\b",
    r"\b__devcardUpdateSelftestRun\b",
    r"manifest_version",
    r"rollout",
    r"__devcardUpdater",
]

# 2) shell/win 禁止的旧更新桥残留 token（注释里也别写，防止顺手复活）。
FORBIDDEN_WIN_SHELL = [
    r"UpdaterWin",
    r"updater_win\.(h|cpp)",
    r"DEVCARD_UPDATER_PUBKEY_HEX",
    r"\bwinhttp\b",
    r"\bbcrypt\b",
    r"__devcardUpdater",
]

# 3) 只许 core 出现的判据调用（生产代码目录）。
CORE_ONLY_PATTERNS = [
    (re.compile(r"build\s*>\s*(?:local|self)"), "build 双键比较"),
    (re.compile(r"rollout_(?:bucket|allows)\s*\("), "rollout 调用"),
]
PROD_DIRS = ("server/src", "shell/mac", "shell/win", "web/js", "web/app.js",
             "modules")

# 5) 公钥 hex 允许出现的代码位置（相对仓库根）。
PUBKEY_HEX = "3f4d9fabc122113e426b98a8f8b2312cb194e406f770de61285e41c607d22403"
PUBKEY_ALLOWED = {"server/src/server.cpp"}
PUBKEY_SCAN_DIRS = ("core", "server", "shell", "web", "modules", "tests")


def iter_files(root: pathlib.Path, rel: str, suffixes=None):
    base = root / rel
    if base.is_file():
        yield base
        return
    if not base.is_dir():
        return
    for path in sorted(base.rglob("*")):
        if not path.is_file():
            continue
        if suffixes and path.suffix not in suffixes:
            continue
        yield path


def scan(files, pattern: re.Pattern):
    hits = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if pattern.search(line):
                hits.append((path, lineno, line.strip()[:100]))
    return hits


def main() -> int:
    # Windows 控制台默认 GBK，✓/✗ 会炸 UnicodeEncodeError——统一切 UTF-8。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=None)
    args = parser.parse_args()
    root = pathlib.Path(args.root) if args.root else pathlib.Path(__file__).resolve().parents[2]

    failures = []

    # 1) 前端无检测判据
    web_files = [p for d in ("web/js", "web/app.js") for p in iter_files(root, d, {".js"})]
    for token in FORBIDDEN_WEB:
        pattern = re.compile(token)
        for path, lineno, snippet in scan(web_files, pattern):
            failures.append(
                f"前端出现检测判据残留 {token}：{path.relative_to(root)}:{lineno}: {snippet}")

    # 2) win 壳无旧桥残留
    win_files = [p for p in iter_files(root, "shell/win")
                 if p.suffix in {".cpp", ".h", ".txt", ".rc", ".manifest"} or p.name == "CMakeLists.txt"]
    for token in FORBIDDEN_WIN_SHELL:
        pattern = re.compile(token, re.IGNORECASE if token in (r"\bwinhttp\b", r"\bbcrypt\b") else 0)
        for path, lineno, snippet in scan(win_files, pattern):
            failures.append(
                f"win 壳出现旧更新桥残留 {token}：{path.relative_to(root)}:{lineno}: {snippet}")

    # 3) 双键比较 / rollout 调用只许 core
    prod_files = [p for d in PROD_DIRS for p in iter_files(root, d,
                {".cpp", ".h", ".js"})]
    for pattern, label in CORE_ONLY_PATTERNS:
        for path, lineno, snippet in scan(prod_files, pattern):
            failures.append(
                f"{label} 只许存在于 core：{path.relative_to(root)}:{lineno}: {snippet}")

    # 4) Sparkle 只当安装执行器
    plist_script = root / "tools" / "release" / "gen_info_plist.py"
    if not plist_script.is_file():
        failures.append("缺少 tools/release/gen_info_plist.py")
    elif "SUEnableAutomaticChecks" not in plist_script.read_text(encoding="utf-8"):
        failures.append("gen_info_plist.py 未写 SUEnableAutomaticChecks——"
                        "Sparkle 会重新成为 mac 上的第二检测源")

    # 5) 清单公钥 hex 单点
    code_files = [p for d in PUBKEY_SCAN_DIRS for p in iter_files(root, d,
                  {".cpp", ".h", ".mm", ".js", ".txt"})]
    code_files += [root / "shell" / "win" / "CMakeLists.txt"]
    for path in code_files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if PUBKEY_HEX in text:
            rel = path.relative_to(root).as_posix()
            if rel not in PUBKEY_ALLOWED:
                failures.append(f"清单公钥 hex 出现在 {rel}（只允许 {sorted(PUBKEY_ALLOWED)}）")

    if failures:
        print("check_update_layering：发现违例")
        for item in failures:
            print(f"  ✗ {item}")
        print("\n分层约定：检测在 core、编排在 server、平台安装在壳；"
              "把判据搬回前端/壳之前，先读 docs/改造技术方案.md §11。")
        return 1

    print("check_update_layering：通过（检测判据仅 core、编排仅 server、"
          "壳无更新状态机残留、Sparkle 仅安装执行器、公钥单点）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
