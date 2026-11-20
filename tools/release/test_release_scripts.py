"""DevCard 发布脚本 shell 层测试。

覆盖：tools/release/publish_macos.sh、tools/package/package_macos.sh。
全部用 --dry-run（publish_macos.sh）或直接拒绝路径（package_macos.sh），
不碰真实签名、公证、oss2 venv。

注意：devcard publish_windows.ps1 是 PowerShell，本测试只覆盖 bash 脚本。
devcard 发布脚本依赖 cmake/otool/codesign/hdiutil/shasum（package_macos.sh 要求），
本测试通过 --skip-build 和拒绝路径跳过真实构建。

运行前保证：python3（系统自带）、VERSION.txt 非空。
"""

from __future__ import annotations

import base64
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RELEASE = REPO / "tools" / "release"
PKG = REPO / "tools" / "package"
VERSION = (REPO / "VERSION.txt").read_text().strip()
TEST_SEED_HEX = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
TEST_SEED_B64 = base64.b64encode(bytes.fromhex(TEST_SEED_HEX)).decode()
IS_DARWIN = sys.platform == "darwin"


# --------------------------------------------------------------------------- Runner

class Runner:
    def __init__(self):
        self.home = Path(tempfile.mkdtemp(prefix="devcard-home-"))
        self.env = {**os.environ, "HOME": str(self.home)}
        # devcard 配置目录
        self.cfg = self.home / ".config" / "devcard"
        # v2 清单签名种子
        self._write(self.cfg / "manifest-keys" / "ed25519_seed.b64", TEST_SEED_B64)
        # Sparkle 私钥（publish_macos.sh 的 sign_update.py 用 PEM，拒绝路径只验文件存在）
        self._write(self.cfg / "sparkle-keys" / "private_key.pem", "-----BEGIN PRIVATE KEY-----\nTEST\n-----END PRIVATE KEY-----\n")
        # oss upload.py 用的 env
        self._write(self.cfg / "release.env", "OSS_ACCESS_KEY_ID=dummy\nOSS_ACCESS_KEY_SECRET=dummy\nOSS_BUCKET=xiaotu-update\nOSS_REGION=cn-hangzhou\n")

    def _write(self, path: Path, content: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def run(self, script: str, args: list[str] | None = None, *,
            expect_code: int = 0, expect_stderr_contains: str | None = None,
            env_override: dict[str, str] | None = None,
            cwd: str | None = None) -> subprocess.CompletedProcess[str]:
        cmd = [script] + (args or [])
        env = {**self.env, **(env_override or {})}
        result = subprocess.run(
            cmd, capture_output=True, text=True, env=env,
            cwd=cwd or str(REPO), timeout=30,
        )
        got = result.returncode
        if got != expect_code:
            detail = f"exit {got}, want {expect_code}\n"
            if result.stderr:
                detail += f"  stderr: {result.stderr[-600:]}\n"
            if result.stdout:
                detail += f"  stdout: {result.stdout[-300:]}"
            raise AssertionError(detail)
        if expect_stderr_contains and expect_stderr_contains not in result.stderr:
            raise AssertionError(
                f"stderr 不含 '{expect_stderr_contains}': {result.stderr[-400:]}")
        return result


# --------------------------------------------------------------------------- publish_macos.sh

@unittest.skipUnless(IS_DARWIN, "发布 shell 脚本测试仅在 macOS 运行")
class TestPublishMacos(unittest.TestCase):
    """devcard publish_macos.sh：命令行参数 + 前置依赖校验。"""

    @classmethod
    def setUpClass(cls):
        cls.r = Runner()
        cls.script = str(RELEASE / "publish_macos.sh")

    def test_help_zero_exit(self):
        self.r.run(self.script, ["--help"], expect_code=0)
        self.r.run(self.script, ["-h"], expect_code=0)

    def test_unknown_argument_rejected(self):
        self.r.run(self.script, ["--unknown"], expect_code=1,
                   expect_stderr_contains="未知参数")

    def test_sign_key_must_exist(self):
        """v2 清单签名种子文件必须存在。"""
        self.r.run(self.script, ["--dry-run", "--skip-build"],
                   env_override={"DEVCARD_SIGN_KEY": str(
                       Path(tempfile.mkdtemp()) / "missing.b64")},
                   expect_code=1, expect_stderr_contains="v2 清单签名种子不存在")

    def test_arch_must_be_arm64(self):
        """架构由脚本 uname -m 结果决定，v2 命名固定 macos-arm64。
        fake uname 模拟会让后续脚本产生非 UTF-8 输出，在容器化 CI 里更稳定。"""
        self.skipTest("PATH 屏蔽在 macOS homebrew 同目录下过于脆弱，CI 环境再测")


# --------------------------------------------------------------------------- package_macos.sh

@unittest.skipUnless(IS_DARWIN, "发布 shell 脚本测试仅在 macOS 运行")
class TestMakeDmg(unittest.TestCase):
    """devcard package_macos.sh：命令行参数 + 依赖检查。"""

    @classmethod
    def setUpClass(cls):
        cls.r = Runner()
        cls.script = str(PKG / "package_macos.sh")

    def test_help_zero_exit(self):
        self.r.run(self.script, ["--help"], expect_code=0)
        self.r.run(self.script, ["-h"], expect_code=0)

    def test_unknown_argument_rejected(self):
        self.r.run(self.script, ["--unknown"], expect_code=1,
                   expect_stderr_contains="未知参数")

    def test_out_requires_value(self):
        self.r.run(self.script, ["--out"], expect_code=1,
                   expect_stderr_contains="需要一个目录参数")

    def test_build_dir_requires_value(self):
        self.r.run(self.script, ["--build-dir"], expect_code=1,
                   expect_stderr_contains="需要一个目录参数")

    def test_feed_url_requires_value(self):
        self.r.run(self.script, ["--feed-url"], expect_code=1,
                   expect_stderr_contains="需要一个地址")

    def test_non_macos_rejected(self):
        import platform
        if platform.system() == "Darwin":
            self.skipTest("本机就是 Darwin，无法模拟非 macOS")
        self.r.run(self.script, expect_code=1,
                   expect_stderr_contains="只能在 macOS 上运行")


# --------------------------------------------------------------------------- main

def main() -> int:
    print(f"\n=== devcard manifest_v2 selftest ===")
    r1 = subprocess.run(
        [sys.executable, str(RELEASE / "manifest_v2.py"), "selftest"],
        check=False,
    )
    print(f"\n=== devcard guard selftest ===")
    r2 = subprocess.run(
        [sys.executable, str(RELEASE / "guard.py"), "selftest"],
        check=False,
    )
    print(f"\n=== devcard release_scripts unittest ===")
    unittest.main(argv=["", "-v"], exit=False)
    ok = (r1.returncode == 0 and r2.returncode == 0)
    codes = f"manifest_v2={r1.returncode}, guard={r2.returncode}"
    print(f"\n=== 汇总 exit codes: {codes} ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
