#!/usr/bin/env python3
"""上传发布物到阿里云 OSS（零依赖，Python 标准库实现 V1 签名）。

凭证只从环境变量读取，不接受命令行传参，避免出现在 shell 历史与进程列表：
    OSS_ACCESS_KEY_ID
    OSS_ACCESS_KEY_SECRET
    OSS_ENDPOINT       形如 https://oss-cn-hongkong.aliyuncs.com
    OSS_BUCKET         桶名

用法：
    python3 upload_oss.py <file> [<file>...] [--prefix devcard/] [--no-public]

按扩展名自动设置 Content-Type 与 Cache-Control：
    .dmg / .exe / .zip / .edSignature   octet-stream，长缓存
    .xml                                application/xml，短缓存（appcast 需及时生效）
    .pdf                                application/pdf，长缓存

环境变量可从 ~/.config/devcard/release.env 载入后再调用：
    set -a; . ~/.config/devcard/release.env; set +a
"""

import argparse
import base64
import hashlib
import hmac
import mimetypes
import os
import sys
import urllib.parse
import urllib.request
from email.utils import formatdate

CONTENT_TYPES = {
    ".dmg": ("application/octet-stream", "public, max-age=31536000, immutable"),
    ".exe": ("application/octet-stream", "public, max-age=31536000, immutable"),
    ".zip": ("application/octet-stream", "public, max-age=31536000, immutable"),
    ".pdf": ("application/pdf", "public, max-age=86400"),
    ".xml": ("application/xml; charset=utf-8", "public, max-age=60"),
    ".json": ("application/json; charset=utf-8", "public, max-age=60"),
    ".html": ("text/html; charset=utf-8", "public, max-age=3600"),
    ".htm": ("text/html; charset=utf-8", "public, max-age=3600"),
    ".css": ("text/css; charset=utf-8", "public, max-age=86400"),
    ".js": ("application/javascript; charset=utf-8", "public, max-age=86400"),
    ".edsignature": ("text/plain; charset=utf-8", "public, max-age=300"),
}


def require_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        sys.exit(f"error: 环境变量 {name} 未设置")
    return value


def content_meta(path: str):
    ext = os.path.splitext(path)[1].lower()
    if ext in CONTENT_TYPES:
        return CONTENT_TYPES[ext]
    guessed = mimetypes.guess_type(path)[0] or "application/octet-stream"
    return guessed, "public, max-age=3600"


def sign(secret: str, string_to_sign: str) -> str:
    digest = hmac.new(secret.encode(), string_to_sign.encode(), hashlib.sha1).digest()
    return base64.b64encode(digest).decode()


def put_object(endpoint: str, bucket: str, key: str, path: str,
               ak_id: str, ak_secret: str, make_public: bool) -> None:
    with open(path, "rb") as f:
        body = f.read()

    content_type, cache_control = content_meta(path)
    content_md5 = base64.b64encode(hashlib.md5(body).digest()).decode()
    date = formatdate(usegmt=True)

    oss_headers = {}
    if make_public:
        oss_headers["x-oss-object-acl"] = "public-read"
    canonical_oss_headers = "".join(
        f"{k}:{v}\n" for k, v in sorted(oss_headers.items()))

    resource = f"/{bucket}/{key}"
    string_to_sign = (
        f"PUT\n{content_md5}\n{content_type}\n{date}\n"
        f"{canonical_oss_headers}{resource}"
    )
    signature = sign(ak_secret, string_to_sign)

    # OSS 3.x 桶必须用三级域名访问：https://<bucket>.<endpoint-host>/<key>
    # CanonicalizedResource 仍是 /<bucket>/<key>。
    parsed = urllib.parse.urlsplit(endpoint)
    host = f"{bucket}.{parsed.netloc}"
    url = urllib.parse.urlunsplit(
        (parsed.scheme or "https", host, "/" + key, "", ""))
    req = urllib.request.Request(url, data=body, method="PUT")
    req.add_header("Date", date)
    req.add_header("Content-Type", content_type)
    req.add_header("Content-MD5", content_md5)
    req.add_header("Cache-Control", cache_control)
    req.add_header("Authorization", f"OSS {ak_id}:{signature}")
    for k, v in oss_headers.items():
        req.add_header(k, v)

    size_mb = len(body) / 1024 / 1024
    print(f"→ PUT {key}  ({size_mb:.2f} MB, {content_type})", flush=True)
    try:
        with urllib.request.urlopen(req, timeout=180) as resp:
            print(f"  {resp.status} {resp.headers.get('ETag', '')}")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:400]
        sys.exit(f"  上传失败 HTTP {e.code}: {detail}")


def main():
    p = argparse.ArgumentParser(description="上传发布物到阿里云 OSS")
    p.add_argument("files", nargs="+", help="本地文件路径")
    p.add_argument("--prefix", default="devcard/", help="OSS 对象 key 前缀（默认 devcard/）")
    p.add_argument("--no-public", action="store_true", help="不设置 public-read ACL")
    args = p.parse_args()

    ak_id = require_env("OSS_ACCESS_KEY_ID")
    ak_secret = require_env("OSS_ACCESS_KEY_SECRET")
    endpoint = require_env("OSS_ENDPOINT")
    bucket = require_env("OSS_BUCKET")

    prefix = args.prefix
    if prefix and not prefix.endswith("/"):
        prefix += "/"

    for path in args.files:
        if not os.path.isfile(path):
            sys.exit(f"error: 文件不存在 {path}")
        key = prefix + os.path.basename(path)
        put_object(endpoint, bucket, key, path, ak_id, ak_secret, not args.no_public)

    print("\n完成。访问前缀：")
    print(f"  {endpoint.rstrip('/')}/{prefix}")


if __name__ == "__main__":
    main()
