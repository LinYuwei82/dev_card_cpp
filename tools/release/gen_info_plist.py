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
    p.add_argument("--version", required=True)
    p.add_argument("--bundle-id", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--feed-url", default="")
    p.add_argument("--pubkey", default="")
    p.add_argument("--output", default="Info.plist")
    args = p.parse_args()

    root = ET.Element("plist", version="1.0")
    d = ET.SubElement(root, "dict")

    def plist(k, v):
        d.append(ET.Element("key"))
        d[-1].text = k
        d.append(ET.Element("string"))
        d[-1].text = v

    plist("CFBundleDevelopmentRegion", "English")
    plist("CFBundleExecutable", args.name)
    plist("CFBundleGetInfoString", "")
    plist("CFBundleIconFile", "DevCard.icns")
    plist("CFBundleIdentifier", args.bundle_id)
    plist("CFBundleInfoDictionaryVersion", "6.0")
    plist("CFBundleLongVersionString", "")
    plist("CFBundleName", args.name)
    plist("CFBundlePackageType", "APPL")
    plist("CFBundleShortVersionString", args.version)
    plist("CFBundleSignature", "????")
    plist("CFBundleVersion", args.version)

    d.append(ET.Element("key"))
    d[-1].text = "CSResourcesFileMapped"
    d.append(ET.Element("true"))

    plist("LSMinimumSystemVersion", "15.0")
    plist("NSHumanReadableCopyright", "")

    # Sparkle 2 两键：必须同时有 SUFeedURL + SUPublicEDKey 才启用。
    # 只给一个是配置错误——Sparkle 会报错退出。
    if args.feed_url and args.pubkey:
        plist("SUFeedURL", args.feed_url)
        plist("SUPublicEDKey", args.pubkey)

    tree = ET.ElementTree(root)
    ET.indent(tree, space="\t")

    # XML 头 + DOCTYPE：ElementTree 默认不写 DOCTYPE，手动补。
    xml_bytes = ET.tostring(root, encoding="utf-8", xml_declaration=True)
    doctype = (
        b'<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" '
        b'"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
    )
    with open(args.output, "wb") as f:
        f.write(xml_bytes.split(b"\n", 1)[0] + b"\n")  # XML 头
        f.write(doctype)
        f.write(xml_bytes.split(b"\n", 1)[1])  # 根元素起

    sys.stderr.write(f"wrote {args.output}\n")
    if not (args.feed_url and args.pubkey):
        sys.stderr.write("  (Sparkle 未启用：缺少 SUFeedURL 或 SUPublicEDKey)\n")
    else:
        sys.stderr.write("  (Sparkle 已启用)\n")


if __name__ == "__main__":
    main()
