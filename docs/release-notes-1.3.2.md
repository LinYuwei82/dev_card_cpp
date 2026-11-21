## DevCard 1.3.2

### 本次更新

- **Windows 运行时编码统一**：新增独立模块 `win_encoding`，统一命令行参数、路径、控制台输出、子进程输出重定向与错误文本的 UTF-8 边界，修复中文乱码。
- **错误文本处理更彻底**：CRT 与系统错误码文本统一转换，补充 GBK 文件端到端测试用例。
- **发布链路修复**：
  - 发布清单前先拉取线上 `manifest.json` 再合并，避免本地旧平台段把另一平台写回旧版本；
  - macOS 包内 `CFBundleVersion` 改按构建号校验（构建号 = tag 序号），避免与版本号混用。
- **发布门禁**：v2 清单校验单测接入 CI；pre-commit 的 Ed25519 公钥校验扩展为全仓扫描。

### 构建与分发

- macOS 安装包使用 **Developer ID 签名 + Apple 公证 + stapler 票据**。
- Windows 安装包与绿色版同步更新，构建号 `3`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `devcard-1.3.2-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `devcard-1.3.2-win64-setup.exe` | 安装版 |
| Windows | `devcard-1.3.2-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile devcard-1.3.2-win64-setup.exe SHA256
```

### 升级说明

- 应用内「检查更新」会自动获取本次版本；也可直接下载安装包覆盖安装。
- 从 1.3.0 升级的 macOS 用户会直接更新到 1.3.2。

### 系统要求

- macOS 14.0 及以上（Apple Silicon）
- Windows 10 及以上（64 位，需 WebView2 Runtime）

---

<sub>使用中遇到问题请在 Issues 中反馈，并附上版本号与系统信息。</sub>
