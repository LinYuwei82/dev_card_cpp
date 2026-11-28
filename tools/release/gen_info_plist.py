#!/usr/bin/env python3
"""生成 DevCard macOS Info.plist（零依赖，纯 Python 标准库）。

由 CMake configure_file 或直接 shell 调：
    python3 gen_info_plist.py \\
        --version 1.2.0 \\
        --bundle-id com.anye.devcard.new \\
        --name DevCard \\
        --feed-url "" \\                 # 空则不写 SUFeedURL
        --pubkey "" \\                   # 空则不写 SUPublicEDKey
        --output Info.plist
"""

import argparse
import sys
import xml.etree.ElementTree as ET


def key(d, k, v):
    """往 dict plist 追加一对 key/value 元素。"""
    d.append(ET.Element("key"))
    d[-1].text = k
    d.append(ET.Element("string"))
    d[-1].text = v


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--version", required=True,
                   help="版本号（x.y.z，CFBundleShortVersionString）")
    p.add_argument("--build", default="",
                   help="构建号（纯数字，CFBundleVersion；留空则回退到 version）")
    p.add_argument("--bundle-id", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--feed-url", default="")
    p.add_argument("--pubkey", default="")
    p.add_argument("--output", default="Info.plist")
    args = p.parse_args()

    # CFBundleVersion 是 Sparkle 版本比较用的整数；构建号传了就用构建号，
    # 没传（开发态 / 同版本不比 build）就回退到 version。
    build = args.build if args.build else args.version

    root = ET.Element("plist", version="1.0")
    d = ET.SubElement(root, "dict")

    def plist(k, v):
        d.append(ET.Element("key"))
        d[-1].text = k
        d.append(ET.Element("string"))
        d[-1].text = v

    # 开发区域设为中文：Sparkle 等第三方框架按 app 的首选本地化挑选资源，
    # app 只声明 English 时会回退 Base（英文界面）。
    plist("CFBundleDevelopmentRegion", "zh_CN")
    plist("CFBundleExecutable", args.name)
    plist("CFBundleGetInfoString", "")
    plist("CFBundleIconFile", "DevCard.icns")
    plist("CFBundleIdentifier", args.bundle_id)
    plist("CFBundleInfoDictionaryVersion", "6.0")

    # 声明支持的语言，否则 Bundle.preferredLocalizations 只会返回 English，
    # Sparkle.framework 自带的 zh_CN.lproj 不会被选中（更新对话框显示英文）。
    d.append(ET.Element("key"))
    d[-1].text = "CFBundleLocalizations"
    _langs = ET.SubElement(d, "array")
    for _lang in ("zh_CN", "en"):
        ET.SubElement(_langs, "string").text = _lang

    plist("CFBundleLongVersionString", "")
    plist("CFBundleName", args.name)
    plist("CFBundlePackageType", "APPL")
    plist("CFBundleShortVersionString", args.version)
    plist("CFBundleSignature", "????")
    plist("CFBundleVersion", build)

    d.append(ET.Element("key"))
    d[-1].text = "CSResourcesFileMapped"
    d.append(ET.Element("true"))

    plist("LSMinimumSystemVersion", "15.0")
    plist("NSHumanReadableCopyright", "© 2026 小兔在线")

    # Sparkle 2 两键：必须同时有 SUFeedURL + SUPublicEDKey 才启用。
    # 只给一个是配置错误——Sparkle 会报错退出。
    #
    # 分层重构后 Sparkle 只当安装执行器：检测/提示统一由 core UpdateEngine +
    # 壳层 NSAlert 负责（消除 mac 上第二个检测源），所以关掉 Sparkle 的后台
    # 自动检查；应用内菜单的 checkForUpdates 手动触发不受此开关影响。
    if args.feed_url and args.pubkey:
        plist("SUFeedURL", args.feed_url)
        plist("SUPublicEDKey", args.pubkey)
        # 布尔键必须写 <false/>，plist() 只产 <string>，这里手工补。
        d.append(ET.Element("key"))
        d[-1].text = "SUEnableAutomaticChecks"
        d.append(ET.Element("false"))

    tree = ET.ElementTree(root)
    ET.indent(tree, space="\t")

    # XML 头 + DOCTYPE：ElementTree 默认不写 DOCTYPE，手动补。
    xml_bytes = ET.tostring(root, encoding="utf-8", xml_declaration=True)
    doctype = (
        b'<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" '
        b'"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
    )
    xml_head, xml_body = xml_bytes.split(b"\n", 1)
    # 结尾补换行：CMake 自己把 MACOSX_BUNDLE_INFO_PLIST 写成 bundle 内的 plist 时
    # 会在末尾加一个 \n。两边字节必须完全一致，否则每次 configure 都会互相覆盖、
    # mtime 反复越过 build.ninja，ninja 以 still dirty 失败。
    data = xml_head + b"\n" + doctype + xml_body + b"\n"

    # 内容没变就不落盘。本脚本在 configure 阶段被调用，无条件重写会让
    # Info.plist 的 mtime 越过同一次 configure 生成的 build.ninja /
    # cmake_install.cmake，ninja 据此反复判定「需要重新 configure」，
    # 直到 100 次后以 still dirty 失败（CMake 4.4.2 + Ninja 实测）。
    unchanged = False
    try:
        with open(args.output, "rb") as f:
            unchanged = f.read() == data
    except OSError:
        pass
    if unchanged:
        sys.stderr.write(f"unchanged {args.output}\n")
    else:
        with open(args.output, "wb") as f:
            f.write(data)
        sys.stderr.write(f"wrote {args.output}\n")
    if not (args.feed_url and args.pubkey):
        sys.stderr.write("  (Sparkle 未启用：缺少 SUFeedURL 或 SUPublicEDKey)\n")
    else:
        sys.stderr.write("  (Sparkle 已启用)\n")


if __name__ == "__main__":
    main()
