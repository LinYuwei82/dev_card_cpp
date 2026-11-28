#!/usr/bin/env python3
"""check_update_layering —— 自动更新分层守卫（双胞胎 bug 防回潮自检）

用法：
    python3 tools/verify/check_update_layering.py [--root PATH]

前置条件：无（只读源码）。
    macOS/Linux 用 tools/verify/check_update_layering.sh 调用
    Windows     用 tools/verify/check_update_layering.ps1 调用

背景：更新状态机历史上实现过多份——win 壳旧桥（updater）、前端 update.js、
server UpdateService、mac Sparkle 后台自查——「修一处漏一处」的双胞胎 bug
反复出现（强制更新不拦截、灰度 0% 误判全量、同版本 build 判定不一致、
installing 不复位、残留 pending…）。v1.4.0 mote 化重构后的唯一归属：

    检测判据 + 编排状态机 → core devcard::UpdateEngine（全仓唯一一份）
    四适配器（清单拉取/设置存储/提示 UI/平台安装）→ 壳
        mac = NSAlert + Sparkle 安装执行器
        win = TaskDialog + setup 静默装（/RELAUNCH=1）/ 绿色版打开文件夹
    server / web → 彻底退出更新链路（无 /api/version、无 /api/update/*、
        无更新横幅；update.js 已删）

判据（任一违例 → 退出码 1）：
    1. web/ 前端不允许出现检测判据 token（版本比较 / 清单解析 / 灰度 /
       期限 / 节流 / 旧更新桥），且 update.js 必须不存在、不允许再引用
       /api/version 与 /api/update。
    2. server 不允许再出现更新编排残留（UpdateService、apply_update、
       update-state.json、/api/version、/api/update）。
    3. shell/win 不允许出现旧更新桥残留（UpdaterWin 类、公钥宏、
       __devcardUpdater shim）。
    4. evaluate_update / parse_signed_manifest 调用与 build 双键比较、
       rollout 调用只许出现在 core/（壳与 server 都不许直接碰判据）。
    5. gen_info_plist.py 必须写 SUEnableAutomaticChecks=false——Sparkle
       只当安装执行器，不允许在 mac 上留第二个检测源。
    6. 清单验签公钥 hex 只许出现在 core/include/devcard/update_manifest.h。

退出码：
    0  通过
    1  存在违例

故意造坏输入的示例（应得到退出码 1）：
    echo "compareVersion(1,2)" >> web/js/core.js
    python3 tools/verify/check_update_layering.py; echo $?
    git checkout web/js/core.js
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
    r"/api/version",
    r"/api/update",
]

# 2) server 禁止的更新编排残留（v1.4.0 起 server 彻底退出更新链路）。
FORBIDDEN_SERVER = [
    r"\bUpdateService\b",
    r"\bapply_update\b",
    r"\brun_apply_update\b",
    r"update-state\.json",
    r"/api/version",
    r"/api/update",
    r"cleanup_update_partials",
    r"write_app_started_marker",
]

# 3) shell/win 禁止的旧更新桥残留 token（注释里也别写，防止顺手复活）。
FORBIDDEN_WIN_SHELL = [
    r"\bUpdaterWin\b",
    r"DEVCARD_UPDATER_PUBKEY_HEX",
    r"__devcardUpdater",
]

# 4) 只许 core 出现的判据调用（生产代码目录）。
CORE_ONLY_PATTERNS = [
    (re.compile(r"build\s*>\s*(?:local|self)"), "build 双键比较"),
    (re.compile(r"rollout_(?:bucket|allows)\s*\("), "rollout 调用"),
    (re.compile(r"\bevaluate_update\s*\("), "evaluate_update 调用"),
    (re.compile(r"\bparse_signed_manifest\s*\("), "parse_signed_manifest 调用"),
]
PROD_DIRS = ("server/src", "server/include", "shell/mac", "shell/win",
             "web/js", "web/app.js", "modules")

# 6) 公钥 hex 允许出现的代码位置（相对仓库根）。
PUBKEY_HEX = "3f4d9fabc122113e426b98a8f8b2312cb194e406f770de61285e41c607d22403"
PUBKEY_ALLOWED = {"core/include/devcard/update_manifest.h"}
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

    # 1) 前端无检测判据、无更新横幅残留
    if (root / "web" / "js" / "update.js").exists():
        failures.append("web/js/update.js 仍然存在——v1.4.0 起更新提示全在壳层，"
                        "前端横幅与状态机已删")
    web_files = [p for d in ("web/js", "web/app.js") for p in iter_files(root, d, {".js"})]
    for token in FORBIDDEN_WEB:
        pattern = re.compile(token)
        for path, lineno, snippet in scan(web_files, pattern):
            failures.append(
                f"前端出现检测判据残留 {token}：{path.relative_to(root)}:{lineno}: {snippet}")

    # 2) server 无更新编排残留
    server_files = [p for d in ("server/src", "server/include")
                    for p in iter_files(root, d, {".cpp", ".h"})]
    for token in FORBIDDEN_SERVER:
        pattern = re.compile(token)
        for path, lineno, snippet in scan(server_files, pattern):
            failures.append(
                f"server 出现更新编排残留 {token}：{path.relative_to(root)}:{lineno}: {snippet}")

    # 3) win 壳无旧桥残留
    win_files = [p for p in iter_files(root, "shell/win")
                 if p.suffix in {".cpp", ".h", ".txt", ".rc", ".manifest"} or p.name == "CMakeLists.txt"]
    for token in FORBIDDEN_WIN_SHELL:
        pattern = re.compile(token)
        for path, lineno, snippet in scan(win_files, pattern):
            failures.append(
                f"win 壳出现旧更新桥残留 {token}：{path.relative_to(root)}:{lineno}: {snippet}")

    # 4) 判据调用只许 core
    prod_files = [p for d in PROD_DIRS for p in iter_files(root, d,
                {".cpp", ".h", ".mm", ".js"})]
    for pattern, label in CORE_ONLY_PATTERNS:
        for path, lineno, snippet in scan(prod_files, pattern):
            failures.append(
                f"{label} 只许存在于 core：{path.relative_to(root)}:{lineno}: {snippet}")

    # 5) Sparkle 只当安装执行器
    plist_script = root / "tools" / "release" / "gen_info_plist.py"
    if not plist_script.is_file():
        failures.append("缺少 tools/release/gen_info_plist.py")
    elif "SUEnableAutomaticChecks" not in plist_script.read_text(encoding="utf-8"):
        failures.append("gen_info_plist.py 未写 SUEnableAutomaticChecks——"
                        "Sparkle 会重新成为 mac 上的第二检测源")

    # 6) 清单公钥 hex 单点
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
        print("\n分层约定：检测判据与编排在 core UpdateEngine、适配器与平台安装在壳、"
              "server/web 退出更新链路；搬动逻辑之前先读 docs/改造技术方案.md §11。")
        return 1

    print("check_update_layering：通过（判据+编排仅 core、壳仅适配器、"
          "server/web 无更新残留、Sparkle 仅安装执行器、公钥单点）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
