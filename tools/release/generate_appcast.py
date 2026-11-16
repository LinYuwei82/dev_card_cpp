#!/usr/bin/env python3
"""生成 Sparkle 2 appcast.xml（纯 Python，零依赖）。

Sparkle 拉这个 feed 做版本检查。appcast.xml 是一个 RSS，每个 channel/item
代表一个发布版本。DevCard 只有一个 macOS 渠道。

用法：
    python3 generate_appcast.py \\
        --version 1.2.0 \\
        --build 2024111601 \\
        --dmg-url https://oss.example.com/devcard/devcard-1.2.0-macos-arm64.dmg \\
        --dmg /path/to/devcard-1.2.0-macos-arm64.dmg \\
        --sig xxxxxxx \\          # 或者直接 --sign-dmg，脚本内部调 OpenSSL 签
        --release-notes "https://..." \\
        --output appcast.xml

注意：appcast.xml 只有一个 item，Sparkle 自己会从 enclosure.url 下载。
签名必须在运行本脚本之前完成（或用 --sign-dmg 让脚本自动签）。
"""

import argparse
import datetime
import hashlib
import os
import stat
import subprocess
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

XML_NAMESPACES = {
    "sparkle": "http://www.andymatuschak.org/xml-namespaces/sparkle",
}
SPARKLE = "{%s}" % XML_NAMESPACES["sparkle"]


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def sign_with_openssl(file_path: str, key_path: str) -> str:
    import base64
    with subprocess.Popen(
        ["openssl", "pkeyutl", "-sign", "-in", file_path,
         "-inkey", key_path, "-rawin"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE) as p:
        sig, err = p.communicate()
    if p.returncode != 0:
        raise RuntimeError(f"openssl sign failed: {err.decode()}")
    return base64.b64encode(sig).decode()


def escape_attr(s: str) -> str:
    return (s.replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace('"', "&quot;")
            .replace("'", "&apos;"))


def build_appcast(args: argparse.Namespace) -> str:
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%a, %d %b %Y %H:%M:%S +0000")
    dmg_size = os.path.getsize(args.dmg) if args.dmg else 0
    dmg_sha256 = sha256_of(args.dmg) if args.dmg else ""

    sig = args.sig
    if not sig and args.dmg and args.sign_key:
        sig = sign_with_openssl(args.dmg, args.sign_key)
        sys.stderr.write(f"signed {os.path.basename(args.dmg)} -> {sig[:20]}...\n")

    if not sig:
        sys.stderr.write("WARNING: no edSignature provided/obtained; Sparkle 会拒绝此发布\n")

    title = f"DevCard {args.version}"
    if args.title:
        title = args.title

    description = ""
    if args.release_notes_html:
        description = args.release_notes_html
    elif args.release_notes:
        # release_notes 可以是 URL 或本地文件路径
        text = args.release_notes
        if os.path.isfile(args.release_notes):
            with open(args.release_notes) as f:
                text = f.read()
        elif args.release_notes.startswith("http"):
            try:
                with urllib.request.urlopen(args.release_notes, timeout=10) as r:
                    text = r.read().decode()
            except Exception as e:
                sys.stderr.write(f"WARN: 拉 release_notes 失败: {e}\n")
                text = args.release_notes
        description = f"<![CDATA[<p>{text.replace(chr(10), '<br/>')}</p>]]>"

    channel = f'<channel>\n    <title>DevCard Updates</title>\n    <link>https://devcard.anye.com/</link>\n    <description>DevCard macOS 发布更新</description>\n    <language>en-us</language>\n    <lastBuildDate>{now}</lastBuildDate>\n\n    <item>\n        <title>{escape_attr(title)}</title>\n        <pubDate>{now}</pubDate>\n        <guid isPermaLink="false">{args.build or args.version}</guid>\n        <description>{description}</description>\n\n        <enclosure url="{escape_attr(args.dmg_url)}"\n                   sparkle:version="{escape_attr(args.version)}"\n                   sparkle:shortVersionString="{escape_attr(args.version)}"\n                   type="application/octet-stream"\n                   length="{dmg_size}"\n                   sparkle:edSignature="{escape_attr(sig or "")}"\n                   sparkle:sha256="{dmg_sha256}"/>\n\n    </item>\n</channel>'

    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">\n'
        f"{channel}\n"
        "</rss>\n"
    )


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--version", required=True, help="版本号，如 1.2.0")
    p.add_argument("--build", default="", help="构建号，如 2024111601（可选）")
    p.add_argument("--title", default="", help="item title（可选）")
    p.add_argument("--dmg-url", required=True, help="DMG 的 OSS 直链 URL")
    p.add_argument("--dmg", default="", help="本地 DMG 路径（用于算 size/sha256 + 可选自动签名）")
    p.add_argument("--sig", default="", help="ed25519 base64 签名（可选，优先级最高）")
    p.add_argument("--sign-key", default="",
                   help="ed25519 私钥 PEM 路径；提供后自动对 --dmg 签名")
    p.add_argument("--release-notes", default="",
                   help="Release notes：纯文本/URL/本地文件（自动转 HTML）")
    p.add_argument("--release-notes-html", default="",
                   help="Release notes：原始 HTML（优先级高于 --release-notes）")
    p.add_argument("--output", default="appcast.xml", help="输出文件（默认 appcast.xml）")
    args = p.parse_args()

    if args.sign_key and not args.dmg:
        sys.stderr.write("error: --sign-key 需要 --dmg\n"); sys.exit(2)

    xml = build_appcast(args)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(xml)
    sys.stderr.write(f"wrote {args.output}\n")


if __name__ == "__main__":
    main()
