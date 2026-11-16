#!/usr/bin/env python3
"""Sparkle 2 EdDSA (ed25519) 签名工具——OpenSSL 命令行版本（零依赖）。

用法：
    python3 sign_update.py <file> [--key /path/to/private_key.pem]

输出：stdout 打印 base64 签名；同目录生成 <file>.edSignature 文件。
私钥默认读 ~/.config/devcard/sparkle-keys/private_key.pem。
"""

import base64
import os
import subprocess
import sys
import tempfile


def sign_with_openssl(file_path: str, key_path: str) -> str:
    """用 openssl pkeyutl 做 ed25519 签名，返回 base64。"""
    with tempfile.NamedTemporaryFile(delete=False) as tmp_sig:
        sig_path = tmp_sig.name
    try:
        proc = subprocess.run(
            ["openssl", "pkeyutl", "-sign",
             "-in", file_path,
             "-inkey", key_path,
             "-rawin",
             "-out", sig_path],
            capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(f"openssl sign failed: {proc.stderr}\n")
            sys.exit(1)
        with open(sig_path, "rb") as f:
            return base64.b64encode(f.read()).decode()
    finally:
        os.unlink(sig_path)


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)

    file_path = sys.argv[1]
    key_path = os.path.expanduser("~/.config/devcard/sparkle-keys/private_key.pem")

    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "--key":
            key_path = sys.argv[i + 1]
            i += 2
        else:
            print(f"unknown arg: {sys.argv[i]}", file=sys.stderr)
            sys.exit(2)

    if not os.path.isfile(file_path):
        print(f"error: file not found: {file_path}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(key_path):
        print(f"error: private key not found: {key_path}", file=sys.stderr)
        sys.exit(1)

    sig = sign_with_openssl(file_path, key_path)
    print(sig)

    out_path = file_path + ".edSignature"
    with open(out_path, "w") as f:
        f.write(sig + "\n")
    sys.stderr.write(f"wrote {out_path}\n")


if __name__ == "__main__":
    main()
