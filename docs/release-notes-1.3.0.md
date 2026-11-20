## DevCard 1.3.0

### 本次变更

- **构建号可核对**：CMake 新增可选参数 `DEVCARD_BUILD=<UTC yyyyMMddHHmm>`，经 `cmake/devcard_version.cmake` 的 `devcard_add_build_stamp()` 拆分为整型与字符串双宏（`DEVCARD_BUILD_STAMP=<整数>` 给 core 里 `compiled_build_number()` 读、`DEVCARD_BUILD_STAMP_STR="<字符串>"` 给 shell/win/main.cpp 拼 `"DEVCARD_BUILD_STAMP="` 可执行文件内 grep 标记），打入所有生产 target（devcard_core / devcard_server / devcard-server / DevCard 壳 / 三测试 target）。客户端同版本重发时按清单 `build > 本机构建号` 才提示更新，未注入时自动回退到只比 version。本机 Windows 验证脚本 `tools/release/verify_build_stamp.ps1`（PowerShell 5 零依赖，T1–T7 全链路覆盖），guard.py preflight 强制 DevCard v2 清单同时含 `platforms.macos + platforms.windows`。
- **Windows 构建适配新版 SDK**：修正接口契约测试中 `winsock2.h` / `ws2tcpip.h` 的包含顺序，解决 Windows SDK 10.0.26100 下的编译失败，CI 恢复通过。
- **macOS 签名与公证**：安装包使用 Developer ID 签名，附带安全时间戳并完成 Apple 公证。

### 构建与分发

- Windows 安装包与绿色版同步更新，构建号 `202611191436`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `devcard-1.3.0-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `devcard-1.3.0-win64-setup.exe` | 安装版 |
| Windows | `devcard-1.3.0-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile devcard-1.3.0-win64-setup.exe SHA256
```

### 系统要求

- macOS 15.0 及以上（Apple Silicon）
- Windows 10 及以上（64 位）

---

<sub>使用中遇到问题请在 Issues 中反馈，并附上版本号与系统信息。</sub>
