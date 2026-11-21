## DevCard 1.3.3

### 本次更新

- **Windows 应用内更新修复**：更新桥改用 v2 清单的 Ed25519 签名公钥做验签，修复此前更新包校验失败报 `SIGNATURE_INVALID` 的问题。
- **发布门禁增强**：pre-commit 增加清单 Ed25519 公钥 hex 一致性校验，包内 `Info.plist` 与清单公钥不一致时直接拦截。
- **发布链路修复**：
  - 发布清单前先拉取线上 `manifest.json` 再合并，避免本地旧平台段把另一平台写回旧版本；
  - macOS 包内 `CFBundleVersion` 改按构建号（tag 序号）校验；
  - Release 标题与说明改为 UTF-8 文件传递，修复 Gitee 标题乱码。
- **构建号统一**：macOS 与 Windows 的 `build` 均等于 tag 序号（本次为 4），两端同号。

### 构建与分发

- macOS 安装包使用 **Developer ID 签名 + Apple 公证 + stapler 票据**。
- Windows 安装包与绿色版同步更新，构建号 `4`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `devcard-1.3.3-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `devcard-1.3.3-win64-setup.exe` | 安装版 |
| Windows | `devcard-1.3.3-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile devcard-1.3.3-win64-setup.exe SHA256
```

### 升级说明

- 应用内「检查更新」会自动获取本次版本；也可直接下载安装包覆盖安装。
- Windows 用户此前若遇到更新失败（`SIGNATURE_INVALID`），安装本版本后即可正常自动更新。

### 系统要求

- macOS 15.0 及以上（Apple Silicon）
- Windows 10 及以上（64 位，需 WebView2 Runtime）

---

<sub>使用中遇到问题请在 Issues 中反馈，并附上版本号与系统信息。</sub>
