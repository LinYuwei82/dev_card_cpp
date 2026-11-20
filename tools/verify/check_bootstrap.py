#!/usr/bin/env python3
"""check_bootstrap —— 中文数据目录下 /api/bootstrap 实跑

用法：
    python3 tools/verify/check_bootstrap.py [--server PATH] [--data-dir PATH]
                                            [--build-dir PATH]

前置条件：
    * 已构建 devcard-server（默认在 build/release/server/devcard-server，
      也接受 --server 指定；Windows 上为 devcard-server.exe）。
    * 默认数据目录是临时目录下的中文路径（不带 emoji：Windows 的窄 argv 经 ACP，
      emoji 不在 ACP 内，等于给服务端喂乱码，测的就不是 bootstrap 了）。

    macOS/Linux 用 tools/verify/check_bootstrap.sh 调用
    Windows     用 tools/verify/check_bootstrap.ps1 调用

判据：
    1. 以该数据目录启动服务，GET /api/bootstrap 返回 200；
    2. 响应的 data_dir 与传入路径一致（按 UTF-8 解码后比对，规范化绝对路径）；
    3. 响应体里不含 U+FFFD（EF BF BD）；
    4. 顺带复跑 ctest 并打印四个用例的耗时。

退出码：
    0  通过
    1  失败（bootstrap 非 200 / data_dir 不符 / 出现 U+FFFD / ctest 红）
    3  环境不具备（没构建产物、端口起不来、ctest 不可用）——
       在 Windows 上请确认已实机构建后跑 check_bootstrap.ps1，
       不要把它当成「通过」。

故意造坏输入的示例（应得到退出码 1）：
    python3 tools/verify/check_bootstrap.py --server /bin/false
"""

import argparse
import http.cookiejar
import json
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

SEED_NAME = "初始账号密码.txt"


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2]


def find_server(root: pathlib.Path, explicit: str) -> pathlib.Path | None:
    if explicit:
        path = pathlib.Path(explicit)
        return path if path.exists() else None
    for candidate in [
        root / "build" / "release" / "server" / "devcard-server",
        root / "build" / "release" / "server" / "devcard-server.exe",
        root / "build" / "windows" / "server" / "devcard-server",
        root / "build" / "windows" / "server" / "devcard-server.exe",
        root / "build" / "server" / "devcard-server",
        root / "build" / "server" / "devcard-server.exe",
    ]:
        if candidate.exists():
            return candidate
    return None


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_health(port: int, timeout: float = 15.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/api/health", timeout=1
            ) as response:
                if response.status == 200:
                    return True
        except Exception:
            time.sleep(0.2)
    return False


def read_seed_password(data_dir: pathlib.Path) -> str | None:
    for entry in data_dir.iterdir():
        if entry.name == SEED_NAME:
            for line in entry.read_text(encoding="utf-8").splitlines():
                if "：" in line and line.startswith("root"):
                    return line.split("：", 1)[1].strip()
    return None


def run_ctest(build_dir: pathlib.Path) -> int:
    if not build_dir.exists():
        print(f"[跳过] ctest：找不到构建目录 {build_dir}")
        return 3
    result = subprocess.run(
        ["ctest", "--output-on-failure"], cwd=build_dir, capture_output=True, text=True
    )
    pattern = re.compile(r"Test\s+#\d+:\s+(\S+)\s+\.+\s+(\w+)\s+([0-9.]+)\s+sec")
    timings = pattern.findall(result.stdout)
    if timings:
        print("ctest 用例耗时")
        for name, status, seconds in timings:
            print(f"  {name:<28} {status:<8} {seconds} s")
    else:
        print("ctest 输出（未解析到用例耗时）：")
        print("\n".join(result.stdout.splitlines()[-10:]))
    return 0 if result.returncode == 0 else 1


def main() -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(description="中文数据目录下 /api/bootstrap 实跑")
    parser.add_argument("--server", default="")
    parser.add_argument("--data-dir", default="")
    parser.add_argument("--build-dir", default="")
    args = parser.parse_args()

    server = find_server(root, args.server)
    if server is None:
        print("[环境不具备] 找不到 devcard-server，请先构建（或 --server 指定）")
        return 3

    holder = None
    if args.data_dir:
        data_dir = pathlib.Path(args.data_dir)
        data_dir.mkdir(parents=True, exist_ok=True)
    else:
        holder = tempfile.TemporaryDirectory(prefix="devcard-verify-")
        # 中文段：纯 ASCII 下编码错误测不出来。
        data_dir = pathlib.Path(holder.name) / "样张-数据目录"
        data_dir.mkdir(parents=True, exist_ok=True)

    if "样张" not in str(data_dir):
        print("[失败] 数据目录必须包含非 ASCII 段，否则这条用例等于没测")
        return 1

    port = free_port()
    process = subprocess.Popen(
        [str(server), "--port", str(port), "--data-dir", str(data_dir)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        if not wait_health(port):
            print("[环境不具备] 服务没有起来，输出如下：")
            print(process.stdout.read() if process.stdout else "")
            return 3

        password = read_seed_password(data_dir)
        if not password:
            print(f"[失败] 数据目录里没有可解析的 {SEED_NAME}")
            return 1

        jar = http.cookiejar.CookieJar()
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        login = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/session/login",
            data=json.dumps({"username": "root", "password": password}).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with opener.open(login, timeout=5) as response:
            login_status = response.status

        with opener.open(
            f"http://127.0.0.1:{port}/api/bootstrap", timeout=5
        ) as response:
            status = response.status
            body = response.read()

        failures = []
        if status != 200:
            failures.append(f"/api/bootstrap 返回 {status}（登录 {login_status}）")
        if b"\xef\xbf\xbd" in body:
            failures.append("响应体里出现 U+FFFD（EF BF BD），说明有字节被替换过")
        reported = ""
        try:
            reported = json.loads(body.decode("utf-8")).get("data_dir", "")
        except Exception as error:  # noqa: BLE001 - 只要解析失败就是失败
            failures.append(f"响应不是合法 JSON：{error}")
        want = str(data_dir.resolve())
        got = str(pathlib.Path(reported).resolve()) if reported else ""
        if got != want:
            failures.append(f"data_dir 不一致：响应 {reported!r}，期望 {want!r}")

        print(f"服务端：{server}")
        print(f"数据目录：{want}")
        print(f"登录：{login_status}　bootstrap：{status}")
        print(f"响应 data_dir：{reported}")

        if failures:
            for item in failures:
                print(f"[失败] {item}")
            return 1
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        if holder is not None:
            holder.cleanup()

    build_dir = pathlib.Path(args.build_dir) if args.build_dir else root / "build" / "release"
    ctest_code = run_ctest(build_dir)
    if ctest_code == 1:
        print("[失败] ctest 未全绿")
        return 1
    print()
    print("通过：中文数据目录下 /api/bootstrap 返回 200，data_dir 一致，无 U+FFFD")
    return 0


if __name__ == "__main__":
    sys.exit(main())
